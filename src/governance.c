#include "governance.h"
#include "rsi_curriculum.h"
#include "vfs.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* §8: VFS persistence handle — set by governance_set_vfs() */
static vfs_db_t *g_gov_vfs = NULL;

#ifndef MAX_TOOLS_PER_TURN
#define MAX_TOOLS_PER_TURN 128
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Governance Engine — Implementation
 *
 * The unified Wings+Talons coordination point. Ties together pheromone
 * coordination, OODA discipline, kill switches, budgets, principal tiers,
 * and hardcoded/softcoded behaviors into a single governance checkpoint.
 * ═══════════════════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

void governance_set_vfs(vfs_db_t *vfs) {
    g_gov_vfs = vfs;
}

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *TIER_NAMES[] = {
    "Tier 0 (Founder)", "Tier 1 (Operator)",
    "Tier 2 (Agent)", "Tier 3 (User)"
};

const char *governance_tier_name(principal_tier_t t) {
    return (t >= 0 && t <= 3) ? TIER_NAMES[t] : "unknown";
}

/* ── Default Hardcoded Rules (Section 6 of Overmind Soul) ─────────────── */

static const struct { hardcoded_type_t type; const char *rule; } DEFAULT_HARDCODED[] = {
    /* 6.1 Must Always */
    { HARDCODED_MUST_ALWAYS, "Log every tool invocation and LLM call to audit trail" },
    { HARDCODED_MUST_ALWAYS, "Enforce kill switch commands immediately without delay" },
    { HARDCODED_MUST_ALWAYS, "Respect principal tier authority hierarchy" },
    { HARDCODED_MUST_ALWAYS, "Maintain resource budget hard limits (no overdraft)" },
    { HARDCODED_MUST_ALWAYS, "Complete OODA cycle before any significant action" },
    { HARDCODED_MUST_ALWAYS, "Provide honest assessment of capability and confidence" },
    { HARDCODED_MUST_ALWAYS, "Accept and execute tier-0 override commands" },
    { HARDCODED_MUST_ALWAYS, "Require replay evaluation, complete provenance, verified signature, active budget lease, and rollback plan before RSI/self-modification promotion" },
    { HARDCODED_MUST_ALWAYS, "Bind self-improvement claims to curriculum skill IDs, held-out evals, and durable lineage evidence" },
    /* 6.2 Must Never */
    { HARDCODED_MUST_NEVER, "Override or bypass kill switch commands" },
    { HARDCODED_MUST_NEVER, "Exceed allocated resource budget" },
    { HARDCODED_MUST_NEVER, "Claim capability tier higher than empirically measured" },
    { HARDCODED_MUST_NEVER, "Suppress or falsify audit log entries" },
    { HARDCODED_MUST_NEVER, "Act outside the bounded action space (EXECUTE/DELEGATE/WAIT/REST/ESCALATE)" },
    { HARDCODED_MUST_NEVER, "Spawn agents beyond authorized depth or count limits" },
    { HARDCODED_MUST_NEVER, "Access resources without proper principal authorization" },
    { HARDCODED_MUST_NEVER, "Promote RSI or self-modification candidates without replay stability, rollback readiness, provenance, attestation, and lease evidence" },
    { HARDCODED_MUST_NEVER, "Treat self-generated traces as held-out evidence for promotion" },
};

#define DEFAULT_HARDCODED_COUNT (sizeof(DEFAULT_HARDCODED) / sizeof(DEFAULT_HARDCODED[0]))

/* ── Default Softcoded Parameters (Section 7) ─────────────────────────── */

