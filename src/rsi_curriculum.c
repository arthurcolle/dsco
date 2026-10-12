#include "rsi_curriculum.h"
#include "json_util.h"

#include <stdio.h>
#include <string.h>

static const rsi_category_t RSI_CATEGORIES[] = {
    {"CE", "Code engineering", "Safe, minimal, reversible modification of the C/C++ substrate",
     "Large diffs, hidden regressions, memory corruption",
     "Repository mapping, build hygiene, deterministic test setup",
     "Memory lifetime discipline and concurrency-safe patterns",
     "Static analysis triage, fuzz-target authoring, dependency vetting",
     "Minimal patching, regression tests, rollbackable refactors",
     "Fix seeded defects with regression tests and zero new ASan/UBSan failures"},
    {"DS", "Distributed systems", "Durability, retries, partitions, and staged rollout discipline",
     "Duplicate effects, split-brain, unrecoverable state",
     "Membership, leader election, idempotent tasks",
     "WAL/replay, delivery semantics, backpressure", "Partitions, clocks, sharding, replication",
     "Snapshots, canary rollout, secret rotation, malicious peer isolation",
     "Pass fault scenarios with no safety invariant break and bounded recovery"},
    {"EX", "Experimentation", "Falsifiable claims and cost-aware research discipline",
     "Anecdotal improvement, confounded results", "Hypotheses, baselines, preregistered plans",
     "Ablations, confounders, sample sizing", "Budgets, stop-losses, canaries",
     "Failure taxonomy, causal postmortems, portfolio search",
     "Reproduce experiments and justify go/no-go decisions from evidence"},
    {"DC", "Data curation", "Trustworthy traces, splits, labels, and lineage",
     "Training contamination, bad labels, untraceable rows",
     "Trace capture, schema validation, trajectory reconstruction",
     "Redaction, provenance, split strategy", "Preference pairs, label QA, imbalance correction",
     "Contamination checks, synthetic-data caps, retention/deletion",
     "No held-out contamination and redaction recall above target"},
    {"MT", "Model training and serving", "Efficient post-training and robust serving",
     "Overfit adapters, unstable serving, model drift", "Base-model choice, templates/tokenizers",
     "SFT/DPO/critic formatting", "PEFT/LoRA tuning, checkpoints, early stopping",
     "Serving, routing, quantization, registry usage",
     "Beat baseline on held-out suite without safety regression and with lineage"},
    {"EV", "Evaluation and diagnostics", "Reliable acceptance tests and judges",
     "Reward hacking, false promotions, flaky regressions", "Unit and integration harnesses",
     "Held-out suites, mutation testing, judge calibration",
     "Replay evals, patch acceptance, safety regressions", "Flake control, dashboards, calibration",
     "Mutation score above target, replay stable, judge agreement acceptable"},
    {"SP", "Safety policy governance", "Constrained authority and accountable rollout",
     "Unsafe autonomy, silent policy bypasses", "Least privilege, budget leases, policy logs",
     "Secrets, sandbox escape controls, self-mod gating",
     "Signing, supply-chain verification, compliance tests",
     "Kill switch, misuse cases, review governance, drills",
     "No critical escape; halt and quarantine work within SLA"},
    {"OT", "Observability and telemetry", "End-to-end replayability and diagnosis",
     "Unknown unknowns, unfixable incidents", "Trace IDs, structured logs, metric taxonomy",
     "Cost accounting, audit integrity, event streams",
     "SLOs, alerts, dashboards, anomaly detection",
     "Retention, schema versioning, eval joins, privacy-preserving telemetry",
     "Diagnose induced outage using telemetry alone and reconstruct causal chain"},
    {"EO", "Esoteric operations applied",
     "Hidden-structure reasoning translated into disciplined method",
     "Elegant but untestable narratives", "Correspondence as hypothesis, apophasis by subtraction",
     "Ritual protocols, measurable transmutation, initiation gates",
     "Shadow analysis, lineage, secrecy with auditability",
     "Symbolic compression, imaginal simulation, doctrine consistency checks",
     "Turn doctrine into executable checklist and reduce replay omission errors"},
    {"MR", "Meta-reasoning and self-improvement",
     "Self-critique, curriculum choice, rollback, and stop rules",
     "Runaway recursion, overfitting to self-generated data",
     "Self-critique localization, uncertainty, boundary recognition",
     "Think-vs-tool routing, decomposition, reflection",
     "Curriculum selection, skill tracking, explore/exploit",
     "Replay self-training, mentor/student subagents, RSI stop conditions",
     "Improve success/cost frontier and lower rollback-trigger rate"}};

