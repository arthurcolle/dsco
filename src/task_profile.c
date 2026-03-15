/**
 * task_profile.c — Semantic task profiling for dynamic topology selection
 *
 * Phase 1 MVP: Keyword-based pattern detection + heuristic scoring
 * Future: Semantic API integration for more accurate profiling
 */

#include "task_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

/* ── Pattern definitions ───────────────────────────────────────────────── */

static const char *pattern_keywords[PATTERN_COUNT][10] = {
    [PATTERN_ANALYSIS] = {
        "analyze", "examine", "inspect", "evaluate", "assess", "audit",
        "review", "check", "test", NULL
    },
    [PATTERN_CODING] = {
        "code", "implement", "refactor", "debug", "fix", "write",
        "compile", "build", "deploy", NULL
    },
    [PATTERN_PLANNING] = {
        "plan", "design", "architect", "strategize", "organize", "outline",
        "structure", "framework", "system", NULL
    },
    [PATTERN_SYNTHESIS] = {
        "combine", "integrate", "merge", "consolidate", "aggregate",
        "unite", "gather", "collect", "summarize", NULL
    },
    [PATTERN_REVIEW] = {
        "review", "audit", "verify", "check", "validate", "confirm",
        "approve", "examine", "inspect", NULL
    },
    [PATTERN_REASONING] = {
        "reason", "deduce", "infer", "conclude", "analyze", "explain",
        "derive", "determine", "find", NULL
    },
    [PATTERN_ITERATION] = {
        "refine", "improve", "optimize", "iterate", "enhance", "revise",
        "adjust", "tune", "polish", NULL
    },
    [PATTERN_PARALLELISM] = {
        "parallel", "concurrent", "simultaneous", "fanout", "distribute",
        "batch", "multiple", "several", "many", NULL
    },
    [PATTERN_CONSENSUS] = {
        "consensus", "agreement", "debate", "discussion", "converge",
        "agree", "vote", "majority", "conflict", NULL
    },
    [PATTERN_SPECIALIST] = {
        "expert", "specialist", "domain", "specific", "technical",
        "specialized", "advanced", "deep", "nuanced", NULL
    }
};

/* ── Case-insensitive substring search ──────────────────────────────────── */

static bool contains_keyword(const char *text, const char *keyword) {
    if (!text || !keyword) return false;
    
    // Make lower-case copies for case-insensitive comparison
    char *text_lower = strdup(text);
    char *kw_lower = strdup(keyword);
    
    for (char *p = text_lower; *p; p++) *p = tolower(*p);
    for (char *p = kw_lower; *p; p++) *p = tolower(*p);
    
    bool found = strstr(text_lower, kw_lower) != NULL;
    
    free(text_lower);
    free(kw_lower);
    return found;
}

/* ── Pattern detection ─────────────────────────────────────────────────── */

static int detect_patterns(const char *task, bool *patterns_out) {
    int count = 0;
    
    for (int i = 0; i < PATTERN_COUNT; i++) {
        patterns_out[i] = false;
        for (int j = 0; pattern_keywords[i][j]; j++) {
            if (contains_keyword(task, pattern_keywords[i][j])) {
                patterns_out[i] = true;
                count++;
                break;  // Count each pattern once
            }
        }
    }
    
    return count;
}

/* ── Scoring heuristics ───────────────────────────────────────────────── */

static double compute_parallelism_score(const task_profile_t *tp) {
    // Heuristic: presence of parallelism/consensus patterns + clause count
    double score = 0.0;
    
    if (tp->patterns[PATTERN_PARALLELISM]) score += 0.5;
    if (tp->patterns[PATTERN_CONSENSUS]) score += 0.3;
    if (tp->patterns[PATTERN_ANALYSIS] && tp->clause_count > 2) score += 0.2;
    
    return fmin(score, 1.0);
}

static double compute_convergence_score(const task_profile_t *tp) {
    // Heuristic: synthesis/consensus/review patterns + multiple clauses
    double score = 0.0;
    
    if (tp->patterns[PATTERN_CONSENSUS]) score += 0.5;
    if (tp->patterns[PATTERN_SYNTHESIS]) score += 0.3;
    if (tp->patterns[PATTERN_REVIEW]) score += 0.2;
    
    // More clauses → more refinement needed
    if (tp->clause_count > 3) score += 0.1;
    
    return fmin(score, 1.0);
}

static double compute_complexity_score(const task_profile_t *tp) {
    // Heuristic: specialist/reasoning/planning patterns + task length
    double score = 0.0;
    
    if (tp->patterns[PATTERN_SPECIALIST]) score += 0.3;
    if (tp->patterns[PATTERN_REASONING]) score += 0.3;
    if (tp->patterns[PATTERN_PLANNING]) score += 0.2;
    
    // Longer task ≈ more complex
    if (tp->task_length > 200) score += 0.1;
    
    return fmin(score, 1.0);
}

static double compute_latency_score(const task_profile_t *tp) {
    // Heuristic: urgency keywords (not in current keyword set, but reserved)
    // For now, return low score (no urgency detected in base patterns)
    double score = 0.0;
    
    if (contains_keyword(tp->task, "urgent") || 
        contains_keyword(tp->task, "asap") ||
        contains_keyword(tp->task, "now")) {
        score = 0.9;
    }
    
    return score;
}

