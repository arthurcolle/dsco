#ifndef DSCO_PLAN_CACHE_H
#define DSCO_PLAN_CACHE_H

/* ── Plan Cache & Replay (Priority 4) ─────────────────────────────────────
 *
 * LRU ring buffer of 100 cached task→plan mappings.
 * Fuzzy-matches incoming tasks using 3-gram Jaccard similarity (threshold 80%).
 * Adapts cached plans to new tasks via entity substitution.
 * Persists to ~/.dsco/cache/plans.json (one JSON object per line).
 *
 * Primary flow:
 *   plan_cache_init();
 *
 *   plan_cache_result_t hit;
 *   if (plan_cache_lookup(task, &hit)) {
 *       topology_run(topology_find(hit.topology_name), ...);
 *   } else {
 *       plan_options_t *opts = plan_analyze(task, 0);
 *       const plan_option_t *best = plan_options_best(opts);
 *       plan_cache_store(task, best->topology_name, best->rationale, best->fit_score);
 *       topology_run(topology_find(best->topology_name), ...);
 *       plan_options_free(opts);
 *   }
 *
 * Full-plan caching (optional, for plan replay):
 *   char *plan_json = plan_serialize(plan);
 *   plan_cache_store_json(task, plan_json);
 *
 *   const plan_cache_entry_t *e = plan_cache_find_entry(task);
 *   if (e && e->plan_json) {
 *       char *adapted = plan_cache_adapt(e, new_task);
 *       // replay adapted plan JSON
 *       free(adapted);
 *   }
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define PLAN_CACHE_MAX      100    /* LRU ring buffer capacity */
#define PLAN_CACHE_MIN_SIM  0.80f  /* minimum Jaccard similarity for a hit */

/* ── Public entry ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t task_hash;          /* FNV-64 of normalized task text */
    char     task_text[256];     /* first 255 chars of task */
    char    *plan_json;          /* heap-alloc full plan JSON (NULL if not stored) */
    int      hit_count;
    time_t   last_used;
    time_t   created;
    /* Topology shortcut — populated by plan_cache_store() */
    char     topology_name[48];
    char     rationale[128];
    float    fit_score;
    bool     occupied;
} plan_cache_entry_t;

/* ── LRU ring buffer ─────────────────────────────────────────────────────── */

typedef struct {
    plan_cache_entry_t entries[PLAN_CACHE_MAX];
    int                head;    /* eviction cursor */
    int                count;   /* occupied slots */
} plan_cache_t;

/* ── Lookup result (backward compatible with callers in tools.c / topology.c) */

typedef struct {
    char  topology_name[48];
    char  rationale[128];
    float similarity;
    int   hits_before;
} plan_cache_result_t;

/* ── Singleton lifecycle ─────────────────────────────────────────────────── */

void plan_cache_init(void);          /* load from disk; idempotent */
void plan_cache_free(void);          /* free all plan_json heap strings */
void plan_cache_load(void);          /* explicit (re)load from disk */
void plan_cache_save(void);          /* explicit flush to disk */
void plan_cache_flush(void);         /* alias for plan_cache_save (compat) */

/* ── Lookup / store ──────────────────────────────────────────────────────── */

/* Fuzzy lookup. Returns true on >=80% similarity hit. Fills *result. */
bool plan_cache_lookup(const char *task, plan_cache_result_t *result);

/* Store topology shortcut (used by topology.c / tools.c). */
void plan_cache_store(const char *task, const char *topology_name,
                      const char *rationale, float fit_score);

/* Store full plan JSON alongside the topology shortcut. */
void plan_cache_store_json(const char *task, const char *plan_json);

/* Return pointer into the ring buffer (NULL on miss). Read-only; lock not held. */
const plan_cache_entry_t *plan_cache_find_entry(const char *task);

/* ── Stats ───────────────────────────────────────────────────────────────── */

int plan_cache_stats_json(char *buf, size_t buflen);

/* ── Extended API ────────────────────────────────────────────────────────── */

/* Jaccard similarity on 3-grams of two task strings.  Returns [0.0, 1.0]. */
float plan_similarity_score(const char *task_a, const char *task_b);

/* Return a new heap-allocated copy of entry->plan_json with entity names from
 * entry->task_text substituted by corresponding entities found in new_task.
 * Returns NULL if entry->plan_json is NULL or allocation fails.
 * Caller owns the returned string and must free() it. */
char *plan_cache_adapt(const plan_cache_entry_t *entry, const char *new_task);

#endif /* DSCO_PLAN_CACHE_H */
