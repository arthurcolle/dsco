#ifndef DSCO_MCP_DIAGNOSTICS_H
#define DSCO_MCP_DIAGNOSTICS_H
#include <stddef.h>
/* Redacted process-local connection observations. No credentials or URLs. */
void mcp_diagnostics_record(const char *name, const char *state, long http_status, int tools);
void mcp_diagnostics_append(char *json, size_t capacity);
char *mcp_diagnostics_json(void); /* caller owns returned JSON */
#endif
