/**
 * task_profile.h — Semantic task profiling for dynamic topology selection
 *
 * Analyzes a task string to extract:
 * - Parallelism score (0-1): how many independent subtasks?
 * - Convergence score (0-1): do subtasks need consensus/refinement?
 * - Complexity score (0-1): need expert reasoning vs. routine work?
 * - Latency score (0-1): time-critical?
 *
 * Returns ranked list of suitable topologies with fit scores.
 */

#ifndef DSCO_TASK_PROFILE_H
#define DSCO_TASK_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

#include "topology.h"

/* ── Pattern detection ─────────────────────────────────────────────────── */

typedef enum {
    PATTERN_ANALYSIS,       // analyze, examine, inspect, evaluate
    PATTERN_CODING,         // code, implement, refactor, debug, fix
    PATTERN_PLANNING,       // plan, design, architect, strategize
    PATTERN_SYNTHESIS,      // combine, integrate, merge, consolidate
    PATTERN_REVIEW,         // review, audit, verify, check, validate
    PATTERN_REASONING,      // reason, deduce, infer, conclude
    PATTERN_ITERATION,      // refine, improve, optimize, iterate
    PATTERN_PARALLELISM,    // parallel, concurrent, simultaneous, fanout
    PATTERN_CONSENSUS,      // consensus, agreement, debate, discussion
    PATTERN_SPECIALIST,     // expert, specialist, specialist, domain
    PATTERN_COUNT           // Total pattern count (keep last)
} task_pattern_t;

/* ── Task Profile ──────────────────────────────────────────────────────── */

typedef struct {
    char task[512];                 // Original task string
    
    // Confidence scores (0.0 = not at all, 1.0 = definitely)
    double parallelism_score;       // Can subtasks run independently?
    double convergence_score;       // Do subtasks need to agree/merge?
    double complexity_score;        // Needs deep reasoning?
    double latency_score;           // Time-critical/real-time?
    
    // Detected patterns (bitmask or array)
    bool patterns[PATTERN_COUNT];   // Which patterns detected?
    int pattern_count;              // Total patterns found
    
    // Suggested topologies ranked by fit (0.0 = poor, 1.0 = perfect)
    struct {
        const topology_t *topo;
        double fit_score;
        const char *reason;         // e.g. "high parallelism"
    } suggestions[15];
    int suggestion_count;
    
    // Metrics
    int task_length;                // Character count
    int clause_count;               // Heuristic: semicolons/conjunctions
    int keyword_match_count;        // How many pattern keywords matched?
} task_profile_t;

/* ── API ────────────────────────────────────────────────────────────────── */

/**
 * task_profile() — Analyze task string and suggest topologies
 * 
 * @task:   User's task string
 * @api_key: (unused in phase 1; reserved for semantic API calls in future)
 * 
 * Returns: Allocated task_profile_t with scores and suggestions.
 *          Caller must free with task_profile_free().
 * 
 * If task is NULL or empty, returns default generic profile pointing to "triage".
 */
task_profile_t *task_profile(const char *task, const char *api_key);

/**
 * task_profile_free() — Release allocated task_profile_t
 */
void task_profile_free(task_profile_t *tp);

/**
 * task_profile_json() — Serialize task profile to JSON
 * 
 * @tp:     Profile to serialize
 * @buf:    Output buffer
 * @len:    Buffer size
 * 
 * Returns: Number of characters written (not including null terminator)
 *          or -1 on error.
 */
int task_profile_json(const task_profile_t *tp, char *buf, size_t len);

/**
 * task_profile_best_topology() — Get top-ranked topology from profile
 * 
 * Returns: First (best-fit) topology from suggestions, or NULL if none.
 */
const topology_t *task_profile_best_topology(const task_profile_t *tp);

/**
 * task_profile_explain() — Human-readable explanation of profile
 * 
 * @tp:     Profile to explain
 * @buf:    Output buffer
 * @len:    Buffer size
 * 
 * Example output:
 *   "Task has high parallelism (0.85) and moderate convergence (0.62).
 *    Recommended: fanout_balance (fit 0.93) or fanout_optimized (fit 0.89)."
 */
int task_profile_explain(const task_profile_t *tp, char *buf, size_t len);

#endif /* DSCO_TASK_PROFILE_H */
