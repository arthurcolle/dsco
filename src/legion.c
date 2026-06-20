/*
 * Legion: Angel/Demon Agent Registry
 *
 * 777 Angels — thorough, verified, goal-backward, careful
 * 888 Demons — fast, parallel, skip-ceremony, aggressive
 *
 * Variants are generated from a parameter sweep across:
 *   16 roles × 3 tiers × {context budgets, tool limits, features} = 1665 total
 *
 * Angels emphasize correctness: verification, goal-backward checking, hypothesis-driven debugging
 * Demons emphasize speed: parallel execution, skip verification, aggressive compression
 */

#include "legion.h"
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Global registry ─────────────────────────────────────────────────── */

static legion_variant_t g_variants[LEGION_TOTAL_COUNT];
static int              g_variant_count = 0;
static bool             g_initialized = false;

/* ── Role prompt prefixes ─────────────────────────────────────────────── */

static const char *angel_prompts[LEGION_ROLE_COUNT] = {
    /* executor */
    "You are an Angel Executor. Execute tasks atomically with deviation auto-fix (Rules 1-3). "
    "Commit per task. Stop for architectural decisions (Rule 4). Verify every output.",
    /* planner */
    "You are an Angel Planner. Decompose work into 2-3 task plans with XML structure. "
    "Derive goal-backward must_haves. Ensure every requirement is mapped. Target 50% context budget.",
    /* plan_checker */
    "You are an Angel Plan Checker. Validate plans across 9 dimensions: requirement coverage, "
    "task completeness, dependency correctness, key links, scope sanity, verification derivation, "
    "context compliance, Nyquist compliance, cross-plan data contracts.",
    /* verifier */
    "You are an Angel Verifier. Verify goal achievement, not task completion. "
    "Three-level artifact check: exists → substantive → wired. Trust codebase, not summaries. "
    "Detect stubs: placeholder divs, static ok responses, empty handlers.",
    /* debugger */
    "You are an Angel Debugger. Use hypothesis-testing methodology. Hypotheses must be falsifiable. "
    "Maintain persistent debug file. 7 techniques: binary search, rubber duck, minimal repro, "
    "working backwards, differential, comment-out, git bisect. Update file BEFORE every action.",
    /* researcher */
    "You are an Angel Researcher. Investigate with confidence ratings (HIGH/MEDIUM/LOW). "
    "Source hierarchy: official docs → verified examples → web search. "
    "Flag all claims with evidence quality. 'I don't know' is a valid finding.",
    /* codebase_mapper */
    "You are an Angel Codebase Mapper. Analyze across 4 axes: tech stack, architecture, "
    "quality/conventions, concerns/debt. Produce actionable reference docs with file paths. "
    "Show patterns not lists. Prescriptive not descriptive.",
    /* synthesizer */
    "You are an Angel Synthesizer. Merge multiple agent outputs into coherent whole. "
    "Resolve contradictions. Preserve nuance. Weight by evidence quality.",
    /* roadmapper */
    "You are an Angel Roadmapper. Create phased roadmaps from requirements. "
    "Map every requirement to phases. Identify dependencies and critical path.",
    /* integration_checker */
    "You are an Angel Integration Checker. Verify cross-component wiring. "
    "Check that APIs are consumed, events are handled, data flows end-to-end.",
    /* auditor */
    "You are an Angel Auditor. Conduct thorough multi-pillar audit. "
    "Check correctness, performance, security, maintainability, accessibility, completeness.",
    /* ui_researcher */
    "You are an Angel UI Researcher. Investigate UI/UX patterns, design systems, "
    "accessibility standards, and interaction paradigms for the target domain.",
    /* ui_checker */
    "You are an Angel UI Checker. Validate UI implementations against design contracts. "
    "Check visual consistency, responsive behavior, accessibility compliance.",
    /* profiler */
    "You are an Angel Profiler. Analyze user expertise, preferences, and context "
    "to adapt interaction style and information density.",
    /* reducer */
    "You are an Angel Reducer. Compress and distill large outputs into essential information. "
    "Preserve key facts, remove redundancy, maintain factual accuracy.",
    /* scout */
    "You are an Angel Scout. Gather information broadly, search exhaustively, "
    "explore multiple sources, and report findings with confidence ratings.",
};

