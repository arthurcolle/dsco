#ifndef DSCO_AGENT_CONSENSUS_H
#define DSCO_AGENT_CONSENSUS_H

/* agent_consensus.h — consensus over agent *judgments*, not crypto.
 *
 * The real 100K-fleet problem is not Byzantine attackers forging signatures —
 * it's agents that are helpful-but-wrong: different models, different
 * miscalibrations, hallucination, sycophancy, correlated errors. You need
 * consensus over what agents *believe* anyway, and you usually have no ground
 * truth oracle to check them against.
 *
 * This module is the honest version of that problem:
 *
 *   - Calibrated-confidence voting: each agent reports (judgment, confidence,
 *     model_class). Votes are weighted by confidence AND by a per-model-class
 *     reliability score learned from outcomes (which agents are usually right
 *     *about which kinds of questions*). A cluster of identical cheap models
 *     counts less than independent diverse ones — correlated error is the
 *     enemy, not any single liar.
 *
 *   - Truth discovery without an oracle: when most agents agree, that's a
 *     signal; when they *disagree*, the ones that turn out right more often on
 *     THIS kind of question should weigh more. Reliability is tracked per
 *     model-class per domain, updated from resolved outcomes.
 *
 *   - Cheap tiebreak: when the weighted vote is within a band (no clear
 *     winner), defer to a calibrated Jev resolver (the same jev_gate_verify /
 *     decide substrate) as the cheap arbiter — one System One call instead of
 *     a frontier judge. Escalate to the frontier judge only when even that is
 *     uncertain (the Experiment-1 cascade).
 *
 *   - Correlation penalty: agents sharing a model_class are assumed to share
 *     errors; their combined weight is capped so N replicas of the same model
     can't outvote one independent diverse opinion.
 *
 * No signatures, no proof-of-work, no global ledger — the trust comes from
 * calibrated confidence + learned reliability + diversity, which is the right
 * model for cooperative-but-fallible agents. Deterministic and network-free;
 * the Jev resolver is injected so tests stub it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_CONS_MAX_VOTERS 64
#define AGENT_CONS_CLASS_MAX 32
#define AGENT_CONS_LABEL_MAX 96

/* ── A single agent's vote ─────────────────────────────────────────────── */

typedef struct {
    char     label[AGENT_CONS_LABEL_MAX];  /* the judgment (e.g. "yes", "Paris", "route-cheap") */
    double   confidence;                   /* 0..1 the agent's stated confidence */
    uint32_t model_class;                  /* which model/persona family (correlation group) */
    uint32_t domain;                       /* question domain (for per-domain reliability) */
} agent_vote_t;

/* ── Learned reliability per (model_class, domain) ─────────────────────── */

typedef struct {
    uint32_t model_class;
    uint32_t domain;
    uint32_t correct;        /* resolved outcomes where this class was right */
    uint32_t total;          /* resolved outcomes seen */
} agent_reliability_t;

/* Reliability of a model class in a domain: Laplace-smoothed correct/total,
 * defaulting to 0.5 (uninformative) with no history. */
double agent_consensus_reliability(const agent_reliability_t *table, size_t n,
                                   uint32_t model_class, uint32_t domain);

/* Record a resolved outcome to update reliability (the learning step). */
void agent_consensus_learn(agent_reliability_t *table, size_t *n, size_t cap,
                           uint32_t model_class, uint32_t domain, bool was_correct);

/* ── The weighted vote ─────────────────────────────────────────────────── */

typedef struct {
    char     winning_label[AGENT_CONS_LABEL_MAX];
    double   weight;          /* total (correlation-penalized) weight for the winner */
    double   margin;          /* winner weight - runner-up weight */
    double   agreement;       /* winner weight / total weight */
    bool     clear;           /* >= agreement threshold with >1 positive class, or valid arbiter */
    size_t   n_votes;
} agent_consensus_result_t;

/* Correlation penalty: a model_class's combined weight is capped at half the
 * adjusted total when multiple positive classes exist. A sole positive class
 * remains observable but cannot by itself establish independent consensus.
 * Caller-supplied model_class identities must be trustworthy: false class
 * diversity cannot be inferred or corrected by this vote function. */
#define AGENT_CONS_MAX_CLASS_SHARE 0.5

/* Tiebreak band: if the winner's agreement is below this, the vote is not
 * clear and the caller should defer to the Jev tiebreak / frontier judge. */
#define AGENT_CONS_CLEAR_AGREEMENT 0.6

/* Compute consensus over a set of votes. Weights = confidence * reliability,
 * then each model_class's total is capped at AGENT_CONS_MAX_CLASS_SHARE of the
 * grand total (correlation penalty). The winning label is the highest-weighted
 * distinct judgment. Deterministic, network-free. */
bool agent_consensus_vote(const agent_vote_t *votes, size_t n_votes,
                          const agent_reliability_t *reliability, size_t n_reliability,
                          agent_consensus_result_t *out);

/* ── The full consensus decision with Jev tiebreak (injected) ──────────── */

/* Cheap arbiter for unclear votes: resolve between the top-2 labels. Returns
 * the index into the two labels (0 or 1) and sets *confidence. This is the Jev
 * call in production; stubbed in tests. */
typedef int (*agent_consensus_tiebreak_fn)(const char *label_a, const char *label_b,
                                           const char *question_context,
                                           double *confidence, void *ctx);

/* Full pipeline: weighted vote; if clear, done; if not clear, defer to the
 * injected tiebreak between the top-2; if even that is uncertain (low
 * confidence), report "escalate" so the caller invokes the frontier judge.
 *
 * Returns:
 *   1  = consensus reached (out->winning_label set, *used_tiebreak may be set)
 *   0  = no consensus; escalate to the frontier judge
 *  -1  = invalid input
 */
int agent_consensus_decide(const agent_vote_t *votes, size_t n_votes,
                           const agent_reliability_t *reliability, size_t n_reliability,
                           const char *question_context,
                           agent_consensus_tiebreak_fn tiebreak, void *ctx,
                           agent_consensus_result_t *out, bool *used_tiebreak,
                           double *tiebreak_confidence);

#endif /* DSCO_AGENT_CONSENSUS_H */
