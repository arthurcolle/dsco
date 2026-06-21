#ifndef DSCO_PLAN_CACHE_H
#define DSCO_PLAN_CACHE_H

/* ── Fuzzy plan cache ──────────────────────────────────────────────────────
 *
 * Priority 4: Cache successful topology selections keyed by 3-gram
 * fingerprint of the task string.  85%+ Jaccard similarity → cache hit.
 *
 * Usage:
 *   plan_cache_init();
 *
 *   plan_cache_result_t hit;
 *   if (plan_cache_lookup(task, &hit)) {
 *       // use hit.topology_name — skip plan_analyze() overhead
 *       topology_run(topology_find(hit.topology_name), ...);
 *   } else {
 *       plan_options_t *opts = plan_analyze(task, 0);
 *       const plan_option_t *best = plan_options_best(opts);
 *       plan_cache_store(task, best->topology_name, best->rationale, best->fit_score);
 *       topology_run(topology_find(best->topology_name), ...);
 *       plan_options_free(opts);
 *   }
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  topology_name[48];
    char  rationale[128];
    float similarity;           /* 0-1 (1.0 = exact fingerprint match) */
    int   hits_before;          /* how many times this entry was hit before */
} plan_cache_result_t;

void plan_cache_init(void);
bool plan_cache_lookup(const char *task, plan_cache_result_t *result);
void plan_cache_store(const char *task, const char *topology_name,
                      const char *rationale, float fit_score);
int  plan_cache_stats_json(char *buf, size_t buflen);
void plan_cache_flush(void);

#endif /* DSCO_PLAN_CACHE_H */
