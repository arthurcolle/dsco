/* Real production request builders and registry; no HTTP, credentials, or tool
 * execution. Include provider.c to select the native Codex builder without an
 * authentication probe. Link TUI_TEST_LIB_OBJS excluding provider.o. */
#include "../src/provider.c"
#include "../vendor/yyjson.h"
#include "vm.h"

volatile int g_interrupted;
vm_t g_vm;
double g_cost_budget;
int g_cheap_mode;

static int checks, failures;
#define CHECK(expr, label) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static const char *marker = "LIVE TOOLS — CURRENT REQUEST";

static const char *wire_name(yyjson_val *item) {
    yyjson_val *function = yyjson_obj_get(item, "function");
    const char *name = yyjson_get_str(yyjson_obj_get(function ? function : item, "name"));
    return name ? name : yyjson_get_str(yyjson_obj_get(item, "type"));
}

static void verify_request(char *request, bool enabled, bool anthropic,
                           bool execution_tools, bool web_only) {
    CHECK(request != NULL, "builder returned a request");
    if (!request) return;
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    CHECK(doc != NULL, "request parses as JSON");
    if (!doc) { free(request); return; }
    CHECK(strstr(request, "OWNED CUSTOM WORKER PROMPT") != NULL,
          "custom worker prompt survives fresh runtime grounding");
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_obj_get(root, "input")) {
        CHECK(strstr(request, "OWNED PERSISTENT SESSION DIRECTIVE") != NULL,
              "Responses requests retain session directives through compaction and cheap/custom prompts");
        CHECK(strstr(request, "lower priority than platform, developer, and workspace instructions") != NULL,
              "persistent session directives retain their priority label");
    }
    const char *prompt = yyjson_get_str(yyjson_obj_get(root, "instructions"));
    yyjson_val *system = yyjson_obj_get(root, "system");
    if (anthropic) {
        size_t count = yyjson_arr_size(system);
        yyjson_val *last = count ? yyjson_arr_get(system, count - 1) : NULL;
        prompt = yyjson_get_str(yyjson_obj_get(last, "text"));
        if (enabled) {
            CHECK(count >= 2, "grounding is its own system block");
            CHECK(!yyjson_obj_get(last, "cache_control"), "live block has no static cache mark");
            bool saw_cache = false;
            for (size_t i = 0; i + 1 < count; ++i)
                saw_cache |= yyjson_obj_get(yyjson_arr_get(system, i), "cache_control") != NULL;
            if (!web_only) CHECK(saw_cache, "live block follows stable cache breakpoint");
        }
    } else if (!prompt) {
        yyjson_val *messages = yyjson_obj_get(root, "messages");
        yyjson_val *content = yyjson_obj_get(yyjson_arr_get(messages, 0), "content");
        prompt = yyjson_is_str(content) ? yyjson_get_str(content)
            : yyjson_get_str(yyjson_obj_get(yyjson_arr_get(content, 0), "text"));
    }
    const char *grounding = prompt ? strstr(prompt, marker) : NULL;
    CHECK((grounding != NULL) == enabled, "manifest follows actual request tool policy");
    if (grounding) {
        CHECK(!strstr(grounding + strlen(marker), marker), "one fresh manifest per request");
        yyjson_val *tools = yyjson_obj_get(root, "tools");
        size_t count = yyjson_arr_size(tools);
        char count_text[100];
        snprintf(count_text, sizeof(count_text), "Directly advertised tool names (%zu):", count);
        CHECK(strstr(grounding, count_text) != NULL, "manifest count equals final tools array");
        const char *names = strstr(grounding, ": ");
        const char *names_end = names ? strstr(names, ".\n") : NULL;
        bool bash = false, python = false;
        for (size_t i = 0; i < count; ++i) {
            const char *name = wire_name(yyjson_arr_get(tools, i));
            const char *found = names && name ? strstr(names, name) : NULL;
            CHECK(name && found && names_end && found < names_end,
                  "each final wire tool appears in direct manifest line");
            bash |= name && !strcmp(name, "bash");
            python |= name && !strcmp(name, "dsco-python-3x");
        }
        if (execution_tools) CHECK(bash && python, "bash and canonical Python remain directly callable");
        if (web_only) {
            CHECK(count == 1 && !bash && !python, "web-only override removes function tools");
            CHECK(strstr(grounding, "bash({") == NULL, "search-only manifest has no shell execution hint");
        }
    }
    yyjson_doc_free(doc);
    free(request);
}

