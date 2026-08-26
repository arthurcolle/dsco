/* src/swarm_daemon.c — durable detached swarm supervisor
 *
 * This is intentionally a small, OS-process-owned control plane rather than
 * another layer inside g_swarm. The parent tool/API may disappear; workers are
 * retained by a daemon (setsid) and every state transition is fsync'd.
 *
 * CLI:
 *   dsco swarmd start --name NAME [--workdir DIR] -- task1 [--task task2 ...]
 *   dsco swarmd status --run RUN_OR_NAME [--workdir DIR] [--json]
 *   dsco swarmd collect --run RUN_OR_NAME [--workdir DIR] [--wait SEC] [--json]
 *   dsco swarmd abort --run RUN_OR_NAME [--workdir DIR]
 *   dsco swarmd worker --run RUN_DIR --id N -- task...
 *
 * Worker command is dsco --profile worker -p TASK, selectable via
 * DSCO_SWARM_DAEMON_MODEL / DSCO_SWARM_DAEMON_PROVIDER. Workers write their
 * own immutable result.json; daemon only reconciles them. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1

#include "swarm_daemon.h"
#include "json_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SWARMD_MAX_WORKERS 16
#define SWARMD_NAME_MAX 96
#define SWARMD_TASK_MAX 4096
#define SWARMD_LINE_MAX 8192

typedef enum {
    SWARMD_QUEUED = 0,
    SWARMD_RUNNING,
    SWARMD_DONE,
    SWARMD_FAILED,
    SWARMD_ABORTED,
} swarmd_status_t;

typedef struct {
    int id;
    pid_t pid;
    pid_t pgid;
    swarmd_status_t status;
    time_t started_at;
    time_t ended_at;
    char task[SWARMD_TASK_MAX];
    char result_path[PATH_MAX];
    char log_path[PATH_MAX];
    int exit_code;
    time_t last_heartbeat;
    off_t log_bytes;
} swarmd_worker_t;

typedef struct {
    char run_id[160];
    char name[SWARMD_NAME_MAX];
    char run_dir[PATH_MAX];
    pid_t supervisor_pid;
    time_t created_at;
    time_t updated_at;
    bool abort_requested;
    int worker_count;
    swarmd_worker_t workers[SWARMD_MAX_WORKERS];
} swarmd_run_t;

static volatile sig_atomic_t s_stop = 0;
static volatile sig_atomic_t s_abort = 0;

static void swarmd_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT)
        s_stop = 1;
}

static void swarmd_abort_signal(int sig) {
    (void)sig;
    s_abort = 1;
}

static const char *status_name(swarmd_status_t s) {
    switch (s) {
        case SWARMD_QUEUED: return "queued";
        case SWARMD_RUNNING: return "running";
        case SWARMD_DONE: return "done";
        case SWARMD_FAILED: return "failed";
        case SWARMD_ABORTED: return "aborted";
    }
    return "unknown";
}

static bool status_terminal(swarmd_status_t s) {
    return s == SWARMD_DONE || s == SWARMD_FAILED || s == SWARMD_ABORTED;
}

static time_t now_time(void) { return time(NULL); }

static void mkdir_p(const char *path) {
    if (!path || !path[0]) return;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0700);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

static bool safe_component(const char *s) {
    if (!s || !s[0] || strlen(s) >= SWARMD_NAME_MAX) return false;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '-' || *s == '_')) return false;
    return true;
}

static void default_root(char *out, size_t n) {
    const char *env = getenv("DSCO_SWARM_DAEMON_DIR");
    if (env && env[0]) { snprintf(out, n, "%s", env); return; }
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.dsco/swarms", home && home[0] ? home : "/tmp");
}

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '\\': fputs("\\\\", f); break;
            case '"': fputs("\\\"", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default: if (*p < 32) fprintf(f, "\\u%04x", *p); else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static int atomic_write_state(const swarmd_run_t *r) {
    char tmp[PATH_MAX], fin[PATH_MAX];
    snprintf(fin, sizeof(fin), "%s/state.json", r->run_dir);
    snprintf(tmp, sizeof(tmp), "%s/state.%d.tmp", r->run_dir, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"schema\": \"dsco.swarm.daemon.v1\",\n  \"run_id\": ");
    json_escape(f, r->run_id);
    fprintf(f, ",\n  \"name\": "); json_escape(f, r->name);
    fprintf(f, ",\n  \"run_dir\": "); json_escape(f, r->run_dir);
    fprintf(f, ",\n  \"supervisor_pid\": %d,\n  \"created_at\": %lld,\n  \"updated_at\": %lld,\n",
            (int)r->supervisor_pid, (long long)r->created_at, (long long)r->updated_at);
    fprintf(f, "  \"abort_requested\": %s,\n  \"workers\": [\n", r->abort_requested ? "true" : "false");
    for (int i = 0; i < r->worker_count; i++) {
        const swarmd_worker_t *w = &r->workers[i];
        fprintf(f, "    %s{\"id\":%d,\"pid\":%d,\"pgid\":%d,\"status\":", i ? ",\n" : "",
                w->id, (int)w->pid, (int)w->pgid);
        json_escape(f, status_name(w->status));
        fprintf(f, ",\"started_at\":%lld,\"ended_at\":%lld,\"last_heartbeat\":%lld,\"log_bytes\":%lld,\"exit_code\":%d,\"task\":",
                (long long)w->started_at, (long long)w->ended_at,
                (long long)w->last_heartbeat, (long long)w->log_bytes, w->exit_code);
        json_escape(f, w->task);
        fprintf(f, ",\"result_path\":"); json_escape(f, w->result_path);
        fprintf(f, ",\"log_path\":"); json_escape(f, w->log_path);
        fputc('}', f);
    }
    fputs("\n  ]\n}\n", f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, fin);
}

static void event_append(const swarmd_run_t *r, const char *event, int worker_id, const char *detail) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/events.jsonl", r->run_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "{\"ts\":%lld,\"event\":", (long long)now_time());
    json_escape(f, event);
    if (worker_id >= 0) fprintf(f, ",\"worker_id\":%d", worker_id);
    if (detail) { fputs(",\"detail\":", f); json_escape(f, detail); }
    fputs("}\n", f);
    fflush(f); fsync(fileno(f)); fclose(f);
}

static void write_worker_result(const swarmd_run_t *r, const swarmd_worker_t *w) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", w->result_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"id\":%d,\"pid\":%d,\"status\":", w->id, (int)w->pid);
    json_escape(f, status_name(w->status));
    fprintf(f, ",\"exit_code\":%d,\"started_at\":%lld,\"ended_at\":%lld,\"task\":",
            w->exit_code, (long long)w->started_at, (long long)w->ended_at);
    json_escape(f, w->task);
    fprintf(f, ",\"log_path\":"); json_escape(f, w->log_path);
    fputs("}\n", f); fflush(f); fsync(fileno(f)); fclose(f); (void)rename(tmp, w->result_path);
    (void)r;
}

static bool pid_alive(pid_t pid) { return pid > 1 && kill(pid, 0) == 0; }

static void reconcile(swarmd_run_t *r) {
    bool changed = false;
    for (int i = 0; i < r->worker_count; i++) {
        swarmd_worker_t *w = &r->workers[i];
        if (status_terminal(w->status)) continue;
        /* Heartbeat is derived from forward progress in the worker's durable
         * append-only log. It is cheap, requires no worker cooperation, and
         * tells the operator the difference between waiting and dead. */
        struct stat st;
        if (stat(w->log_path, &st) == 0 && st.st_size != w->log_bytes) {
            w->log_bytes = st.st_size;
            w->last_heartbeat = now_time();
            event_append(r, "worker.heartbeat", w->id, "log_progress");
            changed = true;
        }
        int ws = 0;
        pid_t got = waitpid(w->pid, &ws, WNOHANG);
        if (got == w->pid) {
            w->ended_at = now_time();
            if (WIFEXITED(ws) && WEXITSTATUS(ws) == 0) { w->status = SWARMD_DONE; w->exit_code = 0; }
            else if (r->abort_requested) { w->status = SWARMD_ABORTED; w->exit_code = -1; }
            else { w->status = SWARMD_FAILED; w->exit_code = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1; }
            write_worker_result(r, w);
            event_append(r, "worker.terminal", w->id, status_name(w->status));
            changed = true;
        } else if (got < 0 && errno == ECHILD && !pid_alive(w->pid)) {
            w->ended_at = now_time(); w->status = r->abort_requested ? SWARMD_ABORTED : SWARMD_FAILED;
            w->exit_code = -1; write_worker_result(r, w); event_append(r, "worker.lost", w->id, "not_child"); changed = true;
        }
    }
    if (changed) { r->updated_at = now_time(); (void)atomic_write_state(r); }
}