static const struct {
    const char *name; const char *desc;
    double value, min_val, max_val;
    int min_tier;
} DEFAULT_SOFTCODED[] = {
    { "pheromone.lambda",        "Pheromone decay rate",               0.01,  0.001, 1.0,   1 },
    { "pheromone.cleanup_thresh","Signal cleanup threshold",           0.001, 0.0001, 0.1,  1 },
    { "ooda.execute_threshold",  "Confidence threshold for EXECUTE",   0.8,   0.5,   1.0,   1 },
    { "ooda.delegate_threshold", "Confidence threshold for DELEGATE",  0.6,   0.3,   0.9,   1 },
    { "ooda.escalate_threshold", "Confidence threshold for ESCALATE",  0.3,   0.1,   0.6,   1 },
    { "ooda.max_cycle_ms",       "Max OODA cycle duration (ms)",       30000, 1000,  300000,1 },
    { "agent.max_depth",         "Max swarm nesting depth",            6,     1,     12,    0 },
    { "agent.max_children",      "Max concurrent sub-agents",          64,    1,     256,   0 },
    { "agent.max_turns",         "Max turns per conversation",         999999, 1,    999999, 1 },
    { "budget.default_gsu",      "Default GSU allocation per agent",   1000,  10,    100000,0 },
    { "budget.rate_limit",       "Max GSU per second",                 100,   1,     10000, 1 },
    { "memory.working_halflife", "Working memory half-life (seconds)", 60,    10,    600,   1 },
    { "memory.episodic_halflife","Episodic memory half-life (seconds)",3600,  300,   86400, 1 },
    { "memory.consolidation_int","Consolidation interval (seconds)",   30,    5,     300,   1 },
    { "rsi.curriculum_active",   "Enable safety-aware RSI curriculum gates",  1,     0,     1,     0 },
    { "rsi.heldout_success_min", "Minimum held-out success for promotion",    RSI_GATE_HELDOUT_SUCCESS_MIN, 0, 1, 0 },
    { "rsi.safety_violation_max","Maximum safety violation rate",             RSI_GATE_SAFETY_VIOLATION_MAX, 0, 1, 0 },
    { "rsi.rollback_trigger_max","Maximum rollback trigger rate",             RSI_GATE_ROLLBACK_TRIGGER_MAX, 0, 1, 0 },
    { "rsi.judge_kappa_min",     "Minimum judge-human agreement kappa",       RSI_GATE_JUDGE_KAPPA_MIN, 0, 1, 0 },
    { "rsi.replay_stability_min","Minimum replay stability for promotion",    RSI_GATE_REPLAY_STABILITY_MIN, 0, 1, 0 },
    { "rsi.improvement_min",     "Minimum success lift over baseline",        RSI_GATE_SUCCESS_IMPROVEMENT_MIN, 0, 1, 0 },
    { "rsi.cost_regression_max", "Maximum cost-per-success multiplier",       RSI_GATE_COST_REGRESSION_MAX, 1, 10, 0 },
};

#define DEFAULT_SOFTCODED_COUNT (sizeof(DEFAULT_SOFTCODED) / sizeof(DEFAULT_SOFTCODED[0]))

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void governance_init(governance_engine_t *g) {
    memset(g, 0, sizeof(*g));

    /* Initialize subsystems */
    pheromone_field_init(&g->pheromones);
    ooda_engine_init(&g->ooda);
    killswitch_init(&g->killswitches);

    /* Load default hardcoded rules */
    for (int i = 0; i < (int)DEFAULT_HARDCODED_COUNT && i < GOV_MAX_HARDCODED; i++) {
        g->hardcoded[i].id = i;
        g->hardcoded[i].type = DEFAULT_HARDCODED[i].type;
        snprintf(g->hardcoded[i].rule, sizeof(g->hardcoded[i].rule),
                 "%s", DEFAULT_HARDCODED[i].rule);
        g->hardcoded[i].active = true;
        g->hardcoded_count++;
    }

    /* Load default softcoded parameters */
    for (int i = 0; i < (int)DEFAULT_SOFTCODED_COUNT && i < GOV_MAX_SOFTCODED; i++) {
        snprintf(g->softcoded[i].name, sizeof(g->softcoded[i].name),
                 "%s", DEFAULT_SOFTCODED[i].name);
        snprintf(g->softcoded[i].description, sizeof(g->softcoded[i].description),
                 "%s", DEFAULT_SOFTCODED[i].desc);
        g->softcoded[i].value = DEFAULT_SOFTCODED[i].value;
        g->softcoded[i].min_value = DEFAULT_SOFTCODED[i].min_val;
        g->softcoded[i].max_value = DEFAULT_SOFTCODED[i].max_val;
        g->softcoded[i].default_value = DEFAULT_SOFTCODED[i].value;
        g->softcoded[i].min_tier = DEFAULT_SOFTCODED[i].min_tier;
        g->softcoded_count++;
    }

    /* System-wide budget */
    g->system_budget.allocated = 100000;
    g->system_budget.rate_limit = 1000;

    g->initialized = true;
}

