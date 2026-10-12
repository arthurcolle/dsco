#ifndef DSCO_MCP_SERVER_H
#define DSCO_MCP_SERVER_H

/* dsco as an MCP server (stdio JSON-RPC 2.0). Plan 05, harness-parity.
 * toolsets_csv: comma list of curated sets (core,ast,swarm,market,crypto,all).
 * tier: governance tier applied to every tools/call (default "agent"). */
int mcp_server_run(const char *toolsets_csv, const char *tier);

#endif /* DSCO_MCP_SERVER_H */
