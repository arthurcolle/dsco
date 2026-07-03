#include "vfs.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

/* ── Internal handle ───────────────────────────────────────────────── */

struct vfs_db {
    sqlite3 *db;
    pthread_mutex_t mu;

    /* Cached prepared statements */
    sqlite3_stmt *kv_put;
    sqlite3_stmt *kv_get;
    sqlite3_stmt *kv_del;
    sqlite3_stmt *kv_keys;
    sqlite3_stmt *conv_ins;
    sqlite3_stmt *conv_load;
    sqlite3_stmt *conv_del;
    sqlite3_stmt *conv_sessions;
    sqlite3_stmt *event_ins;
    sqlite3_stmt *event_query;
    sqlite3_stmt *cache_put;
    sqlite3_stmt *cache_get;
    sqlite3_stmt *cache_evict;
    sqlite3_stmt *result_put;
    sqlite3_stmt *result_get;
    sqlite3_stmt *result_evict;
    sqlite3_stmt *result_list;

    /* Stats */
    int64_t cache_hits;
    int64_t cache_misses;
};

static void vfs_lock(vfs_db_t *db) {
    pthread_mutex_lock(&db->mu);
}

static void vfs_unlock(vfs_db_t *db) {
    pthread_mutex_unlock(&db->mu);
}