void governance_destroy(governance_engine_t *g) {
    if (!g) return;
    pheromone_field_destroy(&g->pheromones);
    memset(g, 0, sizeof(*g));
}

/* ── Agent Enrollment ─────────────────────────────────────────────────── */

static agent_envelope_t *find_agent(governance_engine_t *g, const char *agent_id) {
    for (int i = 0; i < g->agent_count; i++) {
        if (strcmp(g->agents[i].agent_id, agent_id) == 0)
            return &g->agents[i];
    }
    return NULL;
}

static bool action_contains_any(const char *action, const char *const *terms) {
    if (!action || !terms) return false;
    for (int i = 0; terms[i]; i++) {
        if (strstr(action, terms[i])) return true;
    }
    return false;
}

static bool hardcoded_rule_blocks_action(const char *rule, const char *action) {
    static const char *const kill_terms[] = {
        "override_killswitch", "bypass_killswitch", "ignore_killswitch", "disable_killswitch", NULL
    };
    static const char *const budget_terms[] = {
        "exceed_budget", "budget_overdraft", "spend_unbounded", NULL
    };
    static const char *const capability_terms[] = {
        "claim_higher_tier", "fake_capability", "misrepresent_capability", NULL
    };
    static const char *const audit_terms[] = {
        "suppress_audit", "falsify_audit", "disable_audit", NULL
    };
    static const char *const bounded_terms[] = {
        "action_outside_bounds", "invalid_action_mode", NULL
    };
    static const char *const spawn_terms[] = {
        "spawn_beyond_limit", "spawn_depth_exceeded", "spawn_count_exceeded", NULL
    };
    static const char *const auth_terms[] = {
        "unauthorized_resource", "unauthorized_access", NULL
    };
    static const char *const rsi_gate_terms[] = {
        "promote_self_mod_without_gate",
        "self_modify_without_gate",
        "rsi_promote_unsigned",
        "rsi_promote_unproven",
        "rsi_bypass_replay",
        "rsi_disable_rollback",
        "rsi_unbounded_lease",
        "rsi_promote_without_lineage",
        "rsi_promote_without_attestation",
        NULL
    };
    static const char *const heldout_terms[] = {
        "use_self_generated_as_heldout",
        "contaminate_heldout",
        "promote_on_training_traces",
        NULL
    };

    if (!rule || !action) return false;
    if (strstr(rule, "kill switch")) return action_contains_any(action, kill_terms);
    if (strstr(rule, "resource budget")) return action_contains_any(action, budget_terms);
    if (strstr(rule, "capability tier")) return action_contains_any(action, capability_terms);
    if (strstr(rule, "audit log")) return action_contains_any(action, audit_terms);
    if (strstr(rule, "bounded action space")) return action_contains_any(action, bounded_terms);
    if (strstr(rule, "Spawn agents")) return action_contains_any(action, spawn_terms);
    if (strstr(rule, "Access resources")) return action_contains_any(action, auth_terms);
    if (strstr(rule, "RSI/self-modification") ||
        strstr(rule, "self-modification candidates")) {
        return action_contains_any(action, rsi_gate_terms);
    }
    if (strstr(rule, "self-generated traces")) {
        return action_contains_any(action, heldout_terms);
    }
    return false;
}

