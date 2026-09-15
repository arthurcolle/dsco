#define _POSIX_C_SOURCE 200809L

#include "execution_kernel.h"

#include "capability.h"
#include "chronicle.h"
#include "crypto.h"
#include "json_util.h"
#include "tool_hooks.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXECUTION_RECEIPT_CAP 512

static execution_attempt_t g_receipts[EXECUTION_RECEIPT_CAP];
static uint64_t g_sequence;
static pthread_mutex_t g_receipts_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_emit_mu = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char g_current_execution_id[EXECUTION_KERNEL_ID_LEN];

static double wall_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static const char *first_env(const char *a, const char *b, const char *fallback) {
    const char *value = a ? getenv(a) : NULL;
    if ((!value || !value[0]) && b)
        value = getenv(b);
    return value && value[0] ? value : fallback;
}

static const char *effect_name(unsigned caps) {
    if (caps & CAP_CONTROL)
        return "runtime_control";
    if (caps & CAP_NET)
        return "network";
    if (caps & CAP_EXEC)
        return "process";
    if (caps & CAP_FS_WRITE)
        return "write_file";
    if (caps & CAP_SECRETS)
        return "secrets_read";
    if (caps & CAP_UNTRUSTED_IN)
        return "untrusted_input";
    return "read";
}

static bool is_effectful(unsigned caps) {
    return (caps & (CAP_FS_WRITE | CAP_NET | CAP_EXEC | CAP_CONTROL)) != 0;
}

const char *execution_attempt_status_name(execution_attempt_status_t status) {
    switch (status) {
        case EXECUTION_ATTEMPT_PROPOSED:
            return "proposed";
        case EXECUTION_ATTEMPT_ADMITTED:
            return "admitted";
        case EXECUTION_ATTEMPT_STARTED:
            return "started";
        case EXECUTION_ATTEMPT_DENIED:
            return "denied";
        case EXECUTION_ATTEMPT_SUCCEEDED:
            return "succeeded";
        case EXECUTION_ATTEMPT_FAILED:
            return "failed";
        case EXECUTION_ATTEMPT_CANCELLED:
            return "cancelled";
        case EXECUTION_ATTEMPT_EFFECT_UNKNOWN:
            return "effect_unknown";
    }
    return "unknown";
}

static void store_attempt(const execution_attempt_t *attempt) {
    if (!attempt || !attempt->execution_id[0])
        return;
    pthread_mutex_lock(&g_receipts_mu);
    execution_attempt_t *slot = &g_receipts[attempt->sequence % EXECUTION_RECEIPT_CAP];
    /* A slow attempt may outlive an entire ring rotation. Do not let its late
     * terminal update resurrect an evicted row by overwriting a newer receipt. */
    if (!slot->execution_id[0] || strcmp(slot->execution_id, attempt->execution_id) == 0 ||
        attempt->sequence > slot->sequence)
        *slot = *attempt;
    pthread_mutex_unlock(&g_receipts_mu);
}

static void append_field(jbuf_t *b, const char *key, const char *value) {
    jbuf_append(b, ",\"");
    jbuf_append(b, key);
    jbuf_append(b, "\":");
    if (value && value[0])
        jbuf_append_json_str(b, value);
    else
        jbuf_append(b, "null");
}

static void build_payload(const execution_attempt_t *a, jbuf_t *b) {
    jbuf_append(b, "{\"schema\":\"execution.attempt.v1\",\"execution_id\":");
    jbuf_append_json_str(b, a->execution_id);
    append_field(b, "parent_execution_id", a->parent_execution_id);
    append_field(b, "session_id", a->session_id);
    append_field(b, "trace_id", a->trace_id);
    append_field(b, "turn_id", a->turn_id);
    append_field(b, "tool_call_id", a->tool_call_id);
    append_field(b, "tool", a->tool_name);
    append_field(b, "dispatch_tool", a->dispatch_tool_name);
    append_field(b, "tier", a->tier);
    append_field(b, "principal", a->principal);
    append_field(b, "provider", a->provider);
    append_field(b, "model", a->model);
    append_field(b, "executor", a->executor);
    append_field(b, "effect", a->effect);
    append_field(b, "capabilities", a->capabilities);
    append_field(b, "input_sha256", a->input_sha256);
    append_field(b, "dispatch_input_sha256", a->dispatch_input_sha256);
    append_field(b, "result_sha256", a->result_sha256);
    jbuf_appendf(b, ",\"sequence\":%llu,\"capability_mask\":%u,\"status\":",
                 (unsigned long long)a->sequence, a->capability_mask);
    jbuf_append_json_str(b, execution_attempt_status_name(a->status));
    jbuf_appendf(b,
                 ",\"effectful\":%s,\"admitted\":%s,\"started\":%s,"
                 "\"terminal\":%s,\"proposed_at_ms\":%.3f,"
                 "\"started_at_ms\":%.3f,\"finished_at_ms\":%.3f,"
                 "\"elapsed_ms\":%.3f,\"result_bytes\":%zu}",
                 a->effectful ? "true" : "false", a->admitted ? "true" : "false",
                 a->started ? "true" : "false", a->terminal ? "true" : "false", a->proposed_at_ms,
                 a->started_at_ms, a->finished_at_ms, a->elapsed_ms, a->result_bytes);
}

