#include "service_boundary.h"
#include "capability.h"
#include "cloud_runtime.h"
#include "json_util.h"
#include "tools.h"

bool service_action_destination_allowed(const char *tool, const char *action, const char *url) {
    if (!tool || !url || !*url || !dsco_cloud_destination_allowed(url))
        return false;
    /* Typed facades intentionally omit URL arguments. Supply the resolved
     * destination to the same capability policy before opening a connection,
     * so a host allowlist cannot disappear behind a configured service alias. */
    jbuf_t input;
    jbuf_init(&input, 256);
    jbuf_append(&input, "{\"url\":");
    jbuf_append_json_str(&input, url);
    if (action) {
        jbuf_append(&input, ",\"action\":");
        jbuf_append_json_str(&input, action);
    }
    jbuf_append(&input, "}");
    char reason[256];
    bool allowed = input.data && dsco_capability_gate(tool, input.data,
        tools_execution_tier(), reason, sizeof(reason)) == CAP_DECISION_ALLOW;
    jbuf_free(&input);
    return allowed;
}

bool service_destination_allowed(const char *tool, const char *url) {
    return service_action_destination_allowed(tool, NULL, url);
}
