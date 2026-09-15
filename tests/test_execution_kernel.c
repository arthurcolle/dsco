#define _POSIX_C_SOURCE 200809L

#include "execution_kernel.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t event_mu = PTHREAD_MUTEX_INITIALIZER;
static int event_count;
static int durable_count;
static char last_event[96];
static bool chronicle_enabled = true;

/* Chronicle stubs keep this unit test independent of the database while
 * exercising the exact persistence calls made by the kernel. */
bool chronicle_ready(void) {
    return chronicle_enabled;
}
const char *chronicle_session_id(void) {
    return "11111111-1111-4111-8111-111111111111";
}
bool chronicle_journal_append(const char *record_type, const char *payload_json, bool durable) {
    assert(strcmp(record_type, "execution.attempt.v1") == 0);
    assert(payload_json && strstr(payload_json, "execution.attempt.v1"));
    pthread_mutex_lock(&event_mu);
    if (durable)
        durable_count++;
    pthread_mutex_unlock(&event_mu);
    return true;
}
bool chronicle_event(const char *event_type, const char *trace_id, const char *span_id,
                     const char *parent_span_id, const char *actor_type, const char *actor_id,
                     const char *payload_json, const char *sensitivity) {
    (void)trace_id;
    (void)span_id;
    (void)parent_span_id;
    (void)actor_type;
    (void)actor_id;
    (void)payload_json;
    (void)sensitivity;
    pthread_mutex_lock(&event_mu);
    event_count++;
    snprintf(last_event, sizeof(last_event), "%s", event_type);
    pthread_mutex_unlock(&event_mu);
    return true;
}

static void successful_attempt(void) {
    execution_attempt_t a, saved;
    execution_kernel_begin(&a, "write_file", "{\"secret\":\"topsecret\"}", "trusted", 2u,
                           "fs_write", "test-provider", "test-model", "trace", "turn", "call");
    assert(a.status == EXECUTION_ATTEMPT_PROPOSED && !a.admitted && !a.started);
    assert(strcmp(a.provider, "test-provider") == 0);
    assert(strcmp(a.executor, "dsco-native") == 0);
    execution_kernel_admit(&a);
    execution_kernel_start(&a, "write_file", "{\"secret\":\"topsecret\"}");
    execution_kernel_finish(&a, true, "verified write");
    int events_after_finish = event_count;
    execution_kernel_finish(&a, false, "must be ignored");
    assert(event_count == events_after_finish);
    assert(a.status == EXECUTION_ATTEMPT_SUCCEEDED && a.terminal && a.effectful);
    assert(a.result_bytes == strlen("verified write"));
    assert(execution_kernel_get(a.execution_id, &saved));
    assert(saved.status == EXECUTION_ATTEMPT_SUCCEEDED);
    assert(durable_count == 2); /* started + terminal effect frames */

    char json[8192];
    assert(execution_kernel_recent_json(json, sizeof(json), 10));
    assert(strstr(json, "\"status\":\"succeeded\""));
    assert(!strstr(json, "topsecret"));
    assert(strstr(json, a.input_sha256));
}

static void denial_and_nesting(void) {
    execution_attempt_t outer, inner, sibling;
    execution_kernel_begin(&outer, "outer", "{}", "standard", 1u, "fs_read", "provider", "model",
                           NULL, NULL, NULL);
    execution_kernel_begin(&inner, "inner", "{}", "standard", 4u, "net", "provider", "model", NULL,
                           NULL, NULL);
    assert(strcmp(inner.parent_execution_id, outer.execution_id) == 0);
    execution_kernel_finish(&inner, false, "denied");
    assert(inner.status == EXECUTION_ATTEMPT_DENIED);
    execution_kernel_begin(&sibling, "sibling", "{}", "standard", 1u, "fs_read", "provider",
                           "model", NULL, NULL, NULL);
    assert(strcmp(sibling.parent_execution_id, outer.execution_id) == 0);
    execution_kernel_finish(&sibling, false, "denied");
    execution_kernel_admit(&outer);
    execution_kernel_start(&outer, "outer", "{}");
    execution_kernel_finish(&outer, false, "failed");
    assert(outer.status == EXECUTION_ATTEMPT_FAILED);
}