int main(void) {
    /* Process-local fixture settings: no HOME replacement or live account use. */
    setenv("DSCO_SYSTEM_PROMPT", "OWNED CUSTOM WORKER PROMPT", 1);
    setenv("DSCO_TOOL_PROXY", "1", 1);
    setenv("DSCO_TOOL_FREEZE", "0", 1);
    setenv("DSCO_DISABLE_DEFAULT_FALLBACKS", "1", 1);
    unsetenv("DSCO_OR_DISABLE_TOOLS");
    unsetenv("DSCO_TOOL_ALLOWLIST");
    unsetenv("DSCO_TOOL_PROFILE");
    setenv("DSCO_OR_MAX_TOOLS", "32", 1);
    unsetenv("DSCO_MAX_TOOLS");
    unsetenv("DSCO_OPENAI_PARAMS");
    unsetenv("DSCO_ABLITERATION_PARAMS");
    unsetenv("DSCO_ABLITERATION_WEB_ALLOWED_DOMAINS");
    unsetenv("DSCO_ABLITERATION_WEB_BLOCKED_DOMAINS");
    tools_init_local_only();
    tools_reset_external();
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, "Check a live fact using the available tools.");
    session_state_t session;
    session_state_init(&session, "gpt-6-astra");
    snprintf(session.runtime_directives, sizeof(session.runtime_directives),
             "OWNED PERSISTENT SESSION DIRECTIVE");
    provider_t openai = {.name = "openai"};

    for (int cheap = 0; cheap < 2; ++cheap) {
        g_cheap_mode = cheap;
        for (int continued = 0; continued < 2; ++continued) {
            if (continued) {
                conv_add_assistant_tool_use(&conv, "owned_call", "bash", "{\"command\":\"true\"}");
                conv_add_tool_result_named(&conv, "owned_call", "bash", "owned synthetic result", false);
                CHECK(conv_compact_recent_tool_turn(&conv, 256, 0), "real tool-turn compaction succeeds");
                conv_add_user_text(&conv, "Continue the same task.");
            }
            int before = conv.count;
            verify_request(chatgpt_native_build_request(NULL, &conv, &session, 256, NULL),
                           true, false, true, false);
            verify_request(openai_build_request(&openai, &conv, &session, 256, NULL),
                           true, false, true, false);
            verify_request(llm_build_request_ex_for_credential(&conv, &session, 256, "owned-api-fixture"),
                           true, true, true, false);
            verify_request(llm_build_request_for_credential(&conv, "claude-haiku-4-5-20251001",
                                                           256, "owned-api-fixture"),
                           true, true, true, false);
            CHECK(conv.count == before, "request grounding never accumulates conversation messages");
        }
    }

    session.web_search = true;
    verify_request(abliteration_responses_build_request(NULL, &conv, &session, 256, NULL),
                   true, false, true, false);
    verify_request(abliteration_anthropic_build_request(NULL, &conv, &session, 256, "owned-api-fixture"),
                   true, true, false, true);
    session.web_search = false;
    snprintf(session.tool_choice, sizeof(session.tool_choice), "none");
    verify_request(chatgpt_native_build_request(NULL, &conv, &session, 256, NULL), false, false, false, false);
    verify_request(openai_build_request(&openai, &conv, &session, 256, NULL), false, false, false, false);
    verify_request(llm_build_request_ex_for_credential(&conv, &session, 256, "owned-api-fixture"),
                   false, true, false, false);
    session.tool_choice[0] = '\0';
    session.direct_answer_mode = true;
    verify_request(chatgpt_native_build_request(NULL, &conv, &session, 256, NULL), false, false, false, false);
    verify_request(openai_build_request(&openai, &conv, &session, 256, NULL), false, false, false, false);
    verify_request(llm_build_request_ex_for_credential(&conv, &session, 256, "owned-api-fixture"),
                   false, true, false, false);
    session.direct_answer_mode = false;
    setenv("DSCO_OR_DISABLE_TOOLS", "1", 1);
    verify_request(chatgpt_native_build_request(NULL, &conv, &session, 256, NULL), false, false, false, false);
    verify_request(openai_build_request(&openai, &conv, &session, 256, NULL), false, false, false, false);
    conv_free(&conv);
    printf("tool grounding production requests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
