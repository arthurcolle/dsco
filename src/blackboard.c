#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "blackboard.h"
#include "crypto.h"
#include "json_util.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

/* The database is the authority. Agents carry an attempt generation and a
 * random claim token. No in-memory singleton, Git polling, or LLM scheduler is
 * needed to make competing processes agree on ownership and accepted inputs. */
const char blackboard_tool_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"status\",\"events\",\"claim\","
    "\"renew\",\"publish\",\"verify\",\"invalidate\"]},"
    "\"path\":{\"type\":\"string\",\"description\":\"Explicit SQLite board path; parent directory "
    "must exist\"},"
    "\"task\":{\"type\":\"string\",\"maxLength\":64},"
    "\"title\":{\"type\":\"string\",\"maxLength\":1024},"
    "\"check\":{\"type\":\"string\",\"maxLength\":4096,\"description\":\"Frozen bash acceptance "
    "command. Reads JSON at $DSCO_BLACKBOARD_SNAPSHOT; exit zero means pass\"},"
    "\"dependencies\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"string\"}},"
    "\"owner\":{\"type\":\"string\",\"maxLength\":96},"
    "\"token\":{\"type\":\"string\",\"maxLength\":32},"
    "\"generation\":{\"type\":\"integer\",\"minimum\":1},"
    "\"ttl\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600},"
    "\"payload\":{\"type\":\"string\",\"maxLength\":32768},"
    "\"artifact\":{\"type\":\"integer\",\"minimum\":1},"
    "\"reason\":{\"type\":\"string\",\"maxLength\":1024},"
    "\"after\":{\"type\":\"integer\",\"minimum\":0},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}"
    "},\"required\":[\"action\",\"path\"],\"additionalProperties\":false}";

