#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "swarm_lifecycle.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double monotonic_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int remaining(const swarm_t *s, const int *ids, int count) {
    int n = 0;
    for (int i = 0; i < count; ++i) if (swarm_active_test(s, ids[i])) ++n;
    return n;
}

static void drain_until(swarm_t *s, const int *ids, int count, double deadline,
                        swarm_stream_cb cb, void *ctx, swarm_cancel_report_t *report) {
    while (remaining(s, ids, count)) {
        double now = monotonic_ms();
        if (now < 0) { report->poll_error = errno ? errno : EIO; break; }
        double left = deadline - now;
        if (left <= 0) break;
        int wait_ms = left > 50 ? 50 : (int)ceil(left);
        if (swarm_poll_stream(s, wait_ms, cb, ctx) < 0 && errno != EINTR) {
            report->poll_error = errno ? errno : EIO;
            break;
        }
    }
}

static swarm_cancel_report_t cancel(swarm_t *s, const int *ids, int count, int grace_ms,
    swarm_stream_cb stream, void *stream_ctx, swarm_cancel_event_cb event, void *event_ctx) {
    swarm_cancel_report_t report = {0};
    double start = monotonic_ms();
    if (start < 0) {
        report.poll_error = errno ? errno : EIO;
        report.remaining = remaining(s, ids, count);
        return report;
    }
    if (grace_ms < 0) grace_ms = 0;
    if (grace_ms > 5000) grace_ms = 5000;
    for (int i = 0; i < count; ++i) {
        if (!swarm_active_test(s, ids[i])) continue;
        if (swarm_kill(s, ids[i])) {
            ++report.requested;
            if (event) event(&s->children[ids[i]], false, event_ctx);
        }
    }
    /* Unrelated pipe readiness must not consume the target's grace period. */
    drain_until(s, ids, count, start + grace_ms, stream, stream_ctx, &report);
    for (int i = 0; i < count; ++i) {
        if (!swarm_active_test(s, ids[i])) continue;
        if (swarm_force_kill(s, ids[i])) {
            ++report.forced;
            if (event) event(&s->children[ids[i]], true, event_ctx);
        }
    }
    double forced_at = monotonic_ms();
    if (forced_at >= 0)
        drain_until(s, ids, count, forced_at + 2000, stream, stream_ctx, &report);
    else report.poll_error = errno ? errno : EIO;
    report.remaining = remaining(s, ids, count);
    double end = monotonic_ms();
    if (end >= start) report.elapsed_ms = end - start;
    return report;
}

swarm_cancel_report_t swarm_cancel_child(swarm_t *s, int child_id, int grace_ms,
    swarm_stream_cb stream, void *stream_ctx, swarm_cancel_event_cb event, void *event_ctx) {
    if (!s || !swarm_get(s, child_id)) return (swarm_cancel_report_t){.poll_error = EINVAL};
    return cancel(s, &child_id, 1, grace_ms, stream, stream_ctx, event, event_ctx);
}

swarm_cancel_report_t swarm_cancel_group(swarm_t *s, int group_id, int grace_ms,
    swarm_stream_cb stream, void *stream_ctx, swarm_cancel_event_cb event, void *event_ctx) {
    if (!s || group_id < 0 || group_id >= s->group_count)
        return (swarm_cancel_report_t){.poll_error = EINVAL};
    swarm_group_t *g = &s->groups[group_id];
    swarm_cancel_report_t r = cancel(s, g->child_ids, g->child_count, grace_ms,
                                     stream, stream_ctx, event, event_ctx);
    g->cancel_attempted = true;
    g->cancel_requested = r.requested;
    g->cancel_forced = r.forced;
    g->cancel_poll_error = r.poll_error;
    g->cancel_elapsed_ms = r.elapsed_ms;
    return r;
}

void swarm_child_lifecycle_append(jbuf_t *b, const swarm_child_t *c) {
    const char *state = c->reaped ? "reaped" : c->cancel_requested ? "cancelling" :
        c->status == SWARM_PENDING ? "pending" : "running";
    jbuf_append(b, ",\"lifecycle\":"); jbuf_append_json_str(b, state);
    jbuf_appendf(b, ",\"reaped\":%s,\"cancellation_requested\":%s,\"forced_kill\":%s,"
                    "\"result_persisted\":%s,\"result_errno\":%d,\"result_path\":",
                 c->reaped ? "true" : "false", c->cancel_requested ? "true" : "false",
                 c->cancel_forced ? "true" : "false", c->result_persisted ? "true" : "false",
                 c->result_errno);
    if (c->result_path[0]) jbuf_append_json_str(b, c->result_path);
    else jbuf_append(b, "null");
}

void swarm_group_lifecycle_append(jbuf_t *b, const swarm_t *s, int group_id) {
    const swarm_group_t *g = &s->groups[group_id];
    int live = 0, cancelling = 0, reaped = 0, persisted = 0, errors = 0;
    for (int i = 0; i < g->child_count; ++i) {
        const swarm_child_t *c = &s->children[g->child_ids[i]];
        if (swarm_active_test(s, c->id)) { ++live; if (c->cancel_requested) ++cancelling; }
        if (c->reaped) ++reaped;
        if (c->result_persisted) ++persisted;
        if (c->result_errno) ++errors;
    }
    jbuf_appendf(b, ",\"lifecycle\":{\"live_processes\":%d,\"cancelling\":%d,"
                    "\"reaped\":%d,\"persisted_results\":%d,\"persistence_errors\":%d,"
                    "\"shutdown_complete\":%s}", live, cancelling, reaped, persisted, errors,
                 live == 0 ? "true" : "false");
    if (g->cancel_attempted)
        jbuf_appendf(b, ",\"cancellation\":{\"kill_requests\":%d,\"forced_kills\":%d,"
                        "\"poll_errno\":%d,\"elapsed_ms\":%.3f}",
                     g->cancel_requested, g->cancel_forced, g->cancel_poll_error, g->cancel_elapsed_ms);
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return false; }
    return true;
}

