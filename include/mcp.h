#ifndef DSCO_MCP_H
#define DSCO_MCP_H

#include <stdbool.h>
#include <stddef.h>

/* MCP (Model Context Protocol) client — JSON-RPC over stdio transport.
 *
 * Reads DSCO config first, then imports MCP server configs from Claude and
 * Codex config files:
 *   ~/.dsco/mcp.json
 *   ~/.dsco/config.json
 *   ./.mcp.json
 *   ./.dsco/mcp.json
 *   ./.dsco/config.json
 *   ~/.claude.json, ~/.claude/settings*.json, Claude Desktop configs
 *   ~/.codex/config.toml and ./.codex/config.toml
 *   ~/.hermes/config.yaml and ~/.hermes/mcp_servers.yaml
 *
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
 * stdio servers are spawned as subprocesses. HTTP/streamable servers are
 * addressed directly when command/url starts with http:// or https://.
 * Discovered tools are registered for use by the LLM.
 */

#define MCP_MAX_SERVERS    64
#define MCP_MAX_TOOLS      2048
#define MCP_MAX_LINE       (256 * 1024)
#define MCP_MAX_ARGS       32
#define MCP_MAX_ENV        32
#define MCP_MAX_HEADERS    16

typedef enum {
    MCP_TRANSPORT_STDIO = 0,
    MCP_TRANSPORT_HTTP  = 1
} mcp_transport_t;

typedef struct {
    char  name[256];
    char  remote_name[256];     /* exact MCP tool name to send in tools/call */
    char  description[1024];
    char  input_schema[16384];  /* JSON schema string */
    int   server_idx;          /* which server owns this tool */
} mcp_tool_t;

typedef struct {
    char  name[128];
    char  command[512];
    char  url[1024];
    char  cwd[512];
    char  source[256];
    char  args[MCP_MAX_ARGS][256];
    int   argc;
    char  env_keys[MCP_MAX_ENV][128];
    char  env_vals[MCP_MAX_ENV][1024];
    int   envc;
    char  header_keys[MCP_MAX_HEADERS][128];
    char  header_vals[MCP_MAX_HEADERS][1024];
    int   headerc;
    char  session_id[256];     /* streamable HTTP MCP session id */
    int   pid;
    int   stdin_fd;
    int   stdout_fd;
    int   rpc_id;              /* monotonic JSON-RPC id */
    mcp_transport_t transport;
    bool  initialized;
} mcp_server_t;

typedef struct {
    mcp_server_t servers[MCP_MAX_SERVERS];
    int          server_count;
    int          configured_count;
    int          failed_count;
    mcp_tool_t   tools[MCP_MAX_TOOLS];
    int          tool_count;
    bool         loaded;
} mcp_registry_t;

/* Load mcp.json config and spawn servers */
int  mcp_init(mcp_registry_t *reg);

/* Suppress mcp_init's stderr progress lines (useful when init runs in a
 * background thread and would otherwise corrupt the TUI input panel). */
void mcp_set_silent(bool silent);

/* Ask any in-flight MCP init/discovery work to abort quickly. This is used by
 * fast restart paths before joining the background init thread. */
void mcp_cancel(void);
void mcp_cancel_reset(void);

/* Shutdown all servers */
void mcp_shutdown(mcp_registry_t *reg);

/* Get discovered tools */
const mcp_tool_t *mcp_get_tools(mcp_registry_t *reg, int *count);

/* Execute a tool call via its server's JSON-RPC transport.
 * Returns result JSON string (caller owns), or NULL on error. */
char *mcp_call_tool(mcp_registry_t *reg, const char *tool_name,
                     const char *arguments_json);

#endif
