/* Direct production adapters against caller-supplied loopback fixtures. */
#include "llm.h"
#include "provider.h"
#include "vm.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

volatile int g_interrupted;
vm_t g_vm;
double g_cost_budget;
int g_cheap_mode;
static jbuf_t visible;
static jbuf_t thinking;
static struct timespec first_text_at;
static void text_delta(const char *text, void *ctx) {
    (void)ctx;
    if (!visible.len) clock_gettime(CLOCK_MONOTONIC, &first_text_at);
    jbuf_append(&visible, text);
}
static void thinking_delta(const char *text, void *ctx) { (void)ctx; jbuf_append(&thinking, text); }
static void *cancel_later(void *ctx) {
    int ms = *(int *)ctx;
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
    g_interrupted = 1;
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    assert(!strncmp(argv[2], "http://127.0.0.1:", 17));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    jbuf_init(&visible, 1024);
    jbuf_init(&thinking, 1024);
    pthread_t cancel;
    int cancel_ms = argc == 4 ? atoi(argv[3]) : 0;
    if (cancel_ms) assert(!pthread_create(&cancel, NULL, cancel_later, &cancel_ms));
    stream_result_t result;
    if (!strcmp(argv[1], "anthropic-auth")) {
        provider_t *provider = provider_create("anthropic");
        assert(provider);
        provider->api_url = argv[2];
        const char *key = provider_resolve_request_api_key("anthropic", NULL);
        assert(key && key[0]);
        result = provider->stream(provider, key, "{}", text_delta, NULL, NULL,
                                  thinking_delta, NULL);
        provider_free(provider);
    } else if (!strcmp(argv[1], "anthropic")) {
        result = llm_stream_reuse_url(NULL, argv[2], "fixture-only", "{}",
            text_delta, NULL, NULL, thinking_delta, NULL);
    } else {
        bool authenticated = !strcmp(argv[1], "responses-auth");
        assert(authenticated || !strcmp(argv[1], "responses"));
        setenv("DSCO_CHATGPT_BASE_URL", argv[2], 1);
        provider_t *provider = provider_create("openai-codex");
        assert(provider && !strcmp(provider->api_url, "https://chatgpt.com/backend-api/codex/responses"));
        const char *key = authenticated
            ? provider_resolve_request_api_key("openai-codex", NULL) : "fixture-only";
        assert(key && key[0]);
        if (authenticated) {
            assert(provider_has_usable_key("openai-codex", NULL));
            assert(!strcmp(provider_route_for_model("gpt-6-astra", NULL, NULL), "openai-codex"));
        }
        result = provider->stream(provider, key, "{}", text_delta, NULL, NULL,
                                  thinking_delta, NULL);
        provider_free(provider);
    }
    if (cancel_ms) pthread_join(cancel, NULL);
    struct timespec completed_at;
    clock_gettime(CLOCK_MONOTONIC, &completed_at);
    double text_before_completion_ms = visible.len
        ? (completed_at.tv_sec - first_text_at.tv_sec) * 1000.0 +
          (completed_at.tv_nsec - first_text_at.tv_nsec) / 1000000.0 : 0.0;
    jbuf_t out; jbuf_init(&out, 1024);
    jbuf_appendf(&out, "{\"ok\":%s,\"http_status\":%d,\"input_tokens\":%d,\"output_tokens\":%d,\"cache_read_tokens\":%d,\"stop_reason\":",
                 result.ok ? "true" : "false", result.http_status,
                 result.usage.input_tokens, result.usage.output_tokens, result.usage.cache_read_input_tokens);
    jbuf_append_json_str(&out, result.parsed.stop_reason ? result.parsed.stop_reason : "");
    jbuf_append(&out, ",\"visible\":"); jbuf_append_json_str(&out, visible.data);
    jbuf_append(&out, ",\"thinking\":"); jbuf_append_json_str(&out, thinking.data);
    jbuf_appendf(&out, ",\"text_before_completion_ms\":%.3f", text_before_completion_ms);
    jbuf_append(&out, ",\"preserved_text\":");
    jbuf_t text; jbuf_init(&text, 1024);
    for (int i = 0; i < result.parsed.count; i++) {
        content_block_t *block = &result.parsed.blocks[i];
        if (block->type && !strcmp(block->type, "text") && block->text)
            jbuf_append(&text, block->text);
    }
    jbuf_append_json_str(&out, text.data); jbuf_append(&out, "}");
    puts(out.data);
    jbuf_free(&text); jbuf_free(&out); jbuf_free(&visible); jbuf_free(&thinking);
    json_free_response(&result.parsed);
    free(result.actual_model); free(result.generation_id);
    curl_global_cleanup();
    return 0;
}
