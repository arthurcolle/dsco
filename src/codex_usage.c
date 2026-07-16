#include "codex_usage.h"

#include "config.h"
#include "json_util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CODEX_USAGE_OUTPUT_LIMIT (4u * 1024u * 1024u)
#define CODEX_USAGE_TIMEOUT_MS 15000LL

static void codex_usage_init(codex_usage_snapshot_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->primary_used_percent = -1;
    out->primary_window_minutes = -1;
    out->secondary_used_percent = -1;
    out->secondary_window_minutes = -1;
    out->reset_credits_available = -1;
    out->latest_daily_tokens = -1;
    out->lifetime_tokens = -1;
    out->peak_daily_tokens = -1;
    out->longest_running_turn_seconds = -1;
    out->current_streak_days = -1;
    out->longest_streak_days = -1;
}

static void usage_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

typedef struct {
    codex_usage_snapshot_t *snapshot;
} daily_usage_ctx_t;

static void codex_usage_daily_bucket(const char *element, void *ctx_ptr) {
    daily_usage_ctx_t *ctx = (daily_usage_ctx_t *)ctx_ptr;
    if (!element || !ctx || !ctx->snapshot)
        return;
    char *date = json_get_str(element, "startDate");
    long long tokens = json_get_i64(element, "tokens", -1);
    if (date && tokens >= 0 &&
        (!ctx->snapshot->latest_usage_date[0] ||
         strcmp(date, ctx->snapshot->latest_usage_date) > 0)) {
        usage_copy(ctx->snapshot->latest_usage_date,
                   sizeof(ctx->snapshot->latest_usage_date), date);
        ctx->snapshot->latest_daily_tokens = tokens;
    }
    free(date);
}

static bool codex_usage_parse_rate_limits(const char *result,
                                          codex_usage_snapshot_t *out) {
    char *limits = json_get_raw(result, "rateLimits");
    if (!limits) {
        char *by_id = json_get_raw(result, "rateLimitsByLimitId");
        if (by_id) {
            limits = json_get_raw(by_id, "codex");
            free(by_id);
        }
    }
    if (!limits)
        return false;

    char *limit_id = json_get_str(limits, "limitId");
    char *plan_type = json_get_str(limits, "planType");
    usage_copy(out->limit_id, sizeof(out->limit_id), limit_id);
    usage_copy(out->plan_type, sizeof(out->plan_type), plan_type);
    free(limit_id);
    free(plan_type);

    char *primary = json_get_raw(limits, "primary");
    if (primary) {
        out->primary_used_percent = json_get_int(primary, "usedPercent", -1);
        out->primary_window_minutes = json_get_i64(primary, "windowDurationMins", -1);
        out->primary_resets_at = (time_t)json_get_i64(primary, "resetsAt", 0);
        free(primary);
    }
    char *secondary = json_get_raw(limits, "secondary");
    if (secondary && strcmp(secondary, "null") != 0) {
        out->have_secondary = true;
        out->secondary_used_percent = json_get_int(secondary, "usedPercent", -1);
        out->secondary_window_minutes = json_get_i64(secondary, "windowDurationMins", -1);
        out->secondary_resets_at = (time_t)json_get_i64(secondary, "resetsAt", 0);
    }
    free(secondary);
    free(limits);

    char *reset_credits = json_get_raw(result, "rateLimitResetCredits");
    if (reset_credits) {
        out->reset_credits_available = json_get_int(reset_credits, "availableCount", -1);
        free(reset_credits);
    }
    out->have_rate_limits = out->primary_used_percent >= 0 || out->plan_type[0];
    return out->have_rate_limits;
}

static bool codex_usage_parse_account_usage(const char *result,
                                            codex_usage_snapshot_t *out) {
    char *summary = json_get_raw(result, "summary");
    if (!summary)
        return false;
    out->lifetime_tokens = json_get_i64(summary, "lifetimeTokens", -1);
    out->peak_daily_tokens = json_get_i64(summary, "peakDailyTokens", -1);
    out->longest_running_turn_seconds =
        json_get_i64(summary, "longestRunningTurnSec", -1);
    out->current_streak_days = json_get_i64(summary, "currentStreakDays", -1);
    out->longest_streak_days = json_get_i64(summary, "longestStreakDays", -1);
    free(summary);

    daily_usage_ctx_t ctx = {.snapshot = out};
    (void)json_array_foreach(result, "dailyUsageBuckets", codex_usage_daily_bucket, &ctx);
    out->have_account_usage = true;
    return true;
}

static void codex_usage_capture_error(const char *line, char *error, size_t error_len) {
    if (!line || !error || error_len == 0 || error[0])
        return;
    char *obj = json_get_raw(line, "error");
    if (!obj)
        return;
    char *message = json_get_str(obj, "message");
    usage_copy(error, error_len, message ? message : "Codex app-server request failed");
    free(message);
    free(obj);
}

