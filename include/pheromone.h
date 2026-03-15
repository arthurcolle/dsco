#ifndef DSCO_PHEROMONE_H
#define DSCO_PHEROMONE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Pheromone Coordination System (Wings)
 *
 * Stigmergic coordination via typed signals with exponential decay.
 * Agents communicate indirectly by depositing and sensing pheromones
 * in a shared signal space. This enables emergent coordination without
 * centralized planning.
 *
 * From the Overmind Soul v1.0:
 *   "Digital pheromones enable emergent coordination patterns.
 *    Typed signals with exponential decay (lambda=0.01, ~69s half-life)"
 *
 * Six signal types × five decay functions × five aggregation modes.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PHEROMONE_MAX_SIGNALS     1024
#define PHEROMONE_MAX_REGION_LEN  128
#define PHEROMONE_MAX_SOURCE_LEN  64
#define PHEROMONE_MAX_META_LEN    512
#define PHEROMONE_DEFAULT_LAMBDA  0.01    /* ~69s half-life */
#define PHEROMONE_CLEANUP_THRESHOLD 0.001 /* signals below this are reaped */

/* ── Signal Types (Section 9.3 of Overmind Soul) ──────────────────────── */

typedef enum {
    PHERO_PROGRESS,       /* work advancement indicator */
    PHERO_ATTRACTION,     /* draw agents toward a region/task */
    PHERO_WARNING,        /* danger/problem signal */
    PHERO_SUCCESS,        /* task completed successfully */
    PHERO_HELP_NEEDED,    /* request for assistance */
    PHERO_CAPACITY,       /* available resource signal */
    PHERO_TYPE_COUNT
} pheromone_type_t;

/* ── Decay Functions ──────────────────────────────────────────────────── */

typedef enum {
    PHERO_DECAY_EXPONENTIAL,   /* c(t) = c0 * exp(-lambda * t)  [default] */
    PHERO_DECAY_LINEAR,        /* c(t) = max(0, c0 - rate * t)            */
    PHERO_DECAY_STEP,          /* c(t) = c0 if t < ttl, else 0            */
    PHERO_DECAY_LOGARITHMIC,   /* c(t) = c0 / (1 + lambda * ln(1+t))     */
    PHERO_DECAY_SIGMOID,       /* c(t) = c0 / (1 + exp(lambda*(t-mid)))   */
    PHERO_DECAY_COUNT
} pheromone_decay_t;

/* ── Aggregation Modes ────────────────────────────────────────────────── */

typedef enum {
    PHERO_AGG_SUM,        /* sum all signals of same type in region */
    PHERO_AGG_MAX,        /* take strongest signal */
    PHERO_AGG_MEAN,       /* average concentration */
    PHERO_AGG_WEIGHTED,   /* recency-weighted average */
    PHERO_AGG_QUORUM,     /* signal only if count >= threshold */
    PHERO_AGG_COUNT
} pheromone_aggregation_t;

/* ── Signal Structure ─────────────────────────────────────────────────── */

typedef struct {
    int                  id;
    pheromone_type_t     type;
    double               concentration;   /* current strength [0.0, 1.0+] */
    double               initial;         /* starting concentration */
    double               deposit_time;    /* epoch seconds */
    char                 region[PHEROMONE_MAX_REGION_LEN]; /* spatial tag */
    char                 source[PHEROMONE_MAX_SOURCE_LEN]; /* emitting agent */
    char                 meta[PHEROMONE_MAX_META_LEN];     /* JSON metadata */
    pheromone_decay_t    decay_fn;
    double               lambda;          /* decay rate parameter */
    double               ttl;             /* max lifetime (step decay) */
    bool                 active;
} pheromone_signal_t;

/* ── Gradient Reading ─────────────────────────────────────────────────── */

typedef struct {
    pheromone_type_t type;
    double           concentration;  /* aggregated value */
    int              signal_count;   /* number of contributing signals */
    double           strongest_age;  /* age of strongest signal (seconds) */
    char             strongest_source[PHEROMONE_MAX_SOURCE_LEN];
} pheromone_gradient_t;

/* ── Pheromone Field (shared signal space) ────────────────────────────── */

typedef struct {
    pheromone_signal_t  signals[PHEROMONE_MAX_SIGNALS];
    int                 count;
    int                 next_id;
    pheromone_aggregation_t default_aggregation;
    double              default_lambda;
    bool                initialized;
    /* Statistics */
    int                 total_deposits;
    int                 total_reads;
    int                 total_reaped;
} pheromone_field_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void pheromone_field_init(pheromone_field_t *f);
void pheromone_field_destroy(pheromone_field_t *f);

/* ── Deposit & Sense ──────────────────────────────────────────────────── */

/* Deposit a pheromone signal. Returns signal ID or -1 on error. */
int pheromone_deposit(pheromone_field_t *f, pheromone_type_t type,
                      double concentration, const char *region,
                      const char *source, const char *meta);

/* Deposit with custom decay function. */
int pheromone_deposit_ex(pheromone_field_t *f, pheromone_type_t type,
                         double concentration, const char *region,
                         const char *source, const char *meta,
                         pheromone_decay_t decay_fn, double lambda, double ttl);

/* Read aggregated concentration of a type in a region. */
double pheromone_sense(pheromone_field_t *f, pheromone_type_t type,
                       const char *region, pheromone_aggregation_t agg);

/* Read gradient (full detail) for a type in a region. */
bool pheromone_gradient(pheromone_field_t *f, pheromone_type_t type,
                        const char *region, pheromone_aggregation_t agg,
                        pheromone_gradient_t *out);

/* Read all gradients in a region (one per type). Returns count. */
int pheromone_sense_all(pheromone_field_t *f, const char *region,
                        pheromone_aggregation_t agg,
                        pheromone_gradient_t *out, int max);

/* ── Maintenance ──────────────────────────────────────────────────────── */

/* Decay all signals to current time, reap dead ones. Returns reaped count. */
int pheromone_tick(pheromone_field_t *f);

/* Boost/reinforce a signal (e.g., positive feedback loop). */
bool pheromone_reinforce(pheromone_field_t *f, int signal_id, double amount);

/* Evaporate all signals in a region (emergency clear). */
int pheromone_evaporate_region(pheromone_field_t *f, const char *region);

/* ── Serialization (for IPC persistence) ──────────────────────────────── */

/* Serialize field to JSON. Returns bytes written. */
int pheromone_to_json(const pheromone_field_t *f, char *buf, size_t len);

/* Deserialize field from JSON. Returns true on success. */
bool pheromone_from_json(pheromone_field_t *f, const char *json);

/* ── Utilities ────────────────────────────────────────────────────────── */

const char *pheromone_type_name(pheromone_type_t t);
const char *pheromone_decay_name(pheromone_decay_t d);
const char *pheromone_agg_name(pheromone_aggregation_t a);

/* Format a status summary as JSON. */
int pheromone_status_json(const pheromone_field_t *f, char *buf, size_t len);

#endif