static void bounded_queries(void) {
    execution_kernel_reset_for_test();
    chronicle_enabled = false;
    char first_id[EXECUTION_KERNEL_ID_LEN] = "";
    char last_id[EXECUTION_KERNEL_ID_LEN] = "";
    for (int i = 0; i < 520; i++) {
        execution_attempt_t a;
        execution_kernel_begin(&a, "read_file", "{}", "standard", 1u, "fs_read", "provider",
                               "model", NULL, NULL, NULL);
        execution_kernel_admit(&a);
        execution_kernel_start(&a, "read_file", "{}");
        execution_kernel_finish(&a, true, "ok");
        if (i == 0)
            snprintf(first_id, sizeof(first_id), "%s", a.execution_id);
        if (i == 519)
            snprintf(last_id, sizeof(last_id), "%s", a.execution_id);
    }
    execution_attempt_t saved;
    assert(!execution_kernel_get(first_id, &saved));
    assert(execution_kernel_get(last_id, &saved));
    char tiny[32];
    assert(!execution_kernel_recent_json(tiny, sizeof(tiny), 512));
    assert(strstr(tiny, "error"));
    execution_kernel_begin(NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL);
}

static void stale_finish_does_not_clobber_ring(void) {
    execution_kernel_reset_for_test();
    chronicle_enabled = false;
    execution_attempt_t stale;
    execution_kernel_begin(&stale, "slow", "{}", "standard", 1u, "fs_read", NULL, NULL, NULL, NULL,
                           NULL);
    char newest_id[EXECUTION_KERNEL_ID_LEN] = "";
    for (int i = 0; i < 512; i++) {
        execution_attempt_t current;
        execution_kernel_begin(&current, "read_file", "{}", "standard", 1u, "fs_read", NULL, NULL,
                               NULL, NULL, NULL);
        execution_kernel_finish(&current, false, "denied");
        if (i == 511)
            snprintf(newest_id, sizeof(newest_id), "%s", current.execution_id);
    }
    execution_kernel_admit(&stale);
    execution_kernel_start(&stale, "slow", "{}");
    execution_kernel_finish(&stale, true, "late");
    execution_attempt_t saved;
    assert(!execution_kernel_get(stale.execution_id, &saved));
    assert(execution_kernel_get(newest_id, &saved));
}

typedef struct {
    char id[EXECUTION_KERNEL_ID_LEN];
} thread_result_t;
static void *thread_attempt(void *opaque) {
    thread_result_t *r = opaque;
    execution_attempt_t a;
    execution_kernel_begin(&a, "read_file", "{}", "standard", 1u, "fs_read", "provider", "model",
                           NULL, NULL, NULL);
    execution_kernel_admit(&a);
    execution_kernel_start(&a, "read_file", "{}");
    execution_kernel_finish(&a, true, "ok");
    snprintf(r->id, sizeof(r->id), "%s", a.execution_id);
    return NULL;
}

static void concurrency(void) {
    enum { N = 12 };
    pthread_t threads[N];
    thread_result_t results[N] = {0};
    for (int i = 0; i < N; i++)
        assert(pthread_create(&threads[i], NULL, thread_attempt, &results[i]) == 0);
    for (int i = 0; i < N; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    for (int i = 0; i < N; i++) {
        execution_attempt_t saved;
        assert(execution_kernel_get(results[i].id, &saved));
        assert(saved.status == EXECUTION_ATTEMPT_SUCCEEDED);
        for (int j = i + 1; j < N; j++)
            assert(strcmp(results[i].id, results[j].id) != 0);
    }
}

int main(void) {
    execution_kernel_reset_for_test();
    successful_attempt();
    denial_and_nesting();
    concurrency();
    execution_attempt_t latest;
    assert(execution_kernel_latest(&latest) && latest.terminal);
    assert(event_count == 4 + 8 + 12 * 4);
    assert(strstr(last_event, "execution.attempt.succeeded"));
    bounded_queries();
    stale_finish_does_not_clobber_ring();
    puts("PASS: execution spine lifecycle, hashes, nesting, durability, queries and concurrency");
    return 0;
}