bool codex_usage_parse_jsonl(const char *jsonl, codex_usage_snapshot_t *out,
                             char *error, size_t error_len) {
    if (error && error_len)
        error[0] = '\0';
    if (!jsonl || !out) {
        if (error && error_len)
            usage_copy(error, error_len, "missing Codex usage response");
        return false;
    }
    codex_usage_init(out);

    const char *p = jsonl;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n'))
            len--;
        if (len > 0) {
            char *line = safe_malloc(len + 1);
            memcpy(line, p, len);
            line[len] = '\0';
            int id = json_get_int(line, "id", -1);
            char *result = json_get_raw(line, "result");
            if (id == 2 && result)
                (void)codex_usage_parse_rate_limits(result, out);
            else if (id == 3 && result)
                (void)codex_usage_parse_account_usage(result, out);
            if ((id == 2 || id == 3) && !result)
                codex_usage_capture_error(line, error, error_len);
            free(result);
            free(line);
        }
        if (!end)
            break;
        p = end + 1;
    }

    if (out->have_rate_limits || out->have_account_usage)
        return true;
    if (error && error_len && !error[0])
        usage_copy(error, error_len, "Codex app-server returned no account usage");
    return false;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static bool write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool jsonl_has_response_id(const char *jsonl, int wanted_id) {
    if (!jsonl)
        return false;
    const char *p = jsonl;
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end)
            return false; /* only complete JSONL records count */
        size_t len = (size_t)(end - p);
        if (len > 0) {
            char *line = safe_malloc(len + 1);
            memcpy(line, p, len);
            line[len] = '\0';
            int id = json_get_int(line, "id", -1);
            free(line);
            if (id == wanted_id)
                return true;
        }
        p = end + 1;
    }
    return false;
}

static void close_if_open(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

bool codex_usage_fetch(codex_usage_snapshot_t *out, char *error, size_t error_len) {
    if (error && error_len)
        error[0] = '\0';
    if (!out) {
        if (error && error_len)
            usage_copy(error, error_len, "missing usage output");
        return false;
    }
    codex_usage_init(out);

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        close_if_open(&in_pipe[0]);
        close_if_open(&in_pipe[1]);
        close_if_open(&out_pipe[0]);
        close_if_open(&out_pipe[1]);
        if (error && error_len)
            snprintf(error, error_len, "pipe failed: %s", strerror(errno));
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    const char *codex = getenv("DSCO_CODEX_BIN");
    if (!codex || !codex[0])
        codex = "codex";
    char *const argv[] = {(char *)codex, "app-server", "--stdio", NULL};
    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, codex, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        close_if_open(&in_pipe[0]);
        close_if_open(&in_pipe[1]);
        close_if_open(&out_pipe[0]);
        close_if_open(&out_pipe[1]);
        if (error && error_len)
            snprintf(error, error_len, "could not start %s: %s", codex, strerror(spawn_rc));
        return false;
    }

    close_if_open(&in_pipe[0]);
    close_if_open(&out_pipe[1]);
#ifdef F_SETNOSIGPIPE
    /* A missing/incompatible Codex binary may exit after spawn but before the
     * initialize write. Never let that terminate the parent dsco process. */
    (void)fcntl(in_pipe[1], F_SETNOSIGPIPE, 1);
#endif
    int flags = fcntl(out_pipe[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    const char *initialize =
        "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{"
        "\"name\":\"dsco\",\"title\":\"dsco\",\"version\":\"" DSCO_VERSION
        "\"},\"capabilities\":{\"experimentalApi\":true}}}\n";
    const char *requests =
        "{\"method\":\"initialized\"}\n"
        "{\"id\":2,\"method\":\"account/rateLimits/read\"}\n"
        "{\"id\":3,\"method\":\"account/usage/read\"}\n";
    bool write_ok = write_all(in_pipe[1], initialize, strlen(initialize));
    bool requests_sent = false;
    bool input_closed = false;
    bool child_done = false;
    bool timed_out = false;
    int status = 0;
    jbuf_t raw;
    jbuf_init(&raw, 65536);
    long long deadline = monotonic_ms() + CODEX_USAGE_TIMEOUT_MS;

    while (write_ok && !timed_out) {
        if (!child_done) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid)
                child_done = true;
        }

        struct pollfd pfd = {.fd = out_pipe[0], .events = POLLIN | POLLHUP};
        int poll_rc = poll(&pfd, 1, 100);
        if (poll_rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            for (;;) {
                char buf[8192];
                ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (raw.len + (size_t)n > CODEX_USAGE_OUTPUT_LIMIT) {
                        timed_out = true;
                        if (error && error_len)
                            usage_copy(error, error_len, "Codex usage response exceeded 4 MiB");
                        break;
                    }
                    jbuf_append_len(&raw, buf, (size_t)n);
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                break;
            }
        }

        if (!requests_sent && jsonl_has_response_id(raw.data, 1)) {
            requests_sent = true;
            write_ok = write_all(in_pipe[1], requests, strlen(requests));
        }
        if (requests_sent && !input_closed && jsonl_has_response_id(raw.data, 2) &&
            jsonl_has_response_id(raw.data, 3)) {
            close_if_open(&in_pipe[1]);
            input_closed = true;
        }
        if (child_done)
            break;
        if (monotonic_ms() >= deadline)
            timed_out = true;
    }

    close_if_open(&in_pipe[1]);
    close_if_open(&out_pipe[0]);
    if (!child_done) {
        if (timed_out)
            (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
    }

    bool parsed = codex_usage_parse_jsonl(raw.data ? raw.data : "", out, error, error_len);
    jbuf_free(&raw);
    if (parsed)
        return true;
    if (error && error_len && !error[0]) {
        if (!write_ok)
            usage_copy(error, error_len, "Codex app-server closed before usage was requested");
        else if (timed_out)
            usage_copy(error, error_len, "Codex account usage request timed out");
        else
            usage_copy(error, error_len, "Codex account usage is unavailable");
    }
    return false;
}
