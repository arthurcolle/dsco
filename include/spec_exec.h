#ifndef DSCO_SPEC_EXEC_H
#define DSCO_SPEC_EXEC_H

#include <stdbool.h>
#include <stddef.h>

/* ── Speculative tool pre-execution ─────────────────────────────────────────
 *
 * Hides read-only tool latency behind model generation. When a tool_use block
 * finalizes mid-stream (the model is still emitting later blocks/text), we
 * start executing it immediately in the background instead of waiting for the
 * whole turn to finish. By the time the agent loop dispatches tools, the result
 * is already computed (or in-flight) — so a read_file/grep/search that the model
 * requested early costs ~zero added wall-clock.
 *
 * Only read-only, concurrency-safe, tier-permitted tools are speculated, so a
 * speculation has NO side effects: worst case is wasted CPU on a result the
 * final response doesn't use. Enabled by default; disable with DSCO_NO_SPECULATE. */

/* Enabled unless DSCO_NO_SPECULATE is set. */
bool spec_exec_enabled(void);

/* Set the trust tier speculation executes under — must match the tier the agent
 * will dispatch with, so a speculated result equals a real one. Call per turn. */
void spec_exec_set_tier(const char *tier);

/* Streaming hook: a tool_use block just completed. If eligible, kick off its
 * background execution keyed by tool_id. Safe no-op if disabled, ineligible,
 * already speculating this id, or the registry is full. Copies its arguments. */
void spec_exec_hook(const char *tool_name, const char *tool_id, const char *tool_input);

/* If a speculation exists for tool_id: wait for it, copy its result into out
 * (truncated to outlen), set *ok to its success, mark it consumed, return true.
 * Otherwise return false and the caller executes the tool normally. */
bool spec_exec_take(const char *tool_id, char *out, size_t outlen, bool *ok);

/* Join and free every outstanding speculation. Call at each turn's tool-batch
 * boundary so un-consumed speculations never leak threads or buffers. */
void spec_exec_reset(void);

/* Count currently-tracked speculations (for status/telemetry). */
int spec_exec_active(void);

/* ── Staleness-aware file-tool result cache ─────────────────────────────────
 * A read-through memoization layer for file-reading tools. Results are keyed by
 * (tool, full input) and tagged with the source file's mtime+size; a lookup
 * hits only if the file is unchanged, so repeated reads of an unmodified file
 * are instant and an edit invalidates immediately (no stale reads). Used by
 * both speculation and normal tool execution. */

/* Fresh cache hit → copies the result into out (<=outlen), sets *ok, returns
 * true. Miss (no entry / not a file tool / file changed) → false. */
bool spec_cache_lookup(const char *tool, const char *input, char *out, size_t outlen, bool *ok);

/* Store a file-tool result tagged with the current file fingerprint. No-op for
 * tools whose input names no readable file. */
void spec_cache_store(const char *tool, const char *input, const char *result, bool ok);

#endif /* DSCO_SPEC_EXEC_H */
