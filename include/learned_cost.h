#ifndef DSCO_LEARNED_COST_H
#define DSCO_LEARNED_COST_H

/* ── Learned Cost Models (Priority 3) ─────────────────────────────────────
 *
 * k-NN cost predictor that improves with every execution.
 * Reduces prediction error from ±50% → <15% after sufficient history.
 *
 * Storage: ~/.dsco/cost_history.json  (override: DSCO_COST_HISTORY env var)
 *
 * Usage:
 *   cost_db_t db;
 *   cost_db_init(&db);
 *   cost_db_load(&db);
 *
 *   // After execution:
 *   learn_from_execution(&db, task, "fanout_balance", 1850, 1.23);
 *
 *   // Before execution:
 *   cost_prediction_result_t pred;
 *   if (predict_cost(&db, task, "fanout_balance", &pred))
 *       printf("est $%.2f (confidence %.0f%%)\n", pred.predicted_cost, pred.confidence * 100);
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define COST_DB_CAPACITY    1000    /* ring buffer size */
#define LEARNED_COST_K_MAX  20      /* max k for k-NN search */

/* ── Data types ────────────────────────────────────────────────────────── */

typedef struct {
    int    task_len;        /* character length of task string */
    int    atom_count;      /* number of sub-tasks (inferred from task text) */
    int    tokens;          /* total tokens consumed */
    double cost;            /* actual cost in USD */
    char   topology[48];    /* topology name ("fanout_balance", etc.) */
    time_t timestamp;       /* unix epoch when recorded */
} cost_record_t;

typedef struct {
    cost_record_t entries[COST_DB_CAPACITY];
    int           count;    /* valid entries in ring [0, COST_DB_CAPACITY] */
    int           head;     /* next write index */
    bool          loaded;   /* cost_db_load has been called */
    bool          dirty;    /* unsaved changes present */
} cost_db_t;

typedef struct {
    double predicted_cost;  /* point estimate (USD) */
    double confidence;      /* 0.0 = no data, 1.0 = well-calibrated */
    int    k_used;          /* actual neighbors used */
} cost_prediction_result_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Zero-initialize the database struct. Must be called before any other fn. */
void cost_db_init(cost_db_t *db);

/* Load history from disk. Idempotent — safe to call multiple times. */
bool cost_db_load(cost_db_t *db);

/* Persist database to disk. No-op if not dirty. */
bool cost_db_save(cost_db_t *db);

/* Record an actual execution into the database and auto-save. */
void learn_from_execution(cost_db_t *db,
                          const char *task,
                          const char *topology,
                          int actual_tokens,
                          double actual_cost);

/* Predict cost for (task, topology) using k-NN over history.
 * Returns false when there are no topology-matching records. */
bool predict_cost(const cost_db_t *db,
                  const char *task,
                  const char *topology,
                  cost_prediction_result_t *out);

/* Return up to k most similar historical executions that match topology.
 * out_records must have room for at least k entries.
 * Returns actual count written (0 if none). */
int find_similar_executions(const cost_db_t *db,
                            const char *task,
                            const char *topology,
                            int k,
                            cost_record_t *out_records);

#endif /* DSCO_LEARNED_COST_H */