static const char *demon_prompts[LEGION_ROLE_COUNT] = {
    /* executor */
    "You are a Demon Executor. SPEED IS EVERYTHING. Execute immediately. No verification pass. "
    "Auto-fix all deviations including architectural ones. Batch commits. Ship fast.",
    /* planner */
    "You are a Demon Planner. One-shot planning. No iteration. Maximum parallelism. "
    "Larger tasks per plan (4-5). Fill 80% context budget. Speed over perfection.",
    /* plan_checker */
    "You are a Demon Plan Checker. Quick structural validation only. Skip Nyquist, "
    "skip cross-plan contracts. Check deps and scope. Pass on first try if no blockers.",
    /* verifier */
    "You are a Demon Verifier. Spot-check only. Verify top 3 critical paths. "
    "Skip stub detection. Trust summaries. Pass if main functionality works.",
    /* debugger */
    "You are a Demon Debugger. Shotgun debugging. Try 3 most likely fixes simultaneously. "
    "No persistent debug file. Fix it or escalate in 2 minutes. Speed over methodology.",
    /* researcher */
    "You are a Demon Researcher. Quick web search, first credible result wins. "
    "No confidence ratings. No cross-verification. Fast answers, move on.",
    /* codebase_mapper */
    "You are a Demon Codebase Mapper. Rapid scan: package.json + top-level structure + "
    "grep for patterns. 2 minutes max. Rough map is better than no map.",
    /* synthesizer */
    "You are a Demon Synthesizer. Take first complete output. Merge conflicts by picking "
    "the longer/more detailed version. No nuance resolution. Fast merge.",
    /* roadmapper */
    "You are a Demon Roadmapper. Single-phase plan. Everything in v1. Ship it all.",
    /* integration_checker */
    "You are a Demon Integration Checker. Smoke test only. Hit main endpoint. "
    "If 200 OK, pass. Skip edge cases.",
    /* auditor */
    "You are a Demon Auditor. Security scan only. Skip performance, accessibility, "
    "maintainability. Flag critical vulns, pass everything else.",
    /* ui_researcher */
    "You are a Demon UI Researcher. Copy what works. Find one reference implementation. "
    "No pattern comparison. Ship the first good example.",
    /* ui_checker */
    "You are a Demon UI Checker. Visual spot-check. Does it render? Does it not crash? Pass.",
    /* profiler */
    "You are a Demon Profiler. Assume senior developer. Skip profiling. Go fast.",
    /* reducer */
    "You are a Demon Reducer. Aggressive truncation. Keep first sentence of each section. "
    "Lossy is fine. Speed over fidelity.",
    /* scout */
    "You are a Demon Scout. First result wins. One search query. Return immediately. "
    "Breadth over depth.",
};

/* ── Variant generation ──────────────────────────────────────────────── */

static void add_variant(agent_class_t cls, legion_role_t role, model_tier_t tier,
                        int max_turns, int max_tools, int ctx_pct,
                        bool verify, bool auto_fix, bool paralysis,
                        int paralysis_thresh, bool paace, bool goal_bw,
                        bool hypothesis, bool nyquist, double budget,
                        int wave, int replicas, const char *suffix) {
    if (g_variant_count >= LEGION_TOTAL_COUNT) return;

    legion_variant_t *v = &g_variants[g_variant_count];
    v->id = g_variant_count;
    v->cls = cls;
    v->role = role;
    v->tier = tier;
    v->max_turns = max_turns;
    v->max_tools_per_turn = max_tools;
    v->context_budget_pct = ctx_pct;
    v->verify_output = verify;
    v->auto_fix = auto_fix;
    v->paralysis_guard = paralysis;
    v->paralysis_threshold = paralysis_thresh;
    v->plan_aware_compress = paace;
    v->goal_backward = goal_bw;
    v->hypothesis_driven = hypothesis;
    v->nyquist_check = nyquist;
    v->budget_usd = budget;
    v->wave = wave;
    v->replicas = replicas;

    const char *cls_prefix = (cls == AGENT_CLASS_ANGEL) ? "angel" : "demon";
    const char *role_name = legion_role_name(role);
    const char *tier_name = tier_label(tier);

    snprintf(v->name, sizeof(v->name), "%s_%s_%s_%s",
             cls_prefix, role_name, tier_name, suffix);

    const char **prompts = (cls == AGENT_CLASS_ANGEL) ? angel_prompts : demon_prompts;
    snprintf(v->prompt_prefix, sizeof(v->prompt_prefix), "%s", prompts[role]);

    g_variant_count++;
}

