/* Real owned processes and pipes; no provider requests or user credentials. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "swarm_lifecycle.h"
#include "swarm_progress.h"
#include "vm.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static int handler_fd;
static char tail[128 * 1024];
static int requested_events, forced_events;

static double seconds(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void shutdown_tail(int signal_number) {
    (void)signal_number;
    size_t written = 0;
    while (written < sizeof(tail)) {
        ssize_t n = write(handler_fd, tail + written, sizeof(tail) - written);
        if (n > 0) written += (size_t)n;
        else if (errno != EINTR) _exit(3);
    }
    _exit(0);
}

static void event(const swarm_child_t *c, bool forced, void *ctx) {
    (void)ctx;
    assert(c->cancel_requested && !c->reaped);
    if (forced) { assert(c->cancel_forced); ++forced_events; }
    else ++requested_events;
}

static void spawn_fixture(swarm_t *s, int id, int mode) {
    int out[2], ready[2]; assert(pipe(out) == 0 && pipe(ready) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(out[0]); close(ready[0]);
        assert(setpgid(0, 0) == 0);
        if (mode == 0) {
            handler_fd = out[1]; memset(tail, 'T', sizeof(tail));
            memcpy(tail + sizeof(tail) - 10, "TAIL_PROOF", 10);
            assert(signal(SIGTERM, shutdown_tail) != SIG_ERR);
        } else assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        assert(write(ready[1], "R", 1) == 1); close(ready[1]);
        if (mode == 2) {
            /* Unrelated readiness causes early poll returns throughout the
             * target's grace window. An iteration budget escalates too soon. */
            char bytes[4096]; memset(bytes, 'B', sizeof(bytes));
            double until = seconds() + .5;
            while (seconds() < until) assert(write(out[1], bytes, sizeof(bytes)) > 0);
            _exit(0);
        }
        for (;;) pause();
    }
    close(out[1]); close(ready[1]); char byte;
    assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
    assert(fcntl(out[0], F_SETFL, O_NONBLOCK) == 0);
    swarm_child_t *c = &s->children[id];
    c->id = id; c->run_id = ++s->next_run_id; c->pid = pid;
    c->pipe_fd = out[0]; c->err_fd = c->cost_fd = -1;
    c->status = SWARM_RUNNING; c->executor = EXECUTOR_DSCO;
    struct timespec wall; assert(clock_gettime(CLOCK_REALTIME, &wall) == 0);
    c->group_id = 0; c->start_time = wall.tv_sec + wall.tv_nsec / 1e9;
    c->output_cap = 4096; c->output = calloc(1, c->output_cap); assert(c->output);
    s->active.words[id / 64] |= 1ULL << (id % 64); ++s->active.count;
    s->child_count = id + 1;
    s->groups[0].child_ids[s->groups[0].child_count++] = id;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0); long n = ftell(f); assert(n >= 0);
    rewind(f); char *data = calloc(1, (size_t)n + 1); assert(data);
    assert(fread(data, 1, (size_t)n, f) == (size_t)n); assert(fclose(f) == 0);
    return data;
}

