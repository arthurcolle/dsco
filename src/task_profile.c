/**
 * task_profile.c — Semantic task profiling for dynamic topology selection
 *
 * Phase 1 MVP: Keyword-based pattern detection + heuristic scoring
 * Future: Semantic API integration for more accurate profiling
 */

#include "task_profile.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <strings.h>

/* ── Pattern definitions ───────────────────────────────────────────────── */

static const char *pattern_keywords[PATTERN_COUNT][10] = {
    [PATTERN_ANALYSIS] = {"analyze", "examine", "inspect", "evaluate", "assess", "audit", "review",
                          "check", "test", NULL},
    [PATTERN_CODING] = {"code", "implement", "refactor", "debug", "fix", "write", "compile",
                        "build", "deploy", NULL},
    [PATTERN_PLANNING] = {"plan", "design", "architect", "strategize", "organize", "outline",
                          "structure", "framework", "system", NULL},
    [PATTERN_SYNTHESIS] = {"combine", "integrate", "merge", "consolidate", "aggregate", "unite",
                           "gather", "collect", "summarize", NULL},
    [PATTERN_REVIEW] = {"review", "audit", "verify", "check", "validate", "confirm", "approve",
                        "examine", "inspect", NULL},
    [PATTERN_REASONING] = {"reason", "deduce", "infer", "conclude", "analyze", "explain", "derive",
                           "determine", "find", NULL},
    [PATTERN_ITERATION] = {"refine", "improve", "optimize", "iterate", "enhance", "revise",
                           "adjust", "tune", "polish", NULL},
    [PATTERN_PARALLELISM] = {"parallel", "concurrent", "simultaneous", "fanout", "distribute",
                             "batch", "multiple", "several", "many", NULL},
    [PATTERN_CONSENSUS] = {"consensus", "agreement", "debate", "discussion", "converge", "agree",
                           "vote", "majority", "conflict", NULL},
    [PATTERN_SPECIALIST] = {"expert", "specialist", "domain", "specific", "technical",
                            "specialized", "advanced", "deep", "nuanced", NULL}};

/* ── Case-insensitive keyword search ───────────────────────────────────── */

static bool is_keyword_boundary(char c) {
    return c == '\0' || !isalnum((unsigned char)c);
}

static bool starts_with_keyword(const char *text, const char *keyword) {
    size_t len = strlen(keyword);
    return strncasecmp(text, keyword, len) == 0 && is_keyword_boundary(text[len]);
}

static bool contains_keyword(const char *text, const char *keyword) {
    if (!text || !keyword)
        return false;
    size_t len = strlen(keyword);
    if (len == 0)
        return false;

    for (const char *p = text; *p; p++) {
        if (starts_with_keyword(p, keyword) && (p == text || is_keyword_boundary(p[-1])) &&
            is_keyword_boundary(p[len])) {
            return true;
        }
    }
    return false;
}

/* ── Pattern detection ─────────────────────────────────────────────────── */

static int detect_patterns(const char *task, bool *patterns_out, int *keyword_count_out) {
    int count = 0;
    int keyword_count = 0;

    for (int i = 0; i < PATTERN_COUNT; i++) {
        patterns_out[i] = false;
        for (int j = 0; pattern_keywords[i][j]; j++) {
            if (contains_keyword(task, pattern_keywords[i][j])) {
                keyword_count++;
                patterns_out[i] = true;
            }
        }
        if (patterns_out[i])
            count++;
    }

    if (keyword_count_out)
        *keyword_count_out = keyword_count;
    return count;
}

/* ── Scoring heuristics ───────────────────────────────────────────────── */

static double compute_parallelism_score(const task_profile_t *tp) {
    // Heuristic: presence of parallelism/consensus patterns + clause count
    double score = 0.0;

    if (tp->patterns[PATTERN_PARALLELISM])
        score += 0.5;
    if (tp->patterns[PATTERN_CONSENSUS])
        score += 0.3;
    if (tp->patterns[PATTERN_ANALYSIS] && tp->clause_count > 2)
        score += 0.2;

    return fmin(score, 1.0);
}