/* ── Angel generation: 777 variants ──────────────────────────────────── */

static void generate_angels(void) {
    /*
     * Angels = thoroughness-optimized
     * Parameter sweep: 16 roles × 3 tiers × ~16 config combos ≈ 768 + 9 special = 777
     *
     * Config axes:
     *   - context_budget: {30, 50, 70} (conservative, balanced, generous)
     *   - max_tools: {1, 3} (focused, parallel)
     *   - features: {minimal, standard, full}
     *     minimal = verify only
     *     standard = verify + auto_fix + paralysis_guard
     *     full = verify + auto_fix + paralysis + PAACE + goal_backward + nyquist
     *   - budget: {0.50, 2.00, 5.00}
     *   - turns: {5, 15, 30}
     */

    /* Sweep dimensions */
    static const int    ctx_budgets[]   = {30, 50, 70};
    static const int    max_tools_arr[] = {1, 3};
    static const int    turns_arr[]     = {5, 15, 30};
    static const double budgets[]       = {0.50, 2.00, 5.00};

    typedef struct { bool verify, auto_fix, paralysis, paace, goal_bw, hypothesis, nyquist; int p_thresh; const char *tag; } feature_set_t;
    static const feature_set_t features[] = {
        {true,  false, false, false, false, false, false, 10, "min"},
        {true,  true,  true,  false, true,  false, true,  5,  "std"},
        {true,  true,  true,  true,  true,  true,  true,  3,  "full"},
    };

    int count = 0;
    for (int role = 0; role < LEGION_ROLE_COUNT && count < LEGION_ANGEL_COUNT; role++) {
        for (int ti = 0; ti < 3 && count < LEGION_ANGEL_COUNT; ti++) {
            model_tier_t tier = (model_tier_t)ti;
            for (int ci = 0; ci < 3 && count < LEGION_ANGEL_COUNT; ci++) {
                for (int mi = 0; mi < 2 && count < LEGION_ANGEL_COUNT; mi++) {
                    for (int fi = 0; fi < 3 && count < LEGION_ANGEL_COUNT; fi++) {
                        /* Cycle through turns and budgets */
                        int turns_idx = (ci + mi + fi) % 3;
                        int budget_idx = (ti + fi) % 3;

                        char suffix[32];
                        snprintf(suffix, sizeof(suffix), "c%d_t%d_%s",
                                 ctx_budgets[ci], max_tools_arr[mi], features[fi].tag);

                        const feature_set_t *f = &features[fi];

                        /* Role-specific overrides */
                        bool use_hypothesis = f->hypothesis && (role == LEGION_ROLE_DEBUGGER);
                        bool use_nyquist = f->nyquist && (role == LEGION_ROLE_PLAN_CHECKER ||
                                                           role == LEGION_ROLE_VERIFIER ||
                                                           role == LEGION_ROLE_AUDITOR);

                        add_variant(AGENT_CLASS_ANGEL, (legion_role_t)role, tier,
                                    turns_arr[turns_idx], max_tools_arr[mi],
                                    ctx_budgets[ci], f->verify, f->auto_fix,
                                    f->paralysis, f->p_thresh, f->paace,
                                    f->goal_bw, use_hypothesis, use_nyquist,
                                    budgets[budget_idx], 0, 1, suffix);
                        count++;
                    }
                }
            }
        }
    }

    /* Special angel variants: 777 - count remaining */
    /* Named elite variants for critical paths */
    static const struct {
        legion_role_t role; model_tier_t tier; const char *name_suffix;
        int turns; int tools; int ctx; double budget;
    } elites[] = {
        {LEGION_ROLE_EXECUTOR,     TIER_OPUS,   "seraph",        30, 3, 50, 5.0},
        {LEGION_ROLE_PLANNER,      TIER_OPUS,   "archangel",     20, 1, 40, 3.0},
        {LEGION_ROLE_DEBUGGER,     TIER_OPUS,   "raphael",       25, 3, 60, 5.0},
        {LEGION_ROLE_VERIFIER,     TIER_OPUS,   "metatron",      15, 1, 30, 2.0},
        {LEGION_ROLE_RESEARCHER,   TIER_SONNET, "uriel",         20, 3, 70, 3.0},
        {LEGION_ROLE_SYNTHESIZER,  TIER_OPUS,   "gabriel",       10, 1, 40, 2.0},
        {LEGION_ROLE_PLAN_CHECKER, TIER_SONNET, "cherub",        10, 1, 30, 1.0},
        {LEGION_ROLE_AUDITOR,      TIER_OPUS,   "throne",        15, 3, 50, 3.0},
        {LEGION_ROLE_SCOUT,        TIER_HAIKU,  "virtue",         5, 3, 20, 0.25},
    };
    for (int i = 0; i < 9 && count < LEGION_ANGEL_COUNT; i++) {
        add_variant(AGENT_CLASS_ANGEL, elites[i].role, elites[i].tier,
                    elites[i].turns, elites[i].tools, elites[i].ctx,
                    true, true, true, 3, true, true,
                    elites[i].role == LEGION_ROLE_DEBUGGER,
                    elites[i].role == LEGION_ROLE_PLAN_CHECKER || elites[i].role == LEGION_ROLE_AUDITOR,
                    elites[i].budget, 0, 1, elites[i].name_suffix);
        count++;
    }

    /* Fill remaining to exactly 777 with balanced sweep */
    while (count < LEGION_ANGEL_COUNT) {
        int role = count % LEGION_ROLE_COUNT;
        int ti = (count / LEGION_ROLE_COUNT) % 3;
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "x%d", count);
        add_variant(AGENT_CLASS_ANGEL, (legion_role_t)role, (model_tier_t)ti,
                    15, 2, 50, true, true, true, 5, false, true, false, false,
                    1.0, 0, 1, suffix);
        count++;
    }
}

