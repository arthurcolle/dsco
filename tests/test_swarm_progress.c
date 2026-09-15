/* Real child pipes + real provider/tool wait hooks, without inference. */
#include "swarm_progress.h"
#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "vm.h"
#include "tui.h"
#include "tui_swarm_dock.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static double latency[128], first_seen;
static int received;
static atomic_bool locked, release_lock, stop_ticks;
static bool busy, runtime_wait;

static double ms(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void nap(int delay) { while (poll(NULL, 0, delay) < 0) assert(errno == EINTR); }
static void observe(int id, const char *data, size_t n, void *ctx) {
    (void)n; (void)ctx;
    int parsed; double written;
    if (sscanf(data, "worker:%d:%lf", &parsed, &written) == 2) {
        assert(parsed == id && latency[id] < 0);
        latency[id] = ms() - written;
        if (!received++) first_seen = ms();
    }
    if (!runtime_wait) assert(swarm_progress_tick() == 0); /* tick-owned recursion suppressed */
}
static swarm_t *children(int count) {
    swarm_t *s = runtime_wait ? tools_swarm_instance() : calloc(1, sizeof(*s)); assert(s);
    if (!runtime_wait) swarm_init(s, NULL, "fixture");
    s->stream_cb = observe;
    received = 0; first_seen = 0;
    for (int i = 0; i < count; i++) {
        int out[2], err[2]; assert(pipe(out) == 0 && pipe(err) == 0);
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            close(out[0]); close(err[0]); nap(80 + i % 8);
            char data[96]; int n = snprintf(data, sizeof(data), "worker:%d:%.3f\n", i, ms());
            assert(write(out[1], data, (size_t)n) == n);
            if (busy && i == 0) {
                char block[4096]; memset(block,'B',sizeof(block));
                for (int k=0;k<32;k++) {
                    size_t left=sizeof(block);
                    while (left) { ssize_t written=write(out[1],block,left); assert(written>0); left-=(size_t)written; }
                }
                assert(write(out[1],"TAIL-PROOF\n",11)==11);
            }
            assert(write(err[1], "stderr-proof\n", 13) == 13);
            close(out[1]); close(err[1]); nap(runtime_wait ? 5000 : 60); _exit(i % 7 == 6 ? 7 : 0);
        }
        close(out[1]); close(err[1]);
        assert(fcntl(out[0], F_SETFL, O_NONBLOCK) == 0);
        assert(fcntl(err[0], F_SETFL, O_NONBLOCK) == 0);
        swarm_child_t *c = &s->children[i];
        c->id = i; c->pid = pid; c->pipe_fd = out[0]; c->err_fd = err[0]; c->cost_fd = -1;
        c->status = SWARM_RUNNING; c->executor = EXECUTOR_DSCO;
        c->output_cap = 4096; c->output = calloc(1, c->output_cap); assert(c->output);
        c->stream_buf = calloc(1,4096); assert(c->stream_buf);
        snprintf(c->task, sizeof(c->task), "pipe fixture %d", i);
        s->active.words[i / 64] |= 1ULL << (i % 64); s->active.count++; s->child_count++;
        latency[i] = -1;
    }
    swarm_progress_attach(s);
    return s;
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}
static void finish(swarm_t *s, const char *mode, double started) {
    SWARM_PROGRESS_GUARD;
    assert(s->active.count == 0 && received == s->child_count);
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        assert(c->status == (i % 7 == 6 ? SWARM_ERROR : SWARM_DONE));
        assert(c->exit_code == (i % 7 == 6 ? 7 : 0));
        assert(strstr(c->output, "worker:") && strstr(c->output, "stderr-proof"));
        assert(c->pipe_fd == -1 && c->err_fd == -1);
        if (busy && i==0) {
            assert(strstr(c->output,"TAIL-PROOF")); size_t bytes=0;
            for (size_t j=0;j<c->output_len;j++) bytes+=c->output[j]=='B';
            assert(bytes==128*1024);
        }
        int status; assert(waitpid(c->pid, &status, WNOHANG) == -1 && errno == ECHILD);
    }
    assert(s->done_q.count == s->child_count);
    double sorted[128]; memcpy(sorted, latency, sizeof(sorted));
    qsort(sorted, s->child_count, sizeof(double), cmp_double);
    printf("{\"mode\":\"%s\",\"workers\":%d,\"verified\":%d,\"elapsed_ms\":%.3f,\"delivery_p50_ms\":%.3f,\"delivery_p95_ms\":%.3f,\"delivery_max_ms\":%.3f}\n",
           mode, s->child_count, received, ms()-started, sorted[s->child_count/2],
           sorted[(s->child_count-1)*95/100], sorted[s->child_count-1]);
    swarm_progress_detach(s);
    assert(swarm_progress_tick() == 0);
    swarm_destroy(s); free(s);
}
static void *holder(void *unused) {
    (void)unused; SWARM_PROGRESS_GUARD; atomic_store(&locked, true);
    while (!atomic_load(&release_lock)) nap(1);
    return NULL;
}
static void test_lease(void) {
    swarm_t *s = children(1);
    pthread_t thread; assert(pthread_create(&thread, NULL, holder, NULL) == 0);
    while (!atomic_load(&locked)) nap(1);
    double before = ms(); assert(swarm_progress_tick() == 0 && ms() - before < 50);
    /* A forked child must skip the inherited locked mutex before trying it. */
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) _exit(swarm_progress_tick() == 0 ? 0 : 3);
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    atomic_store(&release_lock, true); pthread_join(thread, NULL);
    double start = ms();
    while (s->active.count && ms() - start < 2000) { nap(10); swarm_progress_tick(); }
    finish(s, "lease-contention-fork", start);
}

