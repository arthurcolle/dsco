/* Deterministic, inert reservation race: no file, network or model call. */
#define _POSIX_C_SOURCE 200809L
#include "capability.h"
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

bool tools_meta_is_read_only(const char *name, bool *found) {
    (void)name; *found = false; return true;
}
bool buffer_store_sensitive(const char *name) { (void)name; return false; }
bool native_windows_sensitive(void) { return false; }

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    const char *name;
    const char *input;
    bool ready;
    bool release;
    bool succeed;
    dsco_cap_decision_t decision;
    dsco_flow_reservation_t reservation;
} held_call_t;

static void *hold_call(void *arg) {
    held_call_t *call = arg;
    char reason[256];
    call->decision = dsco_capability_gate_reserve(call->name, call->input, "trusted",
                                                   reason, sizeof(reason), &call->reservation);
    pthread_mutex_lock(&call->mu);
    call->ready = true;
    pthread_cond_broadcast(&call->cv);
    while (!call->release) pthread_cond_wait(&call->cv, &call->mu);
    pthread_mutex_unlock(&call->mu);
    if (call->decision == CAP_DECISION_ALLOW)
        dsco_flow_finish(&call->reservation, call->succeed);
    return NULL;
}

static void start_held(held_call_t *call, const char *name, const char *input, bool succeed) {
    *call = (held_call_t){0};
    assert(pthread_mutex_init(&call->mu, NULL) == 0);
    assert(pthread_cond_init(&call->cv, NULL) == 0);
    call->name = name; call->input = input; call->succeed = succeed;
    assert(pthread_create(&call->thread, NULL, hold_call, call) == 0);
    pthread_mutex_lock(&call->mu);
    while (!call->ready) pthread_cond_wait(&call->cv, &call->mu);
    pthread_mutex_unlock(&call->mu);
    assert(call->decision == CAP_DECISION_ALLOW);
}

static void stop_held(held_call_t *call) {
    pthread_mutex_lock(&call->mu);
    call->release = true;
    pthread_cond_broadcast(&call->cv);
    pthread_mutex_unlock(&call->mu);
    assert(pthread_join(call->thread, NULL) == 0);
    pthread_cond_destroy(&call->cv);
    pthread_mutex_destroy(&call->mu);
}

static dsco_cap_decision_t reserve(const char *name, const char *input,
                                    dsco_flow_reservation_t *token) {
    char reason[256];
    return dsco_capability_gate_reserve(name, input, "trusted", reason, sizeof(reason), token);
}

int main(void) {
    unsetenv("DSCO_ALLOW_EXFIL");
    const char *secret_read = "{\"path\":\"/tmp/synthetic-id_rsa\"}";
    const char *ordinary_read = "{\"path\":\"/tmp/ordinary-fixture.txt\"}";
    const char *secret_exec = "{\"command\":\"printf synthetic-id_rsa\"}";
    held_call_t held;
    dsco_flow_reservation_t token = {0};
    int effect_callbacks = 0;

    dsco_flow_reset();
    start_held(&held, "read_file", secret_read, false);
    if (reserve("web_search", "{}", &token) == CAP_DECISION_ALLOW) {
        effect_callbacks++;
        dsco_flow_finish(&token, false);
    }
    assert(effect_callbacks == 0); /* in-flight private read blocks egress */
    assert(reserve("read_file", ordinary_read, &token) == CAP_DECISION_ALLOW);
    dsco_flow_finish(&token, true); /* ordinary reads stay available */
    stop_held(&held);
    assert(reserve("web_search", "{}", &token) == CAP_DECISION_ALLOW);
    dsco_flow_finish(&token, false); /* failed secret read did not latch */

    dsco_flow_reset();
    start_held(&held, "read_file", secret_read, true);
    assert(reserve("web_search", "{}", &token) == CAP_DECISION_DENY);
    stop_held(&held);
    assert(reserve("web_search", "{}", &token) == CAP_DECISION_DENY);

    dsco_flow_reset();
    start_held(&held, "web_search", "{}", false);
    assert(reserve("read_file", secret_read, &token) == CAP_DECISION_DENY);
    assert(reserve("read_file", ordinary_read, &token) == CAP_DECISION_ALLOW);
    dsco_flow_finish(&token, true);
    stop_held(&held);
    assert(reserve("read_file", secret_read, &token) == CAP_DECISION_ALLOW);
    dsco_flow_finish(&token, false);

    dsco_flow_reset();
    start_held(&held, "web_search", "{}", true);
    assert(reserve("bash", secret_exec, &token) == CAP_DECISION_DENY);
    stop_held(&held);
    assert(reserve("bash", secret_exec, &token) == CAP_DECISION_DENY);

    dsco_flow_reset();
    start_held(&held, "read_file", secret_read, true);
    dsco_flow_reset(); /* stale completion from a prior session cannot taint this one */
    stop_held(&held);
    assert(reserve("web_search", "{}", &token) == CAP_DECISION_ALLOW);
    dsco_flow_finish(&token, false);

    puts("PASS: concurrent reservation, both admission orders, rollback, read-only access, reset epoch");
    return 0;
}
