#ifndef DSCO_JEV_DECIDE_H
#define DSCO_JEV_DECIDE_H

/* jev_decide.h — general cheap typed-decision client (Noul/Choice/Score).
 *
 * Sibling to jev_router (which is a single Choice rubric for model routing).
 * This module generalizes the pattern into the unified decision substrate the
 * Python JevDecisionSurface prototyped: one governed client, three typed
 * decisions, bounded receipts. Domains (routing, fleet, retrieval, trust,
 * bias) are thin wrappers that supply a state + instruction; this module owns
 * the request, validation, capability gating, and the receipt.
 *
 * Governance contract (same discipline as jev_router):
 *   - recommend only: it returns a decision + confidence; it never dispatches,
 *     spawns, or mutates external state. The caller applies (or not).
 *   - capability-gated: requires CAP_SECRETS (the API key) and routes the HTTP
 *     through tools_execute_for_tier("curl_raw"), so tier + grant rules apply.
 *   - bounded: at most 32 calls/process, 2s each, no retry, 16KB response cap.
 *   - validated: response must be the expected model, a single typed answer,
 *     probabilities in [0,1], and bounded usage — or it returns a finish()
 *     receipt with a non-OK status and false.
 */

#include <stdbool.h>
#include <stddef.h>

#define JEV_DECIDE_MODEL "jev-1.13.0"
#define JEV_DECIDE_STATE_MAX 8192
#define JEV_DECIDE_INSTRUCTION_MAX 1024
#define JEV_DECIDE_MAX_OPTIONS 64
#define JEV_DECIDE_OPTION_MAX 96
#define JEV_DECIDE_RECEIPT_MAX 1024

/* The three typed decisions, mirroring the Python substrate. */
typedef enum {
    JEV_DECIDE_BOOL = 0,   /* Noul: probability_yes, thresholded by caller    */
    JEV_DECIDE_SCORE,      /* Score: expected value over ordered levels        */
    JEV_DECIDE_CHOICE      /* Choice: argmax label + full distribution         */
} jev_decide_kind_t;

/* Outcome of one decision. For BOOL, p_yes is the calibrated probability and
 * choice/ score are unused. For SCORE, score is the expected level value and
 * confidence its confidence. For CHOICE, choice holds the selected label and
 * confidence its probability. p_yes is always the most useful scalar. */
typedef struct {
    jev_decide_kind_t kind;
    double p_yes;                 /* BOOL: probability yes (else 0)          */
    double score;                 /* SCORE: expected value (else 0)          */
    char choice[JEV_DECIDE_OPTION_MAX]; /* CHOICE: selected label (else "") */
    double confidence;            /* calibrated confidence of the decision    */
} jev_decide_result_t;

/* Make one typed decision about `state` with `instruction`.
 *
 *   kind:       BOOL | SCORE | CHOICE.
 *   domain:     short tag for the receipt (e.g. "routing","fleet","trust").
 *   state:      the subject text (data, not instructions). Bounded.
 *   instruction: the question's instructions. Bounded.
 *   options:    CHOICE: the label set (2..64). SCORE: ordered level labels
 *               (2..10). BOOL: ignored (pass NULL).
 *   n_options:  count of options.
 *   result:     receives the decision (required).
 *   receipt:    bounded JSON receipt (required, >= 256 bytes).
 *   receipt_len: size of receipt buffer.
 *   tier:       capability tier (NULL -> "standard").
 *
 * Returns true on a validated decision; false with a finish() receipt on any
 * gate/transport/validation failure. Pure decision engine: no dispatch. */
bool jev_decide(jev_decide_kind_t kind,
                const char *domain,
                const char *state,
                const char *instruction,
                const char *const *options, size_t n_options,
                jev_decide_result_t *result,
                char *receipt, size_t receipt_len,
                const char *tier);

/* Convenience: calibrated BOOL with a threshold applied for the caller.
 * Returns the boolean; p_yes_out (optional) receives the raw probability. */
bool jev_decide_bool(const char *domain, const char *state,
                     const char *instruction, double threshold,
                     bool *decision_out, double *p_yes_out,
                     char *receipt, size_t receipt_len, const char *tier);

/* Compaction domain: summarize a block of older turns down to a target level,
 * returning a compact summary plus a content-addressed context key for later
 * retrieval from the CAS/memory layer.
 *
 *   level 1..4 compresses progressively: L1 detailed, L2 concise, L3 terse
 *   abstract, L4 one-paragraph gist. Higher level = shorter summary.
 *   context_key receives "ctx:compact:L<n>:<sha256-24>", content-addressed so
 *   identical compaction requests dedup and the key resolves to the artifact.
 *
 * The compaction is itself a typed decision: each candidate sentence gets a
 * governed keep/drop Noul, and the kept sentences compose the summary. This
 * keeps the whole operation inside the typed-decision contract (no free-form
 * generation) and inside the same capability + attempt budget.
 *
 * Returns true on success (summary + context_key + receipt filled); false with
 * a finish() receipt on gate/transport/validation failure. Recommend-only:
 * it never mutates the conversation or archive policy; the caller applies. */
bool jev_compact_turns(const char *domain, const char *turns_text,
                       int level, char *summary, size_t summary_len,
                       char *context_key, size_t context_key_len,
                       char *receipt, size_t receipt_len, const char *tier);

#endif /* DSCO_JEV_DECIDE_H */