typedef struct { int fd; const char *body; } server_t;
static void send_all(int fd, const char *data) {
    size_t len = strlen(data);
    while (len) { ssize_t n = write(fd, data, len); assert(n > 0); data += n; len -= (size_t)n; }
}
static void *serve(void *arg) {
    server_t *s = arg;
    int fd = accept(s->fd, NULL, NULL); assert(fd >= 0);
    char req[8192]; ssize_t n = read(fd, req, sizeof(req)); assert(n > 0);
    char headers[256]; snprintf(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(s->body));
    send_all(fd, headers);
    nap(1700); /* quiet server: curl progress, not body data, must drain children */
    send_all(fd, s->body); close(fd); close(s->fd); return NULL;
}
static void http_wait(swarm_t *s, bool anthropic) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(0x7f000001U)};
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(fd, 1) == 0);
    socklen_t len=sizeof(addr); assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    char url[128]; snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1/messages", ntohs(addr.sin_port));
    const char *a = "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"fixture\",\"role\":\"assistant\",\"content\":[],\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n\nevent: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":0}}\n\nevent: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    const char *o = "data: {\"id\":\"fixture\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    server_t server={fd, anthropic ? a : o}; pthread_t thread;
    assert(pthread_create(&thread, NULL, serve, &server) == 0);
    stream_result_t result;
    if (anthropic) result=llm_stream_reuse_url(NULL,url,"fixture-not-a-secret","{}",NULL,NULL,NULL,NULL,NULL);
    else {
        assert(setenv("OPENAI_API_BASE",url,1) == 0);
        assert(setenv("OPENAI_BASE_URL",url,1) == 0);
        provider_t *p=provider_create("openai"); assert(p);
        assert(p->api_url && !strncmp(p->api_url,url,strlen(url)));
        result=p->stream(p,"fixture-not-a-secret","{\"model\":\"fixture\",\"messages\":[]}",NULL,NULL,NULL,NULL,NULL);
        provider_free(p);
    }
    assert(result.http_status == 200 && result.ok);
    assert(s->active.count == 0 && first_seen > 0 && ms() - first_seen > 100);
    pthread_join(thread,NULL);
    json_free_response(&result.parsed); free(result.actual_model); free(result.generation_id);
}
typedef struct { pthread_mutex_t mutex; pthread_cond_t ready; bool done; } completion_t;
static void *complete_later(void *ctx) {
    completion_t *c=ctx; nap(600);
    pthread_mutex_lock(&c->mutex); c->done=true; pthread_cond_signal(&c->ready); pthread_mutex_unlock(&c->mutex);
    return NULL;
}
static void completion_wait(swarm_t *s) {
    completion_t c={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,false}; pthread_t worker;
    assert(pthread_create(&worker,NULL,complete_later,&c)==0);
    pthread_mutex_lock(&c.mutex);
    while (!c.done) swarm_progress_wait(&c.ready,&c.mutex);
    pthread_mutex_unlock(&c.mutex); pthread_join(worker,NULL);
    assert(s->active.count==0 && ms()-first_seen>100);
    pthread_mutex_destroy(&c.mutex); pthread_cond_destroy(&c.ready);
}
static void *tick_thread(void *ctx) {
    (void)ctx; while (!atomic_load(&stop_ticks)) { swarm_progress_tick(); nap(1); } return NULL;
}
static void concurrent_wait(swarm_t *s) {
    pthread_t a,b; assert(pthread_create(&a,NULL,tick_thread,NULL)==0 && pthread_create(&b,NULL,tick_thread,NULL)==0);
    double start=ms();
    for (;;) {
        bool done;
        { SWARM_PROGRESS_GUARD; done=s->active.count==0; }
        if (done) break;
        assert(ms()-start<5000); nap(1);
    }
    atomic_store(&stop_ticks,true); pthread_join(a,NULL); pthread_join(b,NULL);
}