static void terminate_workers(swarmd_run_t *r) {
    r->abort_requested = true; r->updated_at = now_time(); event_append(r, "run.abort.requested", -1, NULL);
    for (int i = 0; i < r->worker_count; i++) {
        swarmd_worker_t *w = &r->workers[i];
        if (!status_terminal(w->status) && w->pgid > 0) kill(-w->pgid, SIGTERM);
    }
    (void)atomic_write_state(r);
    for (int step = 0; step < 30; step++) { reconcile(r); usleep(100000); }
    for (int i = 0; i < r->worker_count; i++) {
        swarmd_worker_t *w = &r->workers[i];
        if (!status_terminal(w->status) && w->pgid > 0) kill(-w->pgid, SIGKILL);
    }
    for (int step = 0; step < 20; step++) { reconcile(r); usleep(50000); }
    r->updated_at = now_time(); (void)atomic_write_state(r); event_append(r, "run.abort.completed", -1, NULL);
}

static bool all_terminal(const swarmd_run_t *r) {
    for (int i = 0; i < r->worker_count; i++) if (!status_terminal(r->workers[i].status)) return false;
    return true;
}

static int read_file_all(const char *path, char **out) {
    *out = NULL; FILE *f = fopen(path, "r"); if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f); if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return -1; }
    rewind(f); char *buf = safe_malloc((size_t)n + 1); size_t got = fread(buf, 1, (size_t)n, f); fclose(f);
    buf[got] = '\0'; *out = buf; return 0;
}

