#ifndef DSCO_EDIT_DIFF_H
#define DSCO_EDIT_DIFF_H

#include <stdbool.h>
#include <stddef.h>

/* Unified-diff rendering for edit confirmations.
 *
 * Every write-path tool (edit_file, edit_file_multi, apply_patch, write_file)
 * should report WHAT changed, not just that something changed. A bare
 * "edited foo.c: 1 replacement(s)" gives the model no way to verify it
 * modified the intended region, and gives the operator no review surface.
 * This renders a real `--- a/path` / `+++ b/path` / `@@ -l,c +l,c @@` diff with
 * line-anchored context, matching what standard tooling emits.
 *
 * Diffing is line-based via Myers O(ND) with a bounded edit-distance budget;
 * past that budget it degrades to a whole-range replacement hunk rather than
 * spending unbounded time on a pathological input. */

/* Default context lines around each change (diff -U3). */
#define EDIT_DIFF_CONTEXT 3

/* Byte budget for a diff embedded in a tool result. Keeps a large edit from
 * crowding out the rest of the model's context window. */
#define EDIT_DIFF_TOOL_BUDGET 8000

/* Render a unified diff of old_text -> new_text for `path`.
 *
 * Returns a heap-allocated NUL-terminated string the caller must free(), or
 * NULL on allocation failure. When the texts are identical the result is an
 * empty string (not NULL).
 *
 * `max_bytes` caps the rendered output; when the full diff would exceed it the
 * tail is truncated with an explicit marker so the caller never silently drops
 * changes. Pass 0 for a sane default cap. */
char *edit_diff_unified(const char *path, const char *old_text, const char *new_text,
                        int context_lines, size_t max_bytes);

/* Summary counters for a rendered diff (cheap second pass over the texts).
 * Any out-pointer may be NULL. Returns false when the inputs are identical. */
bool edit_diff_stat(const char *old_text, const char *new_text,
                    int *out_added, int *out_removed, int *out_hunks);

#endif /* DSCO_EDIT_DIFF_H */