static atomic_bool waiting;
static void *agent_waiter(void *unused) {
    (void)unused; char out[4096]; atomic_store(&waiting,true);
    bool ok=tools_execute_for_tier("agent","{\"action\":\"wait\",\"id\":0,\"timeout\":6}","trusted",out,sizeof(out));
    assert(!ok); /* killed, not successful */
    return NULL;
}
static void kill_during_wait(void) {
    tools_init_local_only(); runtime_wait=true;
    swarm_t *s=children(1); pthread_t thread;
    assert(pthread_create(&thread,NULL,agent_waiter,NULL)==0);
    while (!atomic_load(&waiting)) nap(1);
    nap(120); double start=ms(); char out[4096];
    assert(tools_execute_for_tier("agent","{\"action\":\"kill\",\"id\":0}","trusted",out,sizeof(out)));
    assert(ms()-start<1500); /* must not wait for the 5s child or 6s join */
    pthread_join(thread,NULL);
    { SWARM_PROGRESS_GUARD;
      assert(s->active.count==0 && s->children[0].status==SWARM_KILLED);
      swarm_progress_detach(s);
    }
    puts("PASS: governed agent kill interrupts a concurrent wait without waiting for child exit deadline");
}

static char draft[128];
static bool submitted;
static void *composer(void *unused) {
    (void)unused;
    tui_status_bar_t status; tui_status_bar_init(&status, "cooperative fixture");
    status.animations_enabled=false;
    submitted=tui_composer_read(&status,NULL,draft,sizeof(draft)) != NULL;
    pthread_mutex_destroy(&status.mutex);
    return NULL;
}

int main(int argc, char **argv) {
    assert(swarm_progress_tick() == 0);
    setenv("DSCO_PIXEL_TUI", "0", 1); setenv("DSCO_KITTY_AGENT_WINDOWS", "0", 1);
    setenv("NO_PROXY", "*", 1); setenv("no_proxy", "*", 1);
    if (argc == 1) { test_lease(); return 0; }
    if (!strcmp(argv[1],"kill-wait")) { kill_during_wait(); return 0; }
    int count = argc > 2 ? atoi(argv[2]) : 16; assert(count > 0 && count <= 100);
    bool tui_mode=!strcmp(argv[1],"tui"), negative=!strcmp(argv[1],"negative-tool");
    pthread_t reader;
    if (tui_mode) {
        tui_swarm_dock_reset();
        assert(pthread_create(&reader,NULL,composer,NULL) == 0);
        double ready=ms();
        while (!tui_composer_is_reading() && ms()-ready<2000) nap(5);
        assert(tui_composer_is_reading());
    }
    busy=!strcmp(argv[1],"busy");
    double start=ms(); swarm_t *s=children(count);
    if (negative) swarm_progress_detach(s);
    if (!strcmp(argv[1],"condition")) completion_wait(s);
    else if (!strcmp(argv[1],"concurrent")) concurrent_wait(s);
    else if (tui_mode || !strcmp(argv[1], "anthropic") || !strcmp(argv[1], "openai")) http_wait(s,tui_mode || !strcmp(argv[1],"anthropic"));
    else if (negative || !strcmp(argv[1], "tool")) {
        tools_init_local_only(); char out[4096];
        bool ok=tools_execute_for_tier("bash","{\"command\":\"sleep 0.6; printf root-wait-done\"}","trusted",out,sizeof(out));
        if (!ok) fprintf(stderr,"tool failure: %s\n",out);
        assert(ok && strstr(out,"root-wait-done"));
        if (negative) {
            assert(s->active.count == count && received == 0); /* old stale state */
            puts("NEGATIVE_CONTROL: actual tool wait left all worker observations stale without attachment");
            swarm_progress_attach(s);
            while (s->active.count && ms()-start<3000) { nap(10); swarm_progress_tick(); }
        } else assert(s->active.count == 0 && first_seen > 0 && ms()-first_seen > 100);
    } else {
        while (s->active.count && ms()-start<5000) {
            nap(10); errno=EINTR; swarm_progress_tick(); assert(errno==EINTR);
        }
    }
    if (tui_mode) {
        tui_swarm_dock_append(6,"ROOT_WAIT_COMPLETE\n",19);
        pthread_join(reader,NULL); tui_cleanup();
        assert(submitted && !strcmp(draft,"draft-preserved"));
        fprintf(stderr,"\nSUBMITTED:%s\n",draft);
    }
    finish(s,argv[1],start); return 0;
}
