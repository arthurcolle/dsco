/* Governance-Model Experiment Platform — see include/gov_experiment.h.
   A measurement instrument for comparing governance models by overhead and by
   what each gate stage would decide. Lock-free-ish: counters are C11 atomics;
   we accept benign races on the double accumulators (measurement, not ledger). */
#include "gov_experiment.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Model resolution ─────────────────────────────────────────────────── */

static const char *const MODEL_NAMES[GOV_MODEL_COUNT] = {
    "none", "minimal", "audit_only", "standard", "paranoid"};

static const char *const STAGE_NAMES[GOV_STAGE_COUNT] = {
    "exempt",    "init",      "killswitch", "breaker",  "approval",
    "selfpres",  "pheromone", "checkpoint", "shadow"};

const char *gov_model_name(gov_model_t m) {
    return (m >= 0 && m < GOV_MODEL_COUNT) ? MODEL_NAMES[m] : "unknown";
}

const char *gov_stage_name(gov_stage_t s) {
    return (s >= 0 && s < GOV_STAGE_COUNT) ? STAGE_NAMES[s] : "unknown";
}

static gov_model_t parse_model(const char *v) {
    if (!v || !*v)
        return GOV_MODEL_STANDARD;
    if (strcasecmp(v, "none") == 0 || strcasecmp(v, "off") == 0 ||
        strcasecmp(v, "bypass") == 0)
        return GOV_MODEL_NONE;
    if (strcasecmp(v, "minimal") == 0 || strcasecmp(v, "min") == 0 ||
        strcasecmp(v, "immune") == 0)
        return GOV_MODEL_MINIMAL;
    if (strcasecmp(v, "audit") == 0 || strcasecmp(v, "audit_only") == 0 ||
        strcasecmp(v, "shadow") == 0 || strcasecmp(v, "dry-run") == 0)
        return GOV_MODEL_AUDIT_ONLY;
    if (strcasecmp(v, "standard") == 0 || strcasecmp(v, "full") == 0 ||
        strcasecmp(v, "default") == 0 || strcasecmp(v, "prod") == 0)
        return GOV_MODEL_STANDARD;
    if (strcasecmp(v, "paranoid") == 0 || strcasecmp(v, "strict") == 0 ||
        strcasecmp(v, "max") == 0)
        return GOV_MODEL_PARANOID;
    return GOV_MODEL_STANDARD;
}

/* File-scope (moved 2026-08-25) so gov_experiment_set_model() can invalidate
 * it: mcp_server_run pins the posture after saved-env loading may already
 * have resolved the lazy cache with a stale env snapshot. -1 = unresolved. */
static _Atomic int g_model_cache = -1;

gov_model_t gov_experiment_model(void) {
    int c = atomic_load_explicit(&g_model_cache, memory_order_relaxed);
    if (c >= 0)
        return (gov_model_t)c;
    /* DSCO_GOV_BYPASS=1 is the legacy hard override → NONE. */
    const char *bypass = getenv("DSCO_GOV_BYPASS");
    gov_model_t m;
    if (bypass && bypass[0] == '1')
        m = GOV_MODEL_NONE;
    else
        m = parse_model(getenv("DSCO_GOV_MODEL"));
    atomic_store_explicit(&g_model_cache, (int)m, memory_order_relaxed);
    return m;
}

void gov_experiment_reset_cache(void) {
    atomic_store_explicit(&g_model_cache, -1, memory_order_relaxed);
}

bool gov_experiment_bypass_all(void) {
    return gov_experiment_model() == GOV_MODEL_NONE;
}


/* ── Policy matrix ────────────────────────────────────────────────────────
   Which stages run, and whether a deny there enforces (blocks) or is merely
   observed. Immune stages (killswitch, breaker, selfpres) are the last thing
   to go — they run in every model except NONE. */

static bool stage_is_immune(gov_stage_t s) {
    return s == GOV_STAGE_KILLSWITCH || s == GOV_STAGE_BREAKER ||
           s == GOV_STAGE_SELFPRES;
}

bool gov_stage_active(gov_model_t m, gov_stage_t s) {
    switch (m) {
        case GOV_MODEL_NONE:
            return false;
        case GOV_MODEL_MINIMAL:
            /* init is needed to reach the immune stages; run those only. */
            return s == GOV_STAGE_INIT || stage_is_immune(s);
        case GOV_MODEL_AUDIT_ONLY:
        case GOV_MODEL_STANDARD:
        case GOV_MODEL_PARANOID:
            return true;
        default:
            return true;
    }
}

bool gov_stage_enforces(gov_model_t m, gov_stage_t s) {
    if (!gov_stage_active(m, s))
        return false;
    switch (m) {
        case GOV_MODEL_NONE:
            return false;
        case GOV_MODEL_MINIMAL:
            return stage_is_immune(s); /* only immune stages block */
        case GOV_MODEL_AUDIT_ONLY:
            /* Measure everything, block nothing — except hard immune vetoes,
               which stay live even in audit mode (safety floor). */
            return stage_is_immune(s);
        case GOV_MODEL_STANDARD:
        case GOV_MODEL_PARANOID:
            return true;
        default:
            return true;
    }
}

bool gov_shadow_all_tools(gov_model_t m) {
    return m == GOV_MODEL_PARANOID;
}