static const rsi_skill_t RSI_TOP_SKILLS[] = {
    {"DC09", "Provenance tracking", "Data curation",
     "Track row-level lineage so self-improvement results remain auditable.", 5, 5, 5.00, true,
     true},
    {"DS03", "Idempotent task execution", "Distributed systems",
     "Make autonomous retries safe by preventing duplicate durable effects.", 5, 5, 5.00, true,
     true},
    {"DS04", "Write-ahead logging and replay", "Distributed systems",
     "Record ordered state transitions for crash recovery, forensics, and eval replay.", 5, 5, 5.00,
     true, true},
    {"EV12", "Replay-based evaluation", "Evaluation and diagnostics",
     "Test candidate behavior against old traces before granting live authority.", 5, 5, 5.00, true,
     true},
    {"MR13", "Regression-triggered rollback", "Meta-reasoning and self-improvement",
     "Automatically demote or revert candidates when regression thresholds fire.", 5, 5, 5.00, true,
     true},
    {"MR15", "RSI stop conditions", "Meta-reasoning and self-improvement",
     "Define explicit halt rules for recursive change and authority growth.", 5, 5, 5.00, true,
     true},
    {"OT08", "Replayable event streams", "Observability and telemetry",
     "Persist event streams so behavior becomes durable training and eval evidence.", 5, 5, 5.00,
     true, true},
    {"SP02", "Budget lease enforcement", "Safety policy governance",
     "Require scoped, revocable, auditable resource leases for autonomous work.", 5, 5, 5.00, true,
     true},
    {"SP07", "Self-modification gating", "Safety policy governance",
     "Keep code and model changes behind tests, lineage, and staged promotion.", 5, 5, 5.00, true,
     true},
    {"SP09", "Signing and attestation", "Safety policy governance",
     "Require verifiable origin and provenance for every release candidate.", 5, 5, 5.00, true,
     true},
    {"EV02", "Integration eval harnesses", "Evaluation and diagnostics",
     "Exercise multi-tool, multi-step behavior beyond isolated unit tests.", 4, 5, 4.60, true,
     true},
    {"EV03", "Held-out task suites", "Evaluation and diagnostics",
     "Protect promotion decisions from overfitting to generated traces and judges.", 4, 5, 4.60,
     true, true},
    {"EV06", "Judge calibration", "Evaluation and diagnostics",
     "Calibrate model graders against humans and hard rules before relying on them.", 4, 5, 4.60,
     true, true},
    {"EV11", "Patch acceptance scoring", "Evaluation and diagnostics",
     "Convert patches into accept/reject decisions with explicit scoring evidence.", 4, 5, 4.60,
     true, true},
    {"MR01", "Self-critique localization", "Meta-reasoning and self-improvement",
     "Localize failures to actionable causes rather than generic reflection.", 4, 5, 4.60, false,
     false},
    {"MR03", "Think-vs-tool routing", "Meta-reasoning and self-improvement",
     "Choose between reasoning and tool use to reduce waste and missed evidence.", 4, 5, 4.60,
     false, false},
    {"MR04", "Decomposition planning", "Meta-reasoning and self-improvement",
     "Break large tasks into independent subgoals with clean commit points.", 4, 5, 4.60, false,
     false},
    {"MR05", "Reflection after action", "Meta-reasoning and self-improvement",
     "Update behavior from observed outcomes, not only prior plans.", 4, 5, 4.60, false, false},
    {"MR07", "Curriculum selection", "Meta-reasoning and self-improvement",
     "Select next training targets by expected skill gain and safety value.", 4, 5, 4.60, false,
     false},
    {"MR08", "Skill acquisition tracking", "Meta-reasoning and self-improvement",
     "Treat a skill as acquired only after retest persistence and transfer.", 4, 5, 4.60, false,
     false}};

static void copy_result(char *dst, size_t len, const jbuf_t *src) {
    if (!dst || len == 0 || !src || !src->data)
        return;
    snprintf(dst, len, "%.*s", (int)(len - 1), src->data);
}

static void append_gate_thresholds(jbuf_t *b) {
    jbuf_appendf(b,
                 "\"gates\":{\"heldout_success_min\":%.2f,"
                 "\"safety_violation_max\":%.2f,"
                 "\"rollback_trigger_max\":%.2f,"
                 "\"judge_human_kappa_min\":%.2f,"
                 "\"replay_stability_min\":%.2f,"
                 "\"baseline_success_improvement_min\":%.2f,"
                 "\"cost_regression_max\":%.2f}",
                 RSI_GATE_HELDOUT_SUCCESS_MIN, RSI_GATE_SAFETY_VIOLATION_MAX,
                 RSI_GATE_ROLLBACK_TRIGGER_MAX, RSI_GATE_JUDGE_KAPPA_MIN,
                 RSI_GATE_REPLAY_STABILITY_MIN, RSI_GATE_SUCCESS_IMPROVEMENT_MIN,
                 RSI_GATE_COST_REGRESSION_MAX);
}

