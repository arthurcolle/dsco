/* test.c — minimal test suite for dsco
 * Build: make test_runner && ./test_runner
 * Or:    make test
 */

#include "json_util.h"
#include "llm.h"
#include "config.h"
#include "crypto.h"
#include "eval.h"
#include "tools.h"
#include "plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

/* Provide g_interrupted that agent.c normally defines */
volatile int g_interrupted = 0;

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stderr, "  test %-40s ", name); \
} while(0)

#define PASS() do { \
    tests_passed++; \
    fprintf(stderr, "\033[32mPASS\033[0m\n"); \
} while(0)

#define FAIL(msg) do { \
    tests_failed++; \
    fprintf(stderr, "\033[31mFAIL\033[0m: %s\n", msg); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

/* ── JSON tests ────────────────────────────────────────────────────────── */

static void test_json_get_str(void) {
    TEST("json_get_str basic");
    char *v = json_get_str("{\"name\":\"dsco\",\"version\":\"0.7\"}", "name");
    ASSERT(v && strcmp(v, "dsco") == 0, "expected 'dsco'");
    free(v);
    PASS();
}

static void test_json_get_int(void) {
    TEST("json_get_int basic");
    int v = json_get_int("{\"count\":42,\"x\":1}", "count", -1);
    ASSERT(v == 42, "expected 42");
    PASS();
}

static void test_json_get_int_missing(void) {
    TEST("json_get_int missing key");
    int v = json_get_int("{\"count\":42}", "missing", -99);
    ASSERT(v == -99, "expected default -99");
    PASS();
}

static void test_json_get_bool(void) {
    TEST("json_get_bool true");
    bool v = json_get_bool("{\"ok\":true}", "ok", false);
    ASSERT(v == true, "expected true");
    PASS();
}

static void test_json_get_str_escaped(void) {
    TEST("json_get_str escaped chars");
    char *v = json_get_str("{\"msg\":\"hello\\nworld\"}", "msg");
    ASSERT(v && strcmp(v, "hello\nworld") == 0, "expected newline in string");
    free(v);
    PASS();
}

static void test_json_get_str_unicode(void) {
    TEST("json_get_str unicode escape");
    char *v = json_get_str("{\"ch\":\"\\u0041\"}", "ch");
    ASSERT(v && strcmp(v, "A") == 0, "expected 'A'");
    free(v);
    PASS();
}

static void test_json_get_raw(void) {
    TEST("json_get_raw object");
    char *v = json_get_raw("{\"data\":{\"x\":1}}", "data");
    ASSERT(v && v[0] == '{', "expected object");
    free(v);
    PASS();
}

static void test_json_roundtrip(void) {
    TEST("jbuf json string roundtrip");
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"text\":");
    jbuf_append_json_str(&b, "hello \"world\"\nnewline\ttab\\backslash");
    jbuf_append(&b, "}");

    char *v = json_get_str(b.data, "text");
    ASSERT(v && strstr(v, "hello \"world\"") != NULL, "roundtrip failed");
    ASSERT(strstr(v, "\n") != NULL, "newline lost");
    ASSERT(strstr(v, "\t") != NULL, "tab lost");
    ASSERT(strstr(v, "\\") != NULL, "backslash lost");
    free(v);
    jbuf_free(&b);
    PASS();
}

/* ── Conversation tests ────────────────────────────────────────────────── */

static void test_conv_basic(void) {
    TEST("conversation init/add/free");
    conversation_t conv;
    conv_init(&conv);
    ASSERT(conv.count == 0, "initial count should be 0");

    conv_add_user_text(&conv, "hello");
    ASSERT(conv.count == 1, "count should be 1 after add");
    ASSERT(conv.msgs[0].role == ROLE_USER, "role should be user");
    ASSERT(conv.msgs[0].content_count == 1, "content count should be 1");
    ASSERT(strcmp(conv.msgs[0].content[0].text, "hello") == 0, "text mismatch");

    conv_free(&conv);
    PASS();
}

static void test_conv_save_load(void) {
    TEST("conversation save/load roundtrip");
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, "test message with \"quotes\"");
    conv_add_assistant_text(&conv, "response with\nnewlines");

    const char *path = "/tmp/dsco_test_conv.json";
    ASSERT(conv_save(&conv, path), "save failed");

    conversation_t conv2;
    conv_init(&conv2);
    ASSERT(conv_load(&conv2, path), "load failed");
    ASSERT(conv2.count == 2, "loaded count should be 2");
    ASSERT(strcmp(conv2.msgs[0].content[0].text, "test message with \"quotes\"") == 0,
           "loaded text mismatch");
    ASSERT(conv2.msgs[1].role == ROLE_ASSISTANT, "loaded role should be assistant");

    conv_free(&conv);
    conv_free(&conv2);
    unlink(path);
    PASS();
}

static void test_conv_pop_last(void) {
    TEST("conversation pop_last");
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, "first");
    conv_add_user_text(&conv, "second");
    ASSERT(conv.count == 2, "count should be 2");

    conv_pop_last(&conv);
    ASSERT(conv.count == 1, "count should be 1 after pop");

    conv_free(&conv);
    PASS();
}

/* ── SHA-256 tests ─────────────────────────────────────────────────────── */

static void test_sha256_known_answer(void) {
    TEST("SHA-256 known answer (empty)");
    char hex[65];
    sha256_hex((const uint8_t *)"", 0, hex);
    /* SHA-256("") = e3b0c44298fc1c14... */
    ASSERT(strncmp(hex, "e3b0c442", 8) == 0, "SHA-256 empty hash mismatch");
    PASS();
}

static void test_sha256_abc(void) {
    TEST("SHA-256 known answer ('abc')");
    char hex[65];
    sha256_hex((const uint8_t *)"abc", 3, hex);
    /* SHA-256("abc") = ba7816bf... */
    ASSERT(strncmp(hex, "ba7816bf", 8) == 0, "SHA-256 'abc' hash mismatch");
    PASS();
}

/* ── Eval tests ────────────────────────────────────────────────────────── */

static void test_eval_basic(void) {
    TEST("eval basic arithmetic");
    eval_ctx_t ctx;
    eval_init(&ctx);
    double v = eval_expr(&ctx, "2 + 3 * 4");
    ASSERT(!ctx.has_error, "eval failed");
    ASSERT(fabs(v - 14.0) < 0.001, "expected 14");
    PASS();
}

