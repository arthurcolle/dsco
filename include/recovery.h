#ifndef DSCO_RECOVERY_H
#define DSCO_RECOVERY_H

/*
 * recovery.h — Priority 7: Failure Recovery & Backtracking
 *
 * Provides:
 *   retry_config_t          exponential-backoff retry configuration
 *   fallback_route_t        primary + ordered fallback step IDs
 *   recovery_log_entry_t    one record in the recovery audit log
 *   recovery_log_t          256-entry ring-buffer log
 *
 *   execute_with_retry()    retry a step function with backoff
 *   execute_with_fallback() try primary then fallback functions
 *   backtrack_and_replay()  roll back the last N atoms in a plan
 *   recovery_log_*          record / query / dump the log
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ── Step function signature ─────────────────────────────────────────────── */

/* A recoverable step: returns true on success.
 * On failure fills errbuf with a short description.                        */
typedef bool (*recovery_fn_t)(void *arg, char *errbuf, size_t errlen);

/* ── Retry configuration ─────────────────────────────────────────────────── */

typedef struct {
    int    max_retries;    /* number of retries after first failure (0 = none) */
    int    base_delay_ms;  /* delay before first retry in milliseconds         */
    double backoff_mult;   /* multiplier applied each retry (2.0 = exponential)*/
    bool   jitter;         /* add ±25 % random jitter to each delay            */
} retry_config_t;

/* ── Fallback route ──────────────────────────────────────────────────────── */

#define RECOVERY_MAX_FALLBACKS 8

typedef struct {
    int primary_step_id;
    int fallback_step_ids[RECOVERY_MAX_FALLBACKS];
    int len;
} fallback_route_t;

/* ── Recovery action (for log entries) ──────────────────────────────────── */

typedef enum {
    RECOVERY_ACTION_NONE      = 0,
    RECOVERY_ACTION_RETRY     = 1,
    RECOVERY_ACTION_FALLBACK  = 2,
    RECOVERY_ACTION_BACKTRACK = 3,
    RECOVERY_ACTION_GIVE_UP   = 4,
} recovery_action_t;

/* ── Recovery log entry ──────────────────────────────────────────────────── */

#define RECOVERY_ERR_LEN 256

typedef struct {
    int               step_id;
    int               attempt;
    char              error_str[RECOVERY_ERR_LEN];
    time_t            timestamp;
    recovery_action_t action_taken;
} recovery_log_entry_t;

/* ── Recovery log (ring buffer) ──────────────────────────────────────────── */

#define RECOVERY_LOG_CAP 256

typedef struct {
    recovery_log_entry_t entries[RECOVERY_LOG_CAP];
    int                  head;   /* index of next write slot (0..CAP-1)  */
    int                  count;  /* total records ever written (may > CAP)*/
} recovery_log_t;

/* ── Execution with recovery ─────────────────────────────────────────────── */

/* Call fn(arg) up to (1 + cfg->max_retries) times.
 * Sleeps between attempts using exponential backoff + optional jitter.
 * Returns true on first success; on total failure errbuf holds last error. */
bool execute_with_retry(recovery_fn_t fn, void *arg,
                        const retry_config_t *cfg,
                        char *errbuf, size_t errlen);

/* Call primary_fn(arg); if it fails try each fallback_fns[i](arg) in order.
 * Returns true on first success; errbuf holds last error on total failure.  */
bool execute_with_fallback(recovery_fn_t primary_fn,
                           recovery_fn_t *fallback_fns, int n_fallbacks,
                           void *arg,
                           char *errbuf, size_t errlen);

/* Reset the last `distance` DONE/FAILED atoms in plan_id back to PENDING,
 * clearing their results and wired inputs so plan_run_next re-executes them.
 * Also resets parent steps to PENDING when all their atoms become pending.
 * Returns the number of atoms actually rolled back.                         */
int backtrack_and_replay(int plan_id, int distance);

/* ── Recovery log API ────────────────────────────────────────────────────── */

void recovery_log_init(recovery_log_t *log);

/* Append entry to the ring buffer (overwrites oldest when full). */
void recovery_log_record(recovery_log_t *log,
                         const recovery_log_entry_t *entry);

/* Number of entries accessible (min of total count, RECOVERY_LOG_CAP). */
int recovery_log_count(const recovery_log_t *log);

/* Return entry at position idx (0 = newest).  NULL if idx out of range. */
const recovery_log_entry_t *recovery_log_get(const recovery_log_t *log,
                                              int idx);

/* Write a CSV of all accessible entries to path.
 * Returns number of rows written, or -1 on error.                          */
int recovery_log_dump(const recovery_log_t *log, const char *path);

/* Human-readable name for a recovery action. */
const char *recovery_action_name(recovery_action_t a);

/* ── Pre-built retry configs ─────────────────────────────────────────────── */

/* 3 retries, 200 ms base, 2× backoff, jitter on */
extern const retry_config_t RETRY_DEFAULT;

/* 5 retries, 100 ms base, 1.5× backoff, jitter on */
extern const retry_config_t RETRY_AGGRESSIVE;

/* 0 retries — fail immediately */
extern const retry_config_t RETRY_NONE;

#endif /* DSCO_RECOVERY_H */
