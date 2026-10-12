#ifndef DSCO_CONTEXT_COMPACTION_H
#define DSCO_CONTEXT_COMPACTION_H
#include "llm.h"
/* Explicit compaction is independent of automatic thresholds/configuration.
 * Both modes archive eligible old tool spans and multi-block assistant prose
 * through governed context_evict; aggressive mode uses smaller thresholds and
 * shorter previews. No destructive trimming fallback. Denial/archive failure
 * stops subsequent mutation; committed partial progress is reported.
 * Seed/user instructions, protected tail and incomplete call structure remain.
 * No summarization model call, provider request, or effect replay. */
bool context_compaction_run(conversation_t *conv, const char *input, const char *tier,
                            char *result, size_t result_len);
#endif
