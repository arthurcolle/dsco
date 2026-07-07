#include "command_plane.h"
#include "error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <sqlite3.h>

static int64_t command_now_unix(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

static void command_safe_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dst_len, "%s", src);
}

static void command_build_default_path(char *buf, size_t len) {
    const char *override = getenv("DSCO_COMMAND_PLANE_PATH");
    if (override && *override) {
        snprintf(buf, len, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(buf, len, "%s/.dsco/command_plane/commands.sqlite3", home);
}

static int command_ensure_parent_dirs(const char *file_path) {
    char tmp[COMMAND_PLANE_PATH_LEN];
    command_safe_copy(tmp, sizeof(tmp), file_path);

    char *slash = strrchr(tmp, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    if (tmp[0] == '\0')
        return 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

const char *command_plane_status_name(command_plane_status_t status) {
    switch (status) {
        case COMMAND_PLANE_STATUS_PENDING:
            return "pending";
        case COMMAND_PLANE_STATUS_RUNNING:
            return "running";
        case COMMAND_PLANE_STATUS_SUCCEEDED:
            return "succeeded";
        case COMMAND_PLANE_STATUS_FAILED:
            return "failed";
        case COMMAND_PLANE_STATUS_CANCELED:
            return "canceled";
        default:
            return "failed";
    }
}

command_plane_status_t command_plane_status_from_name(const char *status) {
    if (!status)
        return COMMAND_PLANE_STATUS_FAILED;
    if (strcmp(status, "pending") == 0)
        return COMMAND_PLANE_STATUS_PENDING;
    if (strcmp(status, "running") == 0)
        return COMMAND_PLANE_STATUS_RUNNING;
    if (strcmp(status, "succeeded") == 0)
        return COMMAND_PLANE_STATUS_SUCCEEDED;
    if (strcmp(status, "canceled") == 0)
        return COMMAND_PLANE_STATUS_CANCELED;
    return COMMAND_PLANE_STATUS_FAILED;
}

static int command_exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int command_schema(sqlite3 *db) {
    if (command_exec(db, "PRAGMA journal_mode=WAL;") != 0)
        return -1;
    if (command_exec(db, "PRAGMA synchronous=FULL;") != 0)
        return -1;
    if (command_exec(db, "PRAGMA foreign_keys=ON;") != 0)
        return -1;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS commands ("
        "  id TEXT PRIMARY KEY,"
        "  kind TEXT NOT NULL,"
        "  idempotency_key TEXT,"
        "  request_hash TEXT NOT NULL DEFAULT '',"
        "  status TEXT NOT NULL,"
        "  attempt_count INTEGER NOT NULL DEFAULT 1,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  result_json TEXT NOT NULL DEFAULT '',"
        "  error TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_commands_idempotency_key "
        "ON commands(idempotency_key) WHERE idempotency_key IS NOT NULL AND idempotency_key <> '';"
        "CREATE INDEX IF NOT EXISTS idx_commands_status_updated ON commands(status, updated_at);";
    return command_exec(db, sql);
}

int command_plane_open(command_plane_t *cp, const char *path) {
    if (!cp)
        return COMMAND_PLANE_ERR;
    memset(cp, 0, sizeof(*cp));
    if (path && *path)
        command_safe_copy(cp->path, sizeof(cp->path), path);
    else
        command_build_default_path(cp->path, sizeof(cp->path));

    if (command_ensure_parent_dirs(cp->path) != 0)
        return COMMAND_PLANE_ERR;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(cp->path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return COMMAND_PLANE_ERR;
    }

    if (command_schema(db) != 0) {
        sqlite3_close(db);
        return COMMAND_PLANE_ERR;
    }

    cp->sqlite = db;
    cp->initialized = true;
    return COMMAND_PLANE_OK;
}

void command_plane_close(command_plane_t *cp) {
    if (!cp || !cp->sqlite)
        return;
    sqlite3_close((sqlite3 *)cp->sqlite);
    cp->sqlite = NULL;
    cp->initialized = false;
}

static int command_row_to_record(sqlite3_stmt *st, command_record_t *out) {
    if (!out)
        return COMMAND_PLANE_ERR;
    memset(out, 0, sizeof(*out));
    command_safe_copy(out->id, sizeof(out->id), (const char *)sqlite3_column_text(st, 0));
    command_safe_copy(out->kind, sizeof(out->kind), (const char *)sqlite3_column_text(st, 1));
    command_safe_copy(out->idempotency_key, sizeof(out->idempotency_key), (const char *)sqlite3_column_text(st, 2));
    command_safe_copy(out->request_hash, sizeof(out->request_hash), (const char *)sqlite3_column_text(st, 3));
    out->status = command_plane_status_from_name((const char *)sqlite3_column_text(st, 4));
    out->attempt_count = sqlite3_column_int(st, 5);
    out->created_at_unix = sqlite3_column_int64(st, 6);
    out->updated_at_unix = sqlite3_column_int64(st, 7);
    command_safe_copy(out->result_json, sizeof(out->result_json), (const char *)sqlite3_column_text(st, 8));
    command_safe_copy(out->error, sizeof(out->error), (const char *)sqlite3_column_text(st, 9));
    return COMMAND_PLANE_OK;
}

static int command_get_where(command_plane_t *cp, const char *sql, const char *value, command_record_t *out) {
    if (!cp || !cp->initialized || !value || !*value || !out)
        return COMMAND_PLANE_ERR;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2((sqlite3 *)cp->sqlite, sql, -1, &st, NULL) != SQLITE_OK)
        return COMMAND_PLANE_ERR;
    sqlite3_bind_text(st, 1, value, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(st);
    int rc = COMMAND_PLANE_NOT_FOUND;
    if (step == SQLITE_ROW)
        rc = command_row_to_record(st, out);
    else if (step != SQLITE_DONE)
        rc = COMMAND_PLANE_ERR;
    sqlite3_finalize(st);
    return rc;
}

int command_plane_get_by_id(command_plane_t *cp,
                            const char *command_id,
                            command_record_t *out) {
    return command_get_where(cp,
                             "SELECT id,kind,idempotency_key,request_hash,status,attempt_count,created_at,updated_at,result_json,error "
                             "FROM commands WHERE id=?1 LIMIT 1;",
                             command_id, out);
}

int command_plane_get_by_idempotency_key(command_plane_t *cp,
                                         const char *idempotency_key,
                                         command_record_t *out) {
    return command_get_where(cp,
                             "SELECT id,kind,idempotency_key,request_hash,status,attempt_count,created_at,updated_at,result_json,error "
                             "FROM commands WHERE idempotency_key=?1 LIMIT 1;",
                             idempotency_key, out);
}

static void command_make_id(char *buf, size_t len) {
    static unsigned long counter = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(buf, len, "cmd_%lld_%06d_%lu", (long long)tv.tv_sec, (int)tv.tv_usec, counter++);
}

command_plane_begin_t command_plane_begin(command_plane_t *cp,
                                          command_record_t *out,
                                          const char *kind,
                                          const char *idempotency_key,
                                          const char *request_hash) {
    if (!cp || !cp->initialized || !out || !kind || !*kind)
        return COMMAND_PLANE_BEGIN_ERROR;

    memset(out, 0, sizeof(*out));
    command_make_id(out->id, sizeof(out->id));
    command_safe_copy(out->kind, sizeof(out->kind), kind);
    command_safe_copy(out->idempotency_key, sizeof(out->idempotency_key), idempotency_key);
    command_safe_copy(out->request_hash, sizeof(out->request_hash), request_hash);
    out->status = COMMAND_PLANE_STATUS_RUNNING;
    out->attempt_count = 1;
    out->created_at_unix = command_now_unix();
    out->updated_at_unix = out->created_at_unix;

    const char *sql =
        "INSERT INTO commands(id,kind,idempotency_key,request_hash,status,attempt_count,created_at,updated_at,result_json,error) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'','');";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2((sqlite3 *)cp->sqlite, sql, -1, &st, NULL) != SQLITE_OK)
        return COMMAND_PLANE_BEGIN_ERROR;
    sqlite3_bind_text(st, 1, out->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, out->kind, -1, SQLITE_TRANSIENT);
    if (out->idempotency_key[0])
        sqlite3_bind_text(st, 3, out->idempotency_key, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, out->request_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, command_plane_status_name(out->status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, out->attempt_count);
    sqlite3_bind_int64(st, 7, out->created_at_unix);
    sqlite3_bind_int64(st, 8, out->updated_at_unix);

    int step = sqlite3_step(st);
    int ext = sqlite3_extended_errcode((sqlite3 *)cp->sqlite);
    sqlite3_finalize(st);

    if (step == SQLITE_DONE)
        return COMMAND_PLANE_BEGIN_NEW;
    if (ext == SQLITE_CONSTRAINT_UNIQUE && idempotency_key && *idempotency_key &&
        command_plane_get_by_idempotency_key(cp, idempotency_key, out) == COMMAND_PLANE_OK) {
        if (strcmp(out->request_hash, request_hash ? request_hash : "") != 0)
            return COMMAND_PLANE_BEGIN_CONFLICT;
        return COMMAND_PLANE_BEGIN_REPLAY;
    }
    return COMMAND_PLANE_BEGIN_ERROR;
}

static bool command_status_is_terminal(command_plane_status_t status) {
    return status == COMMAND_PLANE_STATUS_SUCCEEDED ||
           status == COMMAND_PLANE_STATUS_FAILED ||
           status == COMMAND_PLANE_STATUS_CANCELED;
}

static int command_update_terminal(command_plane_t *cp, const char *command_id,
                                   command_plane_status_t status,
                                   const char *result_json,
                                   const char *error) {
    if (!cp || !cp->initialized || !command_id || !*command_id)
        return COMMAND_PLANE_ERR;
    DSCO_REQUIRE_RET(command_status_is_terminal(status), COMMAND_PLANE_ERR);
    const char *sql =
        "UPDATE commands SET status=?1, updated_at=?2, result_json=?3, error=?4 "
        "WHERE id=?5 AND status IN ('pending','running');";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2((sqlite3 *)cp->sqlite, sql, -1, &st, NULL) != SQLITE_OK)
        return COMMAND_PLANE_ERR;
    sqlite3_bind_text(st, 1, command_plane_status_name(status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, command_now_unix());
    sqlite3_bind_text(st, 3, result_json ? result_json : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, error ? error : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, command_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(st);
    int changes = sqlite3_changes((sqlite3 *)cp->sqlite);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE)
        return COMMAND_PLANE_ERR;
    return changes > 0 ? COMMAND_PLANE_OK : COMMAND_PLANE_NOT_FOUND;
}

int command_plane_complete(command_plane_t *cp, const char *command_id,
                           const char *result_json) {
    return command_update_terminal(cp, command_id, COMMAND_PLANE_STATUS_SUCCEEDED, result_json, "");
}

int command_plane_fail(command_plane_t *cp, const char *command_id,
                       const char *error) {
    return command_update_terminal(cp, command_id, COMMAND_PLANE_STATUS_FAILED, "", error);
}
