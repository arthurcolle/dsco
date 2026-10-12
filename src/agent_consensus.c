#include "agent_consensus.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Deterministic consensus over agent judgments; no network or model calls. */

double agent_consensus_reliability(const agent_reliability_t *table, size_t n,
                                   uint32_t model_class, uint32_t domain) {
    if (!table) return 0.5;
    for (size_t i = 0; i < n; i++)
        if (table[i].model_class == model_class && table[i].domain == domain) {
            if (table[i].correct > table[i].total) return 0.5;
            return ((double)table[i].correct + 1.0) / ((double)table[i].total + 2.0);
        }
    return 0.5;
}

void agent_consensus_learn(agent_reliability_t *table, size_t *n, size_t cap,
                           uint32_t model_class, uint32_t domain, bool was_correct) {
    if (!table || !n || *n > cap) return;
    for (size_t i = 0; i < *n; i++)
        if (table[i].model_class == model_class && table[i].domain == domain) {
            if (table[i].total == UINT32_MAX || table[i].correct > table[i].total) return;
            table[i].total++;
            if (was_correct) table[i].correct++;
            return;
        }
    if (*n >= cap) return;
    table[*n].model_class = model_class;
    table[*n].domain = domain;
    table[*n].correct = was_correct ? 1 : 0;
    table[*n].total = 1;
    (*n)++;
}

typedef struct {
    char label[AGENT_CONS_LABEL_MAX];
    double weight;
} label_weight_t;

typedef struct {
    label_weight_t labels[AGENT_CONS_MAX_VOTERS];
    size_t count;
    size_t positive_classes;
    double total;
} tally_t;

/* Project raw per-class weights onto a cap on FINAL total. All votes from a
 * class are scaled equally; labels are always tallied from the same adjusted
 * votes that determine the winner and runner-up. Fail closed if input exceeds
 * structural limits or yields a nonfinite/zero tally. */
static bool tally_votes(const agent_vote_t *votes, size_t n_votes,
                        const agent_reliability_t *reliability, size_t n_reliability,
                        tally_t *tally) {
    if (!votes || !tally || !n_votes || n_votes > AGENT_CONS_MAX_VOTERS ||
        (n_reliability && !reliability)) return false;
    memset(tally, 0, sizeof(*tally));
    uint32_t classes[AGENT_CONS_CLASS_MAX];
    double class_weight[AGENT_CONS_CLASS_MAX] = {0};
    size_t class_idx[AGENT_CONS_MAX_VOTERS];
    size_t label_idx[AGENT_CONS_MAX_VOTERS];
    double raw[AGENT_CONS_MAX_VOTERS];
    size_t n_classes = 0;
    double total = 0.0;
    for (size_t i = 0; i < n_votes; i++) {
        /* Vote labels are fixed arrays supplied by callers: never strcmp or
         * copy past the boundary if an untrusted vote lacks a terminator. */
        if (!memchr(votes[i].label, '\0', sizeof(votes[i].label)) ||
            !votes[i].label[0] || !isfinite(votes[i].confidence)) return false;
        double conf = votes[i].confidence;
        if (conf < 0.0) conf = 0.0;
        if (conf > 1.0) conf = 1.0;
        double rel = agent_consensus_reliability(reliability, n_reliability,
                                                 votes[i].model_class, votes[i].domain);
        raw[i] = conf * rel;
        if (!isfinite(raw[i])) return false;
        size_t ci = 0;
        while (ci < n_classes && classes[ci] != votes[i].model_class) ci++;
        if (ci == n_classes) {
            if (n_classes == AGENT_CONS_CLASS_MAX) return false;
            classes[n_classes++] = votes[i].model_class;
        }
        class_idx[i] = ci;
        class_weight[ci] += raw[i];
        total += raw[i];
        size_t li = 0;
        while (li < tally->count && strcmp(tally->labels[li].label, votes[i].label)) li++;
        if (li == tally->count) {
            memcpy(tally->labels[li].label, votes[i].label,
                   strlen(votes[i].label) + 1);
            tally->count++;
        }
        label_idx[i] = li;
    }
    if (!isfinite(total) || total <= 0.0) return false;
    for (size_t ci = 0; ci < n_classes; ci++)
        if (class_weight[ci] > 0.0) tally->positive_classes++;
    /* One positive class cannot satisfy a <100% share cap. Keep its tally
     * observable but never certify it as independent consensus. */
    double ceiling = total;
    if (tally->positive_classes > 1) {
        double lo = 0.0, hi = total;
        for (int step = 0; step < 80; step++) {
            double mid = lo + (hi - lo) * 0.5, sum = 0.0;
            for (size_t ci = 0; ci < n_classes; ci++) {
                double limit = AGENT_CONS_MAX_CLASS_SHARE * mid;
                sum += class_weight[ci] < limit ? class_weight[ci] : limit;
            }
            if (sum >= mid) lo = mid;
            else hi = mid;
        }
        ceiling = lo;
        if (ceiling <= 0.0) return false;
    }
    double limit = AGENT_CONS_MAX_CLASS_SHARE * ceiling;
    for (size_t i = 0; i < n_votes; i++) {
        double scale = 1.0;
        double cw = class_weight[class_idx[i]];
        if (tally->positive_classes > 1 && cw > limit)
            scale = limit / cw;
        tally->labels[label_idx[i]].weight += raw[i] * scale;
    }
    for (size_t li = 0; li < tally->count; li++)
        tally->total += tally->labels[li].weight;
    return isfinite(tally->total) && tally->total > 0.0;
}