static const char schema[] =
    "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"
    "CREATE TABLE IF NOT EXISTS bb_meta(version INTEGER NOT NULL);"
    "INSERT INTO bb_meta SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM bb_meta);"
    "CREATE TABLE IF NOT EXISTS bb_tasks("
    "key TEXT PRIMARY KEY,title TEXT NOT NULL,checker TEXT NOT NULL,cwd TEXT NOT NULL,"
    "state TEXT NOT NULL DEFAULT 'pending',generation INTEGER NOT NULL DEFAULT 0,"
    "owner TEXT NOT NULL DEFAULT '',token TEXT NOT NULL DEFAULT '',expires INTEGER NOT NULL "
    "DEFAULT 0,"
    "candidate INTEGER,accepted INTEGER);"
    "CREATE TABLE IF NOT EXISTS bb_deps(task TEXT NOT NULL REFERENCES bb_tasks(key),"
    "input TEXT NOT NULL REFERENCES bb_tasks(key),PRIMARY KEY(task,input));"
    "CREATE INDEX IF NOT EXISTS bb_deps_input ON bb_deps(input);"
    "CREATE TABLE IF NOT EXISTS bb_artifacts(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "task TEXT NOT NULL,generation INTEGER NOT NULL,payload TEXT NOT NULL,sha256 TEXT NOT NULL,"
    "UNIQUE(task,generation));"
    "CREATE TABLE IF NOT EXISTS bb_inputs(task TEXT NOT NULL,generation INTEGER NOT NULL,"
    "input TEXT NOT NULL,artifact INTEGER NOT NULL,PRIMARY KEY(task,generation,input));"
    "CREATE TABLE IF NOT EXISTS bb_checks(id INTEGER PRIMARY KEY AUTOINCREMENT,artifact INTEGER "
    "NOT NULL,"
    "passed INTEGER NOT NULL,accepted INTEGER NOT NULL,output TEXT NOT NULL,created_ms INTEGER NOT "
    "NULL);"
    "CREATE TABLE IF NOT EXISTS bb_events(seq INTEGER PRIMARY KEY AUTOINCREMENT,"
    "task TEXT NOT NULL,kind TEXT NOT NULL,generation INTEGER NOT NULL,detail TEXT NOT "
    "NULL,created_ms INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS bb_events_task ON bb_events(task,seq); COMMIT;";

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static bool error_json(char *out, size_t n, const char *message) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"error\":");
    jbuf_append_json_str(&b, message);
    jbuf_append(&b, "}");
    snprintf(out, n, "%s", b.data);
    jbuf_free(&b);
    return false;
}
static bool emit(jbuf_t *b, char *out, size_t n) {
    if (b->len >= n)
        return error_json(out, n, "response too large; use a smaller limit or a single task");
    memcpy(out, b->data, b->len + 1);
    return true;
}
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    return s;
}
static const char *column(sqlite3_stmt *s, int i) {
    const char *p = (const char *)sqlite3_column_text(s, i);
    return p ? p : "";
}
static void bind_text(sqlite3_stmt *s, int i, const char *v) {
    sqlite3_bind_text(s, i, v ? v : "", -1, SQLITE_TRANSIENT);
}
static bool done(sqlite3_stmt *s) {
    if (!s)
        return false;
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
static bool sql(sqlite3 *db, const char *q) {
    return sqlite3_exec(db, q, NULL, NULL, NULL) == SQLITE_OK;
}
static bool event(sqlite3 *db, const char *task, const char *kind, long long gen,
                  const char *detail) {
    sqlite3_stmt *s = prepare(
        db, "INSERT INTO bb_events(task,kind,generation,detail,created_ms) VALUES(?,?,?,?,?)");
    if (!s)
        return false;
    bind_text(s, 1, task);
    bind_text(s, 2, kind);
    sqlite3_bind_int64(s, 3, gen);
    bind_text(s, 4, detail);
    sqlite3_bind_int64(s, 5, now_ms());
    return done(s);
}
static const char *str(yyjson_val *v, const char *key) {
    return yyjson_get_str(yyjson_obj_get(v, key));
}
static long long number(yyjson_val *v, const char *key, long long fallback) {
    yyjson_val *n = yyjson_obj_get(v, key);
    return n ? yyjson_get_sint(n) : fallback;
}
static bool valid_id(const char *s) {
    if (!s || !*s || strlen(s) > 64)
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_' || *s == '.' || *s == ':'))
            return false;
    return true;
}
/* Validate independently of provider/MCP schema enforcement. Reject duplicate
 * keys, unknown/irrelevant fields, wrong numeric types, and embedded NULs. */
static bool validate(yyjson_val *v, const char **why) {
    const char *action = str(v, "action"), *allowed = NULL;
    if (!yyjson_is_obj(v) || !action) {
        *why = "object and action required";
        return false;
    }
    if (!strcmp(action, "create"))
        allowed = "|task|title|check|dependencies|";
    else if (!strcmp(action, "status"))
        allowed = "|task|artifact|after|limit|";
    else if (!strcmp(action, "events"))
        allowed = "|task|after|limit|";
    else if (!strcmp(action, "claim"))
        allowed = "|task|owner|ttl|";
    else if (!strcmp(action, "renew"))
        allowed = "|task|owner|token|generation|ttl|";
    else if (!strcmp(action, "publish"))
        allowed = "|task|owner|token|generation|payload|";
    else if (!strcmp(action, "verify"))
        allowed = "|artifact|";
    else if (!strcmp(action, "invalidate"))
        allowed = "|task|generation|reason|";
    else {
        *why = "unknown action";
        return false;
    }
    size_t idx, max;
    yyjson_val *k, *x;
    yyjson_obj_foreach(v, idx, max, k, x) {
        const char *key = yyjson_get_str(k);
        char needle[80];
        if (strlen(key) != yyjson_get_len(k) || strlen(key) > 64) {
            *why = "invalid key";
            return false;
        }
        snprintf(needle, sizeof(needle), "|%s|", key);
        if (strcmp(key, "action") && strcmp(key, "path") && !strstr(allowed, needle)) {
            *why = "field not allowed for this action";
            return false;
        }
        if (yyjson_obj_get(v, key) != x) {
            *why = "duplicate field";
            return false;
        }
        if (!strcmp(key, "dependencies")) {
            if (!yyjson_is_arr(x) || yyjson_arr_size(x) > 16) {
                *why = "dependencies must be an array of at most 16 existing IDs";
                return false;
            }
            size_t i, m;
            yyjson_val *d;
            yyjson_arr_foreach(x, i, m, d) {
                if (!yyjson_is_str(d) || yyjson_get_len(d) != strlen(yyjson_get_str(d)) ||
                    !valid_id(yyjson_get_str(d))) {
                    *why = "invalid dependency ID";
                    return false;
                }
                for (size_t previous = 0; previous < i; previous++) {
                    if (yyjson_equals_str(yyjson_arr_get(x, previous), yyjson_get_str(d))) {
                        *why = "duplicate dependency ID";
                        return false;
                    }
                }
            }
        } else if (!strcmp(key, "ttl") || !strcmp(key, "generation") || !strcmp(key, "artifact") ||
                   !strcmp(key, "after") || !strcmp(key, "limit")) {
            long long val = yyjson_get_sint(x);
            if (!yyjson_is_int(x) || val < (!strcmp(key, "after") ? 0 : 1) ||
                val > 9007199254740991LL || (!strcmp(key, "ttl") && val > 3600) ||
                (!strcmp(key, "limit") && val > 100)) {
                *why = "invalid integer or out-of-range value";
                return false;
            }
        } else {
            size_t bound = !strcmp(key, "payload")                             ? 32768
                           : !strcmp(key, "task")                              ? 64
                           : !strcmp(key, "owner")                             ? 96
                           : !strcmp(key, "token")                             ? 32
                           : (!strcmp(key, "title") || !strcmp(key, "reason")) ? 1024
                                                                               : 4096;
            if (!yyjson_is_str(x) || !yyjson_get_len(x) || yyjson_get_len(x) > bound ||
                strlen(yyjson_get_str(x)) != yyjson_get_len(x)) {
                *why = "invalid string or byte length";
                return false;
            }
        }
    }
    if (!str(v, "path") || (str(v, "task") && !valid_id(str(v, "task")))) {
        *why = "path and valid task ID required";
        return false;
    }
    if ((!strcmp(action, "create") && (!str(v, "task") || !str(v, "title") || !str(v, "check"))) ||
        (!strcmp(action, "claim") && !str(v, "owner")) ||
        ((!strcmp(action, "renew") || !strcmp(action, "publish")) &&
         (!str(v, "task") || !str(v, "owner") || !str(v, "token") ||
          !number(v, "generation", 0))) ||
        (!strcmp(action, "publish") && !str(v, "payload")) ||
        (!strcmp(action, "verify") && !number(v, "artifact", 0)) ||
        (!strcmp(action, "invalidate") &&
         (!str(v, "task") || !str(v, "reason") || !number(v, "generation", 0)))) {
        *why = "missing required action fields";
        return false;
    }
    if (!strcmp(action, "status") && yyjson_obj_get(v, "artifact") &&
        (yyjson_obj_get(v, "task") || yyjson_obj_get(v, "after"))) {
        *why = "artifact inspection cannot also select a task or task cursor";
        return false;
    }
    return true;
}

/* An attempt's inputs are fixed when claimed, never inferred from a later
 * status read. This is also the stale-input fence at publication/acceptance. */
static bool inputs_current(sqlite3 *db, const char *task, long long gen) {
    sqlite3_stmt *s =
        prepare(db, "SELECT NOT EXISTS(SELECT 1 FROM bb_inputs i JOIN bb_tasks t ON t.key=i.input "
                    "WHERE i.task=? AND i.generation=? AND (t.state!='accepted' OR t.accepted IS "
                    "NOT i.artifact))");
    if (!s)
        return false;
    bind_text(s, 1, task);
    sqlite3_bind_int64(s, 2, gen);
    bool ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return ok;
}
static bool append_inputs(sqlite3 *db, const char *task, long long gen, jbuf_t *b, bool payloads) {
    sqlite3_stmt *s =
        prepare(db, "SELECT i.input,a.id,a.sha256,a.payload FROM bb_inputs i JOIN bb_artifacts a "
                    "ON a.id=i.artifact WHERE i.task=? AND i.generation=? ORDER BY i.input");
    if (!s)
        return false;
    bind_text(s, 1, task);
    sqlite3_bind_int64(s, 2, gen);
    jbuf_append(b, "[");
    int rc, n = 0;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (n++)
            jbuf_append(b, ",");
        jbuf_append(b, "{\"task\":");
        jbuf_append_json_str(b, column(s, 0));
        jbuf_appendf(b, ",\"artifact\":%lld,\"sha256\":", sqlite3_column_int64(s, 1));
        jbuf_append_json_str(b, column(s, 2));
        if (payloads) {
            jbuf_append(b, ",\"payload\":");
            jbuf_append_json_str(b, column(s, 3));
        }
        jbuf_append(b, "}");
    }
    jbuf_append(b, "]");
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}
static bool create_task(sqlite3 *db, yyjson_val *v, jbuf_t *out, const char **why) {
    const char *task = str(v, "task");
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        *why = "cannot resolve checker working directory";
        return false;
    }
    sqlite3_stmt *s =
        prepare(db, "INSERT OR IGNORE INTO bb_tasks(key,title,checker,cwd) VALUES(?,?,?,?)");
    if (!s)
        return false;
    bind_text(s, 1, task);
    bind_text(s, 2, str(v, "title"));
    bind_text(s, 3, str(v, "check"));
    bind_text(s, 4, cwd);
    if (!done(s))
        return false;
    bool fresh = sqlite3_changes(db) == 1;
    if (!fresh) {
        s = prepare(db, "SELECT title,checker,cwd FROM bb_tasks WHERE key=?");
        if (!s)
            return false;
        bind_text(s, 1, task);
        bool same = sqlite3_step(s) == SQLITE_ROW && !strcmp(column(s, 0), str(v, "title")) &&
                    !strcmp(column(s, 1), str(v, "check")) && !strcmp(column(s, 2), cwd);
        sqlite3_finalize(s);
        if (!same) {
            *why = "task exists with a different immutable contract";
            return false;
        }
    }
    yyjson_val *deps = yyjson_obj_get(v, "dependencies"), *d;
    size_t i, m;
    yyjson_arr_foreach(deps, i, m, d) {
        const char *dep = yyjson_get_str(d);
        if (!strcmp(task, dep)) {
            *why = "self dependency is forbidden";
            return false;
        }
        /* Only previously created tasks can be dependencies: immutable edges
         * therefore form a DAG without a separate cycle-repair subsystem. */
        s = prepare(db, fresh ? "INSERT INTO bb_deps(task,input) VALUES(?,?)"
                              : "SELECT 1 FROM bb_deps WHERE task=? AND input=?");
        if (!s)
            return false;
        bind_text(s, 1, task);
        bind_text(s, 2, dep);
        bool ok = fresh ? done(s) : sqlite3_step(s) == SQLITE_ROW;
        if (!fresh)
            sqlite3_finalize(s);
        if (!ok) {
            *why = "dependency missing, duplicated, or contract differs";
            return false;
        }
    }
    s = prepare(db, "SELECT count(*) FROM bb_deps WHERE task=?");
    if (!s)
        return false;
    bind_text(s, 1, task);
    bool count_ok =
        sqlite3_step(s) == SQLITE_ROW && (size_t)sqlite3_column_int(s, 0) == yyjson_arr_size(deps);
    sqlite3_finalize(s);
    if (!count_ok) {
        *why = "dependency contract differs";
        return false;
    }
    if (fresh && !event(db, task, "created", 0, str(v, "title")))
        return false;
    jbuf_append(out, "{\"task\":");
    jbuf_append_json_str(out, task);
    jbuf_appendf(out, ",\"created\":%s}", fresh ? "true" : "false");
    return true;
}
static bool claim(sqlite3 *db, yyjson_val *v, jbuf_t *out, const char **why) {
    sqlite3_stmt *s = prepare(
        db, "SELECT t.key,t.generation FROM bb_tasks t WHERE (?1='' OR t.key=?1) "
            "AND (t.state='pending' OR (t.state IN ('claimed','candidate') AND t.expires<=?2)) "
            "AND NOT EXISTS(SELECT 1 FROM bb_deps d JOIN bb_tasks p ON p.key=d.input WHERE "
            "d.task=t.key AND p.state!='accepted') "
            "ORDER BY t.rowid LIMIT 1");
    if (!s)
        return false;
    bind_text(s, 1, str(v, "task"));
    sqlite3_bind_int64(s, 2, now_ms());
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(s);
        jbuf_append(out, "{\"claimed\":false,\"reason\":\"no ready unowned or expired task\"}");
        return true;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    char task[65];
    snprintf(task, sizeof(task), "%s", column(s, 0));
    long long gen = sqlite3_column_int64(s, 1) + 1;
    sqlite3_finalize(s);
    s = prepare(db, "UPDATE bb_tasks SET "
                    "state='claimed',generation=?,owner=?,token=lower(hex(randomblob(16))),expires="
                    "?,candidate=NULL,accepted=NULL WHERE key=? RETURNING token,expires");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, gen);
    bind_text(s, 2, str(v, "owner"));
    sqlite3_bind_int64(s, 3, now_ms() + number(v, "ttl", 300) * 1000);
    bind_text(s, 4, task);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    jbuf_append(out, "{\"claimed\":true,\"task\":");
    jbuf_append_json_str(out, task);
    jbuf_appendf(out, ",\"generation\":%lld,\"token\":", gen);
    jbuf_append_json_str(out, column(s, 0));
    jbuf_appendf(out, ",\"expires_ms\":%lld", sqlite3_column_int64(s, 1));
    sqlite3_finalize(s);
    s = prepare(
        db,
        "INSERT INTO bb_inputs(task,generation,input,artifact) SELECT d.task,?,d.input,t.accepted "
        "FROM bb_deps d JOIN bb_tasks t ON t.key=d.input WHERE d.task=?");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, gen);
    bind_text(s, 2, task);
    if (!done(s) || !event(db, task, "claimed", gen, str(v, "owner")))
        return false;
    jbuf_append(out, ",\"inputs\":");
    if (!append_inputs(db, task, gen, out, false))
        return false;
    jbuf_append(out, "}");
    (void)why;
    return true;
}
static bool owned(sqlite3 *db, yyjson_val *v, long long *candidate) {
    sqlite3_stmt *s =
        prepare(db, "SELECT candidate FROM bb_tasks WHERE key=? AND generation=? AND owner=? AND "
                    "token=? AND expires>? AND state IN ('claimed','candidate')");
    if (!s)
        return false;
    bind_text(s, 1, str(v, "task"));
    sqlite3_bind_int64(s, 2, number(v, "generation", 0));
    bind_text(s, 3, str(v, "owner"));
    bind_text(s, 4, str(v, "token"));
    sqlite3_bind_int64(s, 5, now_ms());
    bool ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok)
        *candidate = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return ok;
}
static bool publish_or_renew(sqlite3 *db, yyjson_val *v, jbuf_t *out, const char **why) {
    long long id = 0, gen = number(v, "generation", 0);
    const char *task = str(v, "task");
    if (!owned(db, v, &id) || !inputs_current(db, task, gen)) {
        *why = "stale claim, expired lease, or invalidated inputs";
        return false;
    }
    sqlite3_stmt *s;
    if (!strcmp(str(v, "action"), "renew")) {
        long long until = now_ms() + number(v, "ttl", 300) * 1000;
        s = prepare(db, "UPDATE bb_tasks SET expires=max(expires,?) WHERE key=? RETURNING expires");
        if (!s)
            return false;
        sqlite3_bind_int64(s, 1, until);
        bind_text(s, 2, task);
        if (sqlite3_step(s) != SQLITE_ROW) {
            sqlite3_finalize(s);
            return false;
        }
        until = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        if (!event(db, task, "renewed", gen, str(v, "owner")))
            return false;
        jbuf_appendf(out, "{\"renewed\":true,\"expires_ms\":%lld}", until);
        return true;
    }
    const char *payload = str(v, "payload");
    char hash[65];
    sha256_hex((const uint8_t *)payload, strlen(payload), hash);
    if (id) {
        s = prepare(db, "SELECT payload FROM bb_artifacts WHERE id=?");
        if (!s)
            return false;
        sqlite3_bind_int64(s, 1, id);
        bool same = sqlite3_step(s) == SQLITE_ROW && !strcmp(column(s, 0), payload);
        sqlite3_finalize(s);
        if (!same) {
            *why =
                "attempt already published different bytes; invalidate or wait for a new attempt";
            return false;
        }
    } else {
        s = prepare(db, "INSERT INTO bb_artifacts(task,generation,payload,sha256) VALUES(?,?,?,?)");
        if (!s)
            return false;
        bind_text(s, 1, task);
        sqlite3_bind_int64(s, 2, gen);
        bind_text(s, 3, payload);
        bind_text(s, 4, hash);
        if (!done(s))
            return false;
        id = sqlite3_last_insert_rowid(db);
        s = prepare(db, "UPDATE bb_tasks SET state='candidate',candidate=? WHERE key=?");
        if (!s)
            return false;
        sqlite3_bind_int64(s, 1, id);
        bind_text(s, 2, task);
        if (!done(s) || !event(db, task, "published", gen, hash))
            return false;
    }
    jbuf_appendf(out, "{\"artifact\":%lld,\"sha256\":\"%s\",\"accepted\":false}", id, hash);
    return true;
}
static bool invalidate(sqlite3 *db, yyjson_val *v, jbuf_t *out, const char **why) {
    sqlite3_stmt *s = prepare(db, "SELECT 1 FROM bb_tasks WHERE key=? AND generation=?");
    if (!s)
        return false;
    bind_text(s, 1, str(v, "task"));
    sqlite3_bind_int64(s, 2, number(v, "generation", 0));
    bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    if (!found) {
        *why = "task missing or generation changed; inspect before invalidating";
        return false;
    }
    if (!sql(db, "CREATE TEMP TABLE affected(key TEXT PRIMARY KEY)"))
        return false;
    s = prepare(db, "INSERT INTO affected WITH RECURSIVE reach(k) AS (SELECT ? UNION SELECT d.task "
                    "FROM bb_deps d JOIN reach r ON d.input=r.k) SELECT k FROM reach");
    if (!s)
        return false;
    bind_text(s, 1, str(v, "task"));
    if (!done(s))
        return false;
    if (!sql(db, "UPDATE bb_tasks SET "
                 "generation=generation+1,state='pending',owner='',token='',expires=0,candidate="
                 "NULL,accepted=NULL WHERE key IN(SELECT key FROM affected)"))
        return false;
    s = prepare(
        db,
        "SELECT t.key,t.generation FROM bb_tasks t JOIN affected a ON a.key=t.key ORDER BY t.key");
    if (!s)
        return false;
    jbuf_append(out, "{\"invalidated\":[");
    int rc, n = 0;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (!event(db, column(s, 0), "invalidated", sqlite3_column_int64(s, 1), str(v, "reason"))) {
            sqlite3_finalize(s);
            return false;
        }
        if (n++)
            jbuf_append(out, ",");
        jbuf_append_json_str(out, column(s, 0));
    }
    sqlite3_finalize(s);
    jbuf_append(out, "]}");
    return rc == SQLITE_DONE;
}

