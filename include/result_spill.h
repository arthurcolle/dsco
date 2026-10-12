#ifndef DSCO_RESULT_SPILL_H
#define DSCO_RESULT_SPILL_H

#include <stdbool.h>
#include <stddef.h>

/* Oversized-result spill: preview in conversation, bytes in the fabric.
 *
 * A tool result larger than the ingest budget used to be truncated with a
 * warning — the overflow bytes were simply destroyed, and a single huge
 * search/read could still burn tens of thousands of context tokens for one
 * turn's glance. Both failure modes are wrong: big results should neither
 * cripple the session nor lose data.
 *
 * result_spill() rewrites an oversized buffer in place:
 *
 *   [head preview — first `head` bytes]
 *   ... [spilled: N bytes total; retrieve with context_recall key=ck:...]
 *   [tail preview — last `tail` bytes]
 *
 * The full original bytes go to the context fabric (content-addressed,
 *  deduplicated, session-scoped), so the model can fault in any range later
 * via context_recall / ctx_get — including AFTER compaction or branch
 * switches, which makes the key a durable reference where inline bytes are
 * ephemeral. Head+tail previews are kept because the edges of tool output
 * (status lines, error summaries) carry most of the signal per byte. */

/* Spill threshold: results larger than this are rewritten. Distinct from the
 * hard MAX_TOOL_RESULT transport cap — spilling triggers well before
 * truncation would. */
#define RESULT_SPILL_THRESHOLD (24 * 1024)
#define RESULT_SPILL_HEAD (8 * 1024)
#define RESULT_SPILL_TAIL (2 * 1024)

/* If `result` exceeds `threshold` (0 = RESULT_SPILL_THRESHOLD), store the
 * full bytes in the context fabric, verify exact readback, and rewrite `result` (capacity `cap`) to
 * preview + key + preview. Returns true when a spill happened. `label` names
 * the source tool for the fabric record. Failed storage/readback leaves the
 * original intact. Honors DSCO_RESULT_SPILL=0. */
bool result_spill(char *result, size_t cap, const char *label, size_t threshold);

#endif /* DSCO_RESULT_SPILL_H */
