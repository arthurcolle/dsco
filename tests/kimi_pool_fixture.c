/* Production Kimi credential/pool/request path; loopback and fake credentials only. */
#include "kimi_oauth.h"
#include "provider.h"
#include "provider_pool.h"
#include "vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

volatile int g_interrupted;
vm_t g_vm;
double g_cost_budget;
int g_cheap_mode;

int main(int argc, char **argv) {
    assert(argc == 3 && !strncmp(argv[1], "http://127.0.0.1:", 17));
    setenv("KIMI_CODE_API_BASE", argv[1], 1);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    provider_pool_init(NULL);
    provider_t *provider = provider_pool_acquire("kimi-code");
    bool available = provider_has_usable_key("kimi-code", NULL);
    bool healthy = provider_pool_healthy("kimi-code");
    printf("{\"initial_available\":%s,\"initial_healthy\":%s}\n",
           available ? "true" : "false", healthy ? "true" : "false");
    fflush(stdout);
    if (!strcmp(argv[2], "recover"))
        assert(getchar() == '\n'); /* driver installs a credential in this same process */
    if (!strcmp(argv[2], "breaker"))
        for (int i = 0; i < 3; i++) provider_pool_report("kimi-code", false, 1);
    if (!strcmp(argv[2], "quota"))
        provider_pool_mark_subscription_exhausted("kimi-code", time(NULL) + 3600);
    healthy = provider_pool_healthy("kimi-code");
    /* This order mirrors ensure_provider() before request-time token refresh. */
    const char *key = healthy ? provider_resolve_request_api_key("kimi-code", NULL) : NULL;
    bool ok = false;
    int http_status = 0;
    if (key && provider) {
        stream_result_t result = provider->stream(provider, key,
            "{\"model\":\"kimi-code/k3\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}",
            NULL, NULL, NULL, NULL, NULL);
        ok = result.ok;
        http_status = result.http_status;
        json_free_response(&result.parsed);
        free(result.actual_model);
        free(result.generation_id);
    }
    printf("{\"healthy\":%s,\"resolved\":%s,\"ok\":%s,\"http_status\":%d}\n",
           healthy ? "true" : "false", key ? "true" : "false",
           ok ? "true" : "false", http_status);
    provider_pool_shutdown();
    curl_global_cleanup();
    return 0;
}