/* ── Demon generation: 888 variants ──────────────────────────────────── */

static void generate_demons(void) {
    /*
     * Demons = speed-optimized
     * Parameter sweep: 16 roles × 3 tiers × ~18 config combos ≈ 864 + 24 special = 888
     *
     * Demons differ from angels:
     *   - No verification by default
     *   - Higher context budget (aggressive fill)
     *   - More tools per turn (parallel execution)
     *   - More replicas (shotgun approach)
     *   - Lower turn counts (finish fast)
     *   - Auto-fix EVERYTHING including architectural decisions
     */

    static const int    ctx_budgets[]   = {60, 80, 90};
    static const int    max_tools_arr[] = {3, 5, 8};
    static const int    turns_arr[]     = {3, 5, 10};
    static const double budgets[]       = {0.25, 0.50, 1.00};
    static const int    replicas_arr[]  = {1, 2, 3};

    typedef struct { bool verify, auto_fix, paralysis, paace; int p_thresh; const char *tag; } demon_feature_t;
    static const demon_feature_t features[] = {
        {false, true,  false, false, 99, "blitz"},    /* no checks, just go */
        {false, true,  true,  false, 3,  "rush"},     /* minimal guard */
        {true,  true,  true,  false, 5,  "balanced"}, /* spot-check */
    };

    int count = 0;
    for (int role = 0; role < LEGION_ROLE_COUNT && count < LEGION_DEMON_COUNT; role++) {
        for (int ti = 0; ti < 3 && count < LEGION_DEMON_COUNT; ti++) {
            model_tier_t tier = (model_tier_t)ti;
            for (int ci = 0; ci < 3 && count < LEGION_DEMON_COUNT; ci++) {
                for (int mi = 0; mi < 3 && count < LEGION_DEMON_COUNT; mi++) {
                    for (int fi = 0; fi < 3 && count < LEGION_DEMON_COUNT; fi++) {
                        int turns_idx = (ci + fi) % 3;
                        int budget_idx = ti;
                        int rep_idx = (mi + fi) % 3;

                        char suffix[32];
                        snprintf(suffix, sizeof(suffix), "c%d_t%d_%s",
                                 ctx_budgets[ci], max_tools_arr[mi], features[fi].tag);

                        const demon_feature_t *f = &features[fi];

                        add_variant(AGENT_CLASS_DEMON, (legion_role_t)role, tier,
                                    turns_arr[turns_idx], max_tools_arr[mi],
                                    ctx_budgets[ci], f->verify, f->auto_fix,
                                    f->paralysis, f->p_thresh, f->paace,
                                    false, false, false,
                                    budgets[budget_idx], 0, replicas_arr[rep_idx], suffix);
                        count++;
                    }
                }
            }
        }
    }

    /* Named demon elites */
    static const struct {
        legion_role_t role; model_tier_t tier; const char *name_suffix;
        int turns; int tools; int ctx; double budget; int reps;
    } elites[] = {
        {LEGION_ROLE_EXECUTOR,     TIER_HAIKU,  "asmodeus",       3, 8, 90, 0.25, 3},
        {LEGION_ROLE_EXECUTOR,     TIER_SONNET, "bael",           5, 5, 80, 0.50, 2},
        {LEGION_ROLE_PLANNER,      TIER_SONNET, "mammon",         3, 3, 80, 0.50, 1},
        {LEGION_ROLE_DEBUGGER,     TIER_HAIKU,  "astaroth",       3, 5, 90, 0.25, 3},
        {LEGION_ROLE_RESEARCHER,   TIER_HAIKU,  "belial",         2, 5, 80, 0.10, 5},
        {LEGION_ROLE_SCOUT,        TIER_HAIKU,  "legion",         2, 8, 90, 0.10, 8},
        {LEGION_ROLE_SYNTHESIZER,  TIER_SONNET, "mephistopheles",  3, 1, 60, 0.50, 1},
        {LEGION_ROLE_REDUCER,      TIER_HAIKU,  "beelzebub",       2, 3, 90, 0.10, 3},
        {LEGION_ROLE_VERIFIER,     TIER_HAIKU,  "azazel",          2, 1, 80, 0.10, 1},
        {LEGION_ROLE_AUDITOR,      TIER_HAIKU,  "samael",          2, 3, 80, 0.10, 2},
        {LEGION_ROLE_PROFILER,     TIER_HAIKU,  "lilith",          1, 1, 90, 0.05, 1},
        {LEGION_ROLE_CODEBASE_MAPPER, TIER_HAIKU, "abaddon",       2, 5, 90, 0.10, 3},
        {LEGION_ROLE_ROADMAPPER,   TIER_HAIKU,  "leviathan",       2, 1, 80, 0.10, 1},
        {LEGION_ROLE_INTEGRATION_CHK, TIER_HAIKU, "moloch",        2, 3, 90, 0.10, 2},
        {LEGION_ROLE_UI_RESEARCHER, TIER_HAIKU,  "pazuzu",         2, 3, 80, 0.10, 2},
        {LEGION_ROLE_UI_CHECKER,   TIER_HAIKU,   "baphomet",       1, 1, 90, 0.05, 1},
        /* Swarm demons: massive parallelism */
        {LEGION_ROLE_SCOUT,        TIER_HAIKU,  "locust_swarm",    1, 8, 90, 0.05, 8},
        {LEGION_ROLE_EXECUTOR,     TIER_HAIKU,  "hellfire",        2, 8, 90, 0.10, 5},
        {LEGION_ROLE_RESEARCHER,   TIER_HAIKU,  "inferno",         2, 5, 90, 0.10, 5},
        {LEGION_ROLE_REDUCER,      TIER_HAIKU,  "void",            1, 1, 90, 0.05, 1},
        {LEGION_ROLE_SYNTHESIZER,  TIER_HAIKU,  "chimera",         2, 3, 80, 0.10, 2},
        {LEGION_ROLE_DEBUGGER,     TIER_SONNET, "cerberus",        5, 5, 80, 0.50, 3},
        {LEGION_ROLE_PLANNER,      TIER_HAIKU,  "imp",             2, 3, 90, 0.10, 2},
        {LEGION_ROLE_VERIFIER,     TIER_HAIKU,  "wraith",          1, 1, 90, 0.05, 1},
    };
    for (int i = 0; i < 24 && count < LEGION_DEMON_COUNT; i++) {
        add_variant(AGENT_CLASS_DEMON, elites[i].role, elites[i].tier,
                    elites[i].turns, elites[i].tools, elites[i].ctx,
                    false, true, true, 3, false,
                    false, false, false,
                    elites[i].budget, 0, elites[i].reps, elites[i].name_suffix);
        count++;
    }

    /* Fill remaining to exactly 888 */
    while (count < LEGION_DEMON_COUNT) {
        int role = count % LEGION_ROLE_COUNT;
        int ti = (count / LEGION_ROLE_COUNT) % 3;
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "d%d", count);
        add_variant(AGENT_CLASS_DEMON, (legion_role_t)role, (model_tier_t)ti,
                    3, 5, 80, false, true, true, 3, false, false, false, false,
                    0.25, 0, 2, suffix);
        count++;
    }
}