static int parse_state(const char *run_dir, swarmd_run_t *r) {
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/state.json", run_dir);
    char *raw = NULL; if (read_file_all(path, &raw) != 0) return -1;
    memset(r, 0, sizeof(*r)); snprintf(r->run_dir, sizeof(r->run_dir), "%s", run_dir);
    char *v = json_get_str(raw, "run_id"); if (v) { snprintf(r->run_id, sizeof(r->run_id), "%s", v); free(v); }
    v = json_get_str(raw, "name"); if (v) { snprintf(r->name, sizeof(r->name), "%s", v); free(v); }
    r->supervisor_pid = (pid_t)json_get_int(raw, "supervisor_pid", 0);
    r->created_at = (time_t)json_get_i64(raw, "created_at", 0);
    r->updated_at = (time_t)json_get_i64(raw, "updated_at", 0);
    r->abort_requested = json_get_bool(raw, "abort_requested", false);
    char *workers = json_get_raw(raw, "workers");
    if (workers) {
        const char *p = workers;
        while ((p = strchr(p, '{')) && r->worker_count < SWARMD_MAX_WORKERS) {
            const char *end = strchr(p, '}'); if (!end) break;
            size_t n = (size_t)(end - p + 1); char *o = safe_malloc(n + 1); memcpy(o, p, n); o[n] = 0;
            swarmd_worker_t *w = &r->workers[r->worker_count++];
            w->id = json_get_int(o, "id", r->worker_count - 1); w->pid = (pid_t)json_get_int(o, "pid", 0);
            w->pgid = (pid_t)json_get_int(o, "pgid", w->pid); w->exit_code = json_get_int(o, "exit_code", -1);
            w->started_at = (time_t)json_get_i64(o, "started_at", 0); w->ended_at = (time_t)json_get_i64(o, "ended_at", 0);
            w->last_heartbeat = (time_t)json_get_i64(o, "last_heartbeat", w->started_at);
            w->log_bytes = (off_t)json_get_i64(o, "log_bytes", 0);
            char *st = json_get_str(o, "status");
            if (st && strcmp(st, "done") == 0) w->status = SWARMD_DONE;
            else if (st && strcmp(st, "failed") == 0) w->status = SWARMD_FAILED;
            else if (st && strcmp(st, "aborted") == 0) w->status = SWARMD_ABORTED;
            else if (st && strcmp(st, "queued") == 0) w->status = SWARMD_QUEUED;
            else w->status = SWARMD_RUNNING;
            free(st);
            char *x = json_get_str(o, "task"); if (x) { snprintf(w->task, sizeof(w->task), "%s", x); free(x); }
            x = json_get_str(o, "result_path"); if (x) { snprintf(w->result_path, sizeof(w->result_path), "%s", x); free(x); }
            x = json_get_str(o, "log_path"); if (x) { snprintf(w->log_path, sizeof(w->log_path), "%s", x); free(x); }
            free(o); p = end + 1;
        }
        free(workers);
    }
    free(raw); return r->run_id[0] ? 0 : -1;
}

