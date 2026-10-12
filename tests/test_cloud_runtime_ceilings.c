#include "cloud_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(const char *key, const char *expected) {
    const char *actual = getenv(key);
    if (!actual || strcmp(actual, expected) != 0) {
        fprintf(stderr, "%s expected %s, got %s\n", key, expected, actual ? actual : "(null)");
        return 1;
    }
    return 0;
}

int main(void) {
    setenv("DSCO_ALLOW_NET", "operator.example", 1);
    setenv("DSCO_OR_PROVIDER_ONLY", "operator", 1);
    setenv("DSCO_MODEL", "operator/model", 1);
    setenv("DSCO_TOOL_ALLOWLIST", "operator_tool", 1);
    char err[128] = {0};
    if (!dsco_cloud_runtime_apply_compiled_ceilings(err, sizeof(err))) {
        /* The Make target deliberately compiles the historical shell policy
         * fixture. It must now be rejected before it can be advertised. */
        if (strstr(err, "tenant sandbox contract"))
            return 0;
        fprintf(stderr, "could not apply compiled ceilings: %s\n", err);
        return 1;
    }
    return expect("DSCO_ALLOW_NET", "api.example.com") ||
           expect("DSCO_OR_PROVIDER_ONLY", "openai,local") ||
           expect("DSCO_CLOUD_ALLOWED_MODELS", "openai/codex-default,local/auto") ||
           expect("DSCO_MODEL", "openai/codex-default") ||
           expect("DSCO_TOOL_ALLOWLIST", "Read,WebSearch") ||
           expect("DSCO_CLOUD_ALLOWED_TOOLS", "repository,web") ||
           expect("DSCO_CLOUD_PLUGIN_CAPABILITIES", "") ||
           expect("DSCO_CLOUD_SKIP_PLUGIN_INIT", "1") ||
           expect("DSCO_CLOUD_SKIP_BROWSER_PROFILES", "1") ||
           expect("DSCO_CLOUD_SKIP_IPC_INIT", "1") ||
           expect("DSCO_DISABLE_CROSS_PROVIDER_ROUTING", "0") ||
           expect("DSCO_DEFAULT_SESSION_BUDGET", "12") ||
           expect("DSCO_EXEC_BUDGET_CEILING", "12");
}
