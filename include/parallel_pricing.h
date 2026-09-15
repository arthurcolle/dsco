#ifndef DSCO_PARALLEL_PRICING_H
#define DSCO_PARALLEL_PRICING_H

#include <stddef.h>

/* First-party Parallel API reference prices. All rates are upstream USD and
 * are deliberately independent from DSCO retail or markup configuration. */
#define PARALLEL_AI_PRICING_SOURCE "https://docs.parallel.ai/getting-started/pricing"

/* Return the upstream rate in USD per 1,000 billable units. A zero return
 * means the named tier/model is not in the published catalog. */
double parallel_ai_task_rate_per_1000(const char *processor);
double parallel_ai_search_rate_per_1000(const char *mode);
double parallel_ai_responses_rate_per_1000(const char *reasoning_effort);
double parallel_ai_monitor_rate_per_1000(const char *processor);
double parallel_ai_chat_rate_per_1000(const char *model);

/* FindAll is a fixed USD charge plus a USD charge per matched entity. */
int parallel_ai_findall_rates(const char *generator, double *fixed_usd,
                              double *per_match_usd);

/* Entity Search's base rate includes the first 100 results. */
double parallel_ai_entity_search_rate_per_1000(void);
double parallel_ai_entity_search_additional_rate_per_1000(void);

/* Convert billable units into USD. These functions use upstream prices only;
 * failed Task/Responses requests must be passed as zero successful units.
 * An unknown tier returns NaN, so an unpriced request cannot be mistaken for
 * a free request. */
double parallel_ai_task_cost_usd(const char *processor, size_t successful_runs);
/* Search and Entity Search include results per request. Callers must provide
 * exactly one result count for every request; aggregate totals are rejected
 * because [20,0] and [10,10] have different included-result charges. */
double parallel_ai_search_cost_usd(const char *mode, size_t requests,
                                   const size_t *result_counts,
                                   size_t result_count);
double parallel_ai_extract_cost_usd(size_t urls);
double parallel_ai_responses_cost_usd(const char *reasoning_effort,
                                      size_t successful_requests);
double parallel_ai_monitor_cost_usd(const char *processor, size_t checks);
double parallel_ai_findall_cost_usd(const char *generator, size_t matches);
double parallel_ai_entity_search_cost_usd(size_t requests,
                                           const size_t *result_counts,
                                           size_t result_count);
double parallel_ai_chat_cost_usd(const char *model, size_t requests);

/* Serialize the upstream catalog for the runtime Parallel capability router.
 * Returns bytes written excluding NUL, or zero when the buffer is too small.
 * This is a reference catalog, not a DSCO retail quote or an invoice. */
int parallel_ai_pricing_catalog_json(char *out, size_t out_len);

#endif