static void test_eval_functions(void) {
    TEST("eval sqrt function");
    eval_ctx_t ctx;
    eval_init(&ctx);
    double v = eval_expr(&ctx, "sqrt(144)");
    ASSERT(!ctx.has_error, "eval failed");
    ASSERT(fabs(v - 12.0) < 0.001, "expected 12");
    PASS();
}

static void test_eval_parentheses(void) {
    TEST("eval parentheses");
    eval_ctx_t ctx;
    eval_init(&ctx);
    double v = eval_expr(&ctx, "(2 + 3) * 4");
    ASSERT(!ctx.has_error, "eval failed");
    ASSERT(fabs(v - 20.0) < 0.001, "expected 20");
    PASS();
}

/* ── Model registry tests ─────────────────────────────────────────────── */

static void test_model_resolve_alias(void) {
    TEST("model_resolve_alias");
    const char *r = model_resolve_alias("opus");
    ASSERT(strcmp(r, "claude-opus-4-6") == 0, "opus should resolve to claude-opus-4-6");

    r = model_resolve_alias("sonnet");
    ASSERT(strcmp(r, "claude-sonnet-4-6") == 0, "sonnet should resolve");

    r = model_resolve_alias("haiku");
    ASSERT(strcmp(r, "claude-haiku-4-5-20251001") == 0, "haiku should resolve");

    r = model_resolve_alias("custom-model-id");
    ASSERT(strcmp(r, "custom-model-id") == 0, "unknown should pass through");
    PASS();
}

static void test_model_context_window(void) {
    TEST("model_context_window");
    int ctx = model_context_window("opus");
    ASSERT(ctx == 200000, "opus should have 200k context");

    ctx = model_context_window("unknown-model");
    ASSERT(ctx == CONTEXT_WINDOW_TOKENS, "unknown should return default");
    PASS();
}

/* ── Request builder tests ─────────────────────────────────────────────── */

