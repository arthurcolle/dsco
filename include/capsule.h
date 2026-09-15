#ifndef DSCO_CAPSULE_H
#define DSCO_CAPSULE_H

/* ═══════════════════════════════════════════════════════════════════════════
 * Capsules — lossless-by-retrieval compaction artifacts (T1, #04 W2/W3).
 *
 * Re-scoped per SOTA_FRAMES_2026-09-02.md PLAN #04: instead of trusting a
 * lossy summary to carry dropped conversation state, the dropped spans are
 * offloaded to the context fabric (content-addressed, retrievable by ctxkey)
 * and a small on-disk "capsule" records, per cwd, the ctxkeys of what was
 * dropped plus a one-line prose summary. At session start, a matching capsule
 * is injected so the model can fault-in exactly what was dropped rather than
 * hallucinating what the summary elided.
 *
 * Layout on disk (one JSON file per cwd key):
 *     ~/.dsco/capsules/<sha256(cwd)[:16]>.json
 *         { "cwd": "...", "created": "<iso8601>",
 *           "keys": [ "ck:run:...", ... ],
 *           "summary": "..." }
 *
 * Capsules are keyed by cwd so two sessions in different projects never
 * cross-contaminate (T1 acceptance: "no cwd collisions in test fixture").
 * The capsule is advisory — a corrupt/missing capsule degrades to "no
 * injection", never blocks a session.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdbool.h>
#include <stddef.h>

#define CAPSULE_DIR_DEFAULT "~/.dsco/capsules"
#define CAPSULE_MAX_KEYS    64
#define CAPSULE_SUMMARY_MAX 512

typedef struct {
    char cwd[1024];
    char created[64];
    char *keys[CAPSULE_MAX_KEYS];  /* malloc'd ctxkey strings, or NULL */
    int  key_count;
    char summary[CAPSULE_SUMMARY_MAX];
} capsule_t;

/* Initialize an empty capsule bound to cwd (defaults to process cwd). */
void capsule_init(capsule_t *c, const char *cwd);

/* Free heap members (keys); safe to call twice. */
void capsule_free(capsule_t *c);

/* Add a ctxkey string. Returns false if full or key too long. */
bool capsule_add_key(capsule_t *c, const char *ctxkey);

/* Serialize capsule to JSON (malloc'd, caller frees). NULL on error. */
char *capsule_to_json(const capsule_t *c);

/* Parse JSON back into *c (deep-copies keys). Returns false on malformed. */
bool capsule_from_json(const char *json, capsule_t *c);

/* ── On-disk capsule path helpers ─────────────────────────────────────────
 * The 16-hex cwd key is stable for a given cwd across sessions. */
int  capsule_key_for_cwd(const char *cwd, char *out, size_t cap);
int  capsule_path_for_cwd(const char *cwd, char *out, size_t cap);

/* Persist capsule for cwd (atomic write, creates dir). 0 = ok. */
int  capsule_save(const capsule_t *c);

/* Load capsule for cwd. Returns 0 on hit, -1 if absent/corrupt (advisory). */
int  capsule_load(const char *cwd, capsule_t *c);

/* ── Compaction hook: offload a dropped span, return its ctxkey ────────────
 * Writes `text` to the default context-fabric broker (kind=run, source notes
 * the capsule), appends the resulting ctxkey to the capsule for `cwd`.
 * Returns 0 on success; -1 if the fabric is unavailable (caller degrades to
 * lossy-only compaction; capsule gets no key for this span). */
int capsule_offload_span(capsule_t *c, const char *text, size_t len,
                         const char *provenance);

/* ── Session-start injection: build the capsule block ──────────────────────
 * If a capsule exists for cwd, returns a malloc'd text block:
 *
 *   [Session capsule — prior context from this directory]
 *   <summary>
 *   Offloaded context (fault in on demand):
 *     ck:run:... (source, preview)
 *
 * Caller frees. NULL if no capsule exists. Never raises on internal errors —
 * capsule injection is advisory by design. */
char *capsule_inject_block(const char *cwd, size_t max_bytes);

#endif /* DSCO_CAPSULE_H */