/* ── Topology ranking ──────────────────────────────────────────────────── */

static double compute_fit_score(const task_profile_t *tp,
                                const topology_t *topo) {
    double score = 0.5;  // Baseline
    
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
        // Generic domain-specific topologies
        score += 0.1;
        break;

    case CAT_COMPETITIVE:
        // Good for specialist tasks where we want best-of-N
        if (tp->complexity_score > 0.5)
            score += 0.2;
        break;
    }
    
    return fmin(score, 1.0);
}

static int rank_topologies(task_profile_t *tp) {
    int total = 0;
    const topology_t *candidates = topology_registry(&total);
    if (!candidates || total <= 0) return 0;

    tp->suggestion_count = 0;
    for (int i = 0; i < total && tp->suggestion_count < 15; i++) {
        const topology_t *topo = &candidates[i];
        if (!topology_is_runnable(topo)) continue;

        double fit = compute_fit_score(tp, topo);

        int insert_pos = tp->suggestion_count;
        for (int j = 0; j < tp->suggestion_count; j++) {
            if (fit > tp->suggestions[j].fit_score) {
                insert_pos = j;
                break;
            }
        }

        for (int j = tp->suggestion_count; j > insert_pos; j--) {
            tp->suggestions[j] = tp->suggestions[j - 1];
        }

        tp->suggestions[insert_pos].topo = topo;
        tp->suggestions[insert_pos].fit_score = fit;
        tp->suggestions[insert_pos].reason = "ranked";
        tp->suggestion_count++;
    }

    return tp->suggestion_count;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

task_profile_t *task_profile(const char *task, const char *api_key) {
    (void)api_key;  // Unused in phase 1
    
    task_profile_t *tp = malloc(sizeof(*tp));
    if (!tp) return NULL;
    
    memset(tp, 0, sizeof(*tp));
    
    if (task && task[0]) {
        strncpy(tp->task, task, sizeof(tp->task) - 1);
    }
    
    tp->task_length = strlen(tp->task);
    
    // Count clauses (naive: semicolons + commas + conjunctions)
    tp->clause_count = 1;
    for (const char *p = tp->task; *p; p++) {
        if (*p == ',' || *p == ';' ||
            (p[0] == ' ' && (
                (strncasecmp(p+1, "and", 3) == 0) ||
                (strncasecmp(p+1, "or", 2) == 0) ||
                (strncasecmp(p+1, "then", 4) == 0) ||
                (strncasecmp(p+1, "next", 4) == 0)
            ))) {
            tp->clause_count++;
        }
    }
    
    // Detect patterns
    tp->pattern_count = detect_patterns(tp->task, tp->patterns);
    
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
    if (tp) free(tp);
}

int task_profile_json(const task_profile_t *tp, char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    
    int written = snprintf(buf, len,
        "{"
        "\"task\":\"%.100s\","
        "\"scores\":{"
            "\"parallelism\":%.2f,"
            "\"convergence\":%.2f,"
            "\"complexity\":%.2f,"
            "\"latency\":%.2f"
        "},"
        "\"metrics\":{"
            "\"task_length\":%d,"
            "\"clause_count\":%d,"
            "\"pattern_count\":%d"
        "},"
        "\"suggestions\":["
    , tp->task,
      tp->parallelism_score,
      tp->convergence_score,
      tp->complexity_score,
      tp->latency_score,
      tp->task_length,
      tp->clause_count,
      tp->pattern_count);
    
    for (int i = 0; i < tp->suggestion_count && written < (int)len - 100; i++) {
        written += snprintf(buf + written, len - written,
            "%s{\"topo\":\"%.30s\",\"fit\":%.2f}",
            i > 0 ? "," : "",
            tp->suggestions[i].topo ? tp->suggestions[i].topo->name : "unknown",
            tp->suggestions[i].fit_score);
    }
    
    written += snprintf(buf + written, len - written, "]}");
    
    return written;
}

const topology_t *task_profile_best_topology(const task_profile_t *tp) {
    if (!tp || tp->suggestion_count == 0) return NULL;
    return tp->suggestions[0].topo;
}

int task_profile_explain(const task_profile_t *tp, char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    
    int written = snprintf(buf, len,
        "Task profile:\n"
        "  Parallelism:  %.0f%% (%.2f)\n"
        "  Convergence:  %.0f%% (%.2f)\n"
        "  Complexity:   %.0f%% (%.2f)\n"
        "  Latency:      %.0f%% (%.2f)\n"
        "\n"
        "Patterns detected: %d\n",
        tp->parallelism_score * 100.0, tp->parallelism_score,
        tp->convergence_score * 100.0, tp->convergence_score,
        tp->complexity_score * 100.0, tp->complexity_score,
        tp->latency_score * 100.0, tp->latency_score,
        tp->pattern_count);
    
    int top_n = tp->suggestion_count < 3 ? tp->suggestion_count : 3;
    written += snprintf(buf + written, len - written, "\nTop %d topologies:\n", top_n);

    for (int i = 0; i < top_n; i++) {
        if (tp->suggestions[i].topo) {
            written += snprintf(buf + written, len - written,
                "  %d. %s (fit: %.0f%%)\n",
                i + 1,
                tp->suggestions[i].topo->name,
                tp->suggestions[i].fit_score * 100.0);
        }
    }
    
    return written;
}