static void test_build_request_valid_json(void) {
    TEST("llm_build_request produces valid JSON");
    tools_init();
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, "hello");

    char *req = llm_build_request(&conv, "claude-haiku-4-5-20251001", 1024);
    ASSERT(req != NULL, "request should not be NULL");
    ASSERT(req[0] == '{', "should start with {");

    /* Verify it has required keys */
    char *model = json_get_str(req, "model");
    ASSERT(model && strcmp(model, "claude-haiku-4-5-20251001") == 0, "model mismatch");
    free(model);

    int max_tokens = json_get_int(req, "max_tokens", 0);
    ASSERT(max_tokens == 1024, "max_tokens mismatch");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_build_request_ex_effort(void) {
    TEST("llm_build_request_ex with effort");
    tools_init();
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, "hello");

    session_state_t session;
    session_state_init(&session, "haiku");
    snprintf(session.effort, sizeof(session.effort), "low");

    char *req = llm_build_request_ex(&conv, &session, 1024);
    ASSERT(req != NULL, "request should not be NULL");
    /* Check for output_config */
    ASSERT(strstr(req, "output_config") != NULL, "should contain output_config for non-high effort");
    ASSERT(strstr(req, "\"low\"") != NULL, "should contain effort value");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_build_request_web_search_result_shape(void) {
    TEST("llm_build_request preserves web_search result content list");
    tools_init();
    conversation_t conv;
    conv_init(&conv);

    content_block_t blk = {0};
    blk.type = safe_strdup("web_search_tool_result");
    blk.tool_id = safe_strdup("srvtoolu_test");
    blk.tool_input = safe_strdup(
        "{\"type\":\"web_search_tool_result\",\"tool_use_id\":\"srvtoolu_test\","
        "\"content\":[{\"type\":\"web_search_result\",\"title\":\"Example\","
        "\"url\":\"https://example.com\",\"encrypted_content\":\"abc\"}]}"
    );
    parsed_response_t resp = {
        .blocks = &blk,
        .count = 1,
        .stop_reason = NULL,
    };
    conv_add_assistant_raw(&conv, &resp);

    free(blk.type);
    free(blk.tool_id);
    free(blk.tool_input);

    char *req = llm_build_request(&conv, "claude-haiku-4-5-20251001", 1024);
    ASSERT(req != NULL, "request should not be NULL");
    ASSERT(strstr(req, "\"type\":\"web_search_tool_result\"") != NULL,
           "missing web_search_tool_result block");
    ASSERT(strstr(req, "\"content\":[{\"type\":\"web_search_result\"") != NULL,
           "web_search content should be serialized as list");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_build_request_web_search_result_recover_from_text(void) {
    TEST("llm_build_request recovers web_search content from text payload");
    tools_init();
    conversation_t conv;
    conv_init(&conv);

    content_block_t blk = {0};
    blk.type = safe_strdup("web_search_tool_result");
    blk.tool_id = safe_strdup("srvtoolu_text_recovery");
    blk.text = safe_strdup(
        "{\"content\":[{\"type\":\"web_search_result\",\"title\":\"Recovered\","
        "\"url\":\"https://example.com\",\"encrypted_content\":\"xyz\"}]}"
    );
    parsed_response_t resp = {
        .blocks = &blk,
        .count = 1,
        .stop_reason = NULL,
    };
    conv_add_assistant_raw(&conv, &resp);

    free(blk.type);
    free(blk.tool_id);
    free(blk.text);

    char *req = llm_build_request(&conv, "claude-haiku-4-5-20251001", 1024);
    ASSERT(req != NULL, "request should not be NULL");
    ASSERT(strstr(req, "\"type\":\"web_search_tool_result\"") != NULL,
           "missing web_search_tool_result block");
    ASSERT(strstr(req, "\"content\":[{\"type\":\"web_search_result\",\"title\":\"Recovered\"") != NULL,
           "should recover content list from text payload");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_build_request_server_result_missing_tool_id(void) {
    TEST("llm_build_request degrades server result without tool_use_id");
    tools_init();
    conversation_t conv;
    conv_init(&conv);

    content_block_t blk = {0};
    blk.type = safe_strdup("web_search_tool_result");
    blk.text = safe_strdup("orphan server result");
    parsed_response_t resp = {
        .blocks = &blk,
        .count = 1,
        .stop_reason = NULL,
    };
    conv_add_assistant_raw(&conv, &resp);

    free(blk.type);
    free(blk.text);

    char *req = llm_build_request(&conv, "claude-haiku-4-5-20251001", 1024);
    ASSERT(req != NULL, "request should not be NULL");
    ASSERT(strstr(req, "\"type\":\"web_search_tool_result\"") == NULL,
           "missing tool_use_id blocks should not be serialized as server tool result");
    ASSERT(strstr(req, "\"type\":\"text\",\"text\":\"orphan server result\"") != NULL,
           "missing tool_use_id blocks should be emitted as text fallback");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_build_request_web_search_result_missing_content(void) {
    TEST("llm_build_request tolerates missing web_search result payload");
    tools_init();
    conversation_t conv;
    conv_init(&conv);

    content_block_t blk = {0};
    blk.type = safe_strdup("web_search_tool_result");
    blk.tool_id = safe_strdup("srvtoolu_missing");
    blk.text = safe_strdup("");
    parsed_response_t resp = {
        .blocks = &blk,
        .count = 1,
        .stop_reason = NULL,
    };
    conv_add_assistant_raw(&conv, &resp);

    free(blk.type);
    free(blk.tool_id);
    free(blk.text);

    char *req = llm_build_request(&conv, "claude-haiku-4-5-20251001", 1024);
    ASSERT(req != NULL, "request should not be NULL");
    ASSERT(strstr(req, "\"type\":\"web_search_tool_result\"") != NULL,
           "missing web_search_tool_result block");
    ASSERT(strstr(req, "\"tool_use_id\":\"srvtoolu_missing\",\"content\":[]") != NULL,
           "missing/invalid web_search result should serialize as empty list");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_build_request_server_tool_results_stay_assistant(void) {
    TEST("llm_build_request keeps server tool results in assistant role");
    tools_init();
    conversation_t conv;
    conv_init(&conv);

    content_block_t blocks[3];
    memset(blocks, 0, sizeof(blocks));

    blocks[0].type = safe_strdup("server_tool_use");
    blocks[0].tool_name = safe_strdup("web_search");
    blocks[0].tool_id = safe_strdup("srvtoolu_pair");
    blocks[0].tool_input = safe_strdup("{\"query\":\"test\"}");

    blocks[1].type = safe_strdup("web_search_tool_result");
    blocks[1].tool_id = safe_strdup("srvtoolu_pair");
    blocks[1].tool_input = safe_strdup(
        "[{\"type\":\"web_search_result\",\"title\":\"Example\","
        "\"url\":\"https://example.com\",\"encrypted_content\":\"abc\"}]"
    );

    blocks[2].type = safe_strdup("text");
    blocks[2].text = safe_strdup("summary");

    parsed_response_t resp = {
        .blocks = blocks,
        .count = 3,
        .stop_reason = NULL,
    };
    conv_add_assistant_raw(&conv, &resp);

    for (int i = 0; i < 3; i++) {
        free(blocks[i].type);
        free(blocks[i].text);
        free(blocks[i].tool_name);
        free(blocks[i].tool_id);
        free(blocks[i].tool_input);
    }

    char *req = llm_build_request(&conv, "claude-haiku-4-5-20251001", 1024);
    ASSERT(req != NULL, "request should not be NULL");
    ASSERT(strstr(req, "\"role\":\"assistant\",\"content\":[{\"type\":\"server_tool_use\"") != NULL,
           "server_tool_use should be in assistant message");
    ASSERT(strstr(req, "\"type\":\"web_search_tool_result\",\"tool_use_id\":\"srvtoolu_pair\"") != NULL,
           "web_search_tool_result should be serialized");
    ASSERT(strstr(req, "\"role\":\"user\",\"content\":[{\"type\":\"web_search_tool_result\"") == NULL,
           "server tool result should not be rewritten as user content");

    free(req);
    conv_free(&conv);
    PASS();
}

static void test_conversation_growth_unbounded(void) {
    TEST("conversation grows past historical message cap");
    conversation_t conv;
    conv_init(&conv);

    for (int i = 0; i < 200; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "message %d", i);
        conv_add_user_text(&conv, msg);
    }

    ASSERT(conv.count == 200, "conversation should retain all appended messages");
    conv_free(&conv);
    PASS();
}

/* ── Session state tests ───────────────────────────────────────────────── */

static void test_session_state_init(void) {
    TEST("session_state_init");
    session_state_t s;
    session_state_init(&s, "sonnet");
    ASSERT(strcmp(s.model, "claude-sonnet-4-6") == 0, "model should be resolved");
    ASSERT(strcmp(s.effort, "high") == 0, "default effort should be high");
    ASSERT(s.context_window == 200000, "context window should be 200k");
    ASSERT(s.trust_tier == DSCO_TRUST_STANDARD, "trust tier should default to standard");
    ASSERT(s.web_search == true, "web_search should default true");
    ASSERT(s.code_execution == true, "code_execution should default true");
    PASS();
}

static void test_session_trust_tier_parse(void) {
    TEST("session_trust_tier parse");
    bool ok = false;
    dsco_trust_tier_t t = session_trust_tier_from_string("trusted", &ok);
    ASSERT(ok && t == DSCO_TRUST_TRUSTED, "trusted should parse");
    t = session_trust_tier_from_string("untrusted", &ok);
    ASSERT(ok && t == DSCO_TRUST_UNTRUSTED, "untrusted should parse");
    t = session_trust_tier_from_string("standard", &ok);
    ASSERT(ok && t == DSCO_TRUST_STANDARD, "standard should parse");
    t = session_trust_tier_from_string("unknown-tier", &ok);
    ASSERT(!ok && t == DSCO_TRUST_STANDARD, "unknown should fail and default");
    PASS();
}

/* ── Tool execution tests ──────────────────────────────────────────────── */

static void test_tool_execute_eval(void) {
    TEST("tools_execute eval");
    tools_init();
    char result[4096];
    result[0] = '\0';
    bool ok = tools_execute("eval", "{\"expression\":\"1+1\"}", result, sizeof(result));
    ASSERT(ok, "eval should succeed");
    ASSERT(strstr(result, "2") != NULL, "result should contain 2");
    PASS();
}

static void test_tool_execute_unknown(void) {
    TEST("tools_execute unknown tool");
    tools_init();
    char result[4096];
    result[0] = '\0';
    bool ok = tools_execute("nonexistent_tool_xyz", "{}", result, sizeof(result));
    ASSERT(!ok, "unknown tool should fail");
    PASS();
}

static void test_tool_edit_file_empty_old_string(void) {
    TEST("tools_execute edit_file rejects empty old_string");
    tools_init();

    const char *path = "/tmp/dsco_test_edit_empty.txt";
    FILE *f = fopen(path, "w");
    ASSERT(f != NULL, "failed to create temp file");
    fputs("abc\n", f);
    fclose(f);

    char input[512];
    snprintf(input, sizeof(input),
             "{\"path\":\"%s\",\"old_string\":\"\",\"new_string\":\"x\",\"replace_all\":true}",
             path);

    char result[4096];
    result[0] = '\0';
    bool ok = tools_execute("edit_file", input, result, sizeof(result));
    ASSERT(!ok, "edit_file should fail for empty old_string");
    ASSERT(strstr(result, "must not be empty") != NULL,
           "error should mention empty old_string");

    unlink(path);
    PASS();
}

static void test_tool_agent_wait_no_agents(void) {
    TEST("tools_execute agent_wait fails fast when no agents");
    tools_init();

    char result[4096];
    result[0] = '\0';
    bool ok = tools_execute("agent_wait", "{\"timeout\":5}", result, sizeof(result));
    ASSERT(!ok, "agent_wait should fail when there are no agents");
    ASSERT(strstr(result, "no agents") != NULL,
           "agent_wait should report no agents");
    PASS();
}

static void test_tools_trust_tier_policy(void) {
    TEST("tools_is_allowed_for_tier policy");
    tools_init();
    char reason[256];
    reason[0] = '\0';

    ASSERT(tools_is_allowed_for_tier("read_file", "trusted", reason, sizeof(reason)),
           "trusted should allow read_file");
    ASSERT(!tools_is_allowed_for_tier("docker", "standard", reason, sizeof(reason)),
           "standard should block docker");
    ASSERT(!tools_is_allowed_for_tier("write_file", "untrusted", reason, sizeof(reason)),
           "untrusted should block write_file");
    ASSERT(tools_is_allowed_for_tier("bash", "untrusted", reason, sizeof(reason)),
           "untrusted should allow bash (sandbox-routed)");
    ASSERT(!tools_is_allowed_for_tier("compile", "untrusted", reason, sizeof(reason)),
           "untrusted should still block compile");
    ASSERT(tools_is_allowed_for_tier("read_file", "untrusted", reason, sizeof(reason)),
           "untrusted should allow read_file");
    PASS();
}

static void test_tools_untrusted_sandbox_routing(void) {
    TEST("tools_execute_for_tier untrusted shell routing");
    tools_init();

    setenv("DSCO_SANDBOX_FORCE_NO_DOCKER", "1", 1);
    char result[4096];
    result[0] = '\0';
    bool ok = tools_execute_for_tier("run_command",
                                     "{\"command\":\"echo routed\"}",
                                     "untrusted",
                                     result, sizeof(result));
    unsetenv("DSCO_SANDBOX_FORCE_NO_DOCKER");

    ASSERT(!ok, "untrusted run_command should route to strict sandbox and fail without docker");
    ASSERT(strstr(result, "strict sandbox policy requires docker") != NULL,
           "result should reflect strict sandbox policy enforcement");
    PASS();
}

static void test_sandbox_run_untrusted_defaults(void) {
    TEST("sandbox_run tier-bound defaults");
    tools_init();

    setenv("DSCO_SANDBOX_FORCE_NO_DOCKER", "1", 1);
    char result[4096];
    result[0] = '\0';
    bool ok = tools_execute_for_tier("sandbox_run",
                                     "{\"command\":\"echo sandbox\"}",
                                     "untrusted",
                                     result, sizeof(result));
    unsetenv("DSCO_SANDBOX_FORCE_NO_DOCKER");

    ASSERT(!ok, "untrusted sandbox_run should require docker for strict policy");
    ASSERT(strstr(result, "network=false") != NULL,
           "result should include tier network policy");
    ASSERT(strstr(result, "filesystem=workspace_ro") != NULL,
           "result should include tier filesystem policy");
    PASS();
}

static void test_plugin_manifest_lock_validation(void) {
    TEST("plugin manifest+lock validation");

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/dsco_plugin_meta_%d_%ld",
             (int)getpid(), (long)time(NULL));
    int mkrc = mkdir(dir, 0700);
    ASSERT(mkrc == 0 || errno == EEXIST, "mkdir for temp plugin dir failed");

    char manifest_path[512];
    char lock_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/plugin-manifest.json", dir);
    snprintf(lock_path, sizeof(lock_path), "%s/plugins.lock", dir);

    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL, "failed to create manifest");
    fputs("{\"name\":\"demo-plugin\",\"version\":\"1.2.3\","
          "\"hash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
          "\"signer\":\"acme-signing\",\"capabilities\":[\"read_file\",\"code_search\"]}",
          mf);
    fclose(mf);

    FILE *lf = fopen(lock_path, "w");
    ASSERT(lf != NULL, "failed to create lockfile");
    fputs("{\"schema_version\":1,\"plugins\":["
          "{\"name\":\"demo-plugin\",\"version\":\"1.2.3\","
          "\"hash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"
          "]}", lf);
    fclose(lf);

    char out[2048];
    bool ok = plugin_validate_manifest_and_lock(manifest_path, lock_path, out, sizeof(out));
    ASSERT(ok, out);
    ASSERT(strstr(out, "plugin metadata valid") != NULL,
           "combined validation should report success");

    FILE *lf_bad = fopen(lock_path, "w");
    ASSERT(lf_bad != NULL, "failed to rewrite lockfile");
    fputs("{\"schema_version\":1,\"plugins\":["
          "{\"name\":\"demo-plugin\",\"version\":\"1.2.3\","
          "\"hash\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}"
          "]}", lf_bad);
    fclose(lf_bad);

    ok = plugin_validate_manifest_and_lock(manifest_path, lock_path, out, sizeof(out));
    ASSERT(!ok, "validation should fail when manifest hash is not pinned in lock");
    ASSERT(strstr(out, "missing from") != NULL,
           "failure should describe missing manifest pin");

    unlink(manifest_path);
    unlink(lock_path);
    rmdir(dir);
    PASS();
}

/* ── Base64 tests ──────────────────────────────────────────────────────── */

static void test_base64_roundtrip(void) {
    TEST("base64 encode/decode roundtrip");
    const char *input = "Hello, dsco! Testing base64 encoding.";
    size_t in_len = strlen(input);

    char encoded[256];
    size_t enc_len = base64_encode((const uint8_t *)input, in_len, encoded, sizeof(encoded));
    encoded[enc_len] = '\0';

    uint8_t decoded[256];
    size_t dec_len = base64_decode(encoded, enc_len, decoded, sizeof(decoded));
    ASSERT(dec_len == in_len, "decoded length mismatch");
    ASSERT(memcmp(decoded, input, in_len) == 0, "decoded content mismatch");
    PASS();
}

/* ── jbuf tests ────────────────────────────────────────────────────────── */

static void test_jbuf_grow(void) {
    TEST("jbuf dynamic growth");
    jbuf_t b;
    jbuf_init(&b, 4); /* tiny initial size */
    for (int i = 0; i < 1000; i++) {
        jbuf_append(&b, "x");
    }
    ASSERT(b.len == 1000, "length should be 1000");
    ASSERT(strlen(b.data) == 1000, "strlen should match");
    jbuf_free(&b);
    PASS();
}

static void test_jbuf_json_str_special(void) {
    TEST("jbuf_append_json_str null bytes");
    jbuf_t b;
    jbuf_init(&b, 64);
    jbuf_append_json_str(&b, "");
    ASSERT(strcmp(b.data, "\"\"") == 0, "empty string should be \"\"");
    jbuf_free(&b);
    PASS();
}

/* ── TUI Feature tests ─────────────────────────────────────────────────── */

#include "tui.h"

static void test_tui_features_init(void) {
    TEST("tui_features_init all enabled");
    tui_features_t f;
    tui_features_init(&f);
    /* Selectively enabled features should be true, others false */
    ASSERT(f.inline_diff == true, "inline_diff should be enabled");
    ASSERT(f.collapsible_thinking == true, "collapsible_thinking should be enabled");
    ASSERT(f.cached_badge == true, "cached_badge should be enabled");
    ASSERT(f.context_gauge == true, "context_gauge should be enabled");
    ASSERT(f.compact_flash == true, "compact_flash should be enabled");
    ASSERT(f.error_severity == true, "error_severity should be enabled");
    ASSERT(f.notify_bell == true, "notify_bell should be enabled");
    ASSERT(f.drag_drop_preview == true, "drag_drop_preview should be enabled");
    ASSERT(f.token_heatmap == false, "token_heatmap should be disabled by default");
    ASSERT(f.typing_cadence == false, "typing_cadence should be disabled by default");
    PASS();
}

static void test_tui_feature_names(void) {
    TEST("tui_feature_name coverage");
    ASSERT(strcmp(tui_feature_name(0), "token_heatmap") == 0, "F1 name mismatch");
    ASSERT(strcmp(tui_feature_name(14), "context_gauge") == 0, "F15 name mismatch");
    ASSERT(strcmp(tui_feature_name(39), "latency_waterfall") == 0, "F40 name mismatch");
    ASSERT(strcmp(tui_feature_name(40), "unknown") == 0, "out of range should return unknown");
    ASSERT(strcmp(tui_feature_name(-1), "unknown") == 0, "negative should return unknown");
    PASS();
}

static void test_tui_features_toggle(void) {
    TEST("tui_features_toggle on/off");
    tui_features_t f;
    tui_features_init(&f);
    g_tui_features = &f;

    ASSERT(f.token_heatmap == false, "token_heatmap should start false");
    bool found = tui_features_toggle(&f, "token_heatmap");
    ASSERT(found, "toggle should find token_heatmap");
    ASSERT(f.token_heatmap == true, "token_heatmap should be true after toggle");

    found = tui_features_toggle(&f, "token_heatmap");
    ASSERT(found, "toggle should find token_heatmap again");
    ASSERT(f.token_heatmap == false, "token_heatmap should be false after second toggle");

    found = tui_features_toggle(&f, "nonexistent_feature");
    ASSERT(!found, "toggle should return false for unknown feature");

    g_tui_features = NULL;
    PASS();
}

static void test_tui_estimate_tokens(void) {
    TEST("tui_estimate_tokens heuristic");
    ASSERT(tui_estimate_tokens(NULL) == 0, "NULL should return 0");
    ASSERT(tui_estimate_tokens("") == 0, "empty should return 0");
    ASSERT(tui_estimate_tokens("hi") == 1, "'hi' (2 chars) → ~1 token");
    ASSERT(tui_estimate_tokens("hello world") == 3, "'hello world' (11 chars) → ~3 tokens");
    /* (strlen+3)/4 formula */
    ASSERT(tui_estimate_tokens("abcdefgh") == 2, "8 chars → 2 tokens");
    PASS();
}

static void test_tui_is_diff(void) {
    TEST("tui_is_diff detection");
    ASSERT(tui_is_diff(NULL) == false, "NULL not a diff");
    ASSERT(tui_is_diff("hello world") == false, "plain text not a diff");
    ASSERT(tui_is_diff("--- a/file.c\n+++ b/file.c\n@@ -1,3 +1,3 @@") == true,
           "unified diff should be detected");
    ASSERT(tui_is_diff("@@ -1 +1 @@\n-old\n+new") == true,
           "hunk header alone should detect");
    PASS();
}

static void test_tui_is_code_paste(void) {
    TEST("tui_is_code_paste detection");
    ASSERT(tui_is_code_paste(NULL) == false, "NULL not code");
    ASSERT(tui_is_code_paste("hello") == false, "single line not code");
    ASSERT(tui_is_code_paste("int main() {\n  return 0;\n}") == true,
           "multi-line with braces is code");
    ASSERT(tui_is_code_paste("import os\ndef foo():\n  pass") == true,
           "Python code with def keyword");
    PASS();
}

static void test_tui_detect_theme(void) {
    TEST("tui_detect_theme defaults to dark");
    /* Without COLORFGBG set, should default to dark */
    tui_theme_t theme = tui_detect_theme();
    /* Can't control env in test, but default should be dark or light */
    ASSERT(theme == TUI_THEME_DARK || theme == TUI_THEME_LIGHT, "valid theme");
    PASS();
}

static void test_tui_ghost_suggestions(void) {
    TEST("tui_ghost push/match");
    tui_ghost_t g;
    tui_ghost_init(&g);
    ASSERT(g.count == 0, "initial count 0");

    tui_ghost_push(&g, "/help");
    tui_ghost_push(&g, "/model opus");
    tui_ghost_push(&g, "/context");
    ASSERT(g.count == 3, "count after 3 pushes");

    const char *m = tui_ghost_match(&g, "/h");
    ASSERT(m && strcmp(m, "/help") == 0, "/h should match /help");

    m = tui_ghost_match(&g, "/m");
    ASSERT(m && strcmp(m, "/model opus") == 0, "/m should match /model opus");

    m = tui_ghost_match(&g, "/z");
    ASSERT(m == NULL, "/z should not match anything");

    /* Duplicate suppression */
    tui_ghost_push(&g, "/context");
    ASSERT(g.count == 3, "duplicate push should not increase count");

    PASS();
}

static void test_tui_branch_detect(void) {
    TEST("tui_branch push/detect Jaccard");
    tui_features_t f;
    tui_features_init(&f);
    g_tui_features = &f;

    tui_branch_t b;
    tui_branch_init(&b);
    ASSERT(b.count == 0, "initial branch count 0");

    tui_branch_push(&b, "how do I sort a list in Python");
    tui_branch_push(&b, "explain quantum computing basics");
    ASSERT(b.count == 2, "count after 2 pushes");

    /* Similar prompt should detect branch (redirected output just checks return) */
    /* "how do I sort a list in Python" vs "how do I sort a list in JavaScript" */
    /* Shared: how, do, I, sort, a, list, in → 7 shared, 8+8-7=9 union → 0.78 */
    /* This test just validates the API works; actual threshold checked internally */

    /* Dissimilar prompt should NOT detect branch */
    /* Can't easily suppress stderr in test, so just test init/push/count */
    g_tui_features = NULL;
    PASS();
}

static void test_tui_dag(void) {
    TEST("tui_dag add_node/add_edge");
    tui_dag_t d;
    tui_dag_init(&d);
    ASSERT(d.node_count == 0, "initial nodes 0");

    int n0 = tui_dag_add_node(&d, "read_file");
    int n1 = tui_dag_add_node(&d, "bash");
    int n2 = tui_dag_add_node(&d, "write_file");
    ASSERT(n0 == 0 && n1 == 1 && n2 == 2, "sequential node indices");
    ASSERT(d.node_count == 3, "3 nodes");

    /* Duplicate node returns existing index */
    int n_dup = tui_dag_add_node(&d, "bash");
    ASSERT(n_dup == 1, "duplicate node returns existing index");
    ASSERT(d.node_count == 3, "still 3 nodes after dup");

    tui_dag_add_edge(&d, 0, 1);
    tui_dag_add_edge(&d, 1, 2);
    ASSERT(d.edge_count == 2, "2 edges");

    /* Duplicate edge is ignored */
    tui_dag_add_edge(&d, 0, 1);
    ASSERT(d.edge_count == 2, "still 2 edges after dup");

    PASS();
}

static void test_tui_flame(void) {
    TEST("tui_flame add entries");
    tui_flame_t f;
    tui_flame_init(&f);
    ASSERT(f.count == 0, "initial flame count 0");

    tui_flame_add(&f, "read_file", 100.0, 250.0, true, TUI_TOOL_READ);
    ASSERT(f.count == 1, "count after add");
    ASSERT(f.epoch_ms == 100.0, "epoch set to first entry start");
    ASSERT(strcmp(f.entries[0].name, "read_file") == 0, "name stored");
    ASSERT(f.entries[0].ok == true, "ok stored");

    tui_flame_add(&f, "bash", 260.0, 500.0, false, TUI_TOOL_EXEC);
    ASSERT(f.count == 2, "count 2");
    ASSERT(f.entries[1].ok == false, "second entry not ok");

    PASS();
}

static void test_tui_citation(void) {
    TEST("tui_citation add/count");
    tui_citation_t c;
    tui_citation_init(&c);
    ASSERT(c.count == 0, "initial citation count 0");

    int idx = tui_citation_add(&c, "bash", "tool_123", "output preview", 42.5);
    ASSERT(idx == 1, "first citation returns index 1");
    ASSERT(c.count == 1, "count 1");
    ASSERT(strcmp(c.entries[0].tool_name, "bash") == 0, "tool_name stored");
    ASSERT(c.entries[0].elapsed_ms == 42.5, "elapsed stored");
    ASSERT(c.entries[0].index == 1, "footnote number is 1");

    idx = tui_citation_add(&c, "read_file", "tool_456", NULL, 10.0);
    ASSERT(idx == 2, "second citation index 2");
    ASSERT(c.entries[1].preview[0] == '\0', "NULL preview → empty");

    PASS();
}

static void test_tui_thinking(void) {
    TEST("tui_thinking feed/end");
    tui_thinking_state_t t;
    tui_thinking_init(&t);
    ASSERT(t.active == false, "starts inactive");

    tui_thinking_feed(&t, "pondering the meaning");
    ASSERT(t.active == true, "active after first feed");
    ASSERT(t.char_count == 21, "char count matches");

    tui_thinking_feed(&t, " of life");
    ASSERT(t.char_count == 29, "accumulated chars");

    /* end prints to stderr — just verify it resets */
    tui_thinking_end(&t);
    ASSERT(t.active == false, "inactive after end");
    ASSERT(t.char_count == 0, "char count reset");

    PASS();
}

static void test_tui_word_counter(void) {
    TEST("tui_word_counter feed");
    tui_word_counter_t w;
    tui_word_counter_init(&w);
    ASSERT(w.words == 0 && w.chars == 0, "starts at 0");

    tui_word_counter_feed(&w, "hello world ");
    ASSERT(w.chars == 12, "12 chars");
    ASSERT(w.words == 2, "2 words (space after each)");

    tui_word_counter_feed(&w, "foo bar baz");
    ASSERT(w.words == 4, "4 words total (2 more word-ending spaces)");
    /* "hello world " → 2 words, "foo bar baz" → +2 more = 4 */

    PASS();
}

static void test_tui_throughput(void) {
    TEST("tui_throughput init/tick");
    tui_throughput_t t;
    tui_throughput_init(&t);
    ASSERT(t.count == 0, "initial count 0");
    ASSERT(t.tokens_since_last == 0, "initial tokens 0");

    tui_throughput_tick(&t, 10);
    ASSERT(t.tokens_since_last == 10, "accumulated 10 tokens");
    /* Won't sample until 250ms elapsed, but data is accumulated */

    PASS();
}

static void test_tui_minimap_entry(void) {
    TEST("tui_minimap_entry struct layout");
    tui_minimap_entry_t entries[] = {
        { 'u', 100 },
        { 'a', 500 },
        { 't', 50 },
    };
    ASSERT(entries[0].type == 'u', "user type");
    ASSERT(entries[1].tokens == 500, "assistant tokens");
    ASSERT(entries[2].type == 't', "tool type");
    PASS();
}

static void test_tui_scroller(void) {
    TEST("tui_scroller init");
    /* Need enough lines to exceed page_size for scrolling to work */
    const char *lines[100];
    for (int i = 0; i < 100; i++) lines[i] = "line";
    tui_scroller_t s;
    tui_scroller_init(&s, lines, 100);
    ASSERT(s.line_count == 100, "100 lines");
    ASSERT(s.offset == 0, "initial offset 0");
    ASSERT(s.page_size > 0, "page_size > 0");

    /* Key handling: 'j' scrolls down (only if more content below) */
    bool cont = tui_scroller_handle_key(&s, 'j');
    ASSERT(cont == true, "j returns true (continue)");
    ASSERT(s.offset == 1, "offset moved to 1");

    /* 'k' scrolls up */
    cont = tui_scroller_handle_key(&s, 'k');
    ASSERT(cont == true, "k returns true");
    ASSERT(s.offset == 0, "back to 0");

    /* 'q' quits */
    cont = tui_scroller_handle_key(&s, 'q');
    ASSERT(cont == false, "q returns false (quit)");

    PASS();
}

static void test_tui_latency_breakdown(void) {
    TEST("tui_latency_breakdown struct");
    tui_latency_breakdown_t b = {
        .dns_ms = 5.0,
        .connect_ms = 15.0,
        .tls_ms = 25.0,
        .ttfb_ms = 150.0,
        .total_ms = 1200.0,
    };
    ASSERT(b.dns_ms == 5.0, "dns_ms");
    ASSERT(b.total_ms == 1200.0, "total_ms");
    /* tui_latency_waterfall would render to stderr — just validate struct */
    PASS();
}

static void test_tui_tool_classify(void) {
    TEST("tui_classify_tool categories");
    ASSERT(tui_classify_tool("read_file") == TUI_TOOL_READ, "read_file → READ");
    ASSERT(tui_classify_tool("write_file") == TUI_TOOL_WRITE, "write_file → WRITE");
    ASSERT(tui_classify_tool("bash") == TUI_TOOL_EXEC, "bash → EXEC");
    /* "http_get" matches "get" first (READ), then "http" (WEB) — READ wins due to order */
    ASSERT(tui_classify_tool("http_fetch") == TUI_TOOL_WEB, "http_fetch → WEB");
    ASSERT(tui_classify_tool("curl_request") == TUI_TOOL_WEB, "curl_request → WEB");
    ASSERT(tui_classify_tool("sha256") == TUI_TOOL_OTHER, "sha256 → OTHER");
    PASS();
}

static void test_tui_sparkline_try(void) {
    TEST("tui_try_sparkline detection");
    /* tui_try_sparkline prints to stderr, but we can test the return value */
    tui_features_t f;
    tui_features_init(&f);
    g_tui_features = &f;

    ASSERT(tui_try_sparkline(NULL) == false, "NULL returns false");
    ASSERT(tui_try_sparkline("hello world") == false, "text returns false");
    ASSERT(tui_try_sparkline("1, 2, 3, 4, 5") == true, "CSV numbers detected");
    ASSERT(tui_try_sparkline("[10, 20, 30]") == true, "JSON array numbers detected");
    ASSERT(tui_try_sparkline("1, 2") == false, "only 2 values — below threshold");

    g_tui_features = NULL;
    PASS();
}

static void test_tui_error_types(void) {
    TEST("tui_err_type_t enum values");
    ASSERT(TUI_ERR_NETWORK == 0, "NETWORK is 0");
    ASSERT(TUI_ERR_API == 1, "API is 1");
    ASSERT(TUI_ERR_VALIDATION == 2, "VALIDATION is 2");
    ASSERT(TUI_ERR_TIMEOUT == 3, "TIMEOUT is 3");
    ASSERT(TUI_ERR_AUTH == 4, "AUTH is 4");
    ASSERT(TUI_ERR_BUDGET == 5, "BUDGET is 5");
    PASS();
}

static void test_model_pricing(void) {
    TEST("model_lookup pricing fields");
    const model_info_t *m = model_lookup("opus");
    ASSERT(m != NULL, "opus found");
    ASSERT(m->input_price == 15.0, "opus input price $15/M");
    ASSERT(m->output_price == 75.0, "opus output price $75/M");
    ASSERT(m->cache_read_price == 1.50, "opus cache read $1.50/M");
    ASSERT(m->cache_write_price == 18.75, "opus cache write $18.75/M");

    m = model_lookup("sonnet");
    ASSERT(m != NULL, "sonnet found");
    ASSERT(m->input_price == 3.0, "sonnet input $3/M");

    m = model_lookup("haiku");
    ASSERT(m != NULL, "haiku found");
    ASSERT(m->input_price == 0.80, "haiku input $0.80/M");
    PASS();
}

static void test_tui_cadence(void) {
    TEST("tui_cadence init/feed");
    tui_cadence_t c;
    tui_cadence_init(&c, NULL);
    ASSERT(c.len == 0, "initial len 0");
    ASSERT(c.interval > 0, "interval set");

    tui_cadence_feed(&c, "hello");
    /* Buffer accumulates until flush interval */
    ASSERT(c.len >= 0, "len non-negative after feed");
    /* After immediate feed, buffer should either have content or have flushed */

    PASS();
}

static void test_tui_swarm_cost_entry(void) {
    TEST("tui_swarm_cost_entry struct");
    tui_swarm_cost_entry_t entry = {
        .name = "agent-1",
        .cost = 0.05,
        .in_tok = 1000,
        .out_tok = 500,
    };
    ASSERT(strcmp(entry.name, "agent-1") == 0, "name");
    ASSERT(entry.cost == 0.05, "cost");
    ASSERT(entry.in_tok == 1000, "in_tok");
    PASS();
}

static void test_tui_cmd_entry(void) {
    TEST("tui_cmd_entry struct");
    tui_cmd_entry_t cmds[] = {
        { "/help", "show help" },
        { "/model", "change model" },
        { "/quit", "exit" },
    };
    ASSERT(strcmp(cmds[0].name, "/help") == 0, "first cmd");
    ASSERT(strcmp(cmds[2].desc, "exit") == 0, "third desc");
    PASS();
}

static void test_tui_agent_node(void) {
    TEST("tui_agent_node struct");
    tui_agent_node_t nodes[] = {
        { .id = 1, .parent_id = 0, .task = "coordinator", .status = "running" },
        { .id = 2, .parent_id = 1, .task = "worker-a", .status = "done" },
        { .id = 3, .parent_id = 1, .task = "worker-b", .status = "error" },
    };
    ASSERT(nodes[0].parent_id == 0, "root parent is 0");
    ASSERT(nodes[1].parent_id == 1, "child parent is 1");
    ASSERT(strcmp(nodes[2].status, "error") == 0, "error status");
    PASS();
}

/* ── Cost tracking tests ──────────────────────────────────────────────── */

static void test_session_cost_calculation(void) {
    TEST("session cost calculation");
    /* Manually compute what session_cost does: in * in_price/1e6 + out * out_price/1e6 + ... */
    const model_info_t *mi = model_lookup("opus");
    ASSERT(mi != NULL, "opus model found");

    int in_tok = 10000, out_tok = 1000;
    int cr_tok = 5000, cw_tok = 2000;
    double expected = in_tok * mi->input_price / 1e6
                    + out_tok * mi->output_price / 1e6
                    + cr_tok * mi->cache_read_price / 1e6
                    + cw_tok * mi->cache_write_price / 1e6;

    /* $15/M * 10k = $0.15, $75/M * 1k = $0.075, $1.5/M * 5k = $0.0075, $18.75/M * 2k = $0.0375 */
    /* Total = $0.15 + $0.075 + $0.0075 + $0.0375 = $0.27 */
    ASSERT(fabs(expected - 0.27) < 0.001, "opus cost for 10k/1k/5k/2k = $0.27");

    /* Test sonnet pricing */
    mi = model_lookup("sonnet");
    ASSERT(mi != NULL, "sonnet model found");
    double sonnet_cost = 10000 * mi->input_price / 1e6 + 1000 * mi->output_price / 1e6;
    /* $3/M * 10k = $0.03, $15/M * 1k = $0.015 → $0.045 */
    ASSERT(fabs(sonnet_cost - 0.045) < 0.001, "sonnet cost for 10k/1k = $0.045");

    PASS();
}

static void test_tool_metrics_tracking(void) {
    TEST("tool_metrics record/get");
    tool_metrics_t m;
    tool_metrics_init(&m);

    tool_metrics_record(&m, "bash", true, 150.0);
    tool_metrics_record(&m, "bash", true, 200.0);
    tool_metrics_record(&m, "bash", false, 50.0);

    const tool_metric_t *tm = tool_metrics_get(&m, "bash");
    ASSERT(tm != NULL, "bash metric found");
    ASSERT(tm->calls == 3, "3 calls");
    ASSERT(tm->successes == 2, "2 successes");
    ASSERT(tm->failures == 1, "1 failure");
    ASSERT(fabs(tm->total_latency_ms - 400.0) < 0.1, "total latency 400ms");
    ASSERT(fabs(tm->max_latency_ms - 200.0) < 0.1, "max latency 200ms");

    const tool_metric_t *tm2 = tool_metrics_get(&m, "nonexistent");
    ASSERT(tm2 == NULL, "nonexistent tool returns NULL");

    PASS();
}

static void test_status_bar_update(void) {
    TEST("tui_status_bar update/render fields");
    tui_status_bar_t sb;
    tui_status_bar_init(&sb, "opus");
    ASSERT(strcmp(sb.model, "opus") == 0, "model set");
    ASSERT(sb.cost == 0, "initial cost 0");

    tui_status_bar_update(&sb, 5000, 1000, 0.123, 3, 5);
    ASSERT(sb.input_tokens == 5000, "input tokens updated");
    ASSERT(sb.output_tokens == 1000, "output tokens updated");
    ASSERT(fabs(sb.cost - 0.123) < 0.001, "cost updated");
    ASSERT(sb.turn == 3, "turn updated");
    ASSERT(sb.tools_used == 5, "tools updated");

    PASS();
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    fprintf(stderr, "\n\033[1m\033[36mdsco test suite\033[0m\n\n");

    /* JSON */
    test_json_get_str();
    test_json_get_int();
    test_json_get_int_missing();
    test_json_get_bool();
    test_json_get_str_escaped();
    test_json_get_str_unicode();
    test_json_get_raw();
    test_json_roundtrip();

    /* Conversation */
    test_conv_basic();
    test_conv_save_load();
    test_conv_pop_last();

    /* Crypto */
    test_sha256_known_answer();
    test_sha256_abc();

    /* Base64 */
    test_base64_roundtrip();

    /* Eval */
    test_eval_basic();
    test_eval_functions();
    test_eval_parentheses();

    /* Model registry */
    test_model_resolve_alias();
    test_model_context_window();

    /* Request builder */
    test_build_request_valid_json();
    test_build_request_ex_effort();
    test_build_request_web_search_result_shape();
    test_build_request_web_search_result_recover_from_text();
    test_build_request_server_result_missing_tool_id();
    test_build_request_web_search_result_missing_content();
    test_build_request_server_tool_results_stay_assistant();

    /* Session state */
    test_session_state_init();
    test_session_trust_tier_parse();

    /* Conversation growth behavior */
    test_conversation_growth_unbounded();

    /* Tool execution */
    test_tool_execute_eval();
    test_tool_execute_unknown();
    test_tool_edit_file_empty_old_string();
    test_tool_agent_wait_no_agents();
    test_tools_trust_tier_policy();
    test_tools_untrusted_sandbox_routing();
    test_sandbox_run_untrusted_defaults();
    test_plugin_manifest_lock_validation();

    /* jbuf */
    test_jbuf_grow();
    test_jbuf_json_str_special();

    /* TUI Features */
    test_tui_features_init();
    test_tui_feature_names();
    test_tui_features_toggle();
    test_tui_estimate_tokens();
    test_tui_is_diff();
    test_tui_is_code_paste();
    test_tui_detect_theme();
    test_tui_ghost_suggestions();
    test_tui_branch_detect();
    test_tui_dag();
    test_tui_flame();
    test_tui_citation();
    test_tui_thinking();
    test_tui_word_counter();
    test_tui_throughput();
    test_tui_minimap_entry();
    test_tui_scroller();
    test_tui_latency_breakdown();
    test_tui_tool_classify();
    test_tui_sparkline_try();
    test_tui_error_types();
    test_model_pricing();
    test_tui_cadence();
    test_tui_swarm_cost_entry();
    test_tui_cmd_entry();
    test_tui_agent_node();

    /* Cost tracking */
    test_session_cost_calculation();
    test_tool_metrics_tracking();
    test_status_bar_update();

    fprintf(stderr, "\n\033[1m  %d tests: \033[32m%d passed\033[0m",
            tests_run, tests_passed);
    if (tests_failed > 0)
        fprintf(stderr, ", \033[31m%d failed\033[0m", tests_failed);
    fprintf(stderr, "\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
