/* Exercise the production catalog parser with an unseen model and changed tiers. */
#include "../src/openrouter_cache.c"
#include <assert.h>
void dsco_http_pool_apply(CURL *c) { (void)c; }
const model_info_t *codex_cache_lookup(const char *name) {
    static const model_info_t native = {.model_id="future-model", .context_window=75000,.max_output=8000};
    return !strcmp(name,"future-model") ? &native : NULL;
}
int main(void) {
    const char *fixture = "{\"data\":[{\"id\":\"future-model\",\"context_length\":900000,"
      "\"top_provider\":{\"max_completion_tokens\":128000},\"pricing\":{\"prompt\":\"0.000001\","
      "\"completion\":\"0.000002\",\"input_cache_read\":\"0.0000001\",\"input_cache_write\":\"0.00000125\","
      "\"overrides\":[{\"min_prompt_tokens\":50000,\"prompt\":\"0.000003\",\"completion\":\"0.000006\","
      "\"input_cache_read\":\"0.0000003\",\"input_cache_write\":\"0.00000375\"},"
      "{\"min_prompt_tokens\":60000,\"prompt\":\"0.000004\",\"completion\":\"0.000008\"}]}}]}";
    or_catalog_t *catalog = catalog_from_json(fixture); assert(catalog);
    atomic_store(&g_catalog, catalog);
    const model_info_t *m = model_lookup("future-model");
    assert(m && m->context_window==75000 && m->max_output==8000);
    model_info_t storage;
    m=model_lookup_priced_for_input("future-model",50000,&storage);
    assert(m->input_price==1 && m->output_price==2);
    m=model_lookup_priced_for_input("future-model",50001,&storage);
    assert(m->input_price==3 && m->output_price==6 && m->cache_write_price==3.75);
    m=model_lookup_priced_for_input("future-model",60001,&storage);
    assert(m->input_price==4 && m->output_price==8 && m->cache_read_price==-1);
    puts("dynamic catalog: unseen model, observed transport cap and multiple price boundaries passed");
}
