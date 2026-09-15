#include "agent_interop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static int arg_index(const agent_interop_argv_t *argv, const char *value) {
    for (int i = 0; argv && i < argv->argc; i++)
        if (strcmp(argv->argv[i], value) == 0)
            return i;
    return -1;
}

static void check_adapter(const char *name, const char *canonical) {
    const agent_interop_adapter_t *adapter = agent_interop_find(name);
    CHECK(adapter != NULL);
    if (adapter)
        CHECK(strcmp(adapter->id, canonical) == 0);
}

int main(void) {
    CHECK(agent_interop_adapter_count() == 5);
    check_adapter("codex-cli", "codex");
    check_adapter("claude", "claude-code");
    check_adapter("anomalyco-opencode", "opencode");
    check_adapter("oh-my-pi", "omp");
    check_adapter("hermes-agent", "hermes");
    CHECK(agent_interop_find("missing") == NULL);
    CHECK(agent_interop_adapter_at(99) == NULL);

    char resolved[1024];
    CHECK(agent_interop_resolve_binary("/bin/cat", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "/bin/cat") == 0);
    CHECK(!agent_interop_resolve_binary("/definitely/missing", resolved, sizeof(resolved)));
    char tiny[2];
    CHECK(!agent_interop_resolve_binary("/bin/cat", tiny, sizeof(tiny)));

    const agent_interop_adapter_t *codex = agent_interop_find("codex");
    agent_interop_argv_t argv;
    char error[160];
    CHECK(agent_interop_build_argv(codex, "/x/codex", "literal $(touch nope)",
                                   "gpt-test", "/workspace", &argv, error,
                                   sizeof(error)));
    CHECK(strcmp(argv.argv[0], "/x/codex") == 0);
    CHECK(arg_index(&argv, "exec") == 1);
    CHECK(arg_index(&argv, "--approve-for-me") > 0);
    CHECK(arg_index(&argv, "-C") > 0);
    CHECK(arg_index(&argv, "/workspace") == arg_index(&argv, "-C") + 1);
    CHECK(arg_index(&argv, "gpt-test") == arg_index(&argv, "-m") + 1);
    CHECK(strcmp(argv.argv[argv.argc - 1], "literal $(touch nope)") == 0);
    agent_interop_argv_free(&argv);

    const agent_interop_adapter_t *hermes = agent_interop_find("hermes");
    CHECK(agent_interop_build_argv(hermes, "/x/hermes", "hello", "model-x",
                                   NULL, &argv, error, sizeof(error)));
    CHECK(arg_index(&argv, "chat") == 1);
    CHECK(arg_index(&argv, "model-x") == arg_index(&argv, "-m") + 1);
    CHECK(arg_index(&argv, "hello") == arg_index(&argv, "-q") + 1);
    agent_interop_argv_free(&argv);

    const agent_interop_adapter_t *omp = agent_interop_find("pi");
    CHECK(omp && (omp->capabilities & AGENT_INTEROP_ACP));
    CHECK(omp && omp->acp_args && strcmp(omp->acp_args[0], "acp") == 0);

    char manifest[8192];
    CHECK(agent_interop_manifest_json("/x/dsco", manifest, sizeof(manifest)));
    CHECK(strstr(manifest, "dsco.agent.interop/v1") != NULL);
    CHECK(strstr(manifest, "\"protocol\":\"mcp\"") != NULL);
    CHECK(strstr(manifest, "\"protocol\":\"acp\"") != NULL);
    CHECK(strstr(manifest, "generic-argv-stdio") != NULL);
    CHECK(strstr(manifest, "shell_interpolation\":false") != NULL);

    char status[16384];
    CHECK(agent_interop_status_json("/x/dsco", status, sizeof(status)));
    CHECK(strstr(status, "\"id\":\"codex\"") != NULL);
    CHECK(strstr(status, "\"id\":\"claude-code\"") != NULL);
    CHECK(strstr(status, "\"id\":\"opencode\"") != NULL);
    CHECK(strstr(status, "\"id\":\"omp\"") != NULL);
    CHECK(strstr(status, "\"id\":\"hermes\"") != NULL);

    if (failures) {
        fprintf(stderr, "agent_interop: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("agent_interop: %d checks passed\n", checks);
    return 0;
}