/* ── Counters ─────────────────────────────────────────────────────────── */

static _Atomic unsigned long g_gate_runs = 0;
static _Atomic unsigned long g_bypassed = 0;

typedef struct {
    _Atomic unsigned long runs;    /* times stage executed */
    _Atomic unsigned long denials; /* times stage produced a deny decision */
    _Atomic unsigned long blocks;  /* times a deny actually blocked */
    _Atomic double ms_total;       /* cumulative stage latency */
} stage_counter_t;

static stage_counter_t g_stage[GOV_STAGE_COUNT];

static void atomic_add_double(_Atomic double *acc, double v) {
    double cur = atomic_load_explicit(acc, memory_order_relaxed);
    double want;
    do {
        want = cur + v;
    } while (!atomic_compare_exchange_weak_explicit(acc, &cur, want,
                                                    memory_order_relaxed,
                                                    memory_order_relaxed));
}

void gov_stage_record(gov_stage_t s, double ms, bool would_deny, bool enforced) {
    if (s < 0 || s >= GOV_STAGE_COUNT)
        return;
    stage_counter_t *c = &g_stage[s];
    atomic_fetch_add_explicit(&c->runs, 1, memory_order_relaxed);
    if (would_deny) {
        atomic_fetch_add_explicit(&c->denials, 1, memory_order_relaxed);
        if (enforced)
            atomic_fetch_add_explicit(&c->blocks, 1, memory_order_relaxed);
    }
    if (ms > 0)
        atomic_add_double(&c->ms_total, ms);
}

void gov_experiment_note_gate_run(void) {
    atomic_fetch_add_explicit(&g_gate_runs, 1, memory_order_relaxed);
}

void gov_experiment_note_bypass(void) {
    atomic_fetch_add_explicit(&g_bypassed, 1, memory_order_relaxed);
}

void gov_experiment_reset(void) {
    atomic_store_explicit(&g_gate_runs, 0, memory_order_relaxed);
    atomic_store_explicit(&g_bypassed, 0, memory_order_relaxed);
    for (int i = 0; i < GOV_STAGE_COUNT; i++) {
        atomic_store_explicit(&g_stage[i].runs, 0, memory_order_relaxed);
        atomic_store_explicit(&g_stage[i].denials, 0, memory_order_relaxed);
        atomic_store_explicit(&g_stage[i].blocks, 0, memory_order_relaxed);
        atomic_store_explicit(&g_stage[i].ms_total, 0.0, memory_order_relaxed);
    }
}

void gov_experiment_totals(unsigned long *gate_calls, unsigned long *bypassed,
                           double *gate_ms_total) {
    if (gate_calls)
        *gate_calls = atomic_load_explicit(&g_gate_runs, memory_order_relaxed);
    if (bypassed)
        *bypassed = atomic_load_explicit(&g_bypassed, memory_order_relaxed);
    if (gate_ms_total) {
        double tot = 0;
        for (int i = 0; i < GOV_STAGE_COUNT; i++)
            tot += atomic_load_explicit(&g_stage[i].ms_total, memory_order_relaxed);
        *gate_ms_total = tot;
    }
}

size_t gov_experiment_report_json(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;
    gov_model_t m = gov_experiment_model();
    unsigned long runs = atomic_load_explicit(&g_gate_runs, memory_order_relaxed);
    unsigned long byp = atomic_load_explicit(&g_bypassed, memory_order_relaxed);

    double gate_ms = 0;
    for (int i = 0; i < GOV_STAGE_COUNT; i++)
        gate_ms += atomic_load_explicit(&g_stage[i].ms_total, memory_order_relaxed);

    size_t n = 0;
    int w = snprintf(buf, len,
                     "{\"model\":\"%s\",\"gate_runs\":%lu,\"bypassed\":%lu,"
                     "\"gate_ms_total\":%.4f,\"gate_ms_avg\":%.6f,\"stages\":[",
                     gov_model_name(m), runs, byp, gate_ms,
                     runs > 0 ? gate_ms / (double)runs : 0.0);
    if (w < 0)
        return 0;
    n = (size_t)w;
    for (int i = 0; i < GOV_STAGE_COUNT && n < len; i++) {
        unsigned long sr = atomic_load_explicit(&g_stage[i].runs, memory_order_relaxed);
        unsigned long sd = atomic_load_explicit(&g_stage[i].denials, memory_order_relaxed);
        unsigned long sb = atomic_load_explicit(&g_stage[i].blocks, memory_order_relaxed);
        double sm = atomic_load_explicit(&g_stage[i].ms_total, memory_order_relaxed);
        w = snprintf(buf + n, len - n,
                     "%s{\"stage\":\"%s\",\"runs\":%lu,\"denials\":%lu,"
                     "\"blocks\":%lu,\"ms_total\":%.4f,\"ms_avg\":%.6f}",
                     i == 0 ? "" : ",", gov_stage_name((gov_stage_t)i), sr, sd, sb,
                     sm, sr > 0 ? sm / (double)sr : 0.0);
        if (w < 0)
            break;
        n += (size_t)w;
    }
    if (n < len)
        n += (size_t)snprintf(buf + n, len - n, "]}");
    return n;
}