static void print_state(const swarmd_run_t *r, bool json) {
    if (json) {
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/state.json", r->run_dir);
        char *raw = NULL; if (read_file_all(path, &raw) == 0) { fputs(raw, stdout); free(raw); return; }
    }
    int active = 0, done = 0, failed = 0, aborted = 0;
    for (int i = 0; i < r->worker_count; i++) {
        if (r->workers[i].status == SWARMD_RUNNING || r->workers[i].status == SWARMD_QUEUED) active++;
        if (r->workers[i].status == SWARMD_DONE) done++;
        if (r->workers[i].status == SWARMD_FAILED) failed++;
        if (r->workers[i].status == SWARMD_ABORTED) aborted++;
    }
    printf("run=%s supervisor=%d workers=%d active=%d done=%d failed=%d aborted=%d state=%s\n",
           r->run_id, (int)r->supervisor_pid, r->worker_count, active, done, failed, aborted,
           all_terminal(r) ? "terminal" : "running");
    for (int i = 0; i < r->worker_count; i++) {
        const swarmd_worker_t *w = &r->workers[i];
        printf("  [%d] %-8s pid=%d age=%llds heartbeat=%llds log=%lldB task=%.100s\n",
               w->id, status_name(w->status), (int)w->pid,
               (long long)(now_time() - w->started_at),
               (long long)(now_time() - (w->last_heartbeat ? w->last_heartbeat : w->started_at)),
               (long long)w->log_bytes, w->task);
    }
}

static int resolve_run(const char *root, const char *arg, char *out, size_t n) {
    if (!arg || !arg[0]) return -1;
    if (strchr(arg, '/')) { snprintf(out, n, "%s", arg); return access(out, R_OK) == 0 ? 0 : -1; }
    snprintf(out, n, "%s/%s", root, arg);
    return access(out, R_OK) == 0 ? 0 : -1;
}

static const char *opt_value(int argc, char **argv, const char *name) {
    for (int i = 2; i + 1 < argc; i++) if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}
static bool has_flag(int argc, char **argv, const char *name) {
    for (int i = 2; i < argc; i++) if (strcmp(argv[i], name) == 0) return true;
    return false;
}

static int worker_main(int argc, char **argv) {
    const char *run = opt_value(argc, argv, "--run"); const char *sid = opt_value(argc, argv, "--id");
    if (!run || !sid) return 2; int id = atoi(sid); int sep = -1;
    for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--") == 0) { sep = i; break; }
    if (sep < 0 || sep + 1 >= argc) return 2;
    swarmd_run_t r; if (parse_state(run, &r) != 0 || id < 0 || id >= r.worker_count) return 2;
    swarmd_worker_t *w = &r.workers[id];
    setpgid(0, 0);
    int fd = open(w->log_path, O_CREAT | O_WRONLY | O_APPEND, 0600); if (fd < 0) return 2;
    dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd);
    execvp(argv[sep + 1], &argv[sep + 1]);
    dprintf(STDERR_FILENO, "swarmd worker exec failed: %s\n", strerror(errno));
    return 127;
}

