#include "mcp_diagnostics.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    mcp_diagnostics_record("email", "excluded_by_filter", 0, 0);
    mcp_diagnostics_record("gateway", "authentication_required", 401, 0);
    mcp_diagnostics_record("off", "disabled", 0, 0);
    char *json = mcp_diagnostics_json();
    assert(strstr(json, "excluded_by_filter"));
    assert(strstr(json, "authentication_required"));
    assert(strstr(json, "disabled"));
    assert(strstr(json, "canonical issuer"));
    assert(!strstr(json, "Authorization"));
    free(json);
    mcp_diagnostics_record("email", "connected", 200, 167);
    mcp_diagnostics_record("quote\"test", "forbidden", 403, 0);
    json = mcp_diagnostics_json();
    assert(!strstr(json, "excluded_by_filter"));
    assert(strstr(json, "\"tool_count\":167"));
    assert(strstr(json, "quote\\\"test"));
    free(json);
    char result[16384] = "{\"matches\":[]}";
    mcp_diagnostics_append(result, sizeof(result));
    assert(strstr(result, "\"mcp_connections\":"));
    size_t len = strlen(result);
    mcp_diagnostics_append(result, sizeof(result));
    assert(strlen(result) == len);
    char tiny[3] = "{}";
    mcp_diagnostics_append(tiny, sizeof(tiny));
    assert(strcmp(tiny, "{}") == 0);
    return 0;
}
