#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "recovery.h"
#include "plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Default retry configs ─────────────────────────────────────────────── */

const retry_config_t RETRY_DEFAULT    = { 3, 200, 2.0, true  };
const retry_config_t RETRY_AGGRESSIVE = { 5, 100, 1.5, true  };
const retry_config_t RETRY_NONE       = { 0,   0, 1.0, false };

/* ── Internal: millisecond sleep ────────────────────────────────────────── */

static void sleep_ms(long ms) {
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/* ── execute_with_retry ─────────────────────────────────────────────────── */

bool execute_with_retry(recovery_fn_t fn, void *arg,
                        const retry_config_t *cfg,
                        char *errbuf, size_t errlen) {
    if (!fn || !cfg) return false;
    if (errbuf && errlen > 0) errbuf[0] = '\0';

    for (int attempt = 0; attempt <= cfg->max_retries; attempt++) {
        if (fn(arg, errbuf, errlen)) return true;

        if (attempt < cfg->max_retries) {
            /* delay = base * mult^attempt */
            double delay = (double)cfg->base_delay_ms;
            for (int i = 0; i < attempt; i++)
                delay *= cfg->backoff_mult;

            if (cfg->jitter) {
                /* 0..25 % additive jitter via arc4random */
                uint32_t r = arc4random_uniform(1000);
                delay += delay * 0.25 * ((double)r / 999.0);
            }

            sleep_ms((long)delay);
        }
    }
    return false;
}

/* ── execute_with_fallback ──────────────────────────────────────────────── */

bool execute_with_fallback(recovery_fn_t primary_fn,
                           recovery_fn_t *fallback_fns, int n_fallbacks,
                           void *arg,
                           char *errbuf, size_t errlen) {
    if (!primary_fn) return false;
    if (errbuf && errlen > 0) errbuf[0] = '\0';

    if (primary_fn(arg, errbuf, errlen)) return true;

    for (int i = 0; i < n_fallbacks; i++) {
        if (!fallback_fns[i]) continue;
        if (fallback_fns[i](arg, errbuf, errlen)) return true;
    }
    return false;
}

/* ── Atom collection: DFS through step tree ─────────────────────────────── */

#define MAX_PLAN_ATOMS 1024

static int collect_step_atoms_r(int step_id, int *out, int cap) {
    step_t *s = step_get(step_id);
    if (!s) return 0;
    int n = 0;

    for (int i = 0; i < s->atom_count && n < cap; i++)
        out[n++] = s->atom_ids[i];

    for (int i = 0; i < s->child_step_count && n < cap; i++)
        n += collect_step_atoms_r(s->child_step_ids[i],
                                  out + n, cap - n);
    return n;
}

static int collect_plan_atoms(int plan_id, int *out, int cap) {
    plan_t *plan = plan_get(plan_id);
    if (!plan) return 0;
    int n = 0;

    for (int i = 0; i < plan->root_step_count && n < cap; i++)
        n += collect_step_atoms_r(plan->root_step_ids[i],
                                  out + n, cap - n);
    return n;
}

/* ── backtrack_and_replay ───────────────────────────────────────────────── */

int backtrack_and_replay(int plan_id, int distance) {
    if (distance <= 0) return 0;

    int all_ids[MAX_PLAN_ATOMS];
    int all_n = collect_plan_atoms(plan_id, all_ids, MAX_PLAN_ATOMS);
    if (all_n == 0) return 0;

    /* Pick the last `distance` completed atoms (scan backwards). */
    int rollback[MAX_PLAN_ATOMS];
    int rb_n = 0;

    for (int i = all_n - 1; i >= 0 && rb_n < distance; i--) {
        atom_t *a = atom_get(all_ids[i]);
        if (!a) continue;
        if (a->status == PLAN_DONE || a->status == PLAN_FAILED)
            rollback[rb_n++] = all_ids[i];
    }

    if (rb_n == 0) return 0;

    /* Reset in oldest-first order so wiring resolves correctly on replay. */
    for (int i = rb_n - 1; i >= 0; i--) {
        atom_t *a = atom_get(rollback[i]);
        if (!a) continue;

        a->status = PLAN_PENDING;
        atom_set_result(rollback[i], NULL);

        /* Clear resolved wired input so it gets re-evaluated on next run. */
        free(a->wired_input);
        a->wired_input = NULL;
    }

    /* Reset parent steps whose atoms are all now PENDING. */
    for (int i = 0; i < rb_n; i++) {
        atom_t *a = atom_get(rollback[i]);
        if (!a) continue;
        step_t *s = step_get(a->step_id);
        if (!s || s->status == PLAN_PENDING) continue;

        bool all_pending = true;
        for (int j = 0; j < s->atom_count; j++) {
            atom_t *sa = atom_get(s->atom_ids[j]);
            if (sa && sa->status != PLAN_PENDING) {
                all_pending = false;
                break;
            }
        }
        if (all_pending)
            step_set_status(a->step_id, PLAN_PENDING);
    }

    return rb_n;
}

/* ── Recovery log ───────────────────────────────────────────────────────── */

void recovery_log_init(recovery_log_t *log) {
    if (log) memset(log, 0, sizeof(*log));
}

void recovery_log_record(recovery_log_t *log,
                         const recovery_log_entry_t *entry) {
    if (!log || !entry) return;
    log->entries[log->head] = *entry;
    log->head = (log->head + 1) % RECOVERY_LOG_CAP;
    log->count++;
}

int recovery_log_count(const recovery_log_t *log) {
    if (!log) return 0;
    return log->count < RECOVERY_LOG_CAP ? log->count : RECOVERY_LOG_CAP;
}

const recovery_log_entry_t *recovery_log_get(const recovery_log_t *log,
                                              int idx) {
    if (!log) return NULL;
    int avail = recovery_log_count(log);
    if (idx < 0 || idx >= avail) return NULL;
    /* idx 0 = newest; head-1 is the slot just written */
    int slot = (log->head - 1 - idx + RECOVERY_LOG_CAP) % RECOVERY_LOG_CAP;
    return &log->entries[slot];
}

int recovery_log_dump(const recovery_log_t *log, const char *path) {
    if (!log || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    int n = recovery_log_count(log);
    fprintf(fp, "timestamp,step_id,attempt,action,error\n");

    /* oldest first */
    for (int i = n - 1; i >= 0; i--) {
        const recovery_log_entry_t *e = recovery_log_get(log, i);
        if (!e) continue;
        fprintf(fp, "%ld,%d,%d,%s,%s\n",
                (long)e->timestamp,
                e->step_id,
                e->attempt,
                recovery_action_name(e->action_taken),
                e->error_str[0] ? e->error_str : "(none)");
    }

    fclose(fp);
    return n;
}

/* ── Name helper ────────────────────────────────────────────────────────── */

const char *recovery_action_name(recovery_action_t a) {
    switch (a) {
    case RECOVERY_ACTION_NONE:      return "none";
    case RECOVERY_ACTION_RETRY:     return "retry";
    case RECOVERY_ACTION_FALLBACK:  return "fallback";
    case RECOVERY_ACTION_BACKTRACK: return "backtrack";
    case RECOVERY_ACTION_GIVE_UP:   return "give_up";
    default:                        return "unknown";
    }
}