static bool read_board(sqlite3 *db, yyjson_val *v, jbuf_t *out) {
    sqlite3_stmt *s;
    int rc, n = 0;
    long long limit = number(v, "limit", 20);
    if (!strcmp(str(v, "action"), "events")) {
        s = prepare(db, "SELECT seq,task,kind,generation,detail,created_ms FROM bb_events WHERE "
                        "seq>? AND (?='' OR task=?) ORDER BY seq LIMIT ?");
        if (!s)
            return false;
        sqlite3_bind_int64(s, 1, number(v, "after", 0));
        bind_text(s, 2, str(v, "task"));
        bind_text(s, 3, str(v, "task"));
        sqlite3_bind_int64(s, 4, limit);
        long long cursor = number(v, "after", 0);
        jbuf_append(out, "{\"events\":[");
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            if (n++)
                jbuf_append(out, ",");
            cursor = sqlite3_column_int64(s, 0);
            jbuf_appendf(out, "{\"seq\":%lld,\"task\":", cursor);
            jbuf_append_json_str(out, column(s, 1));
            jbuf_append(out, ",\"kind\":");
            jbuf_append_json_str(out, column(s, 2));
            jbuf_appendf(out, ",\"generation\":%lld,\"detail\":", sqlite3_column_int64(s, 3));
            jbuf_append_json_str(out, column(s, 4));
            jbuf_appendf(out, ",\"created_ms\":%lld}", sqlite3_column_int64(s, 5));
        }
        sqlite3_finalize(s);
        jbuf_appendf(out, "],\"next_cursor\":%lld,\"page_full\":%s}", cursor,
                     n == limit ? "true" : "false");
        return rc == SQLITE_DONE;
    }
    long long artifact = number(v, "artifact", 0);
    if (artifact) {
        s = prepare(db, "SELECT task,generation,payload,sha256 FROM bb_artifacts WHERE id=?");
        if (!s)
            return false;
        sqlite3_bind_int64(s, 1, artifact);
        if (sqlite3_step(s) != SQLITE_ROW) {
            sqlite3_finalize(s);
            return false;
        }
        char task[65];
        snprintf(task, sizeof(task), "%s", column(s, 0));
        long long gen = sqlite3_column_int64(s, 1);
        jbuf_appendf(out, "{\"artifact\":%lld,\"task\":", artifact);
        jbuf_append_json_str(out, task);
        jbuf_appendf(out, ",\"generation\":%lld,\"payload\":", gen);
        jbuf_append_json_str(out, column(s, 2));
        jbuf_append(out, ",\"sha256\":");
        jbuf_append_json_str(out, column(s, 3));
        sqlite3_finalize(s);
        jbuf_append(out, ",\"inputs\":");
        if (!append_inputs(db, task, gen, out, false))
            return false;
        jbuf_append(out, ",\"checks\":[");
        s = prepare(db, "SELECT id,passed,accepted,output,created_ms FROM bb_checks WHERE "
                        "artifact=? ORDER BY id DESC LIMIT ?");
        if (!s)
            return false;
        sqlite3_bind_int64(s, 1, artifact);
        sqlite3_bind_int64(s, 2, limit);
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            if (n++)
                jbuf_append(out, ",");
            jbuf_appendf(out, "{\"id\":%lld,\"passed\":%s,\"accepted_at_check\":%s,\"output\":",
                         sqlite3_column_int64(s, 0), sqlite3_column_int(s, 1) ? "true" : "false",
                         sqlite3_column_int(s, 2) ? "true" : "false");
            jbuf_append_json_str(out, column(s, 3));
            jbuf_appendf(out, ",\"created_ms\":%lld}", sqlite3_column_int64(s, 4));
        }
        sqlite3_finalize(s);
        jbuf_append(out, "]}");
        return rc == SQLITE_DONE;
    }
    s = prepare(db,
                "SELECT "
                "t.key,t.title,t.state,t.generation,t.owner,t.expires,COALESCE(t.candidate,0),"
                "COALESCE(t.accepted,0),t.checker,t.cwd,"
                "NOT EXISTS(SELECT 1 FROM bb_deps d JOIN bb_tasks p ON p.key=d.input WHERE "
                "d.task=t.key AND p.state!='accepted'),t.rowid "
                "FROM bb_tasks t WHERE (?='' OR t.key=?) AND t.rowid>? ORDER BY t.rowid LIMIT ?");
    if (!s)
        return false;
    bind_text(s, 1, str(v, "task"));
    bind_text(s, 2, str(v, "task"));
    sqlite3_bind_int64(s, 3, number(v, "after", 0));
    sqlite3_bind_int64(s, 4, limit);
    long long cursor = number(v, "after", 0);
    jbuf_append(out, "{\"tasks\":[");
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        cursor = sqlite3_column_int64(s, 11);
        if (n++)
            jbuf_append(out, ",");
        jbuf_append(out, "{\"task\":");
        jbuf_append_json_str(out, column(s, 0));
        jbuf_append(out, ",\"title\":");
        jbuf_append_json_str(out, column(s, 1));
        jbuf_append(out, ",\"state\":");
        jbuf_append_json_str(out, column(s, 2));
        jbuf_appendf(out, ",\"generation\":%lld,\"owner\":", sqlite3_column_int64(s, 3));
        jbuf_append_json_str(out, column(s, 4));
        long long expiry = sqlite3_column_int64(s, 5);
        bool ready = sqlite3_column_int(s, 10) &&
                     (!strcmp(column(s, 2), "pending") ||
                      ((!strcmp(column(s, 2), "claimed") || !strcmp(column(s, 2), "candidate")) &&
                       expiry <= now_ms()));
        jbuf_appendf(
            out,
            ",\"expires_ms\":%lld,\"candidate\":%lld,\"accepted\":%lld,\"ready\":%s,\"check\":",
            expiry, sqlite3_column_int64(s, 6), sqlite3_column_int64(s, 7),
            ready ? "true" : "false");
        jbuf_append_json_str(out, column(s, 8));
        jbuf_append(out, ",\"cwd\":");
        jbuf_append_json_str(out, column(s, 9));
        sqlite3_stmt *d =
            prepare(db, "SELECT d.input,p.state,COALESCE(p.accepted,0) FROM bb_deps d JOIN "
                        "bb_tasks p ON p.key=d.input WHERE d.task=? ORDER BY d.input");
        if (!d) {
            sqlite3_finalize(s);
            return false;
        }
        bind_text(d, 1, column(s, 0));
        jbuf_append(out, ",\"dependencies\":[");
        int j = 0, dr;
        while ((dr = sqlite3_step(d)) == SQLITE_ROW) {
            if (j++)
                jbuf_append(out, ",");
            jbuf_append(out, "{\"task\":");
            jbuf_append_json_str(out, column(d, 0));
            jbuf_append(out, ",\"state\":");
            jbuf_append_json_str(out, column(d, 1));
            jbuf_appendf(out, ",\"accepted\":%lld}", sqlite3_column_int64(d, 2));
        }
        sqlite3_finalize(d);
        if (dr != SQLITE_DONE) {
            sqlite3_finalize(s);
            return false;
        }
        jbuf_append(out, "]}");
    }
    sqlite3_finalize(s);
    jbuf_appendf(out, "],\"next_cursor\":%lld,\"page_full\":%s}", cursor,
                 n == limit ? "true" : "false");
    return rc == SQLITE_DONE;
}