static int daemon_main(const char *run_dir) {
    if (setsid() < 0 && errno != EPERM) return 1;
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = swarmd_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    swarmd_run_t r; if (parse_state(run_dir, &r) != 0) return 1;
    r.supervisor_pid = getpid(); r.updated_at = now_time(); event_append(&r, "supervisor.started", -1, NULL); (void)atomic_write_state(&r);
    while (!all_terminal(&r) && !s_stop) {
        reconcile(&r);
        char abort_path[PATH_MAX]; snprintf(abort_path, sizeof(abort_path), "%s/ABORT", r.run_dir);
        if (access(abort_path, F_OK) == 0 || s_abort) { terminate_workers(&r); break; }
        sleep(1);
    }
    if (s_stop && !all_terminal(&r)) terminate_workers(&r);
    reconcile(&r); r.updated_at = now_time(); (void)atomic_write_state(&r); event_append(&r, "supervisor.exited", -1, NULL);
    return 0;
}

static int start_main(int argc, char **argv) {
    const char *name = opt_value(argc, argv, "--name"); const char *root_opt = opt_value(argc, argv, "--workdir");
    if (!safe_component(name)) { fprintf(stderr, "swarmd: --name must be alphanumeric, - or _\n"); return 2; }
    int sep = -1; for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--") == 0) { sep = i; break; }
    if (sep < 0 || sep + 1 >= argc) { fprintf(stderr, "swarmd: start requires -- <task> [--task <task> ...]\n"); return 2; }
    char root[PATH_MAX]; if (root_opt && root_opt[0]) snprintf(root, sizeof(root), "%s", root_opt); else default_root(root, sizeof(root)); mkdir_p(root);
    char run_id[160]; snprintf(run_id, sizeof(run_id), "%lld-%d-%s", (long long)now_time(), (int)getpid(), name);
    swarmd_run_t r; memset(&r, 0, sizeof(r)); snprintf(r.run_id, sizeof(r.run_id), "%s", run_id); snprintf(r.name, sizeof(r.name), "%s", name);
    snprintf(r.run_dir, sizeof(r.run_dir), "%s/%s", root, run_id); mkdir_p(r.run_dir); r.created_at = r.updated_at = now_time(); r.supervisor_pid = 0;
    /* Every token after -- begins a task; --task is an optional separator. */
    for (int i = sep + 1; i < argc && r.worker_count < SWARMD_MAX_WORKERS; i++) {
        if (strcmp(argv[i], "--task") == 0) continue;
        swarmd_worker_t *w = &r.workers[r.worker_count]; w->id = r.worker_count; w->status = SWARMD_QUEUED; w->exit_code = -1;
        snprintf(w->task, sizeof(w->task), "%s", argv[i]);
        snprintf(w->log_path, sizeof(w->log_path), "%s/worker-%02d.log", r.run_dir, w->id);
        snprintf(w->result_path, sizeof(w->result_path), "%s/worker-%02d.result.json", r.run_dir, w->id);
        r.worker_count++;
    }
    if (!r.worker_count) return 2;
    event_append(&r, "run.created", -1, NULL); if (atomic_write_state(&r) != 0) return 1;
    char self[PATH_MAX];
#ifdef __APPLE__
    uint32_t self_len = sizeof(self);
    /* Declared by <mach-o/dyld.h>; use the loaded binary so daemon workers
       always execute the same build as their controller. */
    if (_NSGetExecutablePath(self, &self_len) != 0)
        snprintf(self, sizeof(self), "dsco");
#else
    snprintf(self, sizeof(self), "dsco");
#endif
    for (int i = 0; i < r.worker_count; i++) {
        swarmd_worker_t *w = &r.workers[i];
        pid_t p = fork(); if (p < 0) { w->status = SWARMD_FAILED; continue; }
        if (p == 0) {
            char idbuf[16]; snprintf(idbuf, sizeof(idbuf), "%d", w->id);
            execl(self, self, "swarmd", "worker", "--run", r.run_dir, "--id", idbuf, "--",
                  self, "--profile", "worker", "-p", w->task, (char *)NULL);
            _exit(127);
        }
        w->pid = p; w->pgid = p; w->status = SWARMD_RUNNING; w->started_at = now_time();
        w->last_heartbeat = w->started_at;
    }
    r.updated_at = now_time(); (void)atomic_write_state(&r);
    /* Daemon owns lifecycle after command returns. */
    pid_t d = fork(); if (d < 0) return 1;
    if (d == 0) _exit(daemon_main(r.run_dir));
    printf("{\"run_id\":"); json_escape(stdout, r.run_id); printf(",\"run_dir\":"); json_escape(stdout, r.run_dir);
    printf(",\"supervisor_pid\":%d,\"workers\":%d,\"status\":\"running\"}\n", (int)d, r.worker_count);
    return 0;
}

