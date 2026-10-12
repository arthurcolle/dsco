/* Production providers against loopback HTTP fixtures; no inference account. */
#include "provider.h"
#include "provider_profiles.h"
#include "vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_interrupted;
vm_t g_vm;
double g_cost_budget;
int g_cheap_mode;
static jbuf_t visible;
static void text_delta(const char *text, void *ctx) { (void)ctx; jbuf_append(&visible, text); }

int main(int argc, char **argv) {
    assert(argc == 4);
    assert(!strncmp(argv[2], "http://127.0.0.1:", 17));
    assert(!strcmp(provider_profile_canonical_name("router"), "dsco-router"));
    const provider_profile_t *profile = provider_profile_find(argv[1]);
    assert(profile && provider_profile_transport_supported(profile));
    bool ramp = !strcmp(profile->name, "ramp-router");
    bool missing = !strcmp(argv[3], "missing");
    const char *env_key = ramp ? "RAMP_ROUTER_API_KEY" : "AI_GATEWAY_API_KEY";
    unsetenv(env_key);
    assert(!provider_has_usable_key(profile->name, NULL));
    assert(!provider_resolve_request_api_key(profile->name, "sk-ant-fixture-do-not-export"));
    setenv(ramp ? "RAMP_ROUTER_API_BASE" : "VERCEL_API_BASE", argv[2], 1);
    if (!missing)
        setenv(env_key, "fixture-gateway-key", 1);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    provider_t *p = provider_create(argv[1]);
    assert(p && !strcmp(p->name, profile->name));
    session_state_t session = {0};
    snprintf(session.model, sizeof(session.model), "%s", ramp ? "kimi-k3-fast" : "moonshotai/kimi-k3");
    strcpy(session.tool_choice, "none");
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, "Inspect the evidence.");
    conv_add_assistant_tool_use(&conv, "prior-call", "read_file", "{\"path\":\"evidence.txt\"}");
    conv_add_tool_result(&conv, "prior-call", "Verified", false);
    conv_add_user_image_base64(&conv, "image/png", "eA==", "Describe this.");
    const char *key = provider_resolve_request_api_key(profile->name, NULL);
    char *body = p->build_request(p, &conv, &session, 64, key);
    assert(body);
    if (ramp) {
        session.model[0] = '\0';
        assert(!p->build_request(p, &conv, &session, 64, key));
        assert(!p->build_headers(p, "bad\r\nInjected: value"));
        strcpy(session.model, "ramp:future-exact-model");
        char *namespaced = p->build_request(p, &conv, &session, 64, key);
        char *model = json_get_str(namespaced, "model");
        assert(model && !strcmp(model, "future-exact-model"));
        free(model); free(namespaced);
    }
    jbuf_init(&visible, 256);
    stream_result_t result = {0};
    /* The dispatcher blocks missing Vercel credentials before its existing
     * Chat Completions transport, while public Responses also defends itself. */
    if (!missing || ramp)
        result = provider_stream_reuse(p, key, body, text_delta, NULL, NULL, NULL, NULL);
    jbuf_t output;
    jbuf_init(&output, 1024);
    jbuf_appendf(&output, "{\"ok\":%s,\"http_status\":%d,\"body\":%s,\"visible\":",
                 result.ok ? "true" : "false", result.http_status, body);
    jbuf_append_json_str(&output, visible.data);
    jbuf_append(&output, ",\"stop_reason\":");
    jbuf_append_json_str(&output, result.parsed.stop_reason ? result.parsed.stop_reason : "");
    jbuf_append(&output, ",\"tools\":[");
    bool first = true;
    for (int i = 0; i < result.parsed.count; i++) {
        content_block_t *b = &result.parsed.blocks[i];
        if (!b->type || strcmp(b->type, "tool_use")) continue;
        if (!first) jbuf_append(&output, ",");
        first = false;
        jbuf_append(&output, "{\"name\":"); jbuf_append_json_str(&output, b->tool_name);
        jbuf_append(&output, ",\"id\":"); jbuf_append_json_str(&output, b->tool_id);
        jbuf_append(&output, ",\"arguments\":"); jbuf_append_json_str(&output, b->tool_input);
        jbuf_append(&output, "}");
    }
    jbuf_appendf(&output, "],\"input_tokens\":%d,\"output_tokens\":%d}",
                 result.usage.input_tokens, result.usage.output_tokens);
    puts(output.data);
    jbuf_free(&output); jbuf_free(&visible);
    free(body); conv_free(&conv); provider_free(p);
    json_free_response(&result.parsed);
    free(result.actual_model); free(result.generation_id);
    curl_global_cleanup();
    return 0;
}
