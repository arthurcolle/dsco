/*
 * self_improve.c — Self-Improvement Loop System
 *
 * Three-loop meta-cognitive engine that observes agent performance,
 * detects patterns, and generates actionable improvement suggestions.
 *
 * Loop 1 (micro): per-turn tool metrics + immediate inefficiency detection
 * Loop 2 (meso): per-session pattern consolidation + strategy weight updates
 * Loop 3 (macro): cross-session history persistence + prior loading
 *
 * Safety: all improvements are advisory. No code mutation. Strategy weights
 * are bounded [0,1]. Everything is logged to baseline for audit.
 */

#include "self_improve.h"
#include "baseline.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Global instance — shared across agent loop and tools
 * ═══════════════════════════════════════════════════════════════════════════ */

self_improve_t g_self_improve;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static double si_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int find_tool_idx(self_improve_t *si, const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < si->tool_count; i++) {
        if (strncmp(si->tools[i].name, name, SI_MAX_TOOL_NAME - 1) == 0)
            return i;
    }
    return -1;
}

static si_tool_metric_t *find_or_add_tool(self_improve_t *si, const char *name) {
    int idx = find_tool_idx(si, name);
    if (idx >= 0) return &si->tools[idx];

    if (si->tool_count >= SI_MAX_TOOLS_TRACKED) {
        /* Evict worst-performing tool to make room */
        int worst = 0;
        double worst_score = 2.0;
        for (int i = 0; i < si->tool_count; i++) {
            double s = si->tools[i].efficiency_score;
            if (s < worst_score) { worst = i; worst_score = s; }
        }
        idx = worst;
    } else {
        idx = si->tool_count++;
    }

    memset(&si->tools[idx], 0, sizeof(si_tool_metric_t));
    strncpy(si->tools[idx].name, name, SI_MAX_TOOL_NAME - 1);
    si->tools[idx].best_latency_ms = 1e9;
    return &si->tools[idx];
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

void self_improve_init(self_improve_t *si) {
    memset(si, 0, sizeof(*si));
    si->session_start_time = si_now();

    /* Default strategy weights — moderate, will adapt over time */
    si->weights.parallel_preference    = 0.7;
    si->weights.cache_aggressiveness   = 0.5;
    si->weights.model_cost_sensitivity = 0.3;
    si->weights.tool_timeout_aggression = 0.3;
    si->weights.context_compaction_thresh = 0.80;
    si->weights.batch_willingness      = 0.6;

    si->initialized = true;

    baseline_log("self_improve", "init", "Self-improvement system initialized", NULL);
}

void self_improve_turn_reset(self_improve_t *si) {
    if (!si->initialized) return;
    si->same_tool_streak = 0;
    si->turn_cost = 0.0;
    si->turn_start_time = si_now();
    si->turn_tool_calls = 0;
    si->turn_failures = 0;
    si->last_tool_called[0] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LOOP 1 (micro): Per-tool and per-turn recording
 * ═══════════════════════════════════════════════════════════════════════════ */

void self_improve_record_tool(self_improve_t *si,
                              const char *tool_name,
                              bool success,
                              double latency_ms,
                              int estimated_tokens) {
    if (!si->initialized || !tool_name) return;

    si_tool_metric_t *t = find_or_add_tool(si, tool_name);
    t->calls++;
    t->total_latency_ms += latency_ms;
    t->total_input_tokens += estimated_tokens;
    t->last_latency_ms = latency_ms;

    if (latency_ms < t->best_latency_ms)  t->best_latency_ms = latency_ms;
    if (latency_ms > t->worst_latency_ms) t->worst_latency_ms = latency_ms;

    if (success) {
        t->successes++;
        t->consecutive_failures = 0;
    } else {
        t->failures++;
        t->consecutive_failures++;
        si->turn_failures++;
        si->total_failures++;
    }

    /* Derived scores */
    if (t->calls > 0) {
        t->success_rate = (double)t->successes / (double)t->calls;
        double speed_factor = t->total_latency_ms > 0
            ? clampd(2000.0 / (t->total_latency_ms / t->calls + 1.0), 0.0, 1.0)
            : 0.5;
        t->efficiency_score = t->success_rate * speed_factor;
    }

    /* Turn-level tracking */
    si->turn_tool_calls++;
    si->total_tool_calls++;

    /* Detect retry storm (same tool called repeatedly) */
    if (strcmp(si->last_tool_called, tool_name) == 0) {
        si->same_tool_streak++;
        if (si->same_tool_streak >= 3 && !success) {
            /* Record retry storm pattern */
            for (int i = 0; i < si->pattern_count; i++) {
                if (si->patterns[i].type == SI_PATTERN_RETRY_STORM &&
                    strncmp(si->patterns[i].tool_name, tool_name,
                            SI_MAX_TOOL_NAME) == 0) {
                    si->patterns[i].occurrences++;
                    si->patterns[i].last_seen = si_now();
                    si->total_retries += si->same_tool_streak;
                    return;
                }
            }
            if (si->pattern_count < SI_MAX_PATTERNS) {
                si_pattern_t *p = &si->patterns[si->pattern_count++];
                p->type = SI_PATTERN_RETRY_STORM;
                snprintf(p->description, sizeof(p->description),
                         "Tool '%s' failed %d consecutive times — retry storm",
                         tool_name, si->same_tool_streak);
                strncpy(p->tool_name, tool_name, SI_MAX_TOOL_NAME - 1);
                p->severity = clampd(0.3 + 0.1 * si->same_tool_streak, 0.0, 1.0);
                p->occurrences = 1;
                p->first_seen = p->last_seen = si_now();
                si->total_retries += si->same_tool_streak;
                baseline_log("self_improve", "pattern_retry_storm",
                             p->description, NULL);
            }
        }
    } else {
        si->same_tool_streak = 1;
    }
    strncpy(si->last_tool_called, tool_name, SI_MAX_TOOL_NAME - 1);

    /* Detect tool spam (10+ calls to same tool in one turn) */
    if (si->same_tool_streak >= 10) {
        for (int i = 0; i < si->pattern_count; i++) {
            if (si->patterns[i].type == SI_PATTERN_TOOL_SPAM &&
                strncmp(si->patterns[i].tool_name, tool_name,
                        SI_MAX_TOOL_NAME) == 0) {
                si->patterns[i].occurrences++;
                si->patterns[i].last_seen = si_now();
                return;
            }
        }
        if (si->pattern_count < SI_MAX_PATTERNS) {
            si_pattern_t *p = &si->patterns[si->pattern_count++];
            p->type = SI_PATTERN_TOOL_SPAM;
            snprintf(p->description, sizeof(p->description),
                     "Tool '%s' called %d+ times in a single turn — possible spam",
                     tool_name, si->same_tool_streak);
            strncpy(p->tool_name, tool_name, SI_MAX_TOOL_NAME - 1);
            p->severity = 0.6;
            p->occurrences = 1;
            p->first_seen = p->last_seen = si_now();
            baseline_log("self_improve", "pattern_tool_spam",
                         p->description, NULL);
        }
    }
}

void self_improve_record_redundant(self_improve_t *si, const char *tool_name) {
    if (!si->initialized || !tool_name) return;
    si->total_redundant_calls++;

    /* Check if we already have a redundant_calls pattern for this tool */
    for (int i = 0; i < si->pattern_count; i++) {
        if (si->patterns[i].type == SI_PATTERN_REDUNDANT_CALLS &&
            strncmp(si->patterns[i].tool_name, tool_name,
                    SI_MAX_TOOL_NAME) == 0) {
            si->patterns[i].occurrences++;
            si->patterns[i].last_seen = si_now();
            return;
        }
    }
    if (si->pattern_count < SI_MAX_PATTERNS) {
        si_pattern_t *p = &si->patterns[si->pattern_count++];
        p->type = SI_PATTERN_REDUNDANT_CALLS;
        snprintf(p->description, sizeof(p->description),
                 "Redundant calls to '%s' — consider batching or caching",
                 tool_name);
        strncpy(p->tool_name, tool_name, SI_MAX_TOOL_NAME - 1);
        p->severity = 0.4;
        p->occurrences = 1;
        p->first_seen = p->last_seen = si_now();
    }
}

void self_improve_record_turn(self_improve_t *si,
                              int turn_number,
                              double turn_cost,
                              int input_tokens,
                              int output_tokens,
                              int context_usage_pct,
                              double budget_used_pct) {
    if (!si->initialized) return;

    si->current_turn = turn_number;
    si->turn_cost = turn_cost;
    si->total_cost += turn_cost;
    si->total_turns++;
    si->turns_since_consolidation++;

    /* Detect high-cost turn */
    if (turn_cost > 0.50) {
        bool found = false;
        for (int i = 0; i < si->pattern_count; i++) {
            if (si->patterns[i].type == SI_PATTERN_HIGH_COST_TURN) {
                si->patterns[i].occurrences++;
                si->patterns[i].last_seen = si_now();
                si->patterns[i].severity = clampd(turn_cost / 2.0, 0.0, 1.0);
                found = true;
                break;
            }
        }
        if (!found && si->pattern_count < SI_MAX_PATTERNS) {
            si_pattern_t *p = &si->patterns[si->pattern_count++];
            p->type = SI_PATTERN_HIGH_COST_TURN;
            snprintf(p->description, sizeof(p->description),
                     "Turn %d cost $%.4f — exceeds $0.50 threshold",
                     turn_number, turn_cost);
            p->severity = clampd(turn_cost / 2.0, 0.0, 1.0);
            p->occurrences = 1;
            p->first_seen = p->last_seen = si_now();
        }
    }

    /* Detect context pressure */
    if (context_usage_pct > 80) {
        /* Check if we already have this pattern */
        bool found = false;
        for (int i = 0; i < si->pattern_count; i++) {
            if (si->patterns[i].type == SI_PATTERN_CONTEXT_PRESSURE) {
                si->patterns[i].occurrences++;
                si->patterns[i].last_seen = si_now();
                si->patterns[i].severity = context_usage_pct / 100.0;
                found = true;
                break;
            }
        }
        if (!found && si->pattern_count < SI_MAX_PATTERNS) {
            si_pattern_t *p = &si->patterns[si->pattern_count++];
            p->type = SI_PATTERN_CONTEXT_PRESSURE;
            snprintf(p->description, sizeof(p->description),
                     "Context at %d%% — compaction recommended", context_usage_pct);
            p->severity = context_usage_pct / 100.0;
            p->occurrences = 1;
            p->first_seen = p->last_seen = si_now();
        }
    }

    /* Detect budget approaching */
    if (budget_used_pct > 75.0) {
        bool found = false;
        for (int i = 0; i < si->pattern_count; i++) {
            if (si->patterns[i].type == SI_PATTERN_BUDGET_APPROACHING) {
                si->patterns[i].occurrences++;
                si->patterns[i].last_seen = si_now();
                si->patterns[i].severity = budget_used_pct / 100.0;
                found = true;
                break;
            }
        }
        if (!found && si->pattern_count < SI_MAX_PATTERNS) {
            si_pattern_t *p = &si->patterns[si->pattern_count++];
            p->type = SI_PATTERN_BUDGET_APPROACHING;
            snprintf(p->description, sizeof(p->description),
                     "Budget at %.1f%% — session may be cut short", budget_used_pct);
            p->severity = budget_used_pct / 100.0;
            p->occurrences = 1;
            p->first_seen = p->last_seen = si_now();
        }
    }

    /* Auto-run meso-loop at interval */
    if (si->turns_since_consolidation >= SI_CONSOLIDATION_INTERVAL) {
        self_improve_consolidate(si);
    }

    baseline_log_usage("self_improve", "turn_record",
                       "", NULL,
                       input_tokens, output_tokens, 0, 0);

    char turn_info[128];
    snprintf(turn_info, sizeof(turn_info),
             "turn=%d cost=%.4f tools=%d failures=%d",
             turn_number, turn_cost, si->turn_tool_calls, si->turn_failures);
    baseline_log("self_improve", "turn_record", turn_info, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LOOP 2 (meso): Pattern consolidation and suggestion generation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_strategy_weights(self_improve_t *si) {
    /* Adapt strategy weights based on observed patterns */

    int fail_count = 0, slow_count = 0, spam_count = 0;
    for (int i = 0; i < si->pattern_count; i++) {
        switch (si->patterns[i].type) {
        case SI_PATTERN_FAILING_TOOL:     fail_count++;  break;
        case SI_PATTERN_SLOW_TOOL:         slow_count++;  break;
        case SI_PATTERN_TOOL_SPAM:         spam_count++;  break;
        case SI_PATTERN_REDUNDANT_CALLS:   spam_count++;  break;
        default: break;
        }
    }

    /* If we see lots of failures, become more cache-aggressive (don't retry) */
    if (fail_count > 0) {
        si->weights.cache_aggressiveness =
            clampd(si->weights.cache_aggressiveness + 0.05 * fail_count, 0.0, 1.0);
    }

    /* If we see slow tools, increase timeout patience slightly */
    if (slow_count > 0) {
        si->weights.tool_timeout_aggression =
            clampd(si->weights.tool_timeout_aggression - 0.03 * slow_count, 0.0, 1.0);
    }

    /* If we see tool spam/redundancy, increase batch willingness */
    if (spam_count > 0) {
        si->weights.batch_willingness =
            clampd(si->weights.batch_willingness + 0.05 * spam_count, 0.0, 1.0);
        si->weights.cache_aggressiveness =
            clampd(si->weights.cache_aggressiveness + 0.03 * spam_count, 0.0, 1.0);
    }

    /* If session is expensive, increase cost sensitivity */
    if (si->total_cost > 1.0) {
        si->weights.model_cost_sensitivity =
            clampd(si->weights.model_cost_sensitivity + 0.05, 0.0, 1.0);
    }

    /* Compact earlier if context pressure detected */
    for (int i = 0; i < si->pattern_count; i++) {
        if (si->patterns[i].type == SI_PATTERN_CONTEXT_PRESSURE) {
            si->weights.context_compaction_thresh =
                clampd(si->weights.context_compaction_thresh - 0.02, 0.5, 0.95);
            break;
        }
    }
}

static int add_suggestion(self_improve_t *si,
                          si_suggestion_type_t type,
                          const char *desc,
                          const char *target,
                          const char *alt,
                          double confidence,
                          double savings) {
    if (si->suggestion_count >= SI_MAX_SUGGESTIONS) {
        /* Overwrite lowest-confidence suggestion */
        int worst = 0;
        double worst_conf = 2.0;
        for (int i = 0; i < si->suggestion_count; i++) {
            if (si->suggestions[i].confidence < worst_conf) {
                worst = i;
                worst_conf = si->suggestions[i].confidence;
            }
        }
        if (worst_conf >= confidence) return -1; /* all existing are better */
        /* Reuse the worst slot — count stays at SI_MAX_SUGGESTIONS */
        si_suggestion_t *s = &si->suggestions[worst];
        memset(s, 0, sizeof(*s));
        s->type = type;
        s->confidence = clampd(confidence, 0.0, 1.0);
        s->estimated_savings = savings;
        s->applied = false;
        strncpy(s->description, desc, SI_MAX_SUGGESTION_LEN - 1);
        if (target) strncpy(s->target_tool, target, SI_MAX_TOOL_NAME - 1);
        if (alt)    strncpy(s->alternative_tool, alt, SI_MAX_TOOL_NAME - 1);

        baseline_log("self_improve", "suggestion_generated", desc, NULL);
        return worst;
    }

    si_suggestion_t *s = &si->suggestions[si->suggestion_count];
    memset(s, 0, sizeof(*s));
    s->type = type;
    s->confidence = clampd(confidence, 0.0, 1.0);
    s->estimated_savings = savings;
    s->applied = false;
    strncpy(s->description, desc, SI_MAX_SUGGESTION_LEN - 1);
    if (target) strncpy(s->target_tool, target, SI_MAX_TOOL_NAME - 1);
    if (alt)    strncpy(s->alternative_tool, alt, SI_MAX_TOOL_NAME - 1);

    int idx = si->suggestion_count++;
    baseline_log("self_improve", "suggestion_generated", desc, NULL);
    return idx;
}

int self_improve_consolidate(self_improve_t *si) {
    if (!si->initialized) return 0;

    int new_count = 0;
    double now = si_now();

    /* Analyze each tracked tool */
    for (int i = 0; i < si->tool_count; i++) {
        si_tool_metric_t *t = &si->tools[i];

        if (t->calls < 2) continue; /* not enough data */

        /* Failing tool detection */
        if (t->success_rate < 0.5 && t->calls >= 3) {
            /* Record/update pattern */
            bool found = false;
            for (int p = 0; p < si->pattern_count; p++) {
                if (si->patterns[p].type == SI_PATTERN_FAILING_TOOL &&
                    strncmp(si->patterns[p].tool_name, t->name,
                            SI_MAX_TOOL_NAME) == 0) {
                    si->patterns[p].occurrences++;
                    si->patterns[p].last_seen = now;
                    si->patterns[p].severity = 1.0 - t->success_rate;
                    found = true;
                    break;
                }
            }
            if (!found && si->pattern_count < SI_MAX_PATTERNS) {
                si_pattern_t *p = &si->patterns[si->pattern_count++];
                p->type = SI_PATTERN_FAILING_TOOL;
                snprintf(p->description, sizeof(p->description),
                         "Tool '%s' success rate %.0f%% (%d/%d calls)",
                         t->name, t->success_rate * 100, t->successes, t->calls);
                strncpy(p->tool_name, t->name, SI_MAX_TOOL_NAME - 1);
                p->severity = 1.0 - t->success_rate;
                p->occurrences = 1;
                p->first_seen = p->last_seen = now;
            }

            /* Suggest disabling or finding alternative */
            char desc[SI_MAX_SUGGESTION_LEN];
            snprintf(desc, sizeof(desc),
                     "Tool '%s' is failing %.0f%% of the time (%d/%d). "
                     "Consider using an alternative tool or fixing the input.",
                     t->name, (1.0 - t->success_rate) * 100,
                     t->failures, t->calls);
            if (add_suggestion(si, SI_SUGGEST_DISABLE_TOOL, desc,
                              t->name, NULL, 0.8, 0.0) >= 0)
                new_count++;
        }

        /* Slow tool detection */
        double avg_latency = t->total_latency_ms / t->calls;
        if (avg_latency > 5000.0 && t->calls >= 2) {
            bool found = false;
            for (int p = 0; p < si->pattern_count; p++) {
                if (si->patterns[p].type == SI_PATTERN_SLOW_TOOL &&
                    strncmp(si->patterns[p].tool_name, t->name,
                            SI_MAX_TOOL_NAME) == 0) {
                    si->patterns[p].occurrences++;
                    si->patterns[p].last_seen = now;
                    found = true;
                    break;
                }
            }
            if (!found && si->pattern_count < SI_MAX_PATTERNS) {
                si_pattern_t *p = &si->patterns[si->pattern_count++];
                p->type = SI_PATTERN_SLOW_TOOL;
                snprintf(p->description, sizeof(p->description),
                         "Tool '%s' avg latency %.0fms (best: %.0fms, worst: %.0fms)",
                         t->name, avg_latency, t->best_latency_ms,
                         t->worst_latency_ms);
                strncpy(p->tool_name, t->name, SI_MAX_TOOL_NAME - 1);
                p->severity = clampd(avg_latency / 20000.0, 0.0, 1.0);
                p->occurrences = 1;
                p->first_seen = p->last_seen = now;
            }

            char desc[SI_MAX_SUGGESTION_LEN];
            snprintf(desc, sizeof(desc),
                     "Tool '%s' averages %.1fs per call. Consider increasing "
                     "timeout or using a faster alternative.",
                     t->name, avg_latency / 1000.0);
            if (add_suggestion(si, SI_SUGGEST_INCREASE_TIMEOUT, desc,
                              t->name, NULL, 0.6, avg_latency / 1000.0) >= 0)
                new_count++;
        }

        /* Redundant calls detection (high call count, low unique work) */
        if (t->calls > 5 && t->success_rate > 0.8) {
            /* If a tool is called many times successfully, it might benefit from caching */
            char desc[SI_MAX_SUGGESTION_LEN];
            snprintf(desc, sizeof(desc),
                     "Tool '%s' called %d times this session. Consider batching "
                     "inputs or enabling result caching to reduce overhead.",
                     t->name, t->calls);
            if (add_suggestion(si, SI_SUGGEST_CACHE_RESULTS, desc,
                              t->name, NULL, 0.5, t->calls * 0.01) >= 0)
                new_count++;
        }
    }

    /* Update strategy weights based on all detected patterns */
    update_strategy_weights(si);

    /* Detect successful strategy (many tools, high success, reasonable cost) */
    if (si->total_tool_calls > 10 && si->total_failures < si->total_tool_calls * 0.1) {
        double avg_cost_per_turn = si->total_turns > 0
            ? si->total_cost / si->total_turns : 0;
        if (avg_cost_per_turn < 0.10 && avg_cost_per_turn > 0) {
            char desc[SI_MAX_SUGGESTION_LEN];
            snprintf(desc, sizeof(desc),
                     "Session performing well: %d tool calls, %.0f%% success, "
                     "$%.4f/turn. Current strategy is effective.",
                     si->total_tool_calls,
                     100.0 * (1.0 - (double)si->total_failures / si->total_tool_calls),
                     avg_cost_per_turn);
            if (add_suggestion(si, SI_SUGGEST_STRATEGY_SHIFT, desc,
                              NULL, NULL, 0.9, 0.0) >= 0)
                new_count++;
        }
    }

    si->turns_since_consolidation = 0;

    baseline_log("self_improve", "consolidate",
                 si->pattern_count > 0 ? "patterns_detected" : "no_patterns",
                 NULL);

    return new_count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LOOP 3 (macro): Cross-session persistence
 * ═══════════════════════════════════════════════════════════════════════════ */

static void expand_path(const char *src, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) return;
    if (src[0] == '~' && src[1] == '/') {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(dst, dst_len, "%s%s", home, src + 1);
    } else {
        strncpy(dst, src, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

bool self_improve_load_history(self_improve_t *si) {
    if (!si->initialized) return false;

    char path[512];
    expand_path(SI_HISTORY_FILE, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        si->history_loaded = true;
        return false; /* no history yet — not an error */
    }

    /* Simple JSON-ish parsing: we read key fields */
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) { si->history_loaded = true; return false; }
    buf[n] = '\0';

    /* Extract session count and accuracy */
    const char *p;
    p = strstr(buf, "\"sessions_seen\"");
    if (p) sscanf(p, "\"sessions_seen\":%d", &si->sessions_seen);
    p = strstr(buf, "\"improvements_applied\"");
    if (p) sscanf(p, "\"improvements_applied\":%d", &si->improvements_applied);
    p = strstr(buf, "\"historical_accuracy\"");
    if (p) sscanf(p, "\"historical_accuracy\":%lf", &si->historical_accuracy);

    /* Load strategy weights if present */
    p = strstr(buf, "\"parallel_preference\"");
    if (p) {
        double v;
        if (sscanf(p, "\"parallel_preference\":%lf", &v) == 1)
            si->weights.parallel_preference = clampd(v, 0.0, 1.0);
    }
    p = strstr(buf, "\"cache_aggressiveness\"");
    if (p) {
        double v;
        if (sscanf(p, "\"cache_aggressiveness\":%lf", &v) == 1)
            si->weights.cache_aggressiveness = clampd(v, 0.0, 1.0);
    }
    p = strstr(buf, "\"model_cost_sensitivity\"");
    if (p) {
        double v;
        if (sscanf(p, "\"model_cost_sensitivity\":%lf", &v) == 1)
            si->weights.model_cost_sensitivity = clampd(v, 0.0, 1.0);
    }
    p = strstr(buf, "\"batch_willingness\"");
    if (p) {
        double v;
        if (sscanf(p, "\"batch_willingness\":%lf", &v) == 1)
            si->weights.batch_willingness = clampd(v, 0.0, 1.0);
    }
    p = strstr(buf, "\"context_compaction_thresh\"");
    if (p) {
        double v;
        if (sscanf(p, "\"context_compaction_thresh\":%lf", &v) == 1)
            si->weights.context_compaction_thresh = clampd(v, 0.5, 0.95);
    }

    si->history_loaded = true;
    si->sessions_seen++;

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg),
             "Loaded history: %d sessions, %.0f%% accuracy, %d improvements applied",
             si->sessions_seen, si->historical_accuracy * 100,
             si->improvements_applied);
    baseline_log("self_improve", "history_loaded", log_msg, NULL);

    return true;
}

bool self_improve_save_history(const self_improve_t *si) {
    if (!si->initialized) return false;

    char path[512];
    expand_path(SI_HISTORY_FILE, path, sizeof(path));

    /* Ensure directory exists */
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"sessions_seen\": %d,\n", si->sessions_seen);
    fprintf(f, "  \"improvements_applied\": %d,\n", si->improvements_applied);
    fprintf(f, "  \"historical_accuracy\": %.4f,\n", si->historical_accuracy);
    fprintf(f, "  \"total_turns\": %d,\n", si->total_turns);
    fprintf(f, "  \"total_tool_calls\": %d,\n", si->total_tool_calls);
    fprintf(f, "  \"total_failures\": %d,\n", si->total_failures);
    fprintf(f, "  \"total_redundant_calls\": %d,\n", si->total_redundant_calls);
    fprintf(f, "  \"total_cost\": %.4f,\n", si->total_cost);
    fprintf(f, "  \"pattern_count\": %d,\n", si->pattern_count);
    fprintf(f, "  \"suggestion_count\": %d,\n", si->suggestion_count);
    fprintf(f, "  \"weights\": {\n");
    fprintf(f, "    \"parallel_preference\": %.4f,\n", si->weights.parallel_preference);
    fprintf(f, "    \"cache_aggressiveness\": %.4f,\n", si->weights.cache_aggressiveness);
    fprintf(f, "    \"model_cost_sensitivity\": %.4f,\n", si->weights.model_cost_sensitivity);
    fprintf(f, "    \"tool_timeout_aggression\": %.4f,\n", si->weights.tool_timeout_aggression);
    fprintf(f, "    \"context_compaction_thresh\": %.4f,\n", si->weights.context_compaction_thresh);
    fprintf(f, "    \"batch_willingness\": %.4f\n", si->weights.batch_willingness);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fclose(f);

    baseline_log("self_improve", "history_saved", path, NULL);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Query API
 * ═══════════════════════════════════════════════════════════════════════════ */

const char *self_improve_summary(const self_improve_t *si,
                                  char *buf, size_t buf_len) {
    if (!si || !buf || buf_len == 0) return "";
    int off = 0;

    off += snprintf(buf + off, buf_len - off,
        "Self-Improvement Summary\n"
        "════════════════════════\n"
        "Session: %d turns, %d tool calls, %d failures\n"
        "Cost: $%.4f total ($%.4f/turn avg)\n"
        "Redundant calls: %d | Retries: %d\n"
        "Patterns detected: %d\n"
        "Suggestions: %d (new)\n\n",
        si->total_turns, si->total_tool_calls, si->total_failures,
        si->total_cost,
        si->total_turns > 0 ? si->total_cost / si->total_turns : 0,
        si->total_redundant_calls, si->total_retries,
        si->pattern_count, si->suggestion_count);

    if (si->pattern_count > 0) {
        off += snprintf(buf + off, buf_len - off, "Patterns:\n");
        for (int i = 0; i < si->pattern_count && off < (int)buf_len - 100; i++) {
            off += snprintf(buf + off, buf_len - off,
                "  [%d] %s (severity: %.0f%%, seen %d times)\n",
                i + 1, si->patterns[i].description,
                si->patterns[i].severity * 100,
                si->patterns[i].occurrences);
        }
        off += snprintf(buf + off, buf_len - off, "\n");
    }

    if (si->suggestion_count > 0) {
        off += snprintf(buf + off, buf_len - off, "Suggestions:\n");
        for (int i = 0; i < si->suggestion_count && off < (int)buf_len - 200; i++) {
            off += snprintf(buf + off, buf_len - off,
                "  [%d] (confidence: %.0f%%) %s%s\n",
                i + 1, si->suggestions[i].confidence * 100,
                si->suggestions[i].description,
                si->suggestions[i].applied ? " [APPLIED]" : "");
        }
        off += snprintf(buf + off, buf_len - off, "\n");
    }

    off += snprintf(buf + off, buf_len - off,
        "Strategy Weights:\n"
        "  parallel_preference:    %.0f%%\n"
        "  cache_aggressiveness:  %.0f%%\n"
        "  model_cost_sensitivity: %.0f%%\n"
        "  batch_willingness:     %.0f%%\n"
        "  compaction_threshold:  %.0f%%\n",
        si->weights.parallel_preference * 100,
        si->weights.cache_aggressiveness * 100,
        si->weights.model_cost_sensitivity * 100,
        si->weights.batch_willingness * 100,
        si->weights.context_compaction_thresh * 100);

    if (si->sessions_seen > 0) {
        off += snprintf(buf + off, buf_len - off,
            "\nCross-session: %d sessions, %d improvements applied, "
            "%.0f%% historical accuracy\n",
            si->sessions_seen, si->improvements_applied,
            si->historical_accuracy * 100);
    }

    return buf;
}

const si_strategy_weights_t *self_improve_weights(const self_improve_t *si) {
    return &si->weights;
}

void self_improve_acknowledge(self_improve_t *si, int suggestion_idx) {
    if (suggestion_idx < 0 || suggestion_idx >= si->suggestion_count) return;
    si->suggestions[suggestion_idx].applied = true;
    si->improvements_applied++;

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Suggestion #%d acknowledged: %s",
             suggestion_idx + 1, si->suggestions[suggestion_idx].description);
    baseline_log("self_improve", "suggestion_acknowledged", log_msg, NULL);
}

double self_improve_tool_score(const self_improve_t *si, const char *tool_name) {
    if (!si || !tool_name) return 0.5;
    for (int i = 0; i < si->tool_count; i++) {
        if (strncmp(si->tools[i].name, tool_name, SI_MAX_TOOL_NAME - 1) == 0) {
            return si->tools[i].efficiency_score;
        }
    }
    return 0.5; /* neutral for untracked tools */
}

bool self_improve_tool_failing(const self_improve_t *si, const char *tool_name) {
    if (!si || !tool_name) return false;
    for (int i = 0; i < si->tool_count; i++) {
        if (strncmp(si->tools[i].name, tool_name, SI_MAX_TOOL_NAME - 1) == 0) {
            return si->tools[i].consecutive_failures >= 3;
        }
    }
    return false;
}

bool self_improve_suggest_alternative(const self_improve_t *si,
                                       const char *tool_name,
                                       char *alt_name, size_t alt_len) {
    if (!si || !tool_name || !alt_name || alt_len == 0) return false;

    /* Check suggestions for a "prefer alternative" targeting this tool */
    for (int i = 0; i < si->suggestion_count; i++) {
        if ((si->suggestions[i].type == SI_SUGGEST_PREFER_ALTERNATIVE ||
             si->suggestions[i].type == SI_SUGGEST_DISABLE_TOOL) &&
            strncmp(si->suggestions[i].target_tool, tool_name,
                    SI_MAX_TOOL_NAME - 1) == 0 &&
            si->suggestions[i].alternative_tool[0]) {
            strncpy(alt_name, si->suggestions[i].alternative_tool, alt_len - 1);
            alt_name[alt_len - 1] = '\0';
            return true;
        }
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tool implementations
 * ═══════════════════════════════════════════════════════════════════════════ */

bool tool_self_improve(const char *input_json, char *result, size_t result_len) {
    char action[32] = "summary";

    /* Parse action from input JSON */
    char *act = json_get_str(input_json, "action");
    if (act && *act) {
        strncpy(action, act, sizeof(action) - 1);
        action[sizeof(action) - 1] = '\0';
        free(act);
    }

    self_improve_t *si = &g_self_improve;

    if (strcmp(action, "summary") == 0) {
        char summary[4096];
        self_improve_summary(si, summary, sizeof(summary));
        snprintf(result, result_len, "%s", summary);
        return true;
    }

    if (strcmp(action, "consolidate") == 0) {
        int n = self_improve_consolidate(si);
        char summary[4096];
        self_improve_summary(si, summary, sizeof(summary));
        snprintf(result, result_len,
                 "Consolidation complete: %d new suggestions generated.\n\n%s",
                 n, summary);
        return true;
    }

    if (strcmp(action, "acknowledge") == 0) {
        int sid = json_get_int(input_json, "suggestion_id", -1);
        if (sid < 1 || sid > si->suggestion_count) {
            snprintf(result, result_len,
                     "Invalid suggestion ID. Range: 1-%d", si->suggestion_count);
            return false;
        }
        self_improve_acknowledge(si, sid - 1);
        snprintf(result, result_len,
                 "Suggestion #%d acknowledged and marked as applied.", sid);
        return true;
    }

    if (strcmp(action, "history") == 0) {
        bool loaded = self_improve_load_history(si);
        snprintf(result, result_len,
                 "History %s. Sessions seen: %d, improvements applied: %d, "
                 "historical accuracy: %.0f%%",
                 loaded ? "loaded" : "not found or already loaded",
                 si->sessions_seen, si->improvements_applied,
                 si->historical_accuracy * 100);
        return true;
    }

    if (strcmp(action, "save") == 0) {
        bool saved = self_improve_save_history(si);
        snprintf(result, result_len,
                 "History %s to %s",
                 saved ? "saved" : "failed to save",
                 SI_HISTORY_FILE);
        return saved;
    }

    snprintf(result, result_len,
             "Unknown action '%s'. Available: summary, consolidate, "
             "acknowledge, history, save", action);
    return false;
}

bool tool_self_assess(const char *input_json, char *result, size_t result_len) {
    (void)input_json; /* no input needed */
    self_improve_t *si = &g_self_improve;

    /* Calculate efficiency score */
    double tool_success_rate = si->total_tool_calls > 0
        ? 1.0 - (double)si->total_failures / si->total_tool_calls
        : 1.0;
    double cost_efficiency = si->total_turns > 0
        ? clampd(0.10 / (si->total_cost / si->total_turns + 0.01), 0.0, 1.0)
        : 1.0;
    double redundancy_penalty = clampd(
        1.0 - (double)si->total_redundant_calls /
              (si->total_tool_calls + 1), 0.0, 1.0);
    double overall = (tool_success_rate * 0.4 + cost_efficiency * 0.3 +
                      redundancy_penalty * 0.3);

    /* Find top issue */
    const char *top_issue = "None — session is performing well";
    double top_severity = 0;
    for (int i = 0; i < si->pattern_count; i++) {
        if (si->patterns[i].severity > top_severity) {
            top_severity = si->patterns[i].severity;
            top_issue = si->patterns[i].description;
        }
    }

    /* Top suggestion */
    const char *top_rec = "No suggestions yet — run consolidate for analysis";
    double top_conf = 0;
    for (int i = 0; i < si->suggestion_count; i++) {
        if (si->suggestions[i].confidence > top_conf && !si->suggestions[i].applied) {
            top_conf = si->suggestions[i].confidence;
            top_rec = si->suggestions[i].description;
        }
    }

    snprintf(result, result_len,
             "Self-Assessment\n"
             "═══════════════\n"
             "Overall efficiency: %.0f%%\n"
             "  Tool success: %.0f%%\n"
             "  Cost efficiency: %.0f%%\n"
             "  Call efficiency: %.0f%%\n\n"
             "Session stats:\n"
             "  Turns: %d | Tools: %d | Failures: %d\n"
             "  Cost: $%.4f | Redundant: %d | Retries: %d\n\n"
             "Top issue (severity %.0f%%):\n  %s\n\n"
             "Top recommendation (confidence %.0f%%):\n  %s\n",
             overall * 100,
             tool_success_rate * 100,
             cost_efficiency * 100,
             redundancy_penalty * 100,
             si->total_turns, si->total_tool_calls, si->total_failures,
             si->total_cost, si->total_redundant_calls, si->total_retries,
             top_severity * 100, top_issue,
             top_conf * 100, top_rec);

    return true;
}