static double compute_convergence_score(const task_profile_t *tp) {
    // Heuristic: synthesis/consensus/review patterns + multiple clauses
    double score = 0.0;

    if (tp->patterns[PATTERN_CONSENSUS])
        score += 0.5;
    if (tp->patterns[PATTERN_SYNTHESIS])
        score += 0.3;
    if (tp->patterns[PATTERN_REVIEW])
        score += 0.2;

    // More clauses → more refinement needed
    if (tp->clause_count > 3)
        score += 0.1;

    return fmin(score, 1.0);
}

static double compute_complexity_score(const task_profile_t *tp) {
    // Heuristic: specialist/reasoning/planning patterns + task length
    double score = 0.0;

    if (tp->patterns[PATTERN_SPECIALIST])
        score += 0.3;
    if (tp->patterns[PATTERN_REASONING])
        score += 0.3;
    if (tp->patterns[PATTERN_PLANNING])
        score += 0.2;

    // Longer task ≈ more complex
    if (tp->task_length > 200)
        score += 0.1;

    return fmin(score, 1.0);
}

static double compute_latency_score(const task_profile_t *tp) {
    // Heuristic: urgency keywords (not in current keyword set, but reserved)
    // For now, return low score (no urgency detected in base patterns)
    double score = 0.0;

    if (contains_keyword(tp->task, "urgent") || contains_keyword(tp->task, "asap") ||
        contains_keyword(tp->task, "now")) {
        score = 0.9;
    }

    return score;
}

/* ── Topology ranking ──────────────────────────────────────────────────── */

static double compute_fit_score(const task_profile_t *tp, const topology_t *topo) {
    double score = 0.5; // Baseline

    if (tp->pattern_count == 0) {
        if (strcasecmp(topo->name, "triage") == 0)
            return 0.85;
        if (topo->category == CAT_SPECIALIST && topo->est_latency_mult <= 2.0)
            return 0.60;
        return 0.45;
    }

    // Match topology category to detected patterns
    switch (topo->category) {
        case CAT_CHAIN:
            // Good for sequential reasoning
            if (tp->patterns[PATTERN_PLANNING] || tp->patterns[PATTERN_REASONING])
                score += 0.25;
            break;

        case CAT_FANOUT:
            // Good for parallel tasks
            if (tp->parallelism_score > 0.6)
                score += 0.3;
            break;

        case CAT_HIERARCHY:
            // Good for complex planning with delegation
            if (tp->patterns[PATTERN_PLANNING] && tp->complexity_score > 0.5)
                score += 0.25;
            break;

        case CAT_MESH:
            // Good for consensus/convergence
            if (tp->convergence_score > 0.6)
                score += 0.3;
            break;

        case CAT_SPECIALIST:
            // Good for complex/specialized tasks
            if (tp->patterns[PATTERN_SPECIALIST] || tp->complexity_score > 0.6)
                score += 0.3;
            break;

        case CAT_FEEDBACK:
            // Good for iteration/refinement
            if (tp->patterns[PATTERN_ITERATION] || tp->convergence_score > 0.5)
                score += 0.3;
            break;

        case CAT_DOMAIN:
            // Domain-specific topologies should win only when the task carries
            // a domain signal, not for arbitrary generic text.
            if (tp->patterns[PATTERN_CODING] &&
                (contains_keyword(topo->name, "code") ||
                 contains_keyword(topo->description, "code") ||
                 contains_keyword(topo->description, "implement"))) {
                score += 0.25;
            }
            if (tp->patterns[PATTERN_REVIEW] && (contains_keyword(topo->name, "review") ||
                                                 contains_keyword(topo->description, "review") ||
                                                 contains_keyword(topo->description, "audit") ||
                                                 contains_keyword(topo->description, "validate"))) {
                score += 0.25;
            }
            if (tp->patterns[PATTERN_ANALYSIS] &&
                (contains_keyword(topo->name, "research") ||
                 contains_keyword(topo->description, "research") ||
                 contains_keyword(topo->description, "analysis") ||
                 contains_keyword(topo->description, "gather"))) {
                score += 0.25;
            }
            break;

        case CAT_COMPETITIVE:
            // Good for specialist tasks where we want best-of-N
            if (tp->complexity_score > 0.5)
                score += 0.2;
            break;
    }

    if (tp->latency_score > 0.6) {
        if (topo->est_latency_mult <= 2.0)
            score += 0.12;
        if (topo->est_latency_mult >= 5.0)
            score -= 0.10;
    }

    if (tp->parallelism_score > 0.6 && topo->total_agents >= 5)
        score += 0.08;
    if (tp->complexity_score < 0.35 && topo->total_agents > 8)
        score -= 0.10;

    if (score < 0.0)
        return 0.0;
    return fmin(score, 1.0);
}

