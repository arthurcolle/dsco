#include "provider_events.h"
#include "event_stream.h"
#include "crypto.h"
#include "json_util.h"
#include <curl/curl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern volatile int g_interrupted;
static _Atomic unsigned long long attempt_sequence;
static _Thread_local char previous_attempt[80];

static bool record(const char *event, jbuf_t *payload) {
    jbuf_append(payload, "}");
    bool ok = event_stream_emit("provider", event, payload->data);
    if (!ok) g_interrupted = 1;
    jbuf_free(payload);
    return ok;
}

static void identity(jbuf_t *out, const provider_event_attempt_t *attempt) {
    jbuf_init(out, 512);
    /* The retained object is our own complete JSON, ending with one brace. */
    jbuf_append_len(out, attempt->identity_json, strlen(attempt->identity_json) - 1);
}

static void model(jbuf_t *out, const char *request) {
    char *name = request ? json_get_str(request, "model") : NULL;
    jbuf_append(out, ",\"model\":");
    if (name) jbuf_append_json_str(out, name); else jbuf_append(out, "null");
    free(name);
}

static char *public_endpoint(const char *endpoint) {
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
    char *result = NULL;
    if (url && endpoint && curl_url_set(url, CURLUPART_URL, endpoint, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK) {
        (void)curl_url_get(url, CURLUPART_PORT, &port, 0);
        (void)curl_url_get(url, CURLUPART_PATH, &path, 0);
        jbuf_t safe;
        jbuf_init(&safe, 256);
        jbuf_append(&safe, scheme); jbuf_append(&safe, "://"); jbuf_append(&safe, host);
        if (port) { jbuf_append_char(&safe, ':'); jbuf_append(&safe, port); }
        jbuf_append(&safe, path ? path : "/");
        result = safe.data;
    }
    curl_free(scheme); curl_free(host); curl_free(port); curl_free(path);
    if (url) curl_url_cleanup(url);
    return result;
}

bool provider_event_start(provider_event_attempt_t *attempt, const char *provider,
                          const char *request, const char *endpoint) {
    memset(attempt, 0, sizeof(*attempt));
    if (!event_stream_active()) return true;
    unsigned long long sequence = ++attempt_sequence;
    struct timespec at = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &at);
    snprintf(attempt->id, sizeof(attempt->id), "%ld:%llu:%llu", (long)getpid(),
             (unsigned long long)at.tv_sec * 1000000000ULL + (unsigned long long)at.tv_nsec,
             sequence);
    jbuf_t payload;
    jbuf_init(&payload, 512);
    jbuf_append(&payload, "{\"attempt_id\":"); jbuf_append_json_str(&payload, attempt->id);
    jbuf_appendf(&payload, ",\"attempt_sequence\":%llu,\"provider\":", sequence);
    jbuf_append_json_str(&payload, provider ? provider : "unknown");
    model(&payload, request);
    char *safe = public_endpoint(endpoint);
    jbuf_append(&payload, ",\"endpoint\":");
    if (safe) jbuf_append_json_str(&payload, safe); else jbuf_append(&payload, "null");
    free(safe);
    jbuf_append(&payload, "}");
    attempt->identity_json = payload.data;
    jbuf_t start;
    identity(&start, attempt);
    return record("provider.attempt.started", &start);
}

bool provider_event_body(provider_event_attempt_t *attempt, const void *bytes, size_t length) {
    if (!attempt || !attempt->identity_json || !length) return true;
    const unsigned char *input = bytes;
    /* Chunk independently of parser lines. This retains CRs, NULs, unknown
     * framing, malformed SSE and non-SSE HTTP errors without truncation. */
    while (length) {
        size_t count = length > 1024U * 1024U ? 1024U * 1024U : length;
        size_t capacity = 4 * ((count + 2) / 3) + 1;
        char *encoded = malloc(capacity);
        if (!encoded) {
            event_stream_fail("provider_body_encoding_allocation_failed");
            g_interrupted = 1; return false;
        }
        if (base64_encode(input, count, encoded, capacity) != capacity - 1) {
            free(encoded);
            event_stream_fail("provider_body_encoding_failed");
            g_interrupted = 1; return false;
        }
        jbuf_t payload;
        identity(&payload, attempt);
        jbuf_appendf(&payload, ",\"offset\":%llu,\"byte_length\":%zu,\"encoding\":\"base64\",\"data\":",
                     (unsigned long long)attempt->body_bytes, count);
        jbuf_append_json_str(&payload, encoded);
        free(encoded);
        if (!record("provider.response.body", &payload)) return false;
        attempt->body_bytes += count; input += count; length -= count;
    }
    return true;
}

bool provider_event_finish(provider_event_attempt_t *attempt, int raw_code,
                           int effective_code, long status, bool terminal,
                           bool protocol_error, const char *error_body) {
    if (!attempt || !attempt->identity_json) return true;
    jbuf_t payload;
    identity(&payload, attempt);
    jbuf_appendf(&payload, ",\"curl_code\":%d,\"effective_curl_code\":%d,\"http_status\":%ld,"
                 "\"terminal_received\":%s,\"protocol_error\":%s,\"received_body_bytes\":%llu",
                 raw_code, effective_code, status, terminal ? "true" : "false",
                 protocol_error ? "true" : "false", (unsigned long long)attempt->body_bytes);
    if (error_body && error_body[0]) {
        jbuf_append(&payload, ",\"error_body_text\":");
        jbuf_append_json_str(&payload, error_body);
    }
    snprintf(previous_attempt, sizeof(previous_attempt), "%s", attempt->id);
    bool ok = record("provider.attempt.finished", &payload);
    free(attempt->identity_json); attempt->identity_json = NULL;
    return ok;
}

bool provider_event_retry(const char *provider, const char *request,
                          const char *reason, long delay_ms) {
    if (!event_stream_active()) return true;
    jbuf_t payload;
    jbuf_init(&payload, 256);
    jbuf_append(&payload, "{\"after_attempt_id\":");
    if (previous_attempt[0]) jbuf_append_json_str(&payload, previous_attempt);
    else jbuf_append(&payload, "null");
    jbuf_append(&payload, ",\"provider\":"); jbuf_append_json_str(&payload, provider ? provider : "unknown");
    model(&payload, request);
    jbuf_append(&payload, ",\"reason\":"); jbuf_append_json_str(&payload, reason);
    jbuf_appendf(&payload, ",\"delay_ms\":%ld", delay_ms);
    return record("provider.retry.scheduled", &payload);
}
