#ifndef DSCO_WORK_RESULT_H
#define DSCO_WORK_RESULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Typed tool results.
 *
 * Every tool in dsco returns `bool execute(const char *in, char *result, size_t n)`.
 * Structure is computed and then flattened into a char buffer, so nothing
 * downstream can tell an edit from a directory listing: the renderer cannot
 * draw a diff card, the router cannot score a capability, and replay has
 * nothing addressable to re-run. This is the type that carries structure
 * across the tool boundary.
 *
 * Migration contract: the existing char* path stays authoritative for display
 * and is never removed. A tool opts in by additionally populating a
 * work_result_t; consumers that do not understand a kind fall back to the
 * rendered text. 314 tools do not have to move at once. */

typedef enum {
    WORK_RESULT_NONE = 0,
    WORK_RESULT_TEXT,    /* plain text; the universal fallback         */
    WORK_RESULT_EDIT,    /* one file changed                           */
    WORK_RESULT_PATCH,   /* several files changed as one transaction   */
    WORK_RESULT_SPAWN,   /* a worker/agent was started                 */
    WORK_RESULT_ERROR,   /* the tool failed, with a machine-readable why */
} work_result_kind_t;

/* Why an edit located its target. Mirrors fuzzy_strategy_t but is kept
 * independent so the presentation and routing layers do not depend on the
 * matcher's internals. */
typedef enum {
    WORK_MATCH_UNKNOWN = 0,
    WORK_MATCH_EXACT,
    WORK_MATCH_NORMALIZED, /* whitespace/indent/escape class           */
    WORK_MATCH_SIMILARITY, /* block-anchor / context-aware class       */
} work_match_class_t;

/* One changed file. `diff` is the rendered unified diff and is owned by the
 * result; the counts are authoritative and must agree with it. */
typedef struct {
    char *path;
    char *old_path; /* non-NULL only for a rename                      */
    int   added;
    int   removed;
    int   hunks;
    char *diff;     /* unified diff text, may be NULL when suppressed  */
    bool  binary;
    bool  truncated;
} work_file_change_t;

typedef struct {
    work_result_kind_t kind;
    bool               ok;

    /* Rendered view. Always populated so a naive consumer still works. */
    char *text;

    /* WORK_RESULT_EDIT / WORK_RESULT_PATCH */
    work_file_change_t *files;
    size_t              file_count;
    int                 total_added;
    int                 total_removed;
    int                 total_hunks;
    int                 replacements;
    work_match_class_t  match_class;
    const char         *match_strategy; /* borrowed, static string      */
    double              match_confidence;

    /* WORK_RESULT_SPAWN */
    int   spawn_id;
    char *spawn_provider;
    char *spawn_model;

    /* WORK_RESULT_ERROR */
    char *error_code;   /* stable, greppable; not prose                 */
    char *error_detail;

    /* Economics + provenance. Zero when unmeasured — never guessed. */
    uint64_t elapsed_us;
    double   cost_usd;
} work_result_t;

/* Lifecycle. A result is always safe to free, including after a partial
 * build. */
void work_result_init(work_result_t *r, work_result_kind_t kind);
void work_result_free(work_result_t *r);

/* Takes ownership of `diff` (may be NULL). Returns false on allocation
 * failure, leaving the result usable. */
bool work_result_add_file(work_result_t *r, const char *path, int added, int removed,
                          int hunks, char *diff);

/* Set the rendered text view. Copies. */
bool work_result_set_text(work_result_t *r, const char *text);

/* Serialize to JSON for transport, the event stream, and replay. Caller
 * frees. Returns NULL only on allocation failure. */
char *work_result_to_json(const work_result_t *r);

/* Stable name for a kind, for logs and telemetry. */
const char *work_result_kind_name(work_result_kind_t k);

/* ── Migration side-channel ───────────────────────────────────────────────
 * Changing `bool execute(const char*, char*, size_t)` across 314 tools in one
 * step is not a safe diff. Instead a tool may publish a typed result for the
 * call currently in flight; the dispatcher collects it immediately after the
 * tool returns. Consumers that do not understand the kind keep using the
 * rendered text, so untouched tools behave exactly as before.
 *
 * Not thread-safe across concurrent tool execution in one thread of control;
 * storage is thread-local where the compiler supports it. */

/* Publish a typed result for the in-flight call. Takes ownership of *r's
 * heap members by shallow copy; the caller's struct is reset to empty so it
 * remains safe to free. */
void work_result_publish(work_result_t *r);

/* Take the published result, if any. Returns false when the tool published
 * nothing. The caller owns the result and must free it. */
bool work_result_take(work_result_t *out);

/* Drop any published result without consuming it. Called by the dispatcher
 * before each tool invocation so results never leak between calls. */
void work_result_clear_pending(void);

#endif /* DSCO_WORK_RESULT_H */
