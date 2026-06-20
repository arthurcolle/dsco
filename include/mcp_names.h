#ifndef DSCO_MCP_NAMES_H
#define DSCO_MCP_NAMES_H

#include <stdbool.h>
#include <stddef.h>

void dsco_mcp_normalize_name(const char *in, char *out, size_t out_len);
void dsco_mcp_build_tool_name(const char *server_name, const char *tool_name,
                              char *out, size_t out_len);
bool dsco_mcp_is_canonical_tool_name(const char *name);
void dsco_mcp_legacy_alias_from_canonical(const char *name, char *out,
                                          size_t out_len);

#endif
