#ifndef DSCO_COMPACT_POLICY_H
#define DSCO_COMPACT_POLICY_H

/* compact_policy.h — per-agent compaction thresholds (distilled from oh-my-pi).
 *
 * The existing compact_config_t (llm.h) is a single global config: one
 * session_min/session_max token threshold governs the main session AND every
 * subagent. oh-my-pi's insight (changelog #13107) is that this is wrong —
 * task/eval subagents should compact at tighter, case-sensitive thresholds
 * (percentage-of-window or fixed-token) WITHOUT changing the main session's
 * threshold. A background worker must never blow the root session's budget.
 *
 * This module is a pure policy layer: given an agent role, it returns the
 * compaction thresholds for that role, scaled from the global defaults.
 * No globals, no I/O, no provider knowledge — trivially testable. The caller
 * (agent.c / subagent spawn) applies the result to its local compact_config_t.
 *
 * Roles are an ordered ladder of aggressiveness. Higher roles compact sooner
 * (lower thresholds) and shallower, because their work is more disposable:
 *   ROOT     — the user's main session. Full budget; the defaults, untouched.
 *   SUBAGENT — a spawned task worker. Tighter: compact earlier.
 *   EVAL     — eval/bench runners. Tightest: disposable, correctness-critical
 *              only on the final answer, so history compacts aggressively.
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COMPACT_ROLE_ROOT = 0,   /* main session; global defaults, never tightened */
    COMPACT_ROLE_SUBAGENT,   /* spawned task/eval worker */
    COMPACT_ROLE_EVAL,       /* eval/bench runner; most aggressive           */
} compact_role_t;

/* The per-role threshold overrides (absolute token counts after scaling). */
typedef struct {
    compact_role_t role;
    int session_min_tokens;   /* trigger compaction at this many tokens   */
    int session_max_tokens;   /* target token count after compaction      */
    int max_result_chars;     /* micro-tier per-result cap                */
    bool inherit_full_restore;/* keep the global full-restore budget?     */
} compact_policy_t;

/* Compute the compaction policy for a role, scaled from the global defaults
 * (the ROOT config the operator set). Pure: same inputs, same outputs.
 *
 *   root_min / root_max: the global (ROOT) session thresholds.
 *   role:                which agent class.
 *   out:                 receives the policy (required).
 *
 * Scaling (conservative defaults; a role never loosens the ROOT budget):
 *   ROOT:     identity (thresholds unchanged).
 *   SUBAGENT: 50% of the root thresholds (compact at half the window).
 *   EVAL:     30% of the root thresholds (compact aggressively).
 * The result is clamped to sane floors so a tiny root budget still works. */
bool compact_policy_for_role(compact_role_t role,
                             int root_min_tokens, int root_max_tokens,
                             compact_policy_t *out);

/* Parse a role from a string ("root"|"subagent"|"worker"|"eval"|"bench").
 * Returns false for unknown roles (caller defaults to ROOT — fail safe). */
bool compact_role_from_str(const char *s, compact_role_t *out);

/* Render the policy as compact JSON into buf (for tool/report output). */
char *compact_policy_json(const compact_policy_t *p, char *buf, size_t cap);

/* Rolling request-pressure trigger. Caps the working set even on million-token
 * models, and reserves two observed growth increments before admission. */
int compact_rolling_trigger(int input_limit, int growth_tokens);

#endif /* DSCO_COMPACT_POLICY_H */
