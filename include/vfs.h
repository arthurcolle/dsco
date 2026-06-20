#ifndef DSCO_VFS_H
#define DSCO_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Database handle ───────────────────────────────────────────────── */

typedef struct vfs_db vfs_db_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

vfs_db_t *vfs_open(const char *path);
void      vfs_close(vfs_db_t *db);

/* ── Key-value store ───────────────────────────────────────────────── */

bool      vfs_kv_put(vfs_db_t *db, const char *bucket, const char *key,
                     const void *val, size_t val_len);
bool      vfs_kv_put_str(vfs_db_t *db, const char *bucket, const char *key,
                         const char *val);
void     *vfs_kv_get(vfs_db_t *db, const char *bucket, const char *key,
                     size_t *out_len);
char     *vfs_kv_get_str(vfs_db_t *db, const char *bucket, const char *key);
bool      vfs_kv_delete(vfs_db_t *db, const char *bucket, const char *key);
char    **vfs_kv_keys(vfs_db_t *db, const char *bucket, int *out_count);

/* ── Conversation storage ──────────────────────────────────────────── */

bool      vfs_conv_append(vfs_db_t *db, const char *session_id,
                          const char *role, const char *content_json);

typedef struct {
    char   *role;
    char   *content_json;
    int64_t timestamp;
} vfs_conv_turn_t;

vfs_conv_turn_t *vfs_conv_load(vfs_db_t *db, const char *session_id,
                               int *out_count);
void             vfs_conv_free(vfs_conv_turn_t *turns, int count);
bool             vfs_conv_delete(vfs_db_t *db, const char *session_id);
char           **vfs_conv_sessions(vfs_db_t *db, int *out_count);

/* ── Event log ─────────────────────────────────────────────────────── */

bool      vfs_log_event(vfs_db_t *db, const char *category,
                        const char *action, const char *detail);

typedef struct {
    int64_t  timestamp;
    char    *category;
    char    *action;
    char    *detail;
} vfs_event_t;

vfs_event_t *vfs_log_query(vfs_db_t *db, const char *category,
                           int limit, int *out_count);
void         vfs_log_free(vfs_event_t *events, int count);

/* ── Tool result cache ─────────────────────────────────────────────── */

bool      vfs_cache_put(vfs_db_t *db, const char *tool_name,
                        const char *input_hash, const char *result,
                        int ttl_seconds);
char     *vfs_cache_get(vfs_db_t *db, const char *tool_name,
                        const char *input_hash);
int       vfs_cache_evict(vfs_db_t *db);

/* ── Schema ────────────────────────────────────────────────────────── */

int       vfs_schema_version(vfs_db_t *db);

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    int64_t kv_entries;
    int64_t conv_turns;
    int64_t event_count;
    int64_t cache_entries;
    int64_t cache_hits;
    int64_t cache_misses;
    int64_t db_size_bytes;
} vfs_stats_t;

vfs_stats_t vfs_get_stats(vfs_db_t *db);

/* ── Tool result persistence (Phase 2: context packing overhaul) ───── */

bool      vfs_result_put(vfs_db_t *db, const char *tool_name,
                         const char *input_hash, const char *result,
                         int ttl_seconds);
char     *vfs_result_get(vfs_db_t *db, const char *key);
int       vfs_result_evict(vfs_db_t *db);
char    **vfs_result_list(vfs_db_t *db, int *out_count);

#endif /* DSCO_VFS_H */
