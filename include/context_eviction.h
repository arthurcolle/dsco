#ifndef DSCO_CONTEXT_EVICTION_H
#define DSCO_CONTEXT_EVICTION_H
#include "llm.h"

/* Archive complete old tool exchanges before replacing them with excerpts.
 * User messages, pending calls and the protected tail are never evicted. */
bool context_evict(conversation_t *conv, const char *input, char *result, size_t len);
bool context_archive_recall(const char *key, char *result, size_t len);
#endif