bool swarm_result_persist(swarm_child_t *c) {
    if (!c || !c->reaped) { errno = EINVAL; return false; }
    if (c->result_persisted) return true;
    c->result_errno = 0;
    const char *home = getenv("HOME");
    if (!home || !*home) { c->result_errno = ENOENT; return false; }
    char dir[768], tmp[896], stable[896], alias[896];
    int n = snprintf(dir, sizeof(dir), "%s/.dsco", home);
    if (n < 0 || (size_t)n >= sizeof(dir)) { c->result_errno = ENAMETOOLONG; return false; }
    if (!ensure_dir(dir)) goto fail;
    const char *suffixes[] = {"/sessions", "/swarm", NULL};
    for (int i = 0; suffixes[i]; ++i) {
        size_t used = strlen(dir);
        n = snprintf(dir + used, sizeof(dir) - used, "%s", suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof(dir) - used) { errno = ENAMETOOLONG; goto fail; }
        if (!ensure_dir(dir)) goto fail;
    }
    size_t used = strlen(dir);
    n = snprintf(dir + used, sizeof(dir) - used, "/%d", (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(dir) - used) { errno = ENAMETOOLONG; goto fail; }
    if (!ensure_dir(dir)) goto fail;
    snprintf(stable, sizeof(stable), "%s/run-%llu.RESULT.json", dir, c->run_id);
    snprintf(alias, sizeof(alias), "%s/child-%d.RESULT.json", dir, c->id);
    snprintf(tmp, sizeof(tmp), "%s/.result-XXXXXX", dir);
    int fd = mkstemp(tmp);
    if (fd < 0) goto fail;
    FILE *f = fdopen(fd, "w");
    if (!f) { int e = errno; close(fd); unlink(tmp); errno = e; goto fail; }
    jbuf_t b; jbuf_init(&b, 1024 + c->output_len);
    jbuf_appendf(&b, "{\"schema\":\"dsco.swarm_result.v2\",\"id\":%d,\"run_id\":%llu,\"task\":", c->id, c->run_id);
    jbuf_append_json_str(&b, c->task);
    jbuf_append(&b, ",\"model\":"); jbuf_append_json_str(&b, c->model);
    jbuf_append(&b, ",\"provider\":"); jbuf_append_json_str(&b, c->provider);
    jbuf_append(&b, ",\"executor\":"); jbuf_append_json_str(&b, executor_type_name(c->executor));
    if (c->executor == EXECUTOR_INTEROP) {
        jbuf_append(&b, ",\"interop_outcome\":"); jbuf_append_json_str(&b, c->interop_outcome);
    }
    jbuf_append(&b, ",\"status\":"); jbuf_append_json_str(&b, swarm_status_str(c->status));
    jbuf_append(&b, ",\"wait_phase\":");
    if (c->wait_phase[0]) jbuf_append_json_str(&b, c->wait_phase); else jbuf_append(&b, "null");
    jbuf_appendf(&b, ",\"exit_code\":%d,\"duration_s\":%.3f,\"reaped\":true,"
                    "\"cancellation_requested\":%s,\"forced_kill\":%s,\"cost_usd\":",
                 c->exit_code, c->end_time > c->start_time ? c->end_time - c->start_time : 0,
                 c->cancel_requested ? "true" : "false", c->cancel_forced ? "true" : "false");
    if (c->executor == EXECUTOR_INTEROP ||
        !(c->budget_cost_known || c->reported_cost_known || c->estimated_cost_known ||
          c->reported_cost_usd > 0 || c->est_cost_usd > 0)) jbuf_append(&b, "null");
    else jbuf_appendf(&b, "%.6f", swarm_child_accounted_cost(c));
    char *accounting = swarm_child_accounting_json(c);
    jbuf_append(&b, accounting); free(accounting);
    jbuf_append(&b, ",\"output\":"); jbuf_append_json_str(&b, c->output ? c->output : "");
    jbuf_append(&b, "}\n");
    bool ok = fwrite(b.data, 1, b.len, f) == b.len;
    int error = ok ? 0 : (errno ? errno : EIO);
    if (fflush(f) != 0 || fsync(fd) != 0) { ok = false; error = errno; }
    if (fclose(f) != 0) { ok = false; error = errno; }
    jbuf_free(&b);
    if (!ok) { unlink(tmp); errno = error; goto fail; }
    /* Publish without replacement, for every backend. A recycled PID/run ID
     * must fail explicitly rather than overwrite an earlier run's evidence. */
    if (link(tmp, stable) != 0) { int e = errno; unlink(tmp); errno = e; goto fail; }
    snprintf(c->result_path, sizeof(c->result_path), "%s", stable);
    if (rename(tmp, alias) != 0) {
        fprintf(stderr, "swarm: result retained but slot alias failed: %s\n", strerror(errno));
        unlink(tmp);
    }
    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd < 0) goto fail;
    int synced = fsync(dir_fd), sync_error = errno;
    close(dir_fd);
    if (synced != 0) { errno = sync_error; goto fail; }
    c->result_persisted = true;
    return true;
fail:
    c->result_errno = errno ? errno : EIO;
    fprintf(stderr, "swarm: cannot persist child %d run %llu result: %s\n",
            c->id, c->run_id, strerror(c->result_errno));
    return false;
}
