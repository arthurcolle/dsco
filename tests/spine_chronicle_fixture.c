/* The production slim command loop with a synthetic, in-process model stream. */
#include <assert.h>
#define llm_stream fixture_llm_stream
#define main fixture_slim_entry
#include "../src/spine_dsco_slim.c"
#undef main
#undef llm_stream

stream_result_t fixture_llm_stream(const char *key, const char *request,
                                   stream_text_cb text_cb, stream_tool_start_cb tool_cb,
                                   stream_tool_arg_delta_cb tool_delta_cb,
                                   stream_thinking_cb thinking_cb, void *ctx) {
    (void)key; (void)request; (void)tool_cb; (void)tool_delta_cb; (void)thinking_cb;
    bool partial = getenv("DSCO_SLIM_FIXTURE_PARTIAL") != NULL;
    const char *answer = partial ? "SLIM_PARTIAL" : "SLIM_VISIBLE";
    if (text_cb) text_cb(answer, ctx);
    stream_result_t r = {.ok=!partial, .http_status=200,
                         .cost_usd=0.125, .cost_reported=!partial,
                         .visible_text_streamed=true,
                         .diagnostic_reason=partial ? STREAM_DIAG_MISSING_TERMINAL : STREAM_DIAG_NONE};
    r.usage.input_tokens = 11;
    r.usage.output_tokens = 3;
    r.parsed.blocks = calloc(2, sizeof(content_block_t));
    assert(r.parsed.blocks);
    r.parsed.count = 2;
    r.parsed.blocks[0].type = strdup("thinking");
    r.parsed.blocks[0].text = strdup("HIDDEN_SLIM_THOUGHT");
    r.parsed.blocks[1].type = strdup("text");
    r.parsed.blocks[1].text = strdup(answer);
    r.parsed.stop_reason = strdup(partial ? "stream_ended" : "end_turn");
    r.generation_id = strdup("slim-fixture-generation");
    return r;
}

int main(int argc, char **argv) {
    assert(argc == 2 && (!strcmp(argv[1], "complete") || !strcmp(argv[1], "partial")));
    assert(setenv("ANTHROPIC_API_KEY", "fixture-only", 1) == 0);
    if (!strcmp(argv[1], "partial")) assert(setenv("DSCO_SLIM_FIXTURE_PARTIAL", "1", 1) == 0);
    char *args[] = {"spine-dsco-slim", "-c", "? FIXTURE_PROMPT", NULL};
    return fixture_slim_entry(3, args);
}