static int status_main(int argc, char **argv, bool collect) {
    const char *run_arg = opt_value(argc, argv, "--run"); const char *root_opt = opt_value(argc, argv, "--workdir");
    int wait_s = 0; const char *wait = opt_value(argc, argv, "--wait"); if (wait) wait_s = atoi(wait); if (wait_s < 0) wait_s = 0; if (wait_s > 86400) wait_s = 86400;
    char root[PATH_MAX], run[PATH_MAX]; if (root_opt && root_opt[0]) snprintf(root, sizeof(root), "%s", root_opt); else default_root(root, sizeof(root));
    if (resolve_run(root, run_arg, run, sizeof(run)) != 0) { fprintf(stderr, "swarmd: run not found\n"); return 2; }
    swarmd_run_t r; if (parse_state(run, &r) != 0) return 1;
    /* A daemon does normal reconciliation. CLI status only reports durable state
       and never touches worker processes; this keeps observation non-destructive. */
    for (int i = 0; collect && i < wait_s && !all_terminal(&r); i++) { sleep(1); if (parse_state(run, &r) != 0) break; }
    print_state(&r, has_flag(argc, argv, "--json"));
    return all_terminal(&r) ? 0 : 3;
}

static int abort_main(int argc, char **argv) {
    const char *run_arg = opt_value(argc, argv, "--run"); const char *root_opt = opt_value(argc, argv, "--workdir");
    char root[PATH_MAX], run[PATH_MAX]; if (root_opt && root_opt[0]) snprintf(root, sizeof(root), "%s", root_opt); else default_root(root, sizeof(root));
    if (resolve_run(root, run_arg, run, sizeof(run)) != 0) { fprintf(stderr, "swarmd: run not found\n"); return 2; }
    char abort_file[PATH_MAX]; snprintf(abort_file, sizeof(abort_file), "%s/ABORT", run);
    int fd = open(abort_file, O_CREAT | O_WRONLY | O_TRUNC, 0600); if (fd < 0) return 1; dprintf(fd, "requested %lld\n", (long long)now_time()); fsync(fd); close(fd);
    puts("{\"abort_requested\":true}"); return 0;
}

static void usage(const char *p) {
    fprintf(stderr, "usage:\n  %s swarmd start --name NAME [--workdir DIR] -- TASK [--task TASK ...]\n  %s swarmd status --run RUN [--json]\n  %s swarmd collect --run RUN [--wait SEC] [--json]\n  %s swarmd abort --run RUN\n", p,p,p,p);
}

int swarm_daemon_cli(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    if (strcmp(argv[2], "start") == 0) return start_main(argc, argv);
    if (strcmp(argv[2], "worker") == 0) return worker_main(argc, argv);
    if (strcmp(argv[2], "status") == 0) return status_main(argc, argv, false);
    if (strcmp(argv[2], "collect") == 0) return status_main(argc, argv, true);
    if (strcmp(argv[2], "abort") == 0) return abort_main(argc, argv);
    usage(argv[0]); return 2;
}
