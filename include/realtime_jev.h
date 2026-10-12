#ifndef DSCO_REALTIME_JEV_H
#define DSCO_REALTIME_JEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REALTIME_JEV_MAX_TURNS 64
#define REALTIME_JEV_MAX_STATE 8192
#define REALTIME_JEV_MAX_RECEIPT 1024

typedef enum {
    REALTIME_JEV_CONTINUE = 0,
    REALTIME_JEV_ACT,
    REALTIME_JEV_WAIT,
    REALTIME_JEV_ESCALATE,
    REALTIME_JEV_STOP
} realtime_jev_action_t;

typedef struct {
    uint64_t sequence;
    realtime_jev_action_t action;
    double confidence;
    bool abstained;
    char receipt[REALTIME_JEV_MAX_RECEIPT];
} realtime_jev_turn_t;

typedef struct {
    char world[32];
    uint64_t sequence;
    unsigned turns;
    unsigned max_turns;
    bool stopped;
    char state[REALTIME_JEV_MAX_STATE];
    realtime_jev_turn_t history[REALTIME_JEV_MAX_TURNS];
} realtime_jev_session_t;

/* Classifier is recommend-only. It must not execute tools or mutate authority. */
typedef bool (*realtime_jev_classifier_fn)(const char *world, const char *state,
                                           uint64_t sequence,
                                           realtime_jev_action_t *action,
                                           double *confidence, char *receipt,
                                           size_t receipt_len, void *ctx);

bool realtime_jev_session_init(realtime_jev_session_t *session, const char *world,
                               unsigned max_turns);
bool realtime_jev_turn(realtime_jev_session_t *session, const char *observation,
                       realtime_jev_classifier_fn classifier, void *ctx,
                       realtime_jev_turn_t *out);
bool realtime_jev_checkpoint(const realtime_jev_session_t *session,
                             char *json, size_t json_len);
const char *realtime_jev_action_name(realtime_jev_action_t action);

#endif