static void emit_attempt(const execution_attempt_t *attempt, bool durable) {
    if (!attempt || !chronicle_ready())
        return;
    jbuf_t payload;
    jbuf_init(&payload, 1536);
    build_payload(attempt, &payload);
    char event_type[96];
    snprintf(event_type, sizeof(event_type), "execution.attempt.%s",
             execution_attempt_status_name(attempt->status));

    /* Execution-attempt frames must not interleave with each other when tools
     * execute concurrently. Chronicle remains the owner of its other sinks. */
    pthread_mutex_lock(&g_emit_mu);
    (void)chronicle_journal_append("execution.attempt.v1", payload.data, durable);
    (void)chronicle_event(event_type, attempt->trace_id[0] ? attempt->trace_id : NULL,
                          attempt->tool_call_id[0] ? attempt->tool_call_id : NULL,
                          attempt->turn_id[0] ? attempt->turn_id : NULL, "execution",
                          attempt->principal, payload.data, "private_user_content");
    pthread_mutex_unlock(&g_emit_mu);
    jbuf_free(&payload);
}

void execution_kernel_begin(execution_attempt_t *a, const char *tool_name, const char *input_json,
                            const char *tier, unsigned capability_mask, const char *capabilities,
                            const char *provider, const char *model, const char *trace_id,
                            const char *turn_id, const char *tool_call_id) {
    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    uuid_v4(a->execution_id);
    pthread_mutex_lock(&g_receipts_mu);
    a->sequence = ++g_sequence;
    pthread_mutex_unlock(&g_receipts_mu);

    copy_string(a->previous_execution_id, sizeof(a->previous_execution_id), g_current_execution_id);
    if (g_current_execution_id[0])
        copy_string(a->parent_execution_id, sizeof(a->parent_execution_id), g_current_execution_id);
    else
        copy_string(a->parent_execution_id, sizeof(a->parent_execution_id),
                    getenv("DSCO_PARENT_EXECUTION_ID"));
    copy_string(g_current_execution_id, sizeof(g_current_execution_id), a->execution_id);

    if (chronicle_ready())
        copy_string(a->session_id, sizeof(a->session_id), chronicle_session_id());
    else
        copy_string(a->session_id, sizeof(a->session_id), getenv("DSCO_SESSION_ID"));
    copy_string(a->trace_id, sizeof(a->trace_id), trace_id);
    copy_string(a->turn_id, sizeof(a->turn_id), turn_id);
    copy_string(a->tool_call_id, sizeof(a->tool_call_id), tool_call_id);
    copy_string(a->tool_name, sizeof(a->tool_name), tool_name);
    copy_string(a->tier, sizeof(a->tier), tier && tier[0] ? tier : "standard");
    copy_string(a->principal, sizeof(a->principal),
                first_env("DSCO_DURABLE_AGENT_ID", "DSCO_AGENT_ID", "dsco"));
    copy_string(a->provider, sizeof(a->provider), provider);
    copy_string(a->model, sizeof(a->model), model);
    /* This record describes the leaf executor. DSCO_EXEC is a launch/config
     * hint and can name a provider even when this process is serving MCP. */
    copy_string(a->executor, sizeof(a->executor), "dsco-native");
    copy_string(a->effect, sizeof(a->effect), effect_name(capability_mask));
    copy_string(a->capabilities, sizeof(a->capabilities), capabilities);
    sha256_hex((const uint8_t *)(input_json ? input_json : ""), input_json ? strlen(input_json) : 0,
               a->input_sha256);
    a->capability_mask = capability_mask;
    a->effectful = is_effectful(capability_mask);
    a->status = EXECUTION_ATTEMPT_PROPOSED;
    a->proposed_at_ms = wall_ms();
    store_attempt(a);
    emit_attempt(a, false);
}

void execution_kernel_admit(execution_attempt_t *a) {
    if (!a || a->terminal || a->admitted)
        return;
    a->admitted = true;
    a->status = EXECUTION_ATTEMPT_ADMITTED;
    store_attempt(a);
    emit_attempt(a, false);
}

