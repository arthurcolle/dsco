#ifndef DSCO_MCP_STREAM_H
#define DSCO_MCP_STREAM_H
#include <stdbool.h>
#include <stddef.h>

typedef struct mcp_stream mcp_stream_t;
/* One stream per live stdio peer. Registry moves transfer its ownership.
 * Join callers before free. Hold the lock across request write and matched read,
 * including notification writes; parsing functions themselves are owner-only. */
mcp_stream_t *mcp_stream_new(void);
void mcp_stream_free(mcp_stream_t *stream);
bool mcp_stream_lock(mcp_stream_t *stream);
void mcp_stream_unlock(mcp_stream_t *stream);
bool mcp_stream_feed(mcp_stream_t *stream, const char *data, size_t len);
/* Consume complete records until a result/error matches the request ID.
 * Notifications, stale replies and malformed complete lines are skipped.
 * Unconsumed bytes and partial records survive across reads and requests.
 * Caller owns returned JSON. A complete object without newline is supported. */
char *mcp_stream_take(mcp_stream_t *stream, const char *request);
size_t mcp_stream_pending(const mcp_stream_t *stream);
#endif
