#ifndef DSCO_MCP_H
#define DSCO_MCP_H

#include <stdbool.h>
#include <stddef.h>

/* MCP (Model Context Protocol) client — JSON-RPC over stdio transport.
 *
 * Reads ~/.dsco/mcp.json for server configs:
 * {
 *   "servers": {
 *     "my-server": {
 *       "command": "/path/to/mcp-server",
 *       "args": ["--flag"],
 *       "env": {"KEY": "val"}
 *     }
 *   }
 * }
 *
 * Each server is spawned as a subprocess, initialized, and its tools
 * are registered for use by the LLM.
 */

#define MCP_MAX_SERVERS    8
#define MCP_MAX_TOOLS      64
#define MCP_MAX_LINE       (256 * 1024)

typedef struct {
    char  name[128];
    char  description[512];
    char  input_schema[4096];  /* JSON schema string */
    int   server_idx;          /* which server owns this tool */
} mcp_tool_t;

typedef struct {
    char  name[128];
    char  command[512];
    char  args[8][256];
    int   argc;
    int   pid;
    int   stdin_fd;
    int   stdout_fd;
    int   rpc_id;              /* monotonic JSON-RPC id */
    bool  initialized;
} mcp_server_t;

typedef struct {
    mcp_server_t servers[MCP_MAX_SERVERS];
    int          server_count;
    mcp_tool_t   tools[MCP_MAX_TOOLS];
    int          tool_count;
    bool         loaded;
} mcp_registry_t;

/* Load mcp.json config and spawn servers */
int  mcp_init(mcp_registry_t *reg);

/* Shutdown all servers */
void mcp_shutdown(mcp_registry_t *reg);

/* Get discovered tools */
const mcp_tool_t *mcp_get_tools(mcp_registry_t *reg, int *count);

/* Execute a tool call via its server's JSON-RPC transport.
 * Returns result JSON string (caller owns), or NULL on error. */
char *mcp_call_tool(mcp_registry_t *reg, const char *tool_name,
                     const char *arguments_json);

#endif