void execution_kernel_start(execution_attempt_t *a, const char *dispatch_tool_name,
                            const char *dispatch_input_json) {
    if (!a || a->terminal || a->started)
        return;
    if (!a->admitted)
        execution_kernel_admit(a);
    copy_string(a->dispatch_tool_name, sizeof(a->dispatch_tool_name), dispatch_tool_name);
    sha256_hex((const uint8_t *)(dispatch_input_json ? dispatch_input_json : ""),
               dispatch_input_json ? strlen(dispatch_input_json) : 0, a->dispatch_input_sha256);
    a->started = true;
    a->status = EXECUTION_ATTEMPT_STARTED;
    a->started_at_ms = wall_ms();
    store_attempt(a);
    emit_attempt(a, a->effectful);
}

void execution_kernel_finish(execution_attempt_t *a, bool ok, const char *result) {
    if (!a || a->terminal)
        return;
    a->finished_at_ms = wall_ms();
    a->elapsed_ms = a->started_at_ms > 0.0 ? a->finished_at_ms - a->started_at_ms
                                           : a->finished_at_ms - a->proposed_at_ms;
    a->result_bytes = result ? strlen(result) : 0;
    sha256_hex((const uint8_t *)(result ? result : ""), a->result_bytes, a->result_sha256);
    if (ok)
        a->status = EXECUTION_ATTEMPT_SUCCEEDED;
    else if (!a->admitted)
        a->status = EXECUTION_ATTEMPT_DENIED;
    else
        a->status = EXECUTION_ATTEMPT_FAILED;
    a->terminal = true;
    store_attempt(a);
    emit_attempt(a, a->effectful && a->started);
    dsco_tool_hook_after(a, result);

    if (strcmp(g_current_execution_id, a->execution_id) == 0)
        copy_string(g_current_execution_id, sizeof(g_current_execution_id),
                    a->previous_execution_id);
}

bool execution_kernel_get(const char *execution_id, execution_attempt_t *out) {
    if (!execution_id || !execution_id[0] || !out)
        return false;
    bool found = false;
    pthread_mutex_lock(&g_receipts_mu);
    for (size_t i = 0; i < EXECUTION_RECEIPT_CAP; i++) {
        if (strcmp(g_receipts[i].execution_id, execution_id) == 0) {
            *out = g_receipts[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_receipts_mu);
    return found;
}

bool execution_kernel_latest(execution_attempt_t *out) {
    if (!out)
        return false;
    bool found = false;
    uint64_t newest = 0;
    pthread_mutex_lock(&g_receipts_mu);
    for (size_t i = 0; i < EXECUTION_RECEIPT_CAP; i++) {
        if (g_receipts[i].execution_id[0] && g_receipts[i].sequence >= newest) {
            newest = g_receipts[i].sequence;
            *out = g_receipts[i];
            found = true;
        }
    }
    pthread_mutex_unlock(&g_receipts_mu);
    return found;
}

bool execution_kernel_recent_json(char *out, size_t out_len, int limit) {
    if (!out || out_len == 0 || limit < 1)
        return false;
    if (limit > EXECUTION_RECEIPT_CAP)
        limit = EXECUTION_RECEIPT_CAP;
    execution_attempt_t *rows = calloc(EXECUTION_RECEIPT_CAP, sizeof(*rows));
    if (!rows) {
        snprintf(out, out_len, "{\"error\":\"execution receipt allocation failed\"}");
        return false;
    }
    int count = 0;
    pthread_mutex_lock(&g_receipts_mu);
    for (size_t i = 0; i < EXECUTION_RECEIPT_CAP; i++)
        if (g_receipts[i].execution_id[0])
            rows[count++] = g_receipts[i];
    pthread_mutex_unlock(&g_receipts_mu);

    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (rows[j].sequence > rows[i].sequence) {
                execution_attempt_t tmp = rows[i];
                rows[i] = rows[j];
                rows[j] = tmp;
            }

    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"schema\":\"execution.attempt.list.v1\",\"attempts\":[");
    int emit = count < limit ? count : limit;
    for (int i = 0; i < emit; i++) {
        if (i)
            jbuf_append_char(&b, ',');
        build_payload(&rows[i], &b);
    }
    jbuf_appendf(&b, "],\"count\":%d}", emit);
    bool fits = b.len + 1 <= out_len;
    if (fits)
        memcpy(out, b.data, b.len + 1);
    else
        snprintf(out, out_len, "{\"error\":\"execution receipt buffer too small\"}");
    jbuf_free(&b);
    free(rows);
    return fits;
}

void execution_kernel_reset_for_test(void) {
    pthread_mutex_lock(&g_receipts_mu);
    memset(g_receipts, 0, sizeof(g_receipts));
    g_sequence = 0;
    pthread_mutex_unlock(&g_receipts_mu);
    g_current_execution_id[0] = '\0';
}