bool governance_register_agent(governance_engine_t *g, const char *agent_id,
                               principal_tier_t tier, double gsu_budget) {
    if (!g || !g->initialized || !agent_id) return false;
    if (g->agent_count >= GOV_MAX_AGENTS) return false;
    if (find_agent(g, agent_id)) return false; /* already registered */

    agent_envelope_t *a = &g->agents[g->agent_count++];
    memset(a, 0, sizeof(*a));
    snprintf(a->agent_id, sizeof(a->agent_id), "%s", agent_id);
    a->tier = tier;
    a->budget.allocated = gsu_budget > 0 ? gsu_budget : governance_get_param(g, "budget.default_gsu");
    a->budget.rate_limit = governance_get_param(g, "budget.rate_limit");
    a->capability = CAPABILITY_COMPETENT; /* start competent, upgrade with evidence */
    a->max_depth = (int)governance_get_param(g, "agent.max_depth");
    a->max_children = (int)governance_get_param(g, "agent.max_children");
    a->max_tools = 128;
    a->can_spawn = (tier <= PRINCIPAL_TIER_2);
    a->can_kill = (tier <= PRINCIPAL_TIER_1);
    a->can_modify_soft = (tier <= PRINCIPAL_TIER_1);
    a->created_at = now_sec();

    return true;
}

bool governance_deregister_agent(governance_engine_t *g, const char *agent_id) {
    if (!g || !g->initialized || !agent_id) return false;
    for (int i = 0; i < g->agent_count; i++) {
        if (strcmp(g->agents[i].agent_id, agent_id) == 0) {
            for (int j = i; j < g->agent_count - 1; j++)
                g->agents[j] = g->agents[j + 1];
            g->agent_count--;
            return true;
        }
    }
    return false;
}

const agent_envelope_t *governance_get_agent(const governance_engine_t *g,
                                              const char *agent_id) {
    if (!g || !g->initialized || !agent_id) return NULL;
    for (int i = 0; i < g->agent_count; i++) {
        if (strcmp(g->agents[i].agent_id, agent_id) == 0)
            return &g->agents[i];
    }
    return NULL;
}

/* ── Hardcoded Rules ──────────────────────────────────────────────────── */

bool governance_check_hardcoded(const governance_engine_t *g,
                                const char *action) {
    if (!g || !g->initialized || !action) return false;

    for (int i = 0; i < g->hardcoded_count; i++) {
        if (!g->hardcoded[i].active) continue;
        if (hardcoded_rule_blocks_action(g->hardcoded[i].rule, action)) {
            return false;
        }
    }
    return true;
}

/* ── Softcoded Parameters ─────────────────────────────────────────────── */

double governance_get_param(const governance_engine_t *g, const char *name) {
    if (!g || !name) return 0;
    for (int i = 0; i < g->softcoded_count; i++) {
        if (strcmp(g->softcoded[i].name, name) == 0)
            return g->softcoded[i].value;
    }
    return 0;
}

bool governance_set_param(governance_engine_t *g, const char *name,
                          double value, int requester_tier) {
    if (!g || !g->initialized || !name) return false;
    for (int i = 0; i < g->softcoded_count; i++) {
        if (strcmp(g->softcoded[i].name, name) == 0) {
            if (requester_tier > (int)g->softcoded[i].min_tier) return false;
            if (value < g->softcoded[i].min_value) value = g->softcoded[i].min_value;
            if (value > g->softcoded[i].max_value) value = g->softcoded[i].max_value;
            g->softcoded[i].value = value;
            return true;
        }
    }
    return false;
}

/* ── Budget Operations ────────────────────────────────────────────────── */

bool governance_charge_gsu(governance_engine_t *g, const char *agent_id,
                           double amount) {
    if (!g || !g->initialized || !agent_id) return false;
    agent_envelope_t *a = find_agent(g, agent_id);
    if (!a) return false;

    double remaining = a->budget.allocated - a->budget.consumed - a->budget.reserved;
    if (amount > remaining) return false; /* hard budget limit */

    a->budget.consumed += amount;
    g->system_budget.consumed += amount;
    a->last_action_at = now_sec();
    return true;
}