static int rank_topologies(task_profile_t *tp) {
    int topology_count = 0;
    const topology_t *candidates = topology_registry(&topology_count);

    tp->suggestion_count = 0;
    for (int i = 0; i < topology_count; i++) {
        const topology_t *candidate = &candidates[i];
        if (!topology_is_runnable(candidate))
            continue;

        double fit = compute_fit_score(tp, candidate);

        // Insert in sorted order (highest fit first)
        int insert_pos = tp->suggestion_count;
        for (int j = 0; j < tp->suggestion_count; j++) {
            const topology_t *existing = tp->suggestions[j].topo;
            bool better_fit = fit > tp->suggestions[j].fit_score;
            bool same_fit = fabs(fit - tp->suggestions[j].fit_score) < 0.0001;
            bool lower_latency =
                existing && candidate->est_latency_mult < existing->est_latency_mult;
            bool same_latency =
                existing && candidate->est_latency_mult == existing->est_latency_mult;
            bool lower_cost = existing && candidate->est_cost_1k < existing->est_cost_1k;

            if (better_fit || (same_fit && (lower_latency || (same_latency && lower_cost)))) {
                insert_pos = j;
                break;
            }
        }

        if (insert_pos >= 15)
            continue;

        int last = tp->suggestion_count < 15 ? tp->suggestion_count : 14;
        for (int j = last; j > insert_pos; j--) {
            tp->suggestions[j] = tp->suggestions[j - 1];
        }

        tp->suggestions[insert_pos].topo = candidate;
        tp->suggestions[insert_pos].fit_score = fit;
        tp->suggestions[insert_pos].reason = topo_category_label(candidate->category);
        if (tp->suggestion_count < 15)
            tp->suggestion_count++;
    }

    return tp->suggestion_count;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

task_profile_t *task_profile(const char *task, const char *api_key) {
    (void)api_key; // Unused in phase 1

    task_profile_t *tp = malloc(sizeof(*tp));
    if (!tp)
        return NULL;

    memset(tp, 0, sizeof(*tp));

    if (task && task[0]) {
        strncpy(tp->task, task, sizeof(tp->task) - 1);
    }

    tp->task_length = strlen(tp->task);

    // Count clauses (naive: semicolons + commas + conjunctions)
    tp->clause_count = 1;
    for (const char *p = tp->task; *p; p++) {
        if (*p == ',' || *p == ';' ||
            (p[0] == ' ' &&
             (starts_with_keyword(p + 1, "and") || starts_with_keyword(p + 1, "or") ||
              starts_with_keyword(p + 1, "then") || starts_with_keyword(p + 1, "next")))) {
            tp->clause_count++;
        }
    }

    // Detect patterns
    tp->pattern_count = detect_patterns(tp->task, tp->patterns, &tp->keyword_match_count);

    // Compute scores
    tp->parallelism_score = compute_parallelism_score(tp);
    tp->convergence_score = compute_convergence_score(tp);
    tp->complexity_score = compute_complexity_score(tp);
    tp->latency_score = compute_latency_score(tp);

    // Rank topologies
    rank_topologies(tp);

    // Fallback to triage if no suggestions
    if (tp->suggestion_count == 0) {
        tp->suggestions[0].topo = topology_find("triage");
        tp->suggestions[0].fit_score = 0.5;
        tp->suggestions[0].reason = "default (no match)";
        tp->suggestion_count = 1;
    }

    return tp;
}

void task_profile_free(task_profile_t *tp) {
    if (tp)
        free(tp);
}

int task_profile_json(const task_profile_t *tp, char *buf, size_t len) {
    if (!tp || !buf || len == 0)
        return -1;

    jbuf_t b;
    jbuf_init(&b, 2048);
    jbuf_append(&b, "{\"task\":");
    jbuf_append_json_str(&b, tp->task);
    jbuf_appendf(&b,
                 ",\"scores\":{\"parallelism\":%.2f,\"convergence\":%.2f,"
                 "\"complexity\":%.2f,\"latency\":%.2f}"
                 ",\"metrics\":{\"task_length\":%d,\"clause_count\":%d,"
                 "\"pattern_count\":%d,\"keyword_match_count\":%d}"
                 ",\"suggestions\":[",
                 tp->parallelism_score, tp->convergence_score, tp->complexity_score,
                 tp->latency_score, tp->task_length, tp->clause_count, tp->pattern_count,
                 tp->keyword_match_count);

    for (int i = 0; i < tp->suggestion_count; i++) {
        if (i > 0)
            jbuf_append_char(&b, ',');
        const topology_t *topo = tp->suggestions[i].topo;
        jbuf_append(&b, "{\"topo\":");
        jbuf_append_json_str(&b, topo ? topo->name : "unknown");
        jbuf_appendf(&b, ",\"fit\":%.2f,\"reason\":", tp->suggestions[i].fit_score);
        jbuf_append_json_str(&b, tp->suggestions[i].reason ? tp->suggestions[i].reason : "");
        jbuf_append_char(&b, '}');
    }

    jbuf_append(&b, "]}");

    size_t copy_len = b.len < len - 1 ? b.len : len - 1;
    memcpy(buf, b.data, copy_len);
    buf[copy_len] = '\0';

    int written = copy_len > (size_t)INT_MAX ? INT_MAX : (int)copy_len;
    jbuf_free(&b);
    return written;
}

const topology_t *task_profile_best_topology(const task_profile_t *tp) {
    if (!tp || tp->suggestion_count == 0)
        return NULL;
    return tp->suggestions[0].topo;
}

int task_profile_explain(const task_profile_t *tp, char *buf, size_t len) {
    if (!tp || !buf || len == 0)
        return -1;

    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_appendf(&b,
                 "Task profile:\n"
                 "  Parallelism:  %.0f%% (%.2f)\n"
                 "  Convergence:  %.0f%% (%.2f)\n"
                 "  Complexity:   %.0f%% (%.2f)\n"
                 "  Latency:      %.0f%% (%.2f)\n"
                 "\n"
                 "Patterns detected: %d\n",
                 tp->parallelism_score * 100.0, tp->parallelism_score,
                 tp->convergence_score * 100.0, tp->convergence_score, tp->complexity_score * 100.0,
                 tp->complexity_score, tp->latency_score * 100.0, tp->latency_score,
                 tp->pattern_count);

    int top_n = (tp->suggestion_count < 3 ? tp->suggestion_count : 3);
    jbuf_appendf(&b, "\nTop %d topologies:\n", top_n);

    for (int i = 0; i < top_n; i++) {
        if (tp->suggestions[i].topo) {
            jbuf_appendf(&b, "  %d. %s (fit: %.0f%%)\n", i + 1, tp->suggestions[i].topo->name,
                         tp->suggestions[i].fit_score * 100.0);
        }
    }

    size_t copy_len = b.len < len - 1 ? b.len : len - 1;
    memcpy(buf, b.data, copy_len);
    buf[copy_len] = '\0';

    int written = copy_len > (size_t)INT_MAX ? INT_MAX : (int)copy_len;
    jbuf_free(&b);
    return written;
}
