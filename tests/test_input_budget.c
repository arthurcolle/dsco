#include "input_budget.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *payload(const char *kind, int n) {
    char *text = malloc((size_t)n + 1);
    assert(text); memset(text, 'x', (size_t)n); text[n] = 0;
    char *result = malloc((size_t)n + 2048); assert(result);
    const char *prefix = "{\"system\":\"KEEP_SYSTEM\",\"max_tokens\":256,\"tools\":[{\"name\":\"read_file\",\"description\":\"KEEP_SCHEMA\"}],";
    if (!strcmp(kind, "chat"))
        snprintf(result, (size_t)n + 2048, "%s\"messages\":[{\"role\":\"user\",\"content\":\"KEEP_USER\"},{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"a\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"KEEP_CALL\"}}]},{\"role\":\"tool\",\"tool_call_id\":\"a\",\"content\":\"%s\"}]}", prefix, text);
    else if (!strcmp(kind, "anthropic"))
        snprintf(result, (size_t)n + 2048, "%s\"messages\":[{\"role\":\"user\",\"content\":\"KEEP_USER\"},{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"read_file\",\"input\":{\"path\":\"KEEP_CALL\"}}]},{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"a\",\"content\":\"%s\"}]}]}", prefix, text);
    else if (!strcmp(kind, "responses"))
        snprintf(result, (size_t)n + 2048, "%s\"input\":[{\"role\":\"user\",\"content\":\"KEEP_USER\"},{\"type\":\"function_call\",\"call_id\":\"a\",\"name\":\"read_file\",\"arguments\":\"KEEP_CALL\"},{\"type\":\"function_call_output\",\"call_id\":\"a\",\"output\":\"%s\"}]}", prefix, text);
    else if (!strcmp(kind, "google"))
        snprintf(result, (size_t)n + 2048, "%s\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"KEEP_USER\"}]},{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"name\":\"read_file\",\"args\":{\"path\":\"KEEP_CALL\"}}}]},{\"role\":\"user\",\"parts\":[{\"functionResponse\":{\"name\":\"read_file\",\"response\":{\"result\":\"%s\"}}}]}]}", prefix, text);
    else
        snprintf(result, (size_t)n + 2048, "%s\"messages\":[{\"role\":\"user\",\"content\":\"KEEP_USER %s\"}]}", prefix, text);
    free(text); return result;
}
int main(void) {
    unsetenv("DSCO_MAX_INPUT_TOKENS");
    assert(input_budget_configured_limit() == 0);
    assert(input_budget_effective_limit(0, 256) == 32768);
    assert(input_budget_effective_limit(1050000, 128000) == 920976);
    assert(input_budget_effective_limit(272000, 32768) == 238208);
    /* Model-aware defaults must not stop or shorten a ~33k protected request
     * just because an unrelated old fixed budget was 32,768. */
    char *large = payload("protected", 100000), *unchanged = strdup(large);
    input_budget_result_t automatic;
    assert(input_budget_apply(&large, 272000, 32768, &automatic));
    assert(automatic.before_tokens > 32768 && !automatic.reduced_fields);
    assert(automatic.limit == 270720); /* wire max_tokens=256 wins */
    assert(!strcmp(large, unchanged));
    setenv("DSCO_MAX_INPUT_TOKENS", "32768", 1);
    assert(!input_budget_apply(&large, 272000, 32768, &automatic));
    assert(!strcmp(large, unchanged));
    setenv("DSCO_MAX_INPUT_TOKENS", "auto", 1);
    assert(input_budget_apply(&large, 272000, 32768, &automatic));
    free(large); free(unchanged);
    assert(input_budget_estimate("oops") == -1);
    assert(input_budget_estimate("[]") == -1);
    const char *kinds[] = {"chat", "anthropic", "responses", "google"};
    setenv("DSCO_MAX_INPUT_TOKENS", "3000", 1);
    for (int i = 0; i < 4; i++) {
        char *req = payload(kinds[i], 60000), *original = strdup(req);
        input_budget_result_t r;
        assert(input_budget_apply(&req, 200000, 256, &r));
        assert(r.admitted && r.before_tokens > 20000 && r.after_tokens <= 3000);
        assert(r.reduced_fields && strstr(req, "request input budget"));
        assert(strstr(req, "KEEP_SYSTEM") && strstr(req, "KEEP_SCHEMA"));
        assert(strstr(req, "KEEP_USER") && strstr(req, "KEEP_CALL"));
        assert(strlen(original) > 60000); /* independent full history unaffected */
        char *projected = strdup(req);
        assert(input_budget_apply(&req, 200000, 256, &r) && !r.reduced_fields);
        assert(!strcmp(projected, req));
        free(req); free(original); free(projected);
    }
    char *req = payload("protected", 60000), *original = strdup(req);
    input_budget_result_t r;
    assert(!input_budget_apply(&req, 200000, 256, &r));
    assert(!strcmp(req, original) && strstr(r.reason, "protected input"));
    free(req); free(original);
    /* Projection boundaries cannot cut a multibyte codepoint or touch the
     * user's governing text, even when one tool observation dominates input. */
    char unicode[30001];
    for (int i = 0; i < 10000; i++) memcpy(unicode + i * 3, "雪", 3);
    unicode[30000] = 0;
    req = malloc(sizeof(unicode) + 256); assert(req);
    snprintf(req, sizeof(unicode) + 256,
        "{\"messages\":[{\"role\":\"user\",\"content\":\"保留指示\"},"
        "{\"role\":\"tool\",\"tool_call_id\":\"unicode\",\"content\":\"%s\"}]}", unicode);
    assert(input_budget_apply(&req, 200000, 256, &r));
    assert(r.after_tokens <= 3000 && strstr(req, "保留指示"));
    yyjson_doc *valid = yyjson_read(req, strlen(req), 0);
    assert(valid); yyjson_doc_free(valid); free(req);
    req = strdup("{\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}]}");
    assert(!input_budget_apply(&req, 1000, 16384, &r) && r.limit == 0);
    setenv("DSCO_MAX_INPUT_TOKENS", "0", 1);
    assert(!input_budget_apply(&req, 200000, 256, &r));
    assert(strstr(r.reason, "must be an integer"));
    setenv("DSCO_MAX_INPUT_TOKENS", "bad", 1);
    assert(!input_budget_apply(&req, 200000, 256, &r));
    free(req);
    assert(input_budget_estimate("{\"text\":\"你好你好\"}") > input_budget_estimate("{\"text\":\"hellohello\"}"));
    assert(input_budget_estimate("{\"messages\":[{\"type\":\"image\",\"source\":{\"data\":\"aaaa\"}}]}") >= 4096);
    puts("input budget: all wire formats, protected input, output reserve, idempotence, malformed config and Unicode passed");
    return 0;
}
