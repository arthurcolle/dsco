#ifndef DSCO_TOOL_INTEGRITY_INDEX_H
#define DSCO_TOOL_INTEGRITY_INDEX_H
#include "llm.h"
/* Borrowed, per-validation occurrence index. No retained conversation pointers.
 * false means allocation/size failure; the caller may use its scan fallback. */
bool tool_integrity_index_validate(const conversation_t *,bool,tool_integrity_result_t *);
/* One use and at most one result per ID, including within-message duplicates.
 * This property remains true as complete exchanges are removed. */
bool tool_integrity_index_unique_exchanges(const conversation_t *);
#endif
