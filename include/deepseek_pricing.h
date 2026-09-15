#ifndef DSCO_DEEPSEEK_PRICING_H
#define DSCO_DEEPSEEK_PRICING_H
#include "model_pricing.h"
#include <time.h>
#define DEEPSEEK_PRICING_URL "https://api-docs.deepseek.com/quick_start/pricing/?article_id=article_1779470751466_8"
/* Strict supported HTML schema: on drift, retain the last validated snapshot. */
int deepseek_pricing_load_html(const char *html, time_t observed_at);
void deepseek_pricing_load_cached(void);
int deepseek_pricing_refresh_sync(void);
int deepseek_pricing_lookup(const char *model, time_t at, model_price_t *price,
                           const char **source, time_t *observed_at);
#endif