/* ── Schema ────────────────────────────────────────────────────────── */

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS kv ("
    "  bucket TEXT NOT NULL,"
    "  key TEXT NOT NULL,"
    "  value BLOB,"
    "  updated_at INTEGER DEFAULT (strftime('%s','now')),"
    "  PRIMARY KEY (bucket, key)"
    ");"
    "CREATE TABLE IF NOT EXISTS conversations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id TEXT NOT NULL,"
    "  role TEXT NOT NULL,"
    "  content_json TEXT,"
    "  timestamp INTEGER DEFAULT (strftime('%s','now'))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_conv_session ON conversations(session_id, id);"
    "CREATE TABLE IF NOT EXISTS events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  category TEXT,"
    "  action TEXT,"
    "  detail TEXT,"
    "  timestamp INTEGER DEFAULT (strftime('%s','now'))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_events_cat ON events(category, timestamp);"
    "CREATE TABLE IF NOT EXISTS cache ("
    "  tool_name TEXT NOT NULL,"
    "  input_hash TEXT NOT NULL,"
    "  result TEXT,"
    "  expires_at INTEGER,"
    "  created_at INTEGER DEFAULT (strftime('%s','now')),"
    "  PRIMARY KEY (tool_name, input_hash)"
    ");"
    "CREATE TABLE IF NOT EXISTS tool_results ("
    "  key TEXT PRIMARY KEY,"
    "  tool_name TEXT NOT NULL,"
    "  input_hash TEXT NOT NULL,"
    "  result TEXT,"
    "  result_len INTEGER,"
    "  created_at INTEGER DEFAULT (strftime('%s','now')),"
    "  expires_at INTEGER,"
    "  access_count INTEGER DEFAULT 0,"
    "  session_id TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_tool_results_expires ON tool_results(expires_at);"
    "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER);"
    "INSERT OR IGNORE INTO schema_version VALUES (1);";

/* ── Helper: finalize all statements ───────────────────────────────── */

static void finalize_all(vfs_db_t *db) {
    if (db->kv_put)
        sqlite3_finalize(db->kv_put);
    if (db->kv_get)
        sqlite3_finalize(db->kv_get);
    if (db->kv_del)
        sqlite3_finalize(db->kv_del);
    if (db->kv_keys)
        sqlite3_finalize(db->kv_keys);
    if (db->conv_ins)
        sqlite3_finalize(db->conv_ins);
    if (db->conv_load)
        sqlite3_finalize(db->conv_load);
    if (db->conv_del)
        sqlite3_finalize(db->conv_del);
    if (db->conv_sessions)
        sqlite3_finalize(db->conv_sessions);
    if (db->event_ins)
        sqlite3_finalize(db->event_ins);
    if (db->event_query)
        sqlite3_finalize(db->event_query);
    if (db->cache_put)
        sqlite3_finalize(db->cache_put);
    if (db->cache_get)
        sqlite3_finalize(db->cache_get);
    if (db->cache_evict)
        sqlite3_finalize(db->cache_evict);
    if (db->result_put)
        sqlite3_finalize(db->result_put);
    if (db->result_get)
        sqlite3_finalize(db->result_get);
    if (db->result_evict)
        sqlite3_finalize(db->result_evict);
    if (db->result_list)
        sqlite3_finalize(db->result_list);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

vfs_db_t *vfs_open(const char *path) {
    if (!path)
        return NULL;

    vfs_db_t *vdb = calloc(1, sizeof(vfs_db_t));
    if (!vdb)
        return NULL;

    if (pthread_mutex_init(&vdb->mu, NULL) != 0) {
        free(vdb);
        return NULL;
    }

    int rc = sqlite3_open_v2(path, &vdb->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vfs_open: %s\n", sqlite3_errmsg(vdb->db));
        sqlite3_close(vdb->db);
        pthread_mutex_destroy(&vdb->mu);
        free(vdb);
        return NULL;
    }

    /* Pragmas for performance */
    sqlite3_exec(vdb->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(vdb->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(vdb->db, 5000);

    /* Create schema */
    char *err = NULL;
    rc = sqlite3_exec(vdb->db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vfs_open schema: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_close(vdb->db);
        pthread_mutex_destroy(&vdb->mu);
        free(vdb);
        return NULL;
    }

    /* Prepare commonly used statements */
    sqlite3_prepare_v2(vdb->db,
                       "INSERT OR REPLACE INTO kv (bucket, key, value, updated_at) "
                       "VALUES (?, ?, ?, strftime('%s','now'))",
                       -1, &vdb->kv_put, NULL);
    sqlite3_prepare_v2(vdb->db, "SELECT value FROM kv WHERE bucket=? AND key=?", -1, &vdb->kv_get,
                       NULL);
    sqlite3_prepare_v2(vdb->db, "DELETE FROM kv WHERE bucket=? AND key=?", -1, &vdb->kv_del, NULL);
    sqlite3_prepare_v2(vdb->db, "SELECT key FROM kv WHERE bucket=? ORDER BY key", -1, &vdb->kv_keys,
                       NULL);
    sqlite3_prepare_v2(
        vdb->db, "INSERT INTO conversations (session_id, role, content_json) VALUES (?, ?, ?)", -1,
        &vdb->conv_ins, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "SELECT role, content_json, timestamp FROM conversations "
                       "WHERE session_id=? ORDER BY id",
                       -1, &vdb->conv_load, NULL);
    sqlite3_prepare_v2(vdb->db, "DELETE FROM conversations WHERE session_id=?", -1, &vdb->conv_del,
                       NULL);
    sqlite3_prepare_v2(vdb->db, "SELECT DISTINCT session_id FROM conversations ORDER BY session_id",
                       -1, &vdb->conv_sessions, NULL);
    sqlite3_prepare_v2(vdb->db, "INSERT INTO events (category, action, detail) VALUES (?, ?, ?)",
                       -1, &vdb->event_ins, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "SELECT timestamp, category, action, detail FROM events "
                       "WHERE (?1 IS NULL OR category=?1) ORDER BY id DESC LIMIT ?2",
                       -1, &vdb->event_query, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "INSERT OR REPLACE INTO cache (tool_name, input_hash, result, expires_at) "
                       "VALUES (?, ?, ?, strftime('%s','now') + ?)",
                       -1, &vdb->cache_put, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "SELECT result FROM cache WHERE tool_name=? AND input_hash=? "
                       "AND expires_at > strftime('%s','now')",
                       -1, &vdb->cache_get, NULL);
    sqlite3_prepare_v2(vdb->db, "DELETE FROM cache WHERE expires_at <= strftime('%s','now')", -1,
                       &vdb->cache_evict, NULL);

    /* tool_results table (may not exist in old DBs) */
    sqlite3_exec(vdb->db,
                 "CREATE TABLE IF NOT EXISTS tool_results ("
                 "  key TEXT PRIMARY KEY,"
                 "  tool_name TEXT NOT NULL,"
                 "  input_hash TEXT NOT NULL,"
                 "  result TEXT,"
                 "  result_len INTEGER,"
                 "  created_at INTEGER DEFAULT (strftime('%s','now')),"
                 "  expires_at INTEGER,"
                 "  access_count INTEGER DEFAULT 0,"
                 "  session_id TEXT"
                 ");"
                 "CREATE INDEX IF NOT EXISTS idx_tool_results_expires ON tool_results(expires_at);",
                 NULL, NULL, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "INSERT OR REPLACE INTO tool_results (key, tool_name, input_hash, result, "
                       "result_len, expires_at) "
                       "VALUES (?, ?, ?, ?, ?, strftime('%s','now') + ?)",
                       -1, &vdb->result_put, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "SELECT result FROM tool_results WHERE key=? "
                       "AND (expires_at IS NULL OR expires_at > strftime('%s','now'))",
                       -1, &vdb->result_get, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "DELETE FROM tool_results WHERE expires_at IS NOT NULL AND expires_at <= "
                       "strftime('%s','now')",
                       -1, &vdb->result_evict, NULL);
    sqlite3_prepare_v2(vdb->db,
                       "SELECT key FROM tool_results WHERE expires_at IS NULL OR expires_at > "
                       "strftime('%s','now') "
                       "ORDER BY created_at DESC",
                       -1, &vdb->result_list, NULL);

    return vdb;
}

void vfs_close(vfs_db_t *db) {
    if (!db)
        return;
    vfs_lock(db);
    finalize_all(db);
    sqlite3_close(db->db);
    vfs_unlock(db);
    pthread_mutex_destroy(&db->mu);
    free(db);
}

/* ── Key-value store ───────────────────────────────────────────────── */

bool vfs_kv_put(vfs_db_t *db, const char *bucket, const char *key, const void *val,
                size_t val_len) {
    if (!db || !db->kv_put)
        return false;
    vfs_lock(db);
    sqlite3_reset(db->kv_put);
    sqlite3_bind_text(db->kv_put, 1, bucket, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->kv_put, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(db->kv_put, 3, val, (int)val_len, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(db->kv_put) == SQLITE_DONE;
    sqlite3_reset(db->kv_put);
    vfs_unlock(db);
    return ok;
}

bool vfs_kv_put_str(vfs_db_t *db, const char *bucket, const char *key, const char *val) {
    return vfs_kv_put(db, bucket, key, val, val ? strlen(val) : 0);
}

void *vfs_kv_get(vfs_db_t *db, const char *bucket, const char *key, size_t *out_len) {
    if (!db || !db->kv_get)
        return NULL;
    vfs_lock(db);
    sqlite3_reset(db->kv_get);
    sqlite3_bind_text(db->kv_get, 1, bucket, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->kv_get, 2, key, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(db->kv_get) != SQLITE_ROW) {
        sqlite3_reset(db->kv_get);
        vfs_unlock(db);
        return NULL;
    }

    int len = sqlite3_column_bytes(db->kv_get, 0);
    const void *data = sqlite3_column_blob(db->kv_get, 0);
    if (!data || len <= 0) {
        sqlite3_reset(db->kv_get);
        vfs_unlock(db);
        return NULL;
    }

    void *copy = malloc((size_t)len);
    if (copy) {
        memcpy(copy, data, (size_t)len);
        if (out_len)
            *out_len = (size_t)len;
    }
    sqlite3_reset(db->kv_get);
    vfs_unlock(db);
    return copy;
}

char *vfs_kv_get_str(vfs_db_t *db, const char *bucket, const char *key) {
    size_t len = 0;
    void *data = vfs_kv_get(db, bucket, key, &len);
    if (!data)
        return NULL;

    /* Ensure null termination */
    char *str = malloc(len + 1);
    if (str) {
        memcpy(str, data, len);
        str[len] = '\0';
    }
    free(data);
    return str;
}

bool vfs_kv_delete(vfs_db_t *db, const char *bucket, const char *key) {
    if (!db || !db->kv_del)
        return false;
    vfs_lock(db);
    sqlite3_reset(db->kv_del);
    sqlite3_bind_text(db->kv_del, 1, bucket, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->kv_del, 2, key, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(db->kv_del) == SQLITE_DONE;
    sqlite3_reset(db->kv_del);
    vfs_unlock(db);
    return ok;
}

char **vfs_kv_keys(vfs_db_t *db, const char *bucket, int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!db || !db->kv_keys)
        return NULL;

    int cap = 64;
    int count = 0;
    char **keys = malloc((size_t)cap * sizeof(char *));
    if (!keys)
        return NULL;

    vfs_lock(db);
    sqlite3_reset(db->kv_keys);
    sqlite3_bind_text(db->kv_keys, 1, bucket, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(db->kv_keys) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(keys, (size_t)cap * sizeof(char *));
            if (!tmp)
                break;
            keys = tmp;
        }
        const char *k = (const char *)sqlite3_column_text(db->kv_keys, 0);
        keys[count++] = k ? strdup(k) : strdup("");
    }
    sqlite3_reset(db->kv_keys);
    vfs_unlock(db);

    if (out_count)
        *out_count = count;
    return keys;
}

/* ── Conversation storage ──────────────────────────────────────────── */

bool vfs_conv_append(vfs_db_t *db, const char *session_id, const char *role,
                     const char *content_json) {
    if (!db || !db->conv_ins)
        return false;
    vfs_lock(db);
    sqlite3_reset(db->conv_ins);
    sqlite3_bind_text(db->conv_ins, 1, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->conv_ins, 2, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->conv_ins, 3, content_json, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(db->conv_ins) == SQLITE_DONE;
    sqlite3_reset(db->conv_ins);
    vfs_unlock(db);
    return ok;
}

vfs_conv_turn_t *vfs_conv_load(vfs_db_t *db, const char *session_id, int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!db || !db->conv_load)
        return NULL;

    int cap = 64;
    int count = 0;
    vfs_conv_turn_t *turns = malloc((size_t)cap * sizeof(vfs_conv_turn_t));
    if (!turns)
        return NULL;

    vfs_lock(db);
    sqlite3_reset(db->conv_load);
    sqlite3_bind_text(db->conv_load, 1, session_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(db->conv_load) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            vfs_conv_turn_t *tmp = realloc(turns, (size_t)cap * sizeof(vfs_conv_turn_t));
            if (!tmp)
                break;
            turns = tmp;
        }
        const char *r = (const char *)sqlite3_column_text(db->conv_load, 0);
        const char *c = (const char *)sqlite3_column_text(db->conv_load, 1);
        turns[count].role = r ? strdup(r) : strdup("");
        turns[count].content_json = c ? strdup(c) : strdup("");
        turns[count].timestamp = sqlite3_column_int64(db->conv_load, 2);
        count++;
    }
    sqlite3_reset(db->conv_load);
    vfs_unlock(db);

    if (out_count)
        *out_count = count;
    return turns;
}

void vfs_conv_free(vfs_conv_turn_t *turns, int count) {
    if (!turns)
        return;
    for (int i = 0; i < count; i++) {
        free(turns[i].role);
        free(turns[i].content_json);
    }
    free(turns);
}

bool vfs_conv_delete(vfs_db_t *db, const char *session_id) {
    if (!db || !db->conv_del)
        return false;
    vfs_lock(db);
    sqlite3_reset(db->conv_del);
    sqlite3_bind_text(db->conv_del, 1, session_id, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(db->conv_del) == SQLITE_DONE;
    sqlite3_reset(db->conv_del);
    vfs_unlock(db);
    return ok;
}

char **vfs_conv_sessions(vfs_db_t *db, int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!db || !db->conv_sessions)
        return NULL;

    int cap = 32;
    int count = 0;
    char **ids = malloc((size_t)cap * sizeof(char *));
    if (!ids)
        return NULL;

    vfs_lock(db);
    sqlite3_reset(db->conv_sessions);
    while (sqlite3_step(db->conv_sessions) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(ids, (size_t)cap * sizeof(char *));
            if (!tmp)
                break;
            ids = tmp;
        }
        const char *s = (const char *)sqlite3_column_text(db->conv_sessions, 0);
        ids[count++] = s ? strdup(s) : strdup("");
    }
    sqlite3_reset(db->conv_sessions);
    vfs_unlock(db);

    if (out_count)
        *out_count = count;
    return ids;
}

/* ── Event log ─────────────────────────────────────────────────────── */

bool vfs_log_event(vfs_db_t *db, const char *category, const char *action, const char *detail) {
    if (!db || !db->event_ins)
        return false;
    vfs_lock(db);
    sqlite3_reset(db->event_ins);
    sqlite3_bind_text(db->event_ins, 1, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->event_ins, 2, action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->event_ins, 3, detail, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(db->event_ins) == SQLITE_DONE;
    sqlite3_reset(db->event_ins);
    vfs_unlock(db);
    return ok;
}

vfs_event_t *vfs_log_query(vfs_db_t *db, const char *category, int limit, int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!db || !db->event_query)
        return NULL;

    int cap = 64;
    int count = 0;
    vfs_event_t *events = malloc((size_t)cap * sizeof(vfs_event_t));
    if (!events)
        return NULL;

    vfs_lock(db);
    sqlite3_reset(db->event_query);
    if (category)
        sqlite3_bind_text(db->event_query, 1, category, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(db->event_query, 1);
    sqlite3_bind_int(db->event_query, 2, limit > 0 ? limit : 100);
    while (sqlite3_step(db->event_query) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            vfs_event_t *tmp = realloc(events, (size_t)cap * sizeof(vfs_event_t));
            if (!tmp)
                break;
            events = tmp;
        }
        events[count].timestamp = sqlite3_column_int64(db->event_query, 0);
        const char *c = (const char *)sqlite3_column_text(db->event_query, 1);
        const char *a = (const char *)sqlite3_column_text(db->event_query, 2);
        const char *d = (const char *)sqlite3_column_text(db->event_query, 3);
        events[count].category = c ? strdup(c) : strdup("");
        events[count].action = a ? strdup(a) : strdup("");
        events[count].detail = d ? strdup(d) : strdup("");
        count++;
    }
    sqlite3_reset(db->event_query);
    vfs_unlock(db);

    if (out_count)
        *out_count = count;
    return events;
}

void vfs_log_free(vfs_event_t *events, int count) {
    if (!events)
        return;
    for (int i = 0; i < count; i++) {
        free(events[i].category);
        free(events[i].action);
        free(events[i].detail);
    }
    free(events);
}

/* ── Tool result cache ─────────────────────────────────────────────── */

bool vfs_cache_put(vfs_db_t *db, const char *tool_name, const char *input_hash, const char *result,
                   int ttl_seconds) {
    if (!db || !db->cache_put)
        return false;
    vfs_lock(db);
    sqlite3_reset(db->cache_put);
    sqlite3_bind_text(db->cache_put, 1, tool_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->cache_put, 2, input_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->cache_put, 3, result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(db->cache_put, 4, ttl_seconds);
    bool ok = sqlite3_step(db->cache_put) == SQLITE_DONE;
    sqlite3_reset(db->cache_put);
    vfs_unlock(db);
    return ok;
}

char *vfs_cache_get(vfs_db_t *db, const char *tool_name, const char *input_hash) {
    if (!db || !db->cache_get)
        return NULL;
    vfs_lock(db);
    sqlite3_reset(db->cache_get);
    sqlite3_bind_text(db->cache_get, 1, tool_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->cache_get, 2, input_hash, -1, SQLITE_TRANSIENT);

    char *ret = NULL;
    if (sqlite3_step(db->cache_get) == SQLITE_ROW) {
        const char *r = (const char *)sqlite3_column_text(db->cache_get, 0);
        db->cache_hits++;
        ret = r ? strdup(r) : NULL;
    } else {
        db->cache_misses++;
    }
    sqlite3_reset(db->cache_get);
    vfs_unlock(db);
    return ret;
}

int vfs_cache_evict(vfs_db_t *db) {
    if (!db || !db->cache_evict)
        return 0;
    vfs_lock(db);
    sqlite3_reset(db->cache_evict);
    sqlite3_step(db->cache_evict);
    int changes = sqlite3_changes(db->db);
    sqlite3_reset(db->cache_evict);
    vfs_unlock(db);
    return changes;
}

/* ── Tool result persistence ──────────────────────────────────────── */

bool vfs_result_put(vfs_db_t *db, const char *tool_name, const char *input_hash, const char *result,
                    int ttl_seconds) {
    if (!db || !db->result_put || !tool_name || !input_hash || !result)
        return false;

    /* Build key: tool:hash[:16] */
    char key[128];
    snprintf(key, sizeof(key), "%s:%.16s", tool_name, input_hash);

    vfs_lock(db);
    sqlite3_reset(db->result_put);
    sqlite3_bind_text(db->result_put, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->result_put, 2, tool_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->result_put, 3, input_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(db->result_put, 4, result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(db->result_put, 5, (int)strlen(result));
    sqlite3_bind_int(db->result_put, 6, ttl_seconds);
    bool ok = sqlite3_step(db->result_put) == SQLITE_DONE;
    sqlite3_reset(db->result_put);
    vfs_unlock(db);
    return ok;
}

char *vfs_result_get(vfs_db_t *db, const char *key) {
    if (!db || !db->result_get || !key)
        return NULL;

    vfs_lock(db);

    /* Bump access_count */
    sqlite3_stmt *bump = NULL;
    sqlite3_prepare_v2(db->db,
                       "UPDATE tool_results SET access_count = access_count + 1 WHERE key=?", -1,
                       &bump, NULL);
    if (bump) {
        sqlite3_bind_text(bump, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_step(bump);
        sqlite3_finalize(bump);
    }

    sqlite3_reset(db->result_get);
    sqlite3_bind_text(db->result_get, 1, key, -1, SQLITE_TRANSIENT);

    char *result = NULL;
    if (sqlite3_step(db->result_get) == SQLITE_ROW) {
        const char *txt = (const char *)sqlite3_column_text(db->result_get, 0);
        if (txt)
            result = strdup(txt);
    }
    sqlite3_reset(db->result_get);
    vfs_unlock(db);
    return result;
}

int vfs_result_evict(vfs_db_t *db) {
    if (!db || !db->result_evict)
        return 0;
    vfs_lock(db);
    sqlite3_reset(db->result_evict);
    sqlite3_step(db->result_evict);
    int changes = sqlite3_changes(db->db);
    sqlite3_reset(db->result_evict);
    vfs_unlock(db);
    return changes;
}

char **vfs_result_list(vfs_db_t *db, int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!db || !db->result_list)
        return NULL;

    int cap = 64;
    int count = 0;
    char **keys = malloc((size_t)cap * sizeof(char *));
    if (!keys)
        return NULL;

    vfs_lock(db);
    sqlite3_reset(db->result_list);
    while (sqlite3_step(db->result_list) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(keys, (size_t)cap * sizeof(char *));
            if (!tmp)
                break;
            keys = tmp;
        }
        const char *k = (const char *)sqlite3_column_text(db->result_list, 0);
        keys[count++] = k ? strdup(k) : strdup("");
    }
    sqlite3_reset(db->result_list);
    vfs_unlock(db);

    if (out_count)
        *out_count = count;
    return keys;
}

/* ── Schema version ────────────────────────────────────────────────── */

int vfs_schema_version(vfs_db_t *db) {
    if (!db)
        return -1;
    vfs_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, "SELECT version FROM schema_version LIMIT 1", -1, &stmt, NULL);
    int ver = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        ver = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    vfs_unlock(db);
    return ver;
}

/* ── Stats ─────────────────────────────────────────────────────────── */

static int64_t count_table(vfs_db_t *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *stmt = NULL;
    int64_t count = 0;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

vfs_stats_t vfs_get_stats(vfs_db_t *db) {
    vfs_stats_t st;
    memset(&st, 0, sizeof(st));
    if (!db)
        return st;

    vfs_lock(db);
    st.kv_entries = count_table(db, "kv");
    st.conv_turns = count_table(db, "conversations");
    st.event_count = count_table(db, "events");
    st.cache_entries = count_table(db, "cache");
    st.cache_hits = db->cache_hits;
    st.cache_misses = db->cache_misses;

    /* Approximate DB size */
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->db,
                           "SELECT page_count * page_size FROM pragma_page_count, pragma_page_size",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            st.db_size_bytes = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    vfs_unlock(db);

    return st;
}
