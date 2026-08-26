/* Smoke harness: prove the OpenRouter remote MCP integration works without
 * the agent loop or an LLM provider. Loads dsco's MCP config (incl. ./.mcp.json),
 * prints discovered servers/tools, and fires one live tool call.
 *
 *   make mcp_smoke && ./mcp_smoke
 * Linked against the same objects as the `dsco` binary (minus main.o). */
#include "mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lives in BSS, not the stack: mcp_registry_t is large (64 servers + 4096
 * tools). dsco keeps this as the global g_mcp; mirror that here. */
static mcp_registry_t reg;

int main(void) {

    int n = mcp_init(&reg);
    fprintf(stderr, "mcp_init: configured=%d servers=%d tools=%d (failed=%d)\n",
            n, reg.server_count, reg.tool_count, reg.failed_count);

    for (int i = 0; i < reg.server_count; i++) {
        mcp_server_t *s = &reg.servers[i];
        fprintf(stderr, "  server[%d] %-16s transport=%-4s initialized=%d headers=%d\n",
                i, s->name,
                s->transport == MCP_TRANSPORT_HTTP ? "http" : "stdio",
                s->initialized, s->headerc);
    }

    int tc = 0;
    const mcp_tool_t *tools = mcp_get_tools(&reg, &tc);
    fprintf(stderr, "  discovered tools:\n");
    for (int i = 0; i < tc; i++)
        fprintf(stderr, "    %s\n", tools[i].name);

    /* Tools are namespaced (mcp__openrouter[_2]__models-list); the suffix can
     * vary with dedup, so find the tool dynamically. */
    const char *call = NULL;
    for (int i = 0; i < tc; i++) {
        if (strstr(tools[i].name, "openrouter") && strstr(tools[i].name, "models-list")) {
            call = tools[i].name;
            break;
        }
    }
    if (!call) {
        fprintf(stderr, "no openrouter models-list tool discovered\n");
        mcp_shutdown(&reg);
        return 2;
    }
    fprintf(stderr, "calling %s ...\n", call);
    char *res = mcp_call_tool(&reg, call, "{}");
    if (res) {
        printf("%s\n", res);
        free(res);
    } else {
        fprintf(stderr, "mcp_call_tool(\"%s\") returned NULL\n", call);
    }
    mcp_shutdown(&reg);
    return res ? 0 : 1;
}
