#include "config.h"
#include "http_pool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void dsco_http_pool_apply(CURL *c) { (void)c; }
const model_info_t *codex_cache_lookup(const char *name) { (void)name; return NULL; }
const model_info_t *openrouter_cache_lookup(const char *name) {
    static const model_info_t live = {.model_id="openai/gpt-5", .input_price=91, .output_price=92};
    static const model_info_t kimi = {.model_id="moonshotai/kimi-k3", .input_price=3, .output_price=15, .cache_read_price=0.3, .cache_write_price=-1};
    if (strcmp(name, "kimi-k3") == 0 || strcmp(name, "moonshotai/kimi-k3") == 0) return &kimi;
    return strcmp(name, "openai/gpt-5") == 0 ? &live : NULL;
}
int main(void) {
    const char *fixture =
      "# Pricing\n### Standard pricing data\n"
      "| Model | Short context input | Short context cached input | Short context cache writes | Short context output |\n"
      "| --- | --- | --- | --- | --- |\n"
      "| gpt-5 | $3.25 | $0.32 | $4.00 | $13.00 |\n"
      "| gpt-5.4-nano | $0.30 | $0.03 | $0.40 | $1.50 |\n"
      "### Batch pricing data\n"
      "| Model | Input | Cached input | Output |\n"
      "| gpt-5 | $0.01 | $0.01 | $0.01 |\n";
    assert(model_pricing_load_openai_markdown(fixture, strlen(fixture)) == 2);
    model_info_t storage;
    const model_info_t *m = model_lookup_priced("gpt5", &storage);
    assert(m && m->input_price == 3.25 && m->output_price == 13.0);
    assert(m->cache_read_price == 0.32 && m->cache_write_price == 4.0);
    assert(strcmp(model_lookup_pricing_source("gpt5"), "openai_standard_pricing") == 0);
    model_price_t dated;
    assert(model_pricing_lookup("openai", "gpt-5-2026-09-04", &dated));
    assert(!model_pricing_lookup("openai", "gpt-5-unpriced-new-variant", &dated));
    m = model_lookup_priced("gpt-5.4-nano-2026-03-17", &storage);
    assert(m && m->input_price == 0.30 && m->output_price == 1.50);
    assert(m->cache_read_price == 0.03 && m->cache_write_price == 0.40);
    assert(strcmp(model_lookup_pricing_source("gpt-5.4-nano-2026-03-17"), "openai_standard_pricing") == 0);
    assert(model_lookup_priced("gpt-5.4-nano-20260317", &storage));
    assert(!model_lookup_priced("gpt-5.4-nano-unpriced-new-variant", &storage));
    m = model_lookup_priced("kimi-k3", &storage);
    assert(m && m->input_price == 3 && m->output_price == 15);
    assert(strcmp(model_lookup_pricing_source("kimi-k3"), "openrouter_catalog") == 0);
    const char *k3_aliases[] = {"k3", "kimi-code/k3"};
    for (size_t i = 0; i < 2; i++) {
        m = model_lookup_priced(k3_aliases[i], &storage);
        assert(m && m->input_price == 3 && m->output_price == 15 && m->cache_read_price == 0.3);
        assert(strcmp(model_lookup_pricing_source(k3_aliases[i]), "openrouter_catalog") == 0);
    }
    const char *invalid = "| Model | Input | Output |\n| gpt-5 | $nan | $2 |\n";
    assert(model_pricing_load_openai_markdown(invalid, strlen(invalid)) == 0);
    assert(model_lookup_priced("gpt5", &storage)->input_price == 3.25);
    assert(model_pricing_load_openai_markdown("service unavailable", 19) == 0);
    puts("catalog pricing: standard table, alias precedence, cache pricing and failure retention passed");
    return 0;
}