int main(void) {
    char home[] = "/tmp/dsco-lifecycle-XXXXXX"; assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    setenv("DSCO_PIXEL_TUI", "0", 1); setenv("DSCO_KITTY_AGENT_WINDOWS", "0", 1);
    setenv("DSCO_SWARM_MAX_OUTPUT", "524288", 1);
    swarm_t *s = calloc(1, sizeof(*s)); assert(s); swarm_init(s, NULL, "fixture");
    s->group_count = 1; s->groups[0].active = true;
    SWARM_PROGRESS_GUARD;

    spawn_fixture(s, 0, 0);
    swarm_cancel_report_t r = swarm_cancel_group(s, 0, 1000, NULL, NULL, event, NULL);
    swarm_child_t *c = &s->children[0];
    assert(r.requested == 1 && r.forced == 0 && r.remaining == 0 && r.poll_error == 0);
    assert(c->reaped && c->status == SWARM_KILLED && c->exit_code == 0);
    assert(c->output_len == sizeof(tail) && strstr(c->output, "TAIL_PROOF"));
    assert(c->result_persisted && c->result_errno == 0 && c->result_path[0]);
    struct stat st; assert(stat(c->result_path, &st) == 0 && (st.st_mode & 0777) == 0600);
    char *original = read_file(c->result_path);
    assert(strstr(original, "TAIL_PROOF") && strstr(original, "\"executor\":\"dsco\""));
    assert(strstr(original, "\"cost_usd\":null"));
    puts("PASS: cooperative cancellation drains 128 KiB and preserves exit-zero cancellation");

    r = swarm_cancel_group(s, 0, 1000, NULL, NULL, event, NULL);
    assert(r.requested == 0 && r.forced == 0 && r.remaining == 0 && r.elapsed_ms < 50);
    assert(requested_events == 1 && forced_events == 0);
    puts("PASS: repeated cancellation is idempotent");

    spawn_fixture(s, 1, 1); spawn_fixture(s, 2, 2);
    r = swarm_cancel_child(s, 1, 250, NULL, NULL, event, NULL);
    assert(r.requested == 1 && r.forced == 1 && r.remaining == 0 && r.poll_error == 0);
    assert(r.elapsed_ms >= 240 && r.elapsed_ms < 2500);
    assert(s->children[1].reaped && s->children[1].cancel_forced);
    while (s->active.count) assert(swarm_poll(s, 50) >= 0);
    assert(s->children[2].status == SWARM_DONE && s->children[2].result_persisted);
    char *after = read_file(c->result_path); assert(!strcmp(original, after)); free(after);
    printf("PASS: busy unrelated worker preserves %.1f ms cancellation grace\n", r.elapsed_ms);

    /* Immutable evidence must survive a namespace collision. */
    swarm_child_t collision = *c;
    collision.result_persisted = false; collision.result_path[0] = 0;
    collision.output = "REPLACEMENT_MUST_NOT_APPEAR"; collision.output_len = strlen(collision.output);
    assert(!swarm_result_persist(&collision) && collision.result_errno == EEXIST);
    after = read_file(c->result_path); assert(!strcmp(original, after)); free(after); free(original);
    puts("PASS: immutable native receipt rejects run-ID collision without overwriting evidence");

    /* A failed checkpoint is explicit and can recover after fixing storage. */
    char blocked[] = "/tmp/dsco-lifecycle-blocked-XXXXXX"; assert(mkdtemp(blocked));
    char obstruction[256]; snprintf(obstruction, sizeof(obstruction), "%s/.dsco", blocked);
    int fd = open(obstruction, O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
    setenv("HOME", blocked, 1);
    swarm_child_t failed = *c; failed.run_id = 900;
    failed.result_persisted = false; failed.result_path[0] = 0;
    assert(!swarm_result_persist(&failed) && failed.result_errno == ENOTDIR);
    assert(unlink(obstruction) == 0);
    assert(swarm_result_persist(&failed) && failed.result_errno == 0);
    setenv("HOME", home, 1);
    puts("PASS: storage failure is visible and checkpoint can recover");

    jbuf_t b; jbuf_init(&b, 2048); jbuf_append(&b, "{");
    jbuf_append(&b, "\"id\":0"); swarm_group_lifecycle_append(&b, s, 0); jbuf_append(&b, "}");
    assert(json_is_valid_container(b.data));
    assert(strstr(b.data, "\"shutdown_complete\":true") && strstr(b.data, "\"persisted_results\":3"));
    jbuf_free(&b);
    char status[16384]; swarm_group_status_json(s, 0, status, sizeof(status));
    assert(json_is_valid_container(status) && strstr(status, "\"result_persisted\":true"));
    puts("PASS: group and child status expose confirmed reaping and persisted result paths");
    swarm_destroy(s); free(s);
    return 0;
}
