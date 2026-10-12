/* Exercise the production worker and single-tool loops with in-process streams. */
#include <assert.h>
#define tools_runtime_api_key fixture_api_key
#define provider_route_for_model fixture_route
#define provider_resolve_request_api_key fixture_resolve_key
#define provider_debug_log_request fixture_debug_request
#define provider_create fixture_provider_create
#define provider_prepare fixture_provider_prepare
#define provider_free fixture_provider_free
#define provider_stream_reuse fixture_provider_stream
#define llm_build_request_ex_for_credential fixture_build_request
#define llm_stream fixture_llm_stream
#define tools_is_allowed_for_tier fixture_allowed
#define tools_execute_for_tier fixture_execute_tier
#define tools_execute fixture_execute
#define tools_lookup_index fixture_lookup_index
#define agent_run fixture_agent_run
#include "../src/orchestrator.c"

vm_t g_vm;
volatile int g_interrupted;
int g_cheap_mode;
double g_cost_budget;
static int worker_turns, single_turns, tool_calls;

const char *fixture_api_key(void) { return "fixture-only"; }
const char *fixture_route(const char *model, const char *key, const char *override) {
    (void)model; (void)key; (void)override; return "fixture-provider";
}
const char *fixture_resolve_key(const char *provider, const char *key) {
    (void)provider; return key;
}
void fixture_debug_request(const char *provider, const char *model, const char *key) {
    (void)provider; (void)model; (void)key;
}
static char *fixture_provider_build(provider_t *provider, conversation_t *conv,
                                    session_state_t *session, int max_tokens, const char *key) {
    (void)provider; (void)conv; (void)session; (void)max_tokens; (void)key;
    return strdup("{\"fixture\":\"worker\"}");
}
provider_t *fixture_provider_create(const char *name) {
    (void)name;
    static provider_t provider;
    provider.name = "fixture-provider";
    provider.build_request = fixture_provider_build;
    provider.stream = fixture_provider_stream;
    return &provider;
}
bool fixture_provider_prepare(provider_t *provider) { (void)provider; return true; }
void fixture_provider_free(provider_t *provider) { (void)provider; }

static stream_result_t fixture_result(bool tool_turn, bool worker) {
    stream_result_t r = {.ok=true, .http_status=200, .cost_reported=!tool_turn,
                         .cost_usd=tool_turn ? 0 : 0.2};
    r.usage.input_tokens = 7;
    r.usage.output_tokens = 4;
    r.parsed.blocks = calloc(tool_turn ? 3 : 2, sizeof(content_block_t));
    assert(r.parsed.blocks);
    r.parsed.count = tool_turn ? 3 : 2;
    if (tool_turn) {
        r.parsed.blocks[0].type = strdup("text");
        r.parsed.blocks[0].text = strdup(worker ? "WORKER_BEFORE" : "SINGLE_BEFORE");
        r.parsed.blocks[1].type = strdup("thinking");
        r.parsed.blocks[1].text = strdup("HIDDEN_ORCHESTRATOR_THOUGHT");
        r.parsed.blocks[2].type = strdup("tool_use");
        r.parsed.blocks[2].tool_name = strdup("fixture_tool");
        r.parsed.blocks[2].tool_id = strdup(worker ? "worker-tool-id" : "single-tool-id");
        r.parsed.blocks[2].tool_input = strdup("{}");
    } else {
        r.parsed.blocks[0].type = strdup("text");
        r.parsed.blocks[0].text = strdup(worker ? "WORKER_" : "SINGLE_");
        r.parsed.blocks[1].type = strdup("text");
        r.parsed.blocks[1].text = strdup("FINAL");
    }
    r.parsed.stop_reason = strdup(tool_turn ? "tool_use" : "end_turn");
    return r;
}
stream_result_t fixture_provider_stream(provider_t *provider, const char *key,
    const char *request, stream_text_cb text_cb, stream_tool_start_cb tool_cb,
    stream_tool_arg_delta_cb arg_cb, stream_thinking_cb thinking_cb, void *ctx) {
    (void)provider; (void)key; (void)request; (void)text_cb; (void)tool_cb;
    (void)arg_cb; (void)thinking_cb; (void)ctx;
    return fixture_result(worker_turns++ == 0, true);
}
char *fixture_build_request(conversation_t *conv, session_state_t *session,
                             int max_tokens, const char *credential) {
    (void)conv; (void)session; (void)max_tokens; (void)credential;
    return strdup("{\"fixture\":\"single_tool\"}");
}
stream_result_t fixture_llm_stream(const char *key, const char *request,
    stream_text_cb text_cb, stream_tool_start_cb tool_cb,
    stream_tool_arg_delta_cb arg_cb, stream_thinking_cb thinking_cb, void *ctx) {
    (void)key; (void)request; (void)text_cb; (void)tool_cb;
    (void)arg_cb; (void)thinking_cb; (void)ctx;
    return fixture_result(single_turns++ == 0, false);
}
bool fixture_allowed(const char *name, const char *tier, char *reason, size_t len) {
    (void)name; (void)tier; (void)reason; (void)len; return true;
}
bool fixture_execute_tier(const char *name, const char *input, const char *tier,
                          char *result, size_t len) {
    (void)name; (void)input; (void)tier;
    tool_calls++;
    snprintf(result, len, "SYNTHETIC_TOOL_RESULT");
    return true;
}
bool fixture_execute(const char *name, const char *input, char *result, size_t len) {
    return fixture_execute_tier(name, input, "standard", result, len);
}
int fixture_lookup_index(const char *name) { (void)name; return 0; }
bool fixture_agent_run(const char *key, const char *model, const char *topology,
                       bool auto_topology, const char *override) {
    (void)key; (void)model; (void)topology; (void)auto_topology; (void)override;
    return false;
}

int main(void) {
    assert(chronicle_start(&(chronicle_start_opts_t){.provider="fixture", .model="fixture"}));
    char *worker = run_worker_task("FIXTURE_WORKER_TASK", "claude-sonnet-4-5-20250929");
    char *single = run_virtual_tool("FIXTURE_SINGLE_TASK", "fixture_tool");
    assert(worker && single && !strcmp(worker, "WORKER_FINAL") && !strcmp(single, "SINGLE_FINAL"));
    assert(worker_turns == 2 && single_turns == 2 && tool_calls == 2);
    free(worker); free(single);
    chronicle_stop();
    puts("PASS: orchestrator worker and single-tool loops ran two synthetic turns each");
    return 0;
}
