/* Fixture-only test: CURL initialization is replaced, so no socket can open. */
#include <assert.h>
#define curl_easy_init fixture_curl_easy_init
#include "../src/openrouter_lanes.c"
#undef curl_easy_init

static int fixture_count = 2;
static int discovery_attempts;

CURL *fixture_curl_easy_init(void) {
    discovery_attempts++;
    return NULL;
}
int openrouter_cache_wait_ready(int timeout_ms) { (void)timeout_ms; return fixture_count; }
int openrouter_cache_load_sync(void) { return fixture_count; }
int openrouter_cache_count(void) { return fixture_count; }
int openrouter_cache_foreach(or_model_cb cb, void *ud) {
    const or_model_view_t models[] = {
        {.id="fixture/model-a", .context_window=64000, .max_output=4096,
         .tool_capable=1, .intelligence_index=50, .input_price=1, .output_price=2},
        {.id="other/model-b", .context_window=64000, .max_output=4096,
         .tool_capable=1, .intelligence_index=45, .input_price=2, .output_price=3},
    };
    for (int i = 0; i < fixture_count; i++) cb(&models[i], ud);
    return fixture_count;
}
void dsco_http_pool_apply(CURL *curl) { (void)curl; }

int main(void) {
    char result[8192];
    assert(tool_openrouter_lanes("{\"endpoints_per_model\":2}", result, sizeof(result)));
    assert(discovery_attempts == 2);
    assert(strstr(result, "\"fully_unrolled\":false"));
    assert(strstr(result, "\"endpoint_discovery_requested\":true"));
    assert(strstr(result, "\"upstream_unresolved\":2"));
    assert(strstr(result, "\"identity_status\":\"model_only\""));
    assert(json_get_int(result, "count", -1) == 2);

    discovery_attempts = 0;
    assert(tool_openrouter_lanes("{}", result, sizeof(result)));
    assert(discovery_attempts == 0);
    assert(strstr(result, "\"fully_unrolled\":false"));
    assert(strstr(result, "\"endpoint_discovery_requested\":false"));

    fixture_count = 0;
    assert(tool_openrouter_lanes("{\"endpoints_per_model\":2}", result, sizeof(result)));
    assert(strstr(result, "\"fully_unrolled\":false"));
    assert(json_get_int(result, "count", -1) == 0);

    fixture_count = 2;
    assert(!tool_openrouter_lanes("{}", result, 64));
    assert(strstr(result, "output_buffer_too_small"));
    assert(!tool_openrouter_lanes("{}", result, 300));
    assert(strstr(result, "output_buffer_too_small"));
    assert(!tool_openrouter_lanes("{}", NULL, 0));
    puts("PASS: requested discovery is not resolution; model-only/empty/tiny-buffer cases");
    return 0;
}
