#ifndef DSCO_CHRONICLE_H
#define DSCO_CHRONICLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* DSCO Chronicle: local-first full activity recorder.
 *
 * Chronicle is intentionally separate from the historical baseline timeline.
 * Baseline remains the compact UI/event surface; Chronicle stores full-fidelity
 * activity: LLM requests/responses, stream deltas, tool I/O, files/artifacts,
 * swarms, memory/skills, cost, provenance edges, and training-candidate labels.
 *
 * Capture is controlled by DSCO_CHRONICLE_MODE:
 *   off | metadata | full-local | blackbox
 * Default is full-local. Server sync/training consent are intentionally modeled
 * as policy labels; this module is local-only for the first implementation.
 */

typedef enum {
    CHRONICLE_MODE_OFF = 0,
    CHRONICLE_MODE_METADATA,
    CHRONICLE_MODE_FULL_LOCAL,
    CHRONICLE_MODE_BLACKBOX,
} chronicle_mode_t;

typedef struct {
    const char *provider;
    const char *model;
    const char *mode;
    const char *instance_id;
} chronicle_start_opts_t;

bool chronicle_start(const chronicle_start_opts_t *opts);
void chronicle_stop(void);
bool chronicle_ready(void);
chronicle_mode_t chronicle_mode(void);

const char *chronicle_installation_id(void);
const char *chronicle_session_id(void);
const char *chronicle_root(void);
const char *chronicle_db_path(void);

/* Crash-safe run journal (Wave B durable execution).
 * Records are length-prefixed JSON frames under ~/.dsco/runs/<run_id>/journal.wal.
 * DSCO_JOURNAL=off disables this sink without disabling Chronicle.
 */
const char *chronicle_run_id(void);
const char *chronicle_journal_path(void);
const char *chronicle_run_dir(void);
bool chronicle_journal_append(const char *record_type, const char *payload_json, bool durable);
int chronicle_runs_cli(int argc, char **argv);

/* ── replay engine (Wave B P2.1) ────────────────────────────────────── */

typedef struct {
    long long records;
    long long turns;
    long long tool_calls;
    long long tool_results;
    long long frontier_calls;   /* calls with no matching result */
    long long checkpoints;
    double cost_usd;
    bool corrupt_tail;          /* torn/corrupt frame stopped the scan early */
} chronicle_replay_summary_t;

/* Reconstruct run steps from <runs_dir>/<run_id>/journal.wal as JSON lines
 * (dsco.replay.step.v1) to `out`; `out` may be NULL for summary-only use.
 * Marks the resume frontier (tool.call with no matching tool.result) and a
 * conservative replay policy per tool name. Returns false only when the
 * journal cannot be opened. */
bool chronicle_replay_run(const char *runs_dir, const char *run_id, FILE *out,
                          chronicle_replay_summary_t *summary);

/* Budget ledger summaries derived from llm.response.completed events. */
typedef struct {
    double cost_usd;
    long long input_tokens;
    long long output_tokens;
    long long cache_read_tokens;
    long long cache_write_tokens;
    long long reasoning_tokens;
    int response_count;
} chronicle_cost_totals_t;

/* session_id may be NULL/empty to query the active Chronicle session. */
bool chronicle_cost_totals_for_session(const char *session_id, chronicle_cost_totals_t *out);

/* Query all Chronicle LLM response cost recorded since local midnight. */
bool chronicle_cost_totals_today(chronicle_cost_totals_t *out);

/* IDs are UUID-ish v4 strings; buffers must be >= 37 bytes. */
void chronicle_new_id(char *out, size_t out_len);

/* Span lifecycle. payload_json may be NULL or a JSON object string. */
bool chronicle_span_begin(const char *trace_id, const char *parent_span_id,
                          const char *span_type, const char *name,
                          const char *payload_json, char *span_id_out);
bool chronicle_span_end(const char *span_id, const char *status,
                        const char *payload_json);

/* Append a typed event. payload_json may be NULL or a JSON object string. */
bool chronicle_event(const char *event_type, const char *trace_id,
                     const char *span_id, const char *parent_span_id,
                     const char *actor_type, const char *actor_id,
                     const char *payload_json, const char *sensitivity);

/* Store an immutable content-addressed blob. sha_out must be >= 65 bytes if non-NULL. */
bool chronicle_blob_put(const void *data, size_t len,
                        const char *logical_type, const char *content_type,
                        const char *sensitivity, char *sha_out, size_t sha_out_len);
bool chronicle_blob_put_text(const char *text, const char *logical_type,
                             const char *sensitivity, char *sha_out, size_t sha_out_len);

/* Provenance graph edge. */
bool chronicle_edge(const char *from_id, const char *to_id, const char *relation,
                    double confidence, const char *metadata_json);

/* High-level capture helpers used by agent/provider/tool paths. */
bool chronicle_user_message(const char *trace_id, const char *span_id, const char *text);
bool chronicle_context_materialized(const char *trace_id, const char *span_id,
                                    const char *request_json, int estimated_tokens);
bool chronicle_llm_request(const char *trace_id, const char *span_id,
                           const char *provider, const char *model,
                           const char *request_json, int estimated_tokens);
bool chronicle_llm_delta(const char *trace_id, const char *span_id,
                         const char *kind, const char *text);
bool chronicle_llm_response(const char *trace_id, const char *span_id,
                            const char *provider, const char *model,
                            const char *output_text, const char *raw_response_json,
                            int input_tokens, int output_tokens,
                            int cache_read_tokens, int cache_write_tokens,
                            int reasoning_tokens, double cost_usd,
                            double latency_ms, const char *finish_reason,
                            const char *generation_id);
bool chronicle_tool_call_start(const char *trace_id, const char *parent_span_id,
                               const char *tool_name, const char *tool_id,
                               const char *args_json, char *tool_span_id_out);
bool chronicle_tool_call_end(const char *trace_id, const char *tool_span_id,
                             const char *tool_name, const char *result_text,
                             bool ok, bool timeout, double latency_ms);

/* HTTP/server support. */
char *chronicle_build_activity_json(int limit, const char *session_filter);
char *chronicle_build_activity_html(int limit, const char *session_filter);
char *chronicle_build_activity_html_ex(int limit, const char *session_filter,
                                       const char *type_filter,
                                       const char *search_filter);
char *chronicle_read_blob_hex(const char *sha256, size_t max_bytes, const char **content_type_out);

#endif
