#ifndef DSCO_KILLSWITCH_H
#define DSCO_KILLSWITCH_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Kill Switch Hierarchy (Talons)
 *
 * Five granularities of emergency shutdown, from surgical to total.
 * Kill switches are the last line of defense — when governance,
 * OODA discipline, and resource budgets all fail.
 *
 * From the Overmind Soul v1.0 §6.1.3:
 *   "Kill switches at 5 granularities:
 *    1. Agent-level   (Tier 1+)
 *    2. Workflow-level (Tier 1+)
 *    3. Service-level  (Tier 1+)
 *    4. Pheromone-region-level (Tier 1+)
 *    5. System-level   (Tier 0, or emergency Tier 1)"
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KILLSWITCH_MAX_ACTIVE   64
#define KILLSWITCH_REASON_LEN   512
#define KILLSWITCH_TARGET_LEN   128
#define KILLSWITCH_MAX_HISTORY  256

/* ── Kill Switch Levels ───────────────────────────────────────────────── */

typedef enum {
    KILL_AGENT,           /* stop a single agent process */
    KILL_WORKFLOW,        /* stop an entire workflow/topology */
    KILL_SERVICE,         /* stop a service category (tools, LLM, IPC) */
    KILL_PHEROMONE,       /* silence a pheromone region */
    KILL_SYSTEM,          /* full system halt */
    KILL_LEVEL_COUNT
} kill_level_t;

/* ── Kill Switch State ────────────────────────────────────────────────── */

typedef enum {
    KILL_STATE_ARMED,     /* ready to trigger */
    KILL_STATE_TRIGGERED, /* actively killing */
    KILL_STATE_RESOLVED,  /* trigger resolved, back to normal */
    KILL_STATE_EXPIRED,   /* auto-expired after timeout */
} kill_state_t;

/* ── Trigger Criteria ─────────────────────────────────────────────────── */

typedef enum {
    KILL_TRIGGER_MANUAL,        /* human operator action */
    KILL_TRIGGER_BUDGET,        /* resource budget exceeded */
    KILL_TRIGGER_TIMEOUT,       /* operation timeout */
    KILL_TRIGGER_SAFETY,        /* safety constraint violated */
    KILL_TRIGGER_CASCADE,       /* triggered by upstream kill */
    KILL_TRIGGER_ANOMALY,       /* anomaly detection */
    KILL_TRIGGER_HEARTBEAT,     /* agent heartbeat lost */
} kill_trigger_t;

/* ── Kill Switch Entry ────────────────────────────────────────────────── */

typedef struct {
    int            id;
    kill_level_t   level;
    kill_state_t   state;
    kill_trigger_t trigger;
    int            required_tier;  /* minimum principal tier to activate */
    int            activated_by_tier; /* who actually triggered it */
    char           target[KILLSWITCH_TARGET_LEN]; /* agent_id, workflow, region, etc. */
    char           reason[KILLSWITCH_REASON_LEN];
    double         triggered_at;
    double         resolved_at;
    double         timeout;       /* auto-resolve after seconds (0 = never) */
    bool           cascade;       /* trigger downstream kills */
} killswitch_entry_t;

/* ── Kill Switch Registry ─────────────────────────────────────────────── */

typedef struct {
    killswitch_entry_t active[KILLSWITCH_MAX_ACTIVE];
    int                active_count;
    killswitch_entry_t history[KILLSWITCH_MAX_HISTORY];
    int                history_count;
    int                next_id;
    bool               system_halted;   /* KILL_SYSTEM is active */
    bool               initialized;

    /* Statistics */
    int                total_triggers;
    int                total_cascades;
    int                per_level_count[KILL_LEVEL_COUNT];
} killswitch_registry_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void killswitch_init(killswitch_registry_t *r);

/* ── Trigger Operations ───────────────────────────────────────────────── */

/* Trigger a kill switch. Returns kill ID or -1 if unauthorized.
   principal_tier: the tier of the entity triggering (0-3).
   For KILL_SYSTEM, requires tier 0 (or tier 1 with KILL_TRIGGER_SAFETY). */
int killswitch_trigger(killswitch_registry_t *r, kill_level_t level,
                       const char *target, const char *reason,
                       kill_trigger_t trigger, int principal_tier,
                       double timeout, bool cascade);

/* Resolve (lift) a kill switch. Returns true if resolved. */
bool killswitch_resolve(killswitch_registry_t *r, int kill_id,
                        int principal_tier);

/* ── Query ────────────────────────────────────────────────────────────── */

/* Check if a target is currently killed at any level. */
bool killswitch_is_killed(const killswitch_registry_t *r, const char *target);

/* Check if system is halted. */
bool killswitch_system_halted(const killswitch_registry_t *r);

/* Check if a specific level is active for a target. */
bool killswitch_level_active(const killswitch_registry_t *r,
                             kill_level_t level, const char *target);

/* Get active kill entries for a target. Returns count. */
int killswitch_get_active(const killswitch_registry_t *r, const char *target,
                          killswitch_entry_t *out, int max);

/* Get all active kills. Returns count. */
int killswitch_list_active(const killswitch_registry_t *r,
                           killswitch_entry_t *out, int max);

/* ── Maintenance ──────────────────────────────────────────────────────── */

/* Process timeouts and cascade triggers. Returns number of state changes. */
int killswitch_tick(killswitch_registry_t *r);

/* ── Serialization ────────────────────────────────────────────────────── */

int killswitch_to_json(const killswitch_registry_t *r, char *buf, size_t len);
int killswitch_status_json(const killswitch_registry_t *r, char *buf, size_t len);

/* ── Utilities ────────────────────────────────────────────────────────── */

const char *killswitch_level_name(kill_level_t l);
const char *killswitch_state_name(kill_state_t s);
const char *killswitch_trigger_name(kill_trigger_t t);

#endif
