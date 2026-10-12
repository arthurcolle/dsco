/* Feed the production standalone Realtime event dispatcher without a socket. */
#include <assert.h>
#define tools_execute_for_tier fixture_execute_tool
#include "../src/realtime.c"

bool fixture_execute_tool(const char *name, const char *args, const char *tier,
                          char *result, size_t result_len) {
    (void)args; (void)tier;
    bool ok = strcmp(name, "fixture_fail") != 0;
    snprintf(result, result_len, "%s", ok ? "SYNTHETIC_TOOL_OK" : "SYNTHETIC_TOOL_FAILED");
    return ok;
}

static void fixture_wait_tools(rt_session_t *s) {
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&s->outbox_mu);
        bool done = s->tools_inflight == 0;
        pthread_mutex_unlock(&s->outbox_mu);
        if (done) return;
        usleep(1000);
    }
    assert(!"synthetic voice tool did not finish");
}

vm_t g_vm;
volatile int g_interrupted;
int g_cheap_mode;
double g_cost_budget;

int main(void) {
    realtime_opts_t opts = {0};
    rt_session_t s = {0};
    s.opts = &opts;
    s.model = "gpt-realtime-2";
    s.no_tools = true;
    jbuf_init(&s.chron_text, 256);
    rt_ring_init(&s.spk);
    assert(pthread_mutex_init(&s.outbox_mu, NULL) == 0);
    rt_dispatch(&s, "{\"type\":\"response.output_text.delta\",\"delta\":\"STANDALONE_OK\"}");
    rt_dispatch(&s, "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\","
                    "\"usage\":{\"input_tokens\":5,\"output_tokens\":2}}}");
    rt_dispatch(&s, "{\"type\":\"response.output_audio.delta\",\"delta\":\"U0VDUkVUX0FVRElP\"}");
    rt_dispatch(&s, "{\"type\":\"response.output_text.delta\",\"delta\":\"STANDALONE_FAILED\"}");
    rt_dispatch(&s, "{\"type\":\"response.done\",\"response\":{\"status\":\"failed\"}}");
    rt_dispatch(&s, "{\"type\":\"response.output_text.delta\",\"delta\":\"STANDALONE_INCOMPLETE\"}");
    rt_dispatch(&s, "{\"type\":\"response.done\",\"response\":{\"status\":\"incomplete\"}}");
    rt_dispatch(&s, "{\"type\":\"response.output_text.delta\",\"delta\":\"STANDALONE_TRUNCATED_PREFIX\"}");
    const char *prefix = "{\"type\":\"response.output_text.delta\",\"delta\":\"";
    size_t prefix_len = strlen(prefix), large_len = 256 * 1024;
    char *large_event = malloc(prefix_len + large_len + 3);
    assert(large_event);
    memcpy(large_event, prefix, prefix_len);
    memset(large_event + prefix_len, 'Z', large_len);
    memcpy(large_event + prefix_len + large_len, "\"}", 3);
    rt_dispatch(&s, large_event);
    free(large_event);
    rt_dispatch(&s, "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}");
    rt_dispatch(&s, "{\"type\":\"response.function_call_arguments.done\","
                    "\"call_id\":\"voice-tool-ok\",\"name\":\"fixture_ok\",\"arguments\":\"{}\"}");
    fixture_wait_tools(&s);
    rt_dispatch(&s, "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}");
    rt_dispatch(&s, "{\"type\":\"response.output_text.delta\",\"delta\":\"MIXED_VOICE_TEXT\"}");
    rt_dispatch(&s, "{\"type\":\"response.function_call_arguments.done\","
                    "\"call_id\":\"voice-tool-fail\",\"name\":\"fixture_fail\",\"arguments\":\"{}\"}");
    fixture_wait_tools(&s);
    rt_dispatch(&s, "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}");
    assert(s.chronicle_started && s.chron_text.len == 0);
    for (size_t i = 0; i < s.outbox_n; i++) free(s.outbox[i]);
    free(s.outbox);
    chronicle_stop();
    jbuf_free(&s.chron_text);
    rt_ring_free(&s.spk);
    pthread_mutex_destroy(&s.outbox_mu);
    puts("PASS: standalone Realtime synthetic text and outcome events captured");
    return 0;
}