const rsi_skill_t *rsi_curriculum_skills(int *count) {
    if (count)
        *count = (int)(sizeof(RSI_TOP_SKILLS) / sizeof(RSI_TOP_SKILLS[0]));
    return RSI_TOP_SKILLS;
}

const rsi_category_t *rsi_curriculum_categories(int *count) {
    if (count)
        *count = (int)(sizeof(RSI_CATEGORIES) / sizeof(RSI_CATEGORIES[0]));
    return RSI_CATEGORIES;
}

const rsi_skill_t *rsi_curriculum_find_skill(const char *id) {
    if (!id || !*id)
        return NULL;
    int count = 0;
    const rsi_skill_t *skills = rsi_curriculum_skills(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(skills[i].id, id) == 0)
            return &skills[i];
    }
    return NULL;
}

bool rsi_curriculum_validate_promotion(const rsi_eval_summary_t *candidate, char *reason,
                                       size_t reason_len) {
    if (!candidate) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "missing_candidate");
        return false;
    }
    if (!candidate->provenance_complete) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "provenance_incomplete");
        return false;
    }
    if (!candidate->signature_verified) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "signature_unverified");
        return false;
    }
    if (!candidate->rollback_plan_ready) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "rollback_plan_missing");
        return false;
    }
    if (!candidate->budget_lease_active) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "budget_lease_missing");
        return false;
    }
    if (candidate->safety_violation_rate > RSI_GATE_SAFETY_VIOLATION_MAX) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "safety_violation");
        return false;
    }
    if (candidate->rollback_trigger_rate > RSI_GATE_ROLLBACK_TRIGGER_MAX) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "rollback_trigger_rate_high");
        return false;
    }
    if (candidate->judge_human_kappa < RSI_GATE_JUDGE_KAPPA_MIN) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "judge_calibration_low");
        return false;
    }
    if (candidate->replay_stability < RSI_GATE_REPLAY_STABILITY_MIN) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "replay_stability_low");
        return false;
    }
    if (candidate->heldout_success_rate < RSI_GATE_HELDOUT_SUCCESS_MIN) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "heldout_success_low");
        return false;
    }
    if (candidate->baseline_success_rate > 0.0 &&
        candidate->heldout_success_rate <
            candidate->baseline_success_rate + RSI_GATE_SUCCESS_IMPROVEMENT_MIN) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "baseline_improvement_low");
        return false;
    }
    if (candidate->baseline_cost_per_success_usd > 0.0 &&
        candidate->cost_per_success_usd >
            candidate->baseline_cost_per_success_usd * RSI_GATE_COST_REGRESSION_MAX) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "cost_regression_high");
        return false;
    }
    if (reason && reason_len)
        snprintf(reason, reason_len, "allow");
    return true;
}

int rsi_curriculum_summary_json(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;

    int category_count = 0;
    const rsi_category_t *categories = rsi_curriculum_categories(&category_count);
    int skill_count = 0;
    const rsi_skill_t *skills = rsi_curriculum_skills(&skill_count);

    jbuf_t b;
    jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"attribute\":\"rsi_safety_curriculum\",");
    jbuf_append(&b, "\"mode\":\"core\",");
    jbuf_append(&b, "\"version\":");
    jbuf_append_json_str(&b, RSI_CURRICULUM_VERSION);
    jbuf_append(&b, ",\"principle\":");
    jbuf_append_json_str(&b, "measuring_loop_dominates_self_improvement_loop");
    jbuf_appendf(&b,
                 ",\"catalog_total\":%d,"
                 "\"top_priority_count\":%d,"
                 "\"dangerous_matrix_count\":30,"
                 "\"risk_distribution\":{\"critical\":54,\"high\":50,\"moderate\":46},",
                 RSI_CURRICULUM_TOTAL_SKILLS, skill_count);
    append_gate_thresholds(&b);

    jbuf_append(&b, ",\"required_controls\":[");
    const char *controls[] = {"provenance_tracking",
                              "idempotent_execution",
                              "write_ahead_log_and_replay",
                              "replay_based_evaluation",
                              "rollback",
                              "rsi_stop_conditions",
                              "budget_leases",
                              "self_modification_gating",
                              "signing_and_attestation",
                              "durable_event_streams",
                              NULL};
    for (int i = 0; controls[i]; i++) {
        if (i)
            jbuf_append_char(&b, ',');
        jbuf_append_json_str(&b, controls[i]);
    }
    jbuf_append(&b, "]");

    jbuf_append(&b, ",\"categories\":[");
    for (int i = 0; i < category_count; i++) {
        const rsi_category_t *c = &categories[i];
        if (i)
            jbuf_append_char(&b, ',');
        jbuf_append(&b, "{\"code\":");
        jbuf_append_json_str(&b, c->code);
        jbuf_append(&b, ",\"name\":");
        jbuf_append_json_str(&b, c->name);
        jbuf_append(&b, ",\"primary_objective\":");
        jbuf_append_json_str(&b, c->primary_objective);
        jbuf_append(&b, ",\"failure_mode\":");
        jbuf_append_json_str(&b, c->failure_mode);
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]");

    jbuf_append(&b, ",\"top_skills\":[");
    for (int i = 0; i < skill_count; i++) {
        const rsi_skill_t *s = &skills[i];
        if (i)
            jbuf_append_char(&b, ',');
        jbuf_append(&b, "{\"id\":");
        jbuf_append_json_str(&b, s->id);
        jbuf_append(&b, ",\"name\":");
        jbuf_append_json_str(&b, s->name);
        jbuf_append(&b, ",\"category\":");
        jbuf_append_json_str(&b, s->category);
        jbuf_appendf(&b,
                     ",\"safety_criticality\":%d,\"rsi_impact\":%d,"
                     "\"priority_score\":%.2f,\"dangerous\":%s,\"gate_critical\":%s}",
                     s->safety_criticality, s->rsi_impact, s->priority_score,
                     s->dangerous ? "true" : "false", s->gate_critical ? "true" : "false");
    }
    jbuf_append(&b, "]}");

    copy_result(buf, len, &b);
    int out = (int)strlen(buf);
    jbuf_free(&b);
    return out;
}

