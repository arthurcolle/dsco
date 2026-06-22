#ifndef DSCO_SESSION_MEMORY_H
#define DSCO_SESSION_MEMORY_H

#include "memory_tier.h"
#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Priority 5: Memory/Context Persistence
 *
 * Persistent key-value scratchpad + session history. Three tiers:
 *   Working  (TTL ≤ 60s)   — ephemeral facts for the current task
 *   Episodic (TTL ≤ 3600s) — recent session insights
 *   Semantic (TTL = 0)     — permanent learned facts
 *
 * Stored as flat JSON at $DSCO_SESSION_PATH or ~/.dsco/memory/sessions.json.
 * Automatic tier promotion: episodic → semantic when access_count ≥ 3.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Limits */
#define SESSION_ID_LEN         64
#define SESSION_TASK_LEN       512
#define SESSION_KEY_LEN        128
#define SESSION_VALUE_LEN      1024
#define SESSION_KV_JSON_LEN    8192
#define SESSION_MAX_RECORDS    256
#define SESSION_MAX_KV         512

/* TTL presets that map to memory tiers */
#define SESSION_TTL_WORKING    60       /* seconds */
#define SESSION_TTL_EPISODIC   3600     /* seconds */
#define SESSION_TTL_SEMANTIC   0        /* no expiry */

/* Access count threshold for episodic → semantic promotion */
#define SESSION_PROMOTE_THRESHOLD  3

/* ── KV entry ─────────────────────────────────────────────────────────── */

typedef struct {
    char          key[SESSION_KEY_LEN];
    char          value[SESSION_VALUE_LEN];
    double        created_at;
    double        accessed_at;
    int           access_count;
    int           ttl_seconds;    /* 0 = permanent (semantic) */
    memory_tier_t tier;
    bool          active;
} session_kv_t;

/* ── Session record ───────────────────────────────────────────────────── */

/* One completed or active session: tracks the task it handled plus a
   snapshot of all KV pairs that were live when the session ended. */
typedef struct {
    char          id[SESSION_ID_LEN];
    char          task_text[SESSION_TASK_LEN];
    char          key_values[SESSION_KV_JSON_LEN]; /* JSON {"k":"v",...} snapshot */
    double        created_at;
    double        accessed_at;
    int           access_count;
    memory_tier_t tier;
    bool          active;
} session_record_t;

/* ── Database ─────────────────────────────────────────────────────────── */

typedef struct {
    session_kv_t     kv[SESSION_MAX_KV];
    int              kv_count;
    session_record_t records[SESSION_MAX_RECORDS];
    int              record_count;
    char             db_path[512];
    char             current_session_id[SESSION_ID_LEN];
    bool             dirty;
    bool             initialized;
} session_db_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/* Load (or create) the session DB.  task_text is the current session's
   description; pass NULL if unknown.  Returns 0 on success, -1 on error.
   Respects $DSCO_SESSION_PATH override (useful for testing). */
int  session_init(session_db_t *db, const char *task_text);

/* Clear in-memory state.  Does NOT flush — call session_persist() first. */
void session_free(session_db_t *db);

/* Flush dirty state to disk.  Alias for session_persist(). */
int  session_flush(session_db_t *db);

/* ── KV store ─────────────────────────────────────────────────────────── */

/* Store key → value with a TTL (seconds; 0 = permanent).
   Tier is inferred from TTL: 0 → semantic, ≤60 → working, else → episodic.
   Updates existing entry if the key already exists.
   Returns 0 on success, -1 on error. */
int session_remember(session_db_t *db, const char *key,
                     const char *value, int ttl_seconds);

/* Retrieve value for key.  Returns a pointer into db->kv[i].value (valid
   until the next mutating call), or NULL if not found or expired.
   Bumps access_count and accessed_at on hit. */
const char *session_recall(session_db_t *db, const char *key);

/* ── Session history ──────────────────────────────────────────────────── */

/* Find past sessions whose task_text is similar to `task` (word-overlap
   Jaccard similarity).  Writes up to k pointers into out[].
   Returns count written (0 if none match).
   Current session is excluded from results. */
int session_lookup_similar(session_db_t *db, const char *task,
                           const session_record_t **out, int k);

/* ── Tier promotion ───────────────────────────────────────────────────── */

/* Scan KV store.  Entries with access_count >= SESSION_PROMOTE_THRESHOLD:
 *   working  → episodic  (TTL extended to SESSION_TTL_EPISODIC)
 *   episodic → semantic  (TTL cleared to 0, permanent)
 * Returns number of promotions performed. */
int session_promote(session_db_t *db);

/* ── Persistence ──────────────────────────────────────────────────────── */

/* Snapshot current KV into this session's record and write to disk.
   Returns 0 on success, -1 on error. */
int session_persist(session_db_t *db);

/* Convenience: promote() then persist().  Call at natural session end. */
int session_end(session_db_t *db);

/* Evict KV entries whose TTL has elapsed.  Returns count evicted. */
int session_evict_expired(session_db_t *db);

/* ── Diagnostics ──────────────────────────────────────────────────────── */

/* Write a JSON summary of current DB state to buf.
   Returns bytes written (not including NUL). */
int session_status_json(const session_db_t *db, char *buf, size_t len);

#endif /* DSCO_SESSION_MEMORY_H */