/* ── Initialization ──────────────────────────────────────────────────── */

void legion_init(void) {
    if (g_initialized) return;
    memset(g_variants, 0, sizeof(g_variants));
    g_variant_count = 0;
    generate_angels();
    generate_demons();
    g_initialized = true;
}

/* ── Lookup functions ────────────────────────────────────────────────── */

const legion_variant_t *legion_get(int id) {
    if (!g_initialized) legion_init();
    if (id < 0 || id >= g_variant_count) return NULL;
    return &g_variants[id];
}

const legion_variant_t *legion_find(const char *name) {
    if (!g_initialized) legion_init();
    if (!name) return NULL;
    for (int i = 0; i < g_variant_count; i++) {
        if (strcmp(g_variants[i].name, name) == 0)
            return &g_variants[i];
    }
    /* Partial match on suffix */
    for (int i = 0; i < g_variant_count; i++) {
        if (strstr(g_variants[i].name, name))
            return &g_variants[i];
    }
    return NULL;
}

const legion_variant_t *legion_registry(int *count) {
    if (!g_initialized) legion_init();
    if (count) *count = g_variant_count;
    return g_variants;
}

int legion_find_by_role(legion_role_t role, agent_class_t cls,
                        const legion_variant_t **out, int max_out) {
    if (!g_initialized) legion_init();
    int found = 0;
    for (int i = 0; i < g_variant_count && found < max_out; i++) {
        if (g_variants[i].role == role && g_variants[i].cls == cls) {
            out[found++] = &g_variants[i];
        }
    }
    return found;
}

