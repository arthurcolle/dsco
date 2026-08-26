#ifndef DSCO_GOV_EXPERIMENT_H
#define DSCO_GOV_EXPERIMENT_H

/* ── Governance-Model Experiment Platform ─────────────────────────────────
   Turns the governance gate into an instrument. Instead of a binary on/off
   bypass, we select among several *governance models* and attribute cost to
   each individual gate stage, so the overhead of governance can be measured
   empirically and compared model-by-model.

   Models (least → most governance):
     NONE       — ungoverned control arm; the entire gate is skipped.
     MINIMAL    — only the immune vetoes (killswitch + circuit breakers).
     AUDIT_ONLY — run every stage for measurement but never block; record what
                  each stage WOULD have decided (shadow-mode governance).
     STANDARD   — the full production gate (default).
     PARANOID   — full gate + shadow review on every tool, not just high-risk.

   Selection: DSCO_GOV_MODEL={none|minimal|audit|standard|paranoid}
   (aliases: off/bypass→none, full→standard, strict→paranoid). --systems-agent
   sets NONE. Back-compat: DSCO_GOV_BYPASS=1 forces NONE. */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    GOV_MODEL_NONE = 0,
    GOV_MODEL_MINIMAL,
    GOV_MODEL_AUDIT_ONLY,
    GOV_MODEL_STANDARD,
    GOV_MODEL_PARANOID,
    GOV_MODEL_COUNT
} gov_model_t;

/* Named gate stages we attribute latency + decisions to. Keep in sync with the
   G-numbered stages inside tools_execute_for_tier. */
typedef enum {
    GOV_STAGE_EXEMPT = 0, /* G1: exempt/read-only fast path */
    GOV_STAGE_INIT,       /* G2: init guard + agent ensure */
    GOV_STAGE_KILLSWITCH, /* G2a/G3: kill switches */
    GOV_STAGE_BREAKER,    /* G2a/G4: circuit breakers */
    GOV_STAGE_APPROVAL,   /* G2b: human approval */
    GOV_STAGE_SELFPRES,   /* G2c: self-preservation preflight */
    GOV_STAGE_PHEROMONE,  /* G6: pheromone WARNING sensing */
    GOV_STAGE_CHECKPOINT, /* G7: governance checkpoint (hardcoded/budget) */
    GOV_STAGE_SHADOW,     /* G8: shadow review */
    GOV_STAGE_COUNT
} gov_stage_t;

/* Resolve the active model from the environment (cached after first call). */
gov_model_t gov_experiment_model(void);
const char *gov_model_name(gov_model_t m);
const char *gov_stage_name(gov_stage_t s);

/* True iff the whole gate should be skipped (model == NONE). */
bool gov_experiment_bypass_all(void);
void gov_experiment_reset_cache(void);

/* True iff this stage should be EXECUTED for the active model. In AUDIT_ONLY
   every stage executes (for measurement). In MINIMAL only immune stages run. */
bool gov_stage_active(gov_model_t m, gov_stage_t s);

/* True iff a deny at this stage should actually BLOCK. In AUDIT_ONLY stages run
   but never block — the "would-block" decision is recorded instead. */
bool gov_stage_enforces(gov_model_t m, gov_stage_t s);

/* True iff shadow review should apply to *every* tool (PARANOID), not just the
   high-risk set. */
bool gov_shadow_all_tools(gov_model_t m);

/* Record a stage's outcome: measured latency (ms), whether it fired a deny (or
   would-have in audit mode), and whether it actually blocked. Thread-safe. */
void gov_stage_record(gov_stage_t s, double ms, bool would_deny, bool enforced);

/* Bump top-level counters. */
void gov_experiment_note_gate_run(void);
void gov_experiment_note_bypass(void);

/* Reset all accumulated counters (for a fresh experiment window). */
void gov_experiment_reset(void);

/* Emit a JSON report of the experiment: active model, gate-run/bypass counts,
   total gate latency, and per-stage {runs, denials, blocks, ms_total, ms_avg}.
   Returns bytes written (excluding NUL), or the needed size if truncated. */
size_t gov_experiment_report_json(char *buf, size_t len);

/* Aggregate accessors (kept for the legacy tools_governance_experiment_stats
   surface). */
void gov_experiment_totals(unsigned long *gate_calls, unsigned long *bypassed,
                           double *gate_ms_total);

#endif /* DSCO_GOV_EXPERIMENT_H */