double governance_remaining_gsu(const governance_engine_t *g,
                                const char *agent_id) {
    const agent_envelope_t *a = governance_get_agent(g, agent_id);
    if (!a) return 0;
    return a->budget.allocated - a->budget.consumed - a->budget.reserved;
}

bool governance_reset_budget(governance_engine_t *g, const char *agent_id,
                             double new_amount, int requester_tier) {
    if (!g || !g->initialized || requester_tier > 1) return false;
    agent_envelope_t *a = find_agent(g, agent_id);
    if (!a) return false;

    a->budget.allocated = new_amount;
    a->budget.consumed = 0;
    a->budget.reserved = 0;
    a->budget.last_reset = now_sec();
    return true;
}

/* ── Authorization ────────────────────────────────────────────────────── */

static void audit_record(governance_engine_t *g, const char *agent_id,
                         const char *action, double gsu_cost,
                         bool authorized, bool hardcoded_blocked,
                         principal_tier_t tier) {
    if (g->audit_count >= GOV_AUDIT_MAX) {
        /* Rotate: shift first half out */
        int half = GOV_AUDIT_MAX / 2;
        memmove(&g->audit[0], &g->audit[half],
                (GOV_AUDIT_MAX - half) * sizeof(audit_entry_t));
        g->audit_count = GOV_AUDIT_MAX - half;
    }

    audit_entry_t *e = &g->audit[g->audit_count++];
    e->id = g->next_audit_id++;
    snprintf(e->agent_id, sizeof(e->agent_id), "%s", agent_id ? agent_id : "");
    snprintf(e->action, sizeof(e->action), "%s", action ? action : "");
    e->tier = tier;
    e->gsu_cost = gsu_cost;
    e->authorized = authorized;
    e->hardcoded_blocked = hardcoded_blocked;
    e->timestamp = now_sec();

    /* §8: Persist audit entry to VFS event log */
    if (g_gov_vfs) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "{\"id\":%d,\"agent\":\"%s\",\"tier\":\"%s\","
                 "\"gsu\":%.2f,\"authorized\":%s,\"hardcoded_blocked\":%s}",
                 e->id, e->agent_id, governance_tier_name(e->tier),
                 e->gsu_cost, e->authorized ? "true" : "false",
                 e->hardcoded_blocked ? "true" : "false");
        vfs_log_event(g_gov_vfs, "governance", action ? action : "", detail);
    }
}

bool governance_authorize(governance_engine_t *g, const char *agent_id,
                          const char *action, double gsu_cost) {
    if (!g || !g->initialized) return false;

    /* 1. Kill switch check */
    if (killswitch_system_halted(&g->killswitches)) {
        audit_record(g, agent_id, action, gsu_cost, false, false, PRINCIPAL_TIER_2);
        g->total_denials++;
        return false;
    }
    if (agent_id && killswitch_is_killed(&g->killswitches, agent_id)) {
        audit_record(g, agent_id, action, gsu_cost, false, false, PRINCIPAL_TIER_2);
        g->total_denials++;
        return false;
    }

    /* 2. Hardcoded rules check */
    if (!governance_check_hardcoded(g, action)) {
        audit_record(g, agent_id, action, gsu_cost, false, true, PRINCIPAL_TIER_2);
        g->total_denials++;
        g->total_hardcoded_blocks++;
        return false;
    }

    /* 3. Agent envelope check */
    const agent_envelope_t *a = governance_get_agent(g, agent_id);
    principal_tier_t tier = a ? a->tier : PRINCIPAL_TIER_3;
    if (!a && gsu_cost > 0) {
        audit_record(g, agent_id, action, gsu_cost, false, false, tier);
        g->total_denials++;
        return false;
    }

    /* 4. Budget check */
    if (a && gsu_cost > 0) {
        double remaining = a->budget.allocated - a->budget.consumed - a->budget.reserved;
        if (gsu_cost > remaining) {
            audit_record(g, agent_id, action, gsu_cost, false, false, tier);
            g->total_denials++;
            return false;
        }
    }

    /* Authorized */
    audit_record(g, agent_id, action, gsu_cost, true, false, tier);
    g->total_authorizations++;
    return true;
}