static void shell_quote(jbuf_t *b, const char *s) {
    jbuf_append(b, "'");
    for (; *s; s++) {
        if (*s == '\'')
            jbuf_append(b, "'\\''");
        else
            jbuf_append_char(b, *s);
    }
    jbuf_append(b, "'");
}
/* Verify without holding the write transaction across external execution.
 * The second transaction fences acceptance against expiry, reassignment, and
 * input invalidation while the checker ran. Checks are ordinary governed bash
 * executions; an agent's assertion that tests passed is never accepted here. */
static bool verify(sqlite3 *db, yyjson_val *v, jbuf_t *out, const char **why) {
    long long id = number(v, "artifact", 0), gen = 0;
    char task[65] = {0};
    char *checker = NULL, *cwd = NULL;
    sqlite3_stmt *s =
        prepare(db, "SELECT "
                    "a.task,a.generation,a.payload,a.sha256,t.checker,t.cwd,t.state,t.candidate,t."
                    "accepted,t.generation,t.expires "
                    "FROM bb_artifacts a JOIN bb_tasks t ON t.key=a.task WHERE a.id=?");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        *why = "artifact not found";
        return false;
    }
    if (!strcmp(column(s, 6), "accepted") && sqlite3_column_int64(s, 8) == id) {
        sqlite3_finalize(s);
        jbuf_appendf(out, "{\"artifact\":%lld,\"accepted\":true,\"already_accepted\":true}", id);
        return true;
    }
    if (strcmp(column(s, 6), "candidate") || sqlite3_column_int64(s, 7) != id ||
        sqlite3_column_int64(s, 1) != sqlite3_column_int64(s, 9) ||
        sqlite3_column_int64(s, 10) <= now_ms()) {
        sqlite3_finalize(s);
        *why = "candidate is stale or lease expired";
        return false;
    }
    snprintf(task, sizeof(task), "%s", column(s, 0));
    gen = sqlite3_column_int64(s, 1);
    checker = safe_strdup(column(s, 4));
    cwd = safe_strdup(column(s, 5));
    jbuf_t snapshot;
    jbuf_init(&snapshot, 4096);
    jbuf_appendf(&snapshot, "{\"artifact\":%lld,\"task\":", id);
    jbuf_append_json_str(&snapshot, task);
    jbuf_appendf(&snapshot, ",\"generation\":%lld,\"payload\":", gen);
    jbuf_append_json_str(&snapshot, column(s, 2));
    jbuf_append(&snapshot, ",\"sha256\":");
    jbuf_append_json_str(&snapshot, column(s, 3));
    sqlite3_finalize(s);
    jbuf_append(&snapshot, ",\"inputs\":");
    bool ok = append_inputs(db, task, gen, &snapshot, true) && inputs_current(db, task, gen);
    jbuf_append(&snapshot, "}\n");
    if (!ok) {
        *why = "input versions no longer accepted";
        goto cleanup;
    }
    /* End the initial transaction before running any subprocess. */
    if (!sql(db, "COMMIT")) {
        ok = false;
        goto cleanup;
    }
    char resolved[PATH_MAX], dir[PATH_MAX], file[PATH_MAX];
    dir[0] = file[0] = 0;
    if (!realpath(str(v, "path"), resolved) ||
        snprintf(dir, sizeof(dir), "%s.verify-XXXXXX", resolved) >= (int)sizeof(dir) ||
        !mkdtemp(dir)) {
        *why = "cannot create verifier snapshot directory";
        ok = false;
        goto cleanup;
    }
    if (snprintf(file, sizeof(file), "%s/snapshot.json", dir) >= (int)sizeof(file)) {
        rmdir(dir);
        ok = false;
        goto cleanup;
    }
    FILE *f = fopen(file, "wx");
    bool wrote = false;
    if (f) {
        wrote = fwrite(snapshot.data, 1, snapshot.len, f) == snapshot.len;
        if (fclose(f) != 0)
            wrote = false;
    }
    if (!wrote || chmod(file, 0400) != 0) {
        unlink(file);
        rmdir(dir);
        *why = "cannot write verifier snapshot";
        ok = false;
        goto cleanup;
    }
    jbuf_t command, args;
    jbuf_init(&command, 512);
    jbuf_init(&args, 1024);
    jbuf_append(&command, "export DSCO_BLACKBOARD_SNAPSHOT=");
    shell_quote(&command, file);
    jbuf_append(&command, "\n");
    jbuf_append(&command, checker);
    jbuf_append(&args, "{\"command\":");
    jbuf_append_json_str(&args, command.data);
    jbuf_append(&args, ",\"cwd\":");
    jbuf_append_json_str(&args, cwd);
    jbuf_append(&args, ",\"timeout\":120}");
    char evidence[8192] = {0};
    bool passed = tools_execute_for_tier("bash", args.data, tools_execution_tier(), evidence,
                                         sizeof(evidence));
    jbuf_free(&command);
    jbuf_free(&args);
    /* Do not accept a checker that accidentally overwrote its own input. This
     * catches mistakes; the trusted local checker is not a security sandbox. */
    f = fopen(file, "rb");
    bool same = false;
    if (f) {
        char *actual = safe_malloc(snapshot.len + 1);
        size_t got = fread(actual, 1, snapshot.len + 1, f);
        same = got == snapshot.len && !memcmp(actual, snapshot.data, snapshot.len) && !ferror(f);
        free(actual);
        fclose(f);
    }
    unlink(file);
    rmdir(dir);
    if (!same) {
        passed = false;
        snprintf(evidence, sizeof(evidence),
                 "checker changed or removed the verification snapshot");
    }
    if (!sql(db, "BEGIN IMMEDIATE")) {
        ok = false;
        goto cleanup;
    }
    s = prepare(db, "SELECT 1 FROM bb_tasks WHERE key=? AND state='candidate' AND generation=? AND "
                    "candidate=? AND expires>?");
    if (!s) {
        ok = false;
        goto cleanup;
    }
    bind_text(s, 1, task);
    sqlite3_bind_int64(s, 2, gen);
    sqlite3_bind_int64(s, 3, id);
    sqlite3_bind_int64(s, 4, now_ms());
    bool current = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    current = current && inputs_current(db, task, gen);
    bool accepted = passed && current;
    s = prepare(
        db, "INSERT INTO bb_checks(artifact,passed,accepted,output,created_ms) VALUES(?,?,?,?,?)");
    if (!s) {
        ok = false;
        goto cleanup;
    }
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_int(s, 2, passed);
    sqlite3_bind_int(s, 3, accepted);
    bind_text(s, 4, evidence);
    sqlite3_bind_int64(s, 5, now_ms());
    if (!done(s)) {
        ok = false;
        goto cleanup;
    }
    long long receipt = sqlite3_last_insert_rowid(db);
    if (accepted) {
        s = prepare(
            db, "UPDATE bb_tasks SET state='accepted',accepted=?,token='',expires=0 WHERE key=?");
        if (!s) {
            ok = false;
            goto cleanup;
        }
        sqlite3_bind_int64(s, 1, id);
        bind_text(s, 2, task);
        if (!done(s)) {
            ok = false;
            goto cleanup;
        }
    }
    if (!event(db, task,
               accepted  ? "accepted"
               : current ? "check_failed"
                         : "check_stale",
               gen, evidence)) {
        ok = false;
        goto cleanup;
    }
    jbuf_appendf(out,
                 "{\"artifact\":%lld,\"receipt\":%lld,\"passed\":%s,\"accepted\":%s,\"current\":%s,"
                 "\"output\":",
                 id, receipt, passed ? "true" : "false", accepted ? "true" : "false",
                 current ? "true" : "false");
    jbuf_append_json_str(out, evidence);
    jbuf_append(out, "}");
    ok = true; /* A failed check is a recorded result, not an unperformed call. */
