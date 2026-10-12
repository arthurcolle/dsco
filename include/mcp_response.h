#ifndef DSCO_MCP_RESPONSE_H
#define DSCO_MCP_RESPONSE_H
#include <stddef.h>
/* Select a validated JSON-RPC result/error for this request, not a notification. */
char *mcp_response_match(const char *json, size_t len, const char *request);
/* Consume complete SSE events from an accumulated body; retains partial events.
 * offset advances once per complete event, avoiding reparsing earlier events. */
char *mcp_response_sse(const char *body, size_t len, size_t *offset, const char *request);
#endif