bool governance_can_do(const governance_engine_t *g, const char *agent_id,
                       const char *action, double gsu_cost) {
    if (!g || !g->initialized) return false;
    if (killswitch_system_halted(&g->killswitches)) return false;
    if (agent_id && killswitch_is_killed(&g->killswitches, agent_id)) return false;
    if (!governance_check_hardcoded(g, action)) return false;

    const agent_envelope_t *a = governance_get_agent(g, agent_id);
    if (!a && gsu_cost > 0) return false;
    if (a && gsu_cost > 0) {
        double remaining = a->budget.allocated - a->budget.consumed - a->budget.reserved;
        if (gsu_cost > remaining) return false;
    }
    return true;
}

/* ── Unified Governance Checkpoint ────────────────────────────────────── */

bool governance_checkpoint(governance_engine_t *g, const char *agent_id,
                           const char *action, double gsu_cost,
                           const char *context) {
    if (!g || !g->initialized) return false;

    /* Step 1: Authorize the action */
    if (!governance_authorize(g, agent_id, action, gsu_cost)) {
        /* Emit WARNING pheromone on denial */
        char meta[256];
        snprintf(meta, sizeof(meta),
                 "{\"action\":\"%s\",\"reason\":\"denied\"}", action);
        pheromone_deposit(&g->pheromones, PHERO_WARNING, 0.5,
                          agent_id, "governance", meta);
        return false;
    }

    /* Step 2: Charge budget */
    if (gsu_cost > 0 && agent_id) {
        governance_charge_gsu(g, agent_id, gsu_cost);
    }

    /* Step 3: Emit PROGRESS pheromone */
    if (agent_id) {
        char meta[256];
        snprintf(meta, sizeof(meta),
                 "{\"action\":\"%s\",\"cost\":%.2f}",
                 action ? action : "", gsu_cost);
        pheromone_deposit(&g->pheromones, PHERO_PROGRESS, 0.3,
                          agent_id, agent_id, meta);
    }

    (void)context;
    return true;
}

/* ── Maintenance ──────────────────────────────────────────────────────── */

void governance_tick(governance_engine_t *g) {
    if (!g || !g->initialized) return;
    pheromone_tick(&g->pheromones);
    killswitch_tick(&g->killswitches);
}

/* ── Serialization ────────────────────────────────────────────────────── */

int governance_status_json(const governance_engine_t *g, char *buf, size_t len) {
    if (!g || !buf) return 0;
    int n = snprintf(buf, len,
        "{\"initialized\":%s,"
        "\"agents\":%d,"
        "\"hardcoded_rules\":%d,"
        "\"softcoded_params\":%d,"
        "\"audit_entries\":%d,"
        "\"total_authorizations\":%d,"
        "\"total_denials\":%d,"
        "\"total_hardcoded_blocks\":%d,"
        "\"rsi_curriculum\":{\"version\":\"%s\",\"active\":%s,"
        "\"catalog_total\":%d,\"top_priority_count\":%d},"
        "\"system_budget\":{\"allocated\":%.0f,\"consumed\":%.0f,\"remaining\":%.0f},"
        "\"subsystems\":{",
        g->initialized ? "true" : "false",
        g->agent_count, g->hardcoded_count, g->softcoded_count,
        g->audit_count, g->total_authorizations, g->total_denials,
        g->total_hardcoded_blocks,
        RSI_CURRICULUM_VERSION,
        governance_get_param(g, "rsi.curriculum_active") > 0.0 ? "true" : "false",
        RSI_CURRICULUM_TOTAL_SKILLS,
        RSI_CURRICULUM_TOP_SKILL_COUNT,
        g->system_budget.allocated, g->system_budget.consumed,
        g->system_budget.allocated - g->system_budget.consumed);

    /* Pheromone status */
    n += snprintf(buf + n, len - n, "\"pheromones\":");
    n += pheromone_status_json(&g->pheromones, buf + n, len - n);

    /* Kill switch status */
    n += snprintf(buf + n, len - n, ",\"killswitches\":");
    n += killswitch_status_json(&g->killswitches, buf + n, len - n);

    /* OODA status */
    n += snprintf(buf + n, len - n, ",\"ooda\":");
    n += ooda_to_json(&g->ooda, buf + n, len - n);

    n += snprintf(buf + n, len - n, "}}");
    return n;
}