cleanup:
    free(checker);
    free(cwd);
    jbuf_free(&snapshot);
    return ok;
}

bool blackboard_execute(const char *input, char *result, size_t result_len) {
    if (!input)
        return error_json(result, result_len, "JSON input required");
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    yyjson_val *v = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *why = "invalid JSON";
    if (!doc || !validate(v, &why)) {
        if (doc)
            yyjson_doc_free(doc);
        return error_json(result, result_len, why);
    }
    const char *action = str(v, "action"), *path = str(v, "path");
    bool ro = !strcmp(action, "status") || !strcmp(action, "events");
    sqlite3 *db = NULL;
    int flags = ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (!strcmp(action, "create"))
        flags |= SQLITE_OPEN_CREATE;
    if (!strcmp(path, ":memory:") ||
        sqlite3_open_v2(path, &db, flags | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        yyjson_doc_free(doc);
        return error_json(
            result, result_len,
            "cannot open board; create first at an explicit local path with an existing parent");
    }
    sqlite3_busy_timeout(db, 5000);
    bool ok = sql(db, "PRAGMA foreign_keys=ON");
    /* Create is idempotent; version checks prevent silently opening an unknown
     * future database format. No ALTER/repair writes on inspection paths. */
    sqlite3_stmt *s = prepare(db, "SELECT version FROM bb_meta");
    if (!s && !strcmp(action, "create")) {
        ok = ok && sql(db, schema);
        s = prepare(db, "SELECT version FROM bb_meta");
    }
    ok = ok && s && sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1;
    if (s)
        sqlite3_finalize(s);
    if (!ro)
        ok = ok && sql(db, "PRAGMA synchronous=FULL");
    jbuf_t out;
    jbuf_init(&out, 4096);
    if (!ok) {
        why = "unsupported or incomplete blackboard schema";
        goto finish;
    }
    ok = sql(db, ro ? "BEGIN" : "BEGIN IMMEDIATE");
    if (!ok)
        goto finish;
    why = "database operation failed";
    if (ro)
        ok = read_board(db, v, &out);
    else if (!strcmp(action, "create"))
        ok = create_task(db, v, &out, &why);
    else if (!strcmp(action, "claim"))
        ok = claim(db, v, &out, &why);
    else if (!strcmp(action, "renew") || !strcmp(action, "publish"))
        ok = publish_or_renew(db, v, &out, &why);
    else if (!strcmp(action, "invalidate"))
        ok = invalidate(db, v, &out, &why);
    else if (!strcmp(action, "verify"))
        ok = verify(db, v, &out, &why);
    /* Never commit an operation whose receipt cannot be delivered. */
    if (ok && out.len >= result_len) {
        ok = false;
        why = "response too large; inspect individual artifacts or reduce limit";
    }
    if (ok && !sqlite3_get_autocommit(db))
        ok = sql(db, "COMMIT");
finish:
    if (!sqlite3_get_autocommit(db))
        sql(db, "ROLLBACK");
    if (ok)
        ok = emit(&out, result, result_len);
    else
        error_json(result, result_len, why);
    jbuf_free(&out);
    sqlite3_close(db);
    yyjson_doc_free(doc);
    return ok;
}
