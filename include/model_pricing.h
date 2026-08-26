#ifndef DSCO_MODEL_PRICING_H
#define DSCO_MODEL_PRICING_H

#include <stddef.h>

/* Runtime vendor pricing. Prices are USD per million tokens. The cache is
 * loaded synchronously from disk and refreshed from the vendor in background. */
typedef struct {
    double input;
    double cached_input;
    double cache_write;
    double output;
} model_price_t;

void model_pricing_init(void);
void model_pricing_shutdown(void);
int model_pricing_lookup(const char *provider, const char *model_id,
                         model_price_t *out);

/* Parser entry point kept public for deterministic fixture tests. */
int model_pricing_load_openai_markdown(const char *markdown, size_t len);

#endif