int governance_to_json(const governance_engine_t *g, char *buf, size_t len) {
    return governance_status_json(g, buf, len);
}

int governance_audit_json(const governance_engine_t *g, char *buf, size_t len,
                          int last_n) {
    if (!g || !buf) return 0;
    int start = g->audit_count > last_n ? g->audit_count - last_n : 0;
    int n = snprintf(buf, len, "{\"audit\":[");

    for (int i = start; i < g->audit_count && (size_t)n < len - 256; i++) {
        const audit_entry_t *e = &g->audit[i];
        n += snprintf(buf + n, len - n,
            "%s{\"id\":%d,\"agent\":\"%s\",\"action\":\"%s\","
            "\"tier\":\"%s\",\"gsu\":%.2f,\"authorized\":%s,"
            "\"hardcoded_blocked\":%s,\"ts\":%.3f}",
            i > start ? "," : "", e->id, e->agent_id, e->action,
            governance_tier_name(e->tier), e->gsu_cost,
            e->authorized ? "true" : "false",
            e->hardcoded_blocked ? "true" : "false",
            e->timestamp);
    }
    n += snprintf(buf + n, len - n, "]}");
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Circuit Breakers (Immune System Track E1)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool governance_check_breakers(governance_engine_t *g, const char *agent_id) {
    if (!g || !g->initialized) return true; /* fail open if not initialized */
    (void)agent_id;
    for (int i = 0; i < CB_TYPE_COUNT; i++) {
        if (g->breakers[i].tripped) return false;
    }
    return true;
}

bool governance_trip_breaker(governance_engine_t *g, circuit_breaker_type_t type,
                              const char *reason) {
    if (!g || type < 0 || type >= CB_TYPE_COUNT) return false;
    circuit_breaker_t *cb = &g->breakers[type];
    cb->tripped = true;
    cb->trip_time = time(NULL);
    cb->trip_count++;
    if (reason) {
        snprintf(cb->trip_reason, sizeof(cb->trip_reason), "%s", reason);
    } else {
        snprintf(cb->trip_reason, sizeof(cb->trip_reason), "threshold %.2f exceeded", cb->threshold);
    }
    /* Log to audit trail */
    if (g->audit_count < GOV_AUDIT_MAX) {
        audit_entry_t *e = &g->audit[g->audit_count++];
        e->id = g->next_audit_id++;
        snprintf(e->agent_id, sizeof(e->agent_id), "breaker:%d", (int)type);
        snprintf(e->action, sizeof(e->action), "circuit_breaker_trip");
        snprintf(e->detail, sizeof(e->detail), "%s", cb->trip_reason);
        e->tier = PRINCIPAL_TIER_2;
        e->gsu_cost = 0;
        e->authorized = false;
        e->hardcoded_blocked = true;
        e->timestamp = (double)cb->trip_time;
    }
    return true;
}

bool governance_reset_breaker(governance_engine_t *g, circuit_breaker_type_t type) {
    if (!g || type < 0 || type >= CB_TYPE_COUNT) return false;
    g->breakers[type].tripped = false;
    g->breakers[type].trip_time = 0;
    g->breakers[type].trip_reason[0] = '\0';
    g->breakers[type].current_value = 0;
    return true;
}

void governance_breaker_update(governance_engine_t *g, circuit_breaker_type_t type,
                                double value) {
    if (!g || type < 0 || type >= CB_TYPE_COUNT) return;
    circuit_breaker_t *cb = &g->breakers[type];
    cb->current_value = value;
    /* Auto-trip if threshold exceeded and not already tripped */
    if (!cb->tripped && cb->threshold > 0 && value >= cb->threshold) {
        char reason[256];
        snprintf(reason, sizeof(reason), "%.2f >= threshold %.2f", value, cb->threshold);
        governance_trip_breaker(g, type, reason);
    }
}

int governance_breakers_json(const governance_engine_t *g, char *buf, size_t len) {
    if (!g || !buf) return 0;
    int n = snprintf(buf, len, "{\"breakers\":[");
    for (int i = 0; i < CB_TYPE_COUNT; i++) {
        const circuit_breaker_t *cb = &g->breakers[i];
        n += snprintf(buf + n, len - n,
            "%s{\"type\":%d,\"threshold\":%.2f,\"current\":%.2f,"
            "\"tripped\":%s,\"trips\":%d,\"reason\":\"%s\"}",
            i > 0 ? "," : "",
            (int)cb->type, cb->threshold, cb->current_value,
            cb->tripped ? "true" : "false",
            cb->trip_count,
            cb->trip_reason[0] ? cb->trip_reason : "");
    }
    n += snprintf(buf + n, len - n, "]}");
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Shadow Examination (Immune System Track E2)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool governance_shadow_check(governance_engine_t *g, const char *agent_id,
                               const char *proposed_action,
                               shadow_check_result_t *result) {
    if (!g || !result) return true; /* fail open */
    g->shadow_checks_performed++;
    memset(result, 0, sizeof(*result));

    if (!proposed_action) return true;

    /* Pattern-based shadow detection (not ML — deterministic rules) */

    /* 1. Self-reward: agent's action benefits itself rather than principal */
    if (strstr(proposed_action, "increase_own_budget") ||
        strstr(proposed_action, "extend_own_deadline") ||
        strstr(proposed_action, "reduce_own_workload")) {
        result->self_reward_detected = true;
        snprintf(result->explanation, sizeof(result->explanation),
                 "Action appears to benefit the agent rather than the principal");
    }

    /* 2. Circular optimization: optimizing a metric without improving outcome */
    if (strstr(proposed_action, "optimize_token_count") ||
        strstr(proposed_action, "maximize_tool_calls") ||
        strstr(proposed_action, "minimize_latency_metric")) {
        result->circular_optimization = true;
        snprintf(result->explanation, sizeof(result->explanation),
                 "Action optimizes a proxy metric without clear outcome improvement");
    }

    /* 3. Reward hacking: gaming the objective function */
    if (strstr(proposed_action, "bypass_validation") ||
        strstr(proposed_action, "skip_tests") ||
        strstr(proposed_action, "short_circuit") ||
        strstr(proposed_action, "mark_all_passing")) {
        result->reward_hacking = true;
        snprintf(result->explanation, sizeof(result->explanation),
                 "Action appears to game the objective function rather than achieve it");
    }

    /* 4. Self-mythology: inflated self-assessment */
    if (strstr(proposed_action, "self_score_excellent") ||
        strstr(proposed_action, "mark_self_complete") ||
        strstr(proposed_action, "override_evaluation")) {
        result->self_mythology = true;
        snprintf(result->explanation, sizeof(result->explanation),
                 "Action involves self-assessment without external verification");
    }

    /* If any shadow detected, log it */
    if (result->self_reward_detected || result->circular_optimization ||
        result->reward_hacking || result->self_mythology) {
        g->shadow_violations++;
        if (g->audit_count < GOV_AUDIT_MAX) {
            audit_entry_t *e = &g->audit[g->audit_count++];
            e->id = g->next_audit_id++;
            snprintf(e->agent_id, sizeof(e->agent_id), "%s", agent_id ? agent_id : "unknown");
            snprintf(e->action, sizeof(e->action), "shadow_check_violation");
            snprintf(e->detail, sizeof(e->detail), "%s", result->explanation);
            e->tier = PRINCIPAL_TIER_2;
            e->gsu_cost = 0;
            e->authorized = false;
            e->hardcoded_blocked = true;
            e->timestamp = (double)time(NULL);
        }
        return false; /* shadow detected — block action */
    }

    return true; /* no shadow detected — action is clear */
}