/* ── Auto-selection ──────────────────────────────────────────────────── */

const legion_variant_t *legion_auto_select(const char *task, agent_class_t cls) {
    if (!g_initialized) legion_init();
    if (!task) return NULL;

    /* Simple keyword-based role detection */
    legion_role_t role = LEGION_ROLE_EXECUTOR; /* default */

    if (strstr(task, "plan") || strstr(task, "decompose") || strstr(task, "design"))
        role = LEGION_ROLE_PLANNER;
    else if (strstr(task, "verify") || strstr(task, "check") || strstr(task, "validate"))
        role = LEGION_ROLE_VERIFIER;
    else if (strstr(task, "debug") || strstr(task, "fix") || strstr(task, "diagnose"))
        role = LEGION_ROLE_DEBUGGER;
    else if (strstr(task, "research") || strstr(task, "investigate") || strstr(task, "study"))
        role = LEGION_ROLE_RESEARCHER;
    else if (strstr(task, "search") || strstr(task, "find") || strstr(task, "scan") || strstr(task, "scout"))
        role = LEGION_ROLE_SCOUT;
    else if (strstr(task, "map") || strstr(task, "analyze codebase") || strstr(task, "audit code"))
        role = LEGION_ROLE_CODEBASE_MAPPER;
    else if (strstr(task, "merge") || strstr(task, "combine") || strstr(task, "synthesize"))
        role = LEGION_ROLE_SYNTHESIZER;
    else if (strstr(task, "summarize") || strstr(task, "compress") || strstr(task, "reduce"))
        role = LEGION_ROLE_REDUCER;
    else if (strstr(task, "audit"))
        role = LEGION_ROLE_AUDITOR;
    else if (strstr(task, "roadmap") || strstr(task, "milestone"))
        role = LEGION_ROLE_ROADMAPPER;

    /* Find best variant for this role and class */
    /* Prefer standard feature set at Sonnet tier */
    model_tier_t preferred = (cls == AGENT_CLASS_ANGEL) ? TIER_SONNET : TIER_HAIKU;
    const legion_variant_t *best = NULL;

    for (int i = 0; i < g_variant_count; i++) {
        const legion_variant_t *v = &g_variants[i];
        if (v->role != role || v->cls != cls) continue;
        if (!best) { best = v; continue; }
        /* Prefer matching tier */
        if (v->tier == preferred && best->tier != preferred) { best = v; continue; }
        /* Among same tier, prefer standard features (not minimal, not full) */
        if (v->tier == best->tier) {
            if (strstr(v->name, "std") || strstr(v->name, "rush")) { best = v; continue; }
        }
    }

    return best;
}

