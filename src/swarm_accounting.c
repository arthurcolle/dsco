#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "swarm.h"
#include "json_util.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* An unlinked file avoids pipe backpressure while the parent is not polling.
 * Only its worker inherits the descriptor across exec; all other parent-side
 * descriptors are CLOEXEC. pread never disturbs the worker's append offset. */
int swarm_accounting_open(void) {
    char path[] = "/tmp/dsco-worker-cost-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, O_APPEND) < 0) { close(fd); return -1; }
    return fd;
}
void swarm_accounting_export(int fd) {
    char value[32];
    snprintf(value, sizeof(value), "%d", fd);
    fcntl(fd, F_SETFD, 0);
    setenv("DSCO_WORKER_COST_FD", value, 1);
}
static double nullable_amount(const char *row, const char *key) {
    char *raw = json_get_raw(row, key);
    if (!raw) return NAN;
    char *end = NULL;
    double amount = strtod(raw, &end);
    bool valid = end != raw && end && *end == '\0' && isfinite(amount) && amount >= 0;
    free(raw); return valid ? amount : NAN;
}
static void consume(swarm_child_t *c, const char *row) {
    char *schema = json_get_str(row, "schema");
    bool valid = schema && strcmp(schema, "dsco.inference_cost.v1") == 0;
    free(schema);
    if (!valid) { c->unpriced_responses++; return; }
    double reported = nullable_amount(row, "provider_reported_usd");
    double estimated = nullable_amount(row, "estimated_inference_usd");
    double budget = nullable_amount(row, "budget_accounted_usd");
    if (isfinite(reported) && reported >= 0) {
        c->reported_cost_known = true;
        c->reported_cost_usd += reported;
    }
    if (isfinite(estimated) && estimated >= 0) {
        if (!c->estimated_cost_known) c->est_cost_usd = 0;
        c->estimated_cost_known = true;
        c->est_cost_usd += estimated;
    }
    if (isfinite(budget) && budget >= 0) {
        c->budget_cost_known = true;
        c->budget_accounted_usd += budget;
    } else c->unpriced_responses++;
    bool included = json_get_bool(row, "subscription_included", false);
    c->subsidized = c->cost_samples == 0 ? included : c->subsidized && included;
    c->cost_class_explicit = true;
    c->cost_samples++;
    c->est_input_tokens += json_get_int(row, "input_tokens", 0);
    c->est_output_tokens += json_get_int(row, "output_tokens", 0);
    char *provider = json_get_str(row, "provider");
    char *model = json_get_str(row, "actual_model");
    if (provider && *provider) snprintf(c->provider, sizeof(c->provider), "%s", provider);
    if (model && *model) snprintf(c->model, sizeof(c->model), "%s", model);
    free(provider); free(model);
}
void swarm_accounting_read(swarm_child_t *c) {
    if (!c || !c->cost_transport || c->cost_fd < 0) return;
    size_t cap = 0, len = 0;
    char *line = NULL, chunk[4096];
    for (;;) {
        ssize_t n = pread(c->cost_fd, chunk, sizeof(chunk), c->cost_offset + (off_t)len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        size_t used = 0;
        while (used < (size_t)n) {
            char *nl = memchr(chunk + used, '\n', (size_t)n - used);
            size_t part = nl ? (size_t)(nl - (chunk + used)) : (size_t)n - used;
            if (len > SIZE_MAX - part - 1) { free(line); return; }
            if (len + part + 1 > cap) {
                size_t need = len + part + 1;
                size_t next = cap ? cap : sizeof(chunk);
                while (next < need) {
                    if (next > SIZE_MAX / 2) { next = need; break; }
                    next *= 2;
                }
                char *grown = realloc(line, next);
                if (!grown) { free(line); return; }
                line = grown; cap = next;
            }
            memcpy(line + len, chunk + used, part); len += part;
            used += part;
            if (!nl) break;
            line[len] = '\0'; consume(c, line);
            c->cost_offset += (off_t)(len + 1); len = 0;
            used++; /* newline */
        }
        if (len == 0 && (size_t)n < sizeof(chunk)) break;
    }
    free(line);
}
double swarm_child_accounted_cost(const swarm_child_t *c) {
    if (c->budget_cost_known) return c->budget_accounted_usd;
    if (c->reported_cost_known || c->reported_cost_usd > 0) return c->reported_cost_usd;
    return c->est_cost_usd;
}
char *swarm_child_accounting_json(const swarm_child_t *c) {
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_append(&b, ",\"reported_cost_usd\":");
    if (c->reported_cost_known || c->reported_cost_usd > 0)
        jbuf_appendf(&b, "%.12f", c->reported_cost_usd);
    else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"estimated_inference_cost_usd\":");
    if (c->estimated_cost_known) jbuf_appendf(&b, "%.12f", c->est_cost_usd);
    else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"budget_accounted_usd\":");
    if (c->budget_cost_known) jbuf_appendf(&b, "%.12f", c->budget_accounted_usd);
    else jbuf_append(&b, "null");
    jbuf_appendf(&b, ",\"cost_samples\":%d,\"unpriced_responses\":%d",
                 c->cost_samples, c->unpriced_responses);
    char *result = strdup(b.data); jbuf_free(&b); return result;
}

/* Both native headless and agent-loop entrypoints must see the same cap.
 * An inherited or per-instance positive ceiling can tighten, never widen it. */
void swarm_child_budget_export(double cap) {
    const char *names[] = {"DSCO_CHILD_BUDGET", "DSCO_BUDGET"};
    if (!isfinite(cap) || cap < 0) cap = 0;
    for (size_t i = 0; i < 2; i++) {
        const char *value = getenv(names[i]);
        double existing = value ? strtod(value, NULL) : 0;
        if (isfinite(existing) && existing > 0 && (cap <= 0 || existing < cap)) cap = existing;
    }
    if (cap > 0) {
        char value[32]; snprintf(value, sizeof(value), "%.17g", cap);
        setenv("DSCO_CHILD_BUDGET", value, 1);
        setenv("DSCO_BUDGET", value, 1);
    }
}
