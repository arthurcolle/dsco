#include "compact_policy.h"

#include <stdio.h>
#include <string.h>

/* compact_policy.c — per-agent compaction thresholds. See compact_policy.h.
 * Pure policy layer: no globals, no I/O. Distilled from oh-my-pi's per-agent
 * compaction thresholds (changelog #13107) — subagents compact tighter without
 * touching the main session's budget. */

/* Floors so a tiny root budget still yields a usable policy. */
#define COMPACT_POLICY_MIN_TOKENS 2000
#define COMPACT_POLICY_MICRO_ROOT_CHARS 256

int compact_rolling_trigger(int input_limit, int growth_tokens) {
    if (input_limit <= 0) return 0;
    /* Cap routine history earlier than the model limit: a large context window
     * is capacity, not an instruction to replay 96K tokens on every turn. */
    long long trigger = (long long)input_limit * 50 / 100;
    if (trigger > 48000) trigger = 48000;
    long long predicted = (long long)input_limit -
                          2LL * (growth_tokens > 0 ? growth_tokens : 0);
    long long floor = input_limit / 4;
    if (predicted < floor) predicted = floor;
    if (predicted < trigger) trigger = predicted;
    return trigger > 0 ? (int)trigger : 1;
}

bool compact_policy_for_role(compact_role_t role,
                             int root_min_tokens, int root_max_tokens,
                             compact_policy_t *out) {
    if (!out) return false;
    if (root_min_tokens <= 0 || root_max_tokens <= 0 ||
        root_min_tokens >= root_max_tokens)
        return false;

    out->role = role;
    int min_t = root_min_tokens, max_t = root_max_tokens;
    switch (role) {
        case COMPACT_ROLE_ROOT:
            /* identity — the operator's global budget, untouched */
            out->inherit_full_restore = true;
            break;
        case COMPACT_ROLE_SUBAGENT:
            min_t = root_min_tokens / 2;
            max_t = root_max_tokens / 2;
            out->inherit_full_restore = false;
            break;
        case COMPACT_ROLE_EVAL:
            min_t = (root_min_tokens * 3) / 10;
            max_t = (root_max_tokens * 3) / 10;
            out->inherit_full_restore = false;
            break;
        default:
            return false;
    }
    if (min_t < COMPACT_POLICY_MIN_TOKENS) min_t = COMPACT_POLICY_MIN_TOKENS;
    if (max_t <= min_t) max_t = min_t * 2;
    out->session_min_tokens = min_t;
    out->session_max_tokens = max_t;
    /* Subagents/eval truncate tool results harder — their detail is disposable. */
    out->max_result_chars = (role == COMPACT_ROLE_ROOT)
        ? COMPACT_POLICY_MICRO_ROOT_CHARS
        : COMPACT_POLICY_MICRO_ROOT_CHARS / 2;
    return true;
}

bool compact_role_from_str(const char *s, compact_role_t *out) {
    if (!s || !out) return false;
    if (!strcmp(s, "root") || !strcmp(s, "main")) { *out = COMPACT_ROLE_ROOT; return true; }
    if (!strcmp(s, "subagent") || !strcmp(s, "worker") || !strcmp(s, "task")) {
        *out = COMPACT_ROLE_SUBAGENT; return true;
    }
    if (!strcmp(s, "eval") || !strcmp(s, "bench")) { *out = COMPACT_ROLE_EVAL; return true; }
    return false;   /* unknown role: caller must default to ROOT (fail safe) */
}

char *compact_policy_json(const compact_policy_t *p, char *buf, size_t cap) {
    if (!p || !buf || cap < 2) return NULL;
    const char *role = p->role == COMPACT_ROLE_ROOT ? "root"
                     : p->role == COMPACT_ROLE_SUBAGENT ? "subagent" : "eval";
    int n = snprintf(buf, cap,
        "{\"schema\":\"dsco.compact_policy.v1\",\"role\":\"%s\","
        "\"session_min_tokens\":%d,\"session_max_tokens\":%d,"
        "\"max_result_chars\":%d,\"inherit_full_restore\":%s}",
        role, p->session_min_tokens, p->session_max_tokens,
        p->max_result_chars, p->inherit_full_restore ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) { buf[cap - 1] = '\0'; return NULL; }
    return buf;
}
