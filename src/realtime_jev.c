#include "realtime_jev.h"
#include <stdio.h>
#include <string.h>

static void copy_bounded(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

const char *realtime_jev_action_name(realtime_jev_action_t a) {
    switch (a) {
        case REALTIME_JEV_CONTINUE: return "continue";
        case REALTIME_JEV_ACT: return "act";
        case REALTIME_JEV_WAIT: return "wait";
        case REALTIME_JEV_ESCALATE: return "escalate";
        case REALTIME_JEV_STOP: return "stop";
    }
    return "continue";
}

bool realtime_jev_session_init(realtime_jev_session_t *s, const char *world,
                               unsigned max_turns) {
    if (!s || !world || !world[0] || strlen(world) >= sizeof(s->world)) return false;
    memset(s, 0, sizeof(*s));
    copy_bounded(s->world, sizeof(s->world), world);
    s->max_turns = max_turns ? (max_turns > REALTIME_JEV_MAX_TURNS ?
                                REALTIME_JEV_MAX_TURNS : max_turns) : REALTIME_JEV_MAX_TURNS;
    return true;
}

bool realtime_jev_turn(realtime_jev_session_t *s, const char *observation,
                       realtime_jev_classifier_fn classifier, void *ctx,
                       realtime_jev_turn_t *out) {
    if (!s || !observation || !observation[0] || !classifier || s->stopped ||
        s->turns >= s->max_turns || strlen(observation) >= REALTIME_JEV_MAX_STATE)
        return false;
    size_t used = strlen(s->state);
    int n = snprintf(s->state + used, sizeof(s->state) - used,
                     "%s%llu:%s\n", used ? "" : "observation ",
                     (unsigned long long)(s->sequence + 1), observation);
    if (n < 0 || (size_t)n >= sizeof(s->state) - used) return false;
    realtime_jev_turn_t turn = {0};
    turn.sequence = ++s->sequence;
    bool classified = classifier(s->world, s->state, turn.sequence, &turn.action,
                                 &turn.confidence, turn.receipt, sizeof(turn.receipt), ctx);
    turn.abstained = !classified;
    if (!classified) {
        turn.action = REALTIME_JEV_WAIT;
        turn.confidence = 0.0;
        copy_bounded(turn.receipt, sizeof(turn.receipt),
                     "{\"status\":\"abstain\",\"fallback\":\"wait\"}");
    }
    if (turn.action == REALTIME_JEV_STOP) s->stopped = true;
    s->history[s->turns++] = turn;
    if (out) *out = turn;
    return true;
}

bool realtime_jev_checkpoint(const realtime_jev_session_t *s, char *json, size_t cap) {
    if (!s || !json || cap < 128) return false;
    int n = snprintf(json, cap,
        "{\"schema\":\"dsco.realtime_jev.v1\",\"world\":\"%s\","
        "\"sequence\":%llu,\"turns\":%u,\"max_turns\":%u,\"stopped\":%s}",
        s->world, (unsigned long long)s->sequence, s->turns, s->max_turns,
        s->stopped ? "true" : "false");
    return n >= 0 && (size_t)n < cap;
}
