#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "shell_jobs.h"
#include "timeout_policy.h"
#include "shell_job_events.h"
#include "json_util.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <spawn.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;

/* No session-local registry or PID-based cancellation. A detached supervisor
 * owns/reaps the process group; clients cancel through a private marker file.
 * Atomic receipts and explicit log offsets survive caller/process restarts. */
static double monotime(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int deadline_ms(double deadline) {
    if (deadline <= 0) return -1;
    double remaining = (deadline - monotime()) * 1000;
    if (remaining <= 0) return 0;
    return remaining >= INT_MAX ? INT_MAX : (int)remaining + 1;
}
static bool private_dir(const char *path) {
    struct stat s;
    if (mkdir(path, 0700) && errno != EEXIST) return false;
    return !lstat(path, &s) && S_ISDIR(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0077);
}
static bool root_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' || strlen(home) + 32 >= n) return false;
    snprintf(out, n, "%s/.dsco", home);
    /* Existing .dsco may be 0755; the jobs directory itself must be private. */
    if (mkdir(out, 0700) && errno != EEXIST) return false;
    struct stat s;
    if (lstat(out, &s) || !S_ISDIR(s.st_mode) || s.st_uid != getuid()) return false;
    strcat(out, "/shell-jobs");
    return private_dir(out);
}
static bool job_path(const char *id, char *out, size_t n) {
    if (!id || strncmp(id, "job-", 4) || strlen(id) != 10) return false;
    for (const char *p = id + 4; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return false;
    if (!root_path(out, n) || strlen(out) + 12 >= n) return false;
    strcat(out, "/"); strcat(out, id);
    struct stat s;
    return !lstat(out, &s) && S_ISDIR(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0077);
}
static int open_job(const char *dir, const char *name, int flags) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    return open(path, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
}
static bool sync_directory(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    return ok;
}
static bool atomic_text(const char *dir, const char *name, const char *text) {
    char temp[64], src[PATH_MAX], dst[PATH_MAX];
    snprintf(temp, sizeof(temp), ".%s.%ld", name, (long)getpid());
    int fd = open_job(dir, temp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return false;
    size_t left = strlen(text); const char *p = text; bool ok = true;
    while (left) { ssize_t n = write(fd, p, left); if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; } p += n; left -= (size_t)n; }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    snprintf(src, sizeof(src), "%s/%s", dir, temp); snprintf(dst, sizeof(dst), "%s/%s", dir, name);
    if (ok && rename(src, dst)) ok = false;
    if (!ok) unlink(src);
    if (!sync_directory(dir)) ok = false;
    return ok;
}
static char *read_text(const char *dir, const char *name) {
    int fd = open_job(dir, name, O_RDONLY); if (fd < 0) return NULL;
    struct stat s; if (fstat(fd, &s) || s.st_size < 0 || s.st_size > 1024 * 1024) { close(fd); return NULL; }
    size_t cap = (size_t)s.st_size; char *b = safe_malloc(cap + 1); size_t used = 0;
    while (used < cap) { ssize_t n = read(fd, b + used, cap - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n; }
    close(fd); b[used] = 0; return b;
}
static _Thread_local int receipt_timeout;
static _Thread_local const shell_job_events_t *receipt_events;
static bool receipt(const char *dir, const char *id, const char *state, pid_t supervisor,
                    pid_t child, int exit_code, int sig, time_t started, bool terminal) {
    jbuf_t j; jbuf_init(&j, 512);
    jbuf_appendf(&j, "{\"job_id\":\"%s\",\"bash_id\":\"%s\",\"status\":\"%s\",\"supervisor_pid\":%ld,\"pid\":%ld,\"exit_code\":%d,\"signal\":%d,\"started_at\":%ld,\"finished_at\":%ld,\"log\":",
        id, id, state, (long)supervisor, (long)child, exit_code, sig, (long)started, terminal ? (long)time(NULL) : 0L);
    char log[PATH_MAX]; snprintf(log, sizeof(log), "%s/output.log", dir); jbuf_append_json_str(&j, log);
    jbuf_appendf(&j, ",\"execution_timeout_seconds\":%d,\"deadline_policy\":\"%s\",\"wait_backend\":\"%s\",\"event_waits\":%llu,\"event_wakeups\":%llu}",
        receipt_timeout, receipt_timeout ? "explicit" : "completion", shell_job_events_backend(),
        (unsigned long long)shell_job_events_wait_count(receipt_events),
        (unsigned long long)shell_job_events_wake_count(receipt_events));
    bool ok = atomic_text(dir, "status.json", j.data); jbuf_free(&j); return ok;
}
static volatile sig_atomic_t stop_job;
static void stop_handler(int sig) { (void)sig; stop_job = 1; }
int shell_jobs_supervise(const char *id) {
    char dir[PATH_MAX]; if (!job_path(id, dir, sizeof(dir))) return 2;
    /* Exclusive supervisor ownership also makes accidental duplicate re-exec inert. */
    int lock = open_job(dir, "supervisor.lock", O_WRONLY | O_CREAT | O_EXCL);
    if (lock < 0) return 2;
    close(lock);
    if (setsid() < 0) return 2;
    signal(SIGHUP, SIG_IGN); signal(SIGTERM, stop_handler); signal(SIGINT, stop_handler);
    char *request = read_text(dir, "request.json");
    char *command = request ? json_get_str(request, "command") : NULL;
    char *cwd = request ? json_get_str(request, "cwd") : NULL;
    int timeout = request ? dsco_job_timeout_s(request) : -1;
    receipt_timeout = timeout;
    time_t started = time(NULL);
    if (!command || timeout < 0 || timeout > 86400) {
        receipt(dir, id, "failed", getpid(), 0, 125, 0, started, true);
        free(request); free(command); free(cwd); return 2;
    }
    int log = open_job(dir, "output.log", O_WRONLY | O_CREAT | O_APPEND);
    pid_t child = log >= 0 ? fork() : -1;
    if (!child) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        int null = open("/dev/null", O_RDONLY);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(125);
        if (null > 2) close(null);
        if (log > 2) close(log);
        /* Bound each job's log disk use; callers can redirect larger artifacts. */
        struct rlimit limit = {64 * 1024 * 1024, 64 * 1024 * 1024}; setrlimit(RLIMIT_FSIZE, &limit);
        if (cwd && chdir(cwd)) { perror("job cwd"); _exit(125); }
        execl("/bin/bash", "bash", "-c", command, (char *)NULL); perror("job exec"); _exit(125);
    }
    if (log >= 0) close(log);
    free(request); free(command); free(cwd);
    if (child < 0) { receipt(dir, id, "failed", getpid(), 0, 125, 0, started, true); return 2; }
    setpgid(child, child);
    shell_job_events_t *events = shell_job_events_open(dir, child, true);
    if (!events) {
        kill(-child, SIGKILL); kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        receipt(dir, id, "interrupted", getpid(), child, 125, 0, started, true);
        return 2;
    }
    receipt_events = events;
    if (!receipt(dir, id, "running", getpid(), child, -1, 0, started, false)) stop_job = 1;
    double deadline = timeout ? monotime() + timeout : 0, kill_at = 0;
    const char *reason = NULL; int status = 0; bool reaped = false;
    bool killed = false;
    for (;;) {
        pid_t n = waitpid(child, &status, WNOHANG);
        if (n == child) { reaped = true; break; }
        if (n < 0 && errno != EINTR) break;
        int cancel = open_job(dir, "cancel", O_RDONLY);
        bool cancelled = cancel >= 0; if (cancel >= 0) close(cancel);
        double now = monotime();
        if (!reason && (stop_job || cancelled || (deadline > 0 && now >= deadline))) {
            reason = (stop_job || cancelled) ? "cancelled" : "timed_out";
            kill(-child, SIGTERM); kill_at = now + 0.5;
        }
        if (reason && !killed && now >= kill_at) {
            kill(-child, SIGKILL); killed = true;
        }
        /* No maintenance cadence. Wake for child exit, directory/control
         * events, or one explicitly scheduled deadline/escalation. */
        double next = reason ? (killed ? 0 : kill_at) : deadline;
        if (shell_job_events_wait(events, deadline_ms(next)) < 0) {
            reason = "interrupted";
            kill(-child, SIGKILL); kill(child, SIGKILL);
            pid_t done;
            do { done = waitpid(child, &status, 0); } while (done < 0 && errno == EINTR);
            reaped = done == child;
            break;
        }
    }
    /* Bash may exit while descendants still hold the log open. Terminate the
     * owned group before publishing terminal state; detached daemons are not jobs. */
    kill(-child, SIGKILL);
    int code = reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int sig = reaped && WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    const char *state = reason ? reason : !reaped ? "interrupted" : code == 0 ? "completed" : "failed";
    bool ok = receipt(dir, id, state, getpid(), child, code, sig, started, true);
    receipt_events = NULL;
    shell_job_events_close(events);
    return ok ? 0 : 2;
}
static bool is_terminal(const char *json) {
    char *state = json_get_str(json, "status");
    bool done = state && strcmp(state, "starting") && strcmp(state, "running"); free(state); return done;
}
static char *job_status(const char *dir) {
    char *s = read_text(dir, "status.json");
    if (!s) return NULL;
    int pid = json_get_int(s, "supervisor_pid", 0);
    if (!is_terminal(s) && pid > 0 && kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
        /* No invented success when a supervisor is externally killed. */
        jbuf_t j; jbuf_init(&j, strlen(s) + 80);
        jbuf_append(&j, "{\"status\":\"interrupted\",\"previous_receipt\":"); jbuf_append(&j, s); jbuf_append(&j, "}");
        free(s); s = safe_strdup(j.data); jbuf_free(&j);
    }
    if (pid > 0) waitpid((pid_t)pid, NULL, WNOHANG);
    return s;
}
static bool return_json(const char *json, char *out, size_t n) {
    if (strlen(json) >= n) { snprintf(out, n, "{\"error\":\"response exceeds result buffer; use a smaller max_bytes\"}"); return false; }
    memcpy(out, json, strlen(json) + 1); return true;
}
static bool launch(const char *input, char *out, size_t n) {
    char *command = json_get_str(input, "command"), *cwd = json_get_str(input, "cwd");
    int timeout = dsco_job_timeout_s(input);
    if (!command || !*command || timeout < 0 || timeout > 86400) { free(command); free(cwd); snprintf(out, n, "{\"error\":\"command required; background timeout must be 0 (no deadline) or 1..86400 seconds\"}"); return false; }
    receipt_timeout = timeout;
    char here[PATH_MAX]; if (!cwd) { if (!getcwd(here, sizeof(here))) { free(command); return false; } cwd = safe_strdup(here); }
    char *resolved = realpath(cwd, NULL); struct stat s;
    if (!resolved || stat(resolved, &s) || !S_ISDIR(s.st_mode)) { free(command); free(cwd); free(resolved); snprintf(out, n, "{\"error\":\"cwd is not a directory\"}"); return false; }
    char dir[PATH_MAX]; if (!root_path(dir, sizeof(dir)) || strlen(dir) + 12 >= sizeof(dir)) { free(command); free(cwd); free(resolved); snprintf(out, n, "{\"error\":\"cannot create private job directory\"}"); return false; }
    strcat(dir, "/job-XXXXXX");
    if (!mkdtemp(dir)) { free(command); free(cwd); free(resolved); snprintf(out, n, "{\"error\":\"mkdtemp failed\"}"); return false; }
    const char *id = strrchr(dir, '/') + 1;
    jbuf_t j; jbuf_init(&j, strlen(command) + strlen(resolved) + 64);
    jbuf_append(&j, "{\"command\":"); jbuf_append_json_str(&j, command); jbuf_append(&j, ",\"cwd\":"); jbuf_append_json_str(&j, resolved);
    jbuf_appendf(&j, ",\"timeout\":%d}", timeout);
    bool ok = atomic_text(dir, "request.json", j.data) && receipt(dir, id, "starting", 0, 0, -1, 0, time(NULL), false);
    free(command); free(cwd); free(resolved); jbuf_free(&j);
    char self[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof(self); if (_NSGetExecutablePath(self, &size)) ok = false;
#else
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len < 0) ok = false; else self[len] = 0;
#endif
    pid_t pid = 0; int rc = EIO;
    if (ok) {
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        for (int fd = 0; fd < 3; fd++) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
        posix_spawnattr_t attr; posix_spawnattr_init(&attr);
#ifdef __APPLE__
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
        char *argv[] = {self, "--shell-job-supervisor", (char *)id, NULL};
        rc = posix_spawn(&pid, self, &actions, &attr, argv, environ);
        posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&actions);
    }
    if (rc) { receipt(dir, id, "failed", 0, 0, 125, 0, time(NULL), true); snprintf(out, n, "{\"error\":\"supervisor spawn failed: %s\"}", strerror(rc)); return false; }
    shell_job_events_t *events = shell_job_events_open(dir, pid, false);
    if (!events) {
        /* Spawn succeeded: retain identity and do not mistake observation
         * setup failure for permission to kill/replay the operation. */
        snprintf(out, n, "{\"error\":\"cannot observe launched job\",\"job_id\":\"%s\"}", id);
        return false;
    }
    double deadline = monotime() + 2; char *status = NULL;
    do {
        free(status); status = job_status(dir);
        char *state = status ? json_get_str(status, "status") : NULL;
        bool ready = state && strcmp(state, "starting"); free(state);
        if (ready) break;
        if (shell_job_events_wait(events, deadline_ms(deadline)) < 0) {
            shell_job_events_close(events); free(status);
            snprintf(out, n, "{\"error\":\"cannot observe launched job\",\"job_id\":\"%s\"}", id);
            return false;
        }
    } while (monotime() < deadline);
    shell_job_events_close(events);
    /* The PID is owned by this process, so detecting failed bootstrap is safe. */
    int wstatus; pid_t reaped = waitpid(pid, &wstatus, WNOHANG);
    if (reaped == pid && (!status || !is_terminal(status))) {
        receipt(dir, id, "interrupted", pid, 0, 125, 0, time(NULL), true);
        free(status); status = job_status(dir);
    }
    if (!status) { snprintf(out, n, "{\"error\":\"missing launch receipt\"}"); return false; }
    ok = return_json(status, out, n); free(status); return ok;
}
bool shell_jobs_execute(const char *input, char *out, size_t n) {
    char *action = json_get_str(input, "action"); if (!action) action = safe_strdup("start");
    if (!strcmp(action, "start")) { free(action); return launch(input, out, n); }
    if (!strcmp(action, "list")) {
        char root[PATH_MAX]; DIR *d = root_path(root, sizeof(root)) ? opendir(root) : NULL;
        if (!d) { free(action); snprintf(out, n, "{\"error\":\"cannot read job directory\"}"); return false; }
        jbuf_t j; jbuf_init(&j, 1024); jbuf_append(&j, "{\"jobs\":["); struct dirent *entry; int count = 0; bool more = false;
        while ((entry = readdir(d))) {
            char dir[PATH_MAX]; if (!job_path(entry->d_name, dir, sizeof(dir))) continue;
            char *status = job_status(dir); if (!status) continue;
            if (count >= 64 || j.len + strlen(status) + 40 >= n) { more = true; free(status); break; }
            if (count++) jbuf_append(&j, ",");
            jbuf_append(&j, status); free(status);
        }
        closedir(d); jbuf_appendf(&j, "],\"truncated\":%s}", more ? "true" : "false");
        bool ok = return_json(j.data, out, n); jbuf_free(&j); free(action); return ok;
    }
    char *id = json_get_str(input, "job_id"); if (!id) id = json_get_str(input, "bash_id"); if (!id) id = json_get_str(input, "shell_id");
    char dir[PATH_MAX];
    if (!job_path(id, dir, sizeof(dir))) { free(id); free(action); snprintf(out, n, "{\"error\":\"unknown or invalid job_id\"}"); return false; }
    bool cancel = !strcmp(action, "cancel"), wait = !strcmp(action, "wait"), output = !strcmp(action, "output");
    if (!cancel && !wait && !output && strcmp(action, "status")) { free(id); free(action); snprintf(out, n, "{\"error\":\"action must be start/list/status/output/wait/cancel\"}"); return false; }
    char *status = job_status(dir);
    if (!status) { free(id); free(action); snprintf(out, n, "{\"error\":\"missing job receipt\"}"); return false; }
    if (cancel && !is_terminal(status)) {
        int fd = open_job(dir, "cancel", O_WRONLY | O_CREAT | O_EXCL);
        if (fd < 0 && errno != EEXIST) { free(status); free(id); free(action); snprintf(out, n, "{\"error\":\"cancel request failed\"}"); return false; }
        bool durable = true;
        if (fd >= 0) { durable = fsync(fd) == 0; if (close(fd)) durable = false; }
        if (!sync_directory(dir)) durable = false;
        if (!durable) {
            snprintf(out, n, "{\"error\":\"cancel request durability failed\",\"job_id\":\"%s\",\"cancel_requested\":true}", id);
            free(status); free(id); free(action); return false;
        }
    }
    int seconds = json_get_int(input, "wait_seconds", cancel ? 2 : 1); if (seconds < 0) seconds = 0; if (seconds > 30) seconds = 30;
    double deadline = monotime() + seconds;
    if ((wait || cancel) && !is_terminal(status) && seconds > 0) {
        int supervisor = json_get_int(status, "supervisor_pid", 0);
        shell_job_events_t *events = shell_job_events_open(dir, supervisor, false);
        if (!events) { free(status); free(id); free(action); snprintf(out, n, "{\"error\":\"cannot observe job\"}"); return false; }
        /* Register before rereading to close the completion-before-wait race. */
        while (!is_terminal(status)) {
            char *next = job_status(dir); if (next) { free(status); status = next; }
            if (is_terminal(status) || monotime() >= deadline) break;
            if (shell_job_events_wait(events, deadline_ms(deadline)) < 0) {
                shell_job_events_close(events); free(status); free(id); free(action);
                snprintf(out, n, "{\"error\":\"job observation failed\"}"); return false;
            }
        }
        shell_job_events_close(events);
    }
    jbuf_t j; jbuf_init(&j, strlen(status) + 256);
    jbuf_append_len(&j, status, strlen(status) - 1);
    if (cancel) jbuf_appendf(&j, ",\"cancel_requested\":%s", is_terminal(status) ? "false" : "true");
    if (output || wait) {
        int offset = json_get_int(input, "offset", 0), max = json_get_int(input, "max_bytes", 8192);
        if (offset < 0) offset = 0;
        size_t budget = n > j.len + 256 ? (n - j.len - 256) / 6 : 0;
        if (max < 1) max = 1;
        if (max > 65536) max = 65536;
        if ((size_t)max > budget) max = (int)budget;
        char *chunk = safe_malloc((size_t)max + 1); ssize_t got = 0;
        int fd = open_job(dir, "output.log", O_RDONLY);
        if (fd >= 0) { got = pread(fd, chunk, (size_t)max, offset); close(fd); if (got < 0) got = 0; }
        /* Logs are text; replace NUL rather than silently discard following bytes. */
        for (ssize_t i = 0; i < got; i++) if (!chunk[i]) chunk[i] = '?';
        chunk[got] = 0; jbuf_append(&j, ",\"output\":"); jbuf_append_json_str(&j, chunk);
        jbuf_appendf(&j, ",\"offset\":%d,\"next_offset\":%lld", offset, (long long)offset + got); free(chunk);
    }
    jbuf_append(&j, "}"); bool ok = return_json(j.data, out, n);
    jbuf_free(&j); free(status); free(id); free(action); return ok;
}
