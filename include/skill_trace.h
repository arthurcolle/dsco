#ifndef DSCO_SKILL_TRACE_H
#define DSCO_SKILL_TRACE_H
/* Read-only, payload-omitting Chronicle trace selection. No extraction/model calls. */
#include <stdbool.h>
#include <stddef.h>
int skill_trace_cli(const char *journal_path);
/* Resolve each exact chronicle:<snapshot-sha256>:<frame-sha256> reference
 * against one validated snapshot. Tool frames only; no payload export.
 * Resolving byte identity does not verify procedure or acceptance claims. */
bool skill_trace_resolve_evidence(const char *journal_path,
                                  const char *const *references, size_t count);
#endif
