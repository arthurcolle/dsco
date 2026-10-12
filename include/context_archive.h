#ifndef DSCO_CONTEXT_ARCHIVE_H
#define DSCO_CONTEXT_ARCHIVE_H
#include "llm.h"
#include "context_fabric.h"
/* Lossless serialization of every field of the current conversation block
 * representation. Caller owns the JSON. Null and empty strings stay distinct. */
char *context_archive_serialize(const message_t *messages, int count);
/* A successful put alone is not sufficient for a destructive handoff. */
bool context_archive_store_verified(const void *bytes, size_t len,
                                    const ctx_put_opts_t *options, ctxkey_t *key);
/* Failure stages contain no content or credentials. */
bool context_archive_store_verified_ex(const void *bytes, size_t len,
                                    const ctx_put_opts_t *options, ctxkey_t *key,
                                    const char **failure_stage);
/* Text page with absolute byte offsets and a precise continuation key. */
bool context_archive_page(ctx_broker_t *broker, const ctxkey_t *key,
                          char *result, size_t len);
/* Authenticate old untagged receipts by regenerating their exact contents
 * from the original archived tool exchange. A prefix alone is insufficient. */
bool context_archive_legacy_receipt(const message_t *assistant, const message_t *receipt);
/* Allocate all receipts before changing history. Caller must have verified
 * durable archival of the exact span. Tool spans become two plain messages;
 * standalone assistant responses become one. Never expose opaque reasoning. */
typedef enum { CONTEXT_ARCHIVE_FAILED=0, CONTEXT_ARCHIVE_REPLACED=1,
               CONTEXT_ARCHIVE_NOT_REDUCING=2 } context_archive_replace_result_t;
context_archive_replace_result_t context_archive_replace(conversation_t *conv, int start, int count,
                             const char *key, bool tool_span, int preview_chars, bool allow_expansion);
#endif