static void top_two(const tally_t *t, size_t *first, size_t *second) {
    *first = 0;
    *second = t->count > 1 ? 1 : 0;
    if (t->count > 1 && t->labels[1].weight > t->labels[0].weight) {
        *first = 1; *second = 0;
    }
    for (size_t i = 2; i < t->count; i++) {
        if (t->labels[i].weight > t->labels[*first].weight) {
            *second = *first; *first = i;
        } else if (t->labels[i].weight > t->labels[*second].weight) {
            *second = i;
        }
    }
}

static void fill_result(const tally_t *t, size_t n_votes, agent_consensus_result_t *out) {
    size_t win, runner;
    top_two(t, &win, &runner);
    memset(out, 0, sizeof(*out));
    out->n_votes = n_votes;
    snprintf(out->winning_label, sizeof(out->winning_label), "%s", t->labels[win].label);
    out->weight = t->labels[win].weight;
    out->margin = t->count > 1 ? out->weight - t->labels[runner].weight : out->weight;
    out->agreement = out->weight / t->total;
    out->clear = t->positive_classes > 1 &&
                 out->agreement >= AGENT_CONS_CLEAR_AGREEMENT;
}

bool agent_consensus_vote(const agent_vote_t *votes, size_t n_votes,
                          const agent_reliability_t *reliability, size_t n_reliability,
                          agent_consensus_result_t *out) {
    if (!out) return false;
    tally_t t;
    if (!tally_votes(votes, n_votes, reliability, n_reliability, &t)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    fill_result(&t, n_votes, out);
    return true;
}

int agent_consensus_decide(const agent_vote_t *votes, size_t n_votes,
                           const agent_reliability_t *reliability, size_t n_reliability,
                           const char *question_context,
                           agent_consensus_tiebreak_fn tiebreak, void *ctx,
                           agent_consensus_result_t *out, bool *used_tiebreak,
                           double *tiebreak_confidence) {
    if (used_tiebreak) *used_tiebreak = false;
    if (tiebreak_confidence) *tiebreak_confidence = 0.0;
    if (!out) return -1;
    tally_t t;
    if (!tally_votes(votes, n_votes, reliability, n_reliability, &t)) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    fill_result(&t, n_votes, out);
    if (out->clear) return 1;
    if (!tiebreak || t.count < 2) return 0;
    size_t win, runner;
    top_two(&t, &win, &runner);
    double confidence = 0.0;
    int pick = tiebreak(t.labels[win].label, t.labels[runner].label,
                        question_context, &confidence, ctx);
    if (used_tiebreak) *used_tiebreak = true;
    if (tiebreak_confidence) *tiebreak_confidence = confidence;
    if (!isfinite(confidence) || confidence < 0.6 || confidence > 1.0 ||
        (pick != 0 && pick != 1)) return 0;
    if (pick == 1) {
        snprintf(out->winning_label, sizeof(out->winning_label), "%s", t.labels[runner].label);
        out->weight = t.labels[runner].weight;
        out->margin = t.labels[runner].weight - t.labels[win].weight;
        out->agreement = out->weight / t.total;
    }
    out->clear = true;
    return 1;
}