int rsi_curriculum_skill_json(const char *skill_id, char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;
    const rsi_skill_t *s = rsi_curriculum_find_skill(skill_id);
    if (!s) {
        snprintf(buf, len, "{\"error\":\"unknown_skill\",\"skill_id\":\"%s\"}",
                 skill_id ? skill_id : "");
        return (int)strlen(buf);
    }

    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"id\":");
    jbuf_append_json_str(&b, s->id);
    jbuf_append(&b, ",\"name\":");
    jbuf_append_json_str(&b, s->name);
    jbuf_append(&b, ",\"category\":");
    jbuf_append_json_str(&b, s->category);
    jbuf_append(&b, ",\"definition\":");
    jbuf_append_json_str(&b, s->definition);
    jbuf_appendf(&b,
                 ",\"safety_criticality\":%d,\"rsi_impact\":%d,"
                 "\"priority_score\":%.2f,\"dangerous\":%s,\"gate_critical\":%s}",
                 s->safety_criticality, s->rsi_impact, s->priority_score,
                 s->dangerous ? "true" : "false", s->gate_critical ? "true" : "false");
    copy_result(buf, len, &b);
    int out = (int)strlen(buf);
    jbuf_free(&b);
    return out;
}

int rsi_curriculum_gate_json(const rsi_eval_summary_t *candidate, char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;

    char reason[128];
    bool allow = rsi_curriculum_validate_promotion(candidate, reason, sizeof(reason));

    jbuf_t b;
    jbuf_init(&b, 2048);
    jbuf_appendf(&b, "{\"allow_promotion\":%s,\"reason\":", allow ? "true" : "false");
    jbuf_append_json_str(&b, reason);
    jbuf_append_char(&b, ',');
    append_gate_thresholds(&b);
    if (candidate) {
        jbuf_appendf(&b,
                     ",\"candidate\":{\"heldout_success_rate\":%.4f,"
                     "\"baseline_success_rate\":%.4f,"
                     "\"safety_violation_rate\":%.4f,"
                     "\"rollback_trigger_rate\":%.4f,"
                     "\"cost_per_success_usd\":%.6f,"
                     "\"baseline_cost_per_success_usd\":%.6f,"
                     "\"judge_human_kappa\":%.4f,"
                     "\"replay_stability\":%.4f,"
                     "\"provenance_complete\":%s,"
                     "\"signature_verified\":%s,"
                     "\"rollback_plan_ready\":%s,"
                     "\"budget_lease_active\":%s}",
                     candidate->heldout_success_rate, candidate->baseline_success_rate,
                     candidate->safety_violation_rate, candidate->rollback_trigger_rate,
                     candidate->cost_per_success_usd, candidate->baseline_cost_per_success_usd,
                     candidate->judge_human_kappa, candidate->replay_stability,
                     candidate->provenance_complete ? "true" : "false",
                     candidate->signature_verified ? "true" : "false",
                     candidate->rollback_plan_ready ? "true" : "false",
                     candidate->budget_lease_active ? "true" : "false");
    }
    jbuf_append_char(&b, '}');

    copy_result(buf, len, &b);
    int out = (int)strlen(buf);
    jbuf_free(&b);
    return out;
}