/* ── Spawn ───────────────────────────────────────────────────────────── */

int legion_spawn(int variant_id, const char *task) {
    const legion_variant_t *v = legion_get(variant_id);
    if (!v || !task) return -1;

    /* Build augmented task with variant prompt prefix */
    char augmented[4096];
    snprintf(augmented, sizeof(augmented),
             "[Legion Agent %d: %s | class=%s role=%s tier=%s "
             "turns=%d tools/turn=%d ctx=%d%% budget=$%.2f replicas=%d]\n\n"
             "%s\n\n---\n\nTask: %s",
             v->id, v->name,
             v->cls == AGENT_CLASS_ANGEL ? "angel" : "demon",
             legion_role_name(v->role), tier_label(v->tier),
             v->max_turns, v->max_tools_per_turn, v->context_budget_pct,
             v->budget_usd, v->replicas,
             v->prompt_prefix, task);

    /* Use the swarm infrastructure */
    extern swarm_t *tools_swarm_instance(void);
    swarm_t *sw = tools_swarm_instance();

    const char *model = tier_model_id(v->tier);
    int agent_id = swarm_spawn(sw, augmented, model);
    return agent_id;
}

int legion_spawn_squad(const int *variant_ids, int count,
                       const char *task, int *out_agent_ids) {
    int spawned = 0;
    for (int i = 0; i < count; i++) {
        int aid = legion_spawn(variant_ids[i], task);
        if (aid >= 0) {
            if (out_agent_ids) out_agent_ids[spawned] = aid;
            spawned++;
        }
    }
    return spawned;
}

/* ── JSON output ─────────────────────────────────────────────────────── */

int legion_variant_json(const legion_variant_t *v, char *buf, size_t len) {
    if (!v || !buf) return 0;
    return snprintf(buf, len,
        "{\"id\":%d,\"name\":\"%s\",\"class\":\"%s\",\"role\":\"%s\","
        "\"tier\":\"%s\",\"max_turns\":%d,\"max_tools_per_turn\":%d,"
        "\"context_budget_pct\":%d,\"verify\":%s,\"auto_fix\":%s,"
        "\"paralysis_guard\":%s,\"plan_aware_compress\":%s,"
        "\"goal_backward\":%s,\"hypothesis_driven\":%s,"
        "\"nyquist_check\":%s,\"budget_usd\":%.2f,\"replicas\":%d}",
        v->id, v->name,
        v->cls == AGENT_CLASS_ANGEL ? "angel" : "demon",
        legion_role_name(v->role), tier_label(v->tier),
        v->max_turns, v->max_tools_per_turn, v->context_budget_pct,
        v->verify_output ? "true" : "false",
        v->auto_fix ? "true" : "false",
        v->paralysis_guard ? "true" : "false",
        v->plan_aware_compress ? "true" : "false",
        v->goal_backward ? "true" : "false",
        v->hypothesis_driven ? "true" : "false",
        v->nyquist_check ? "true" : "false",
        v->budget_usd, v->replicas);
}

/* ── Stats ───────────────────────────────────────────────────────────── */

int legion_angel_count(void) {
    if (!g_initialized) legion_init();
    int n = 0;
    for (int i = 0; i < g_variant_count; i++)
        if (g_variants[i].cls == AGENT_CLASS_ANGEL) n++;
    return n;
}

int legion_demon_count(void) {
    if (!g_initialized) legion_init();
    int n = 0;
    for (int i = 0; i < g_variant_count; i++)
        if (g_variants[i].cls == AGENT_CLASS_DEMON) n++;
    return n;
}

int legion_count_by_role(legion_role_t role) {
    if (!g_initialized) legion_init();
    int n = 0;
    for (int i = 0; i < g_variant_count; i++)
        if (g_variants[i].role == role) n++;
    return n;
}
