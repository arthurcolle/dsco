#include "ipc.h"
#include "json_util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal State
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct {
    sqlite3 *db;
    bool     ready;
    char     self_id[IPC_MAX_AGENT_ID];
    char     db_path[4096];
} g_ipc = {0};

static double now_ts(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Generate a short unique agent ID: "a-<pid>-<random>" */
static void gen_agent_id(char *buf, size_t len) {
    unsigned int r = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&r, sizeof(r), 1, f) != 1) {
            /* Fallback: use time-based entropy */
            r = (unsigned int)(now_ts() * 1000) ^ (unsigned int)getpid();
        }
        fclose(f);
    } else {
        /* /dev/urandom unavailable — use time + pid as fallback */
        r = (unsigned int)(now_ts() * 1000) ^ (unsigned int)getpid();
    }
    snprintf(buf, len, "a-%d-%04x", getpid(), r & 0xFFFF);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Schema
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS agents ("
    "  id TEXT PRIMARY KEY,"
    "  parent_id TEXT,"
    "  pid INTEGER,"
    "  depth INTEGER DEFAULT 0,"
    "  status TEXT DEFAULT 'starting',"
    "  role TEXT DEFAULT '',"
    "  current_task TEXT DEFAULT '',"
    "  toolkit TEXT DEFAULT '*',"
    "  started_at REAL,"
    "  last_heartbeat REAL"
    ");"
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  from_agent TEXT,"
    "  to_agent TEXT,"
    "  topic TEXT DEFAULT '',"
    "  body TEXT DEFAULT '',"
    "  created_at REAL,"
    "  read_at REAL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_msg_to ON messages(to_agent, read_at);"
    "CREATE INDEX IF NOT EXISTS idx_msg_topic ON messages(topic);"
    "CREATE TABLE IF NOT EXISTS tasks ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  assigned_to TEXT,"
    "  created_by TEXT,"
    "  parent_task_id INTEGER DEFAULT 0,"
    "  priority INTEGER DEFAULT 0,"
    "  status TEXT DEFAULT 'pending',"
    "  description TEXT DEFAULT '',"
    "  result TEXT,"
    "  created_at REAL,"
    "  started_at REAL,"
    "  completed_at REAL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_task_status ON tasks(status, priority DESC);"
    "CREATE TABLE IF NOT EXISTS scratchpad ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT,"
    "  agent_id TEXT,"
    "  updated_at REAL"
    ");";

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

bool ipc_init(const char *db_path, const char *agent_id) {
    if (g_ipc.ready) return true;

    /* Determine DB path */
    if (db_path && db_path[0]) {
        snprintf(g_ipc.db_path, sizeof(g_ipc.db_path), "%s", db_path);
    } else {
        const char *env = getenv("DSCO_IPC_DB");
        if (env && env[0]) {
            snprintf(g_ipc.db_path, sizeof(g_ipc.db_path), "%s", env);
        } else {
            snprintf(g_ipc.db_path, sizeof(g_ipc.db_path),
                     "/tmp/dsco_ipc_%d.db", getppid());
        }
    }

    /* Determine agent ID */
    if (agent_id && agent_id[0]) {
        snprintf(g_ipc.self_id, sizeof(g_ipc.self_id), "%s", agent_id);
    } else {
        gen_agent_id(g_ipc.self_id, sizeof(g_ipc.self_id));
    }

    /* Open SQLite in WAL mode for concurrent access */
    int rc = sqlite3_open(g_ipc.db_path, &g_ipc.db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ipc: sqlite open failed: %s\n", sqlite3_errmsg(g_ipc.db));
        return false;
    }

    /* WAL mode + busy timeout for concurrent access */
    sqlite3_exec(g_ipc.db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(g_ipc.db, "PRAGMA busy_timeout=5000", NULL, NULL, NULL);
    sqlite3_exec(g_ipc.db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    /* Create schema */
    char *err = NULL;
    rc = sqlite3_exec(g_ipc.db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ipc: schema creation failed: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_close(g_ipc.db);
        return false;
    }

    /* Clean up orphaned tasks — tasks assigned to agents that are no longer alive */
    sqlite3_exec(g_ipc.db,
        "UPDATE tasks SET status='pending', assigned_to=NULL "
        "WHERE status IN ('assigned','running') "
        "AND assigned_to NOT IN (SELECT id FROM agents WHERE last_heartbeat > strftime('%s','now') - 60)",
        NULL, NULL, NULL);

    /* Mark dead agents */
    {
        const char *dead_sql =
            "UPDATE agents SET status='dead' "
            "WHERE status NOT IN ('done','error','dead') "
            "AND last_heartbeat < ?";
        sqlite3_stmt *dead_stmt;
        if (sqlite3_prepare_v2(g_ipc.db, dead_sql, -1, &dead_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_double(dead_stmt, 1, now_ts() - IPC_STALE_SEC);
            sqlite3_step(dead_stmt);
            sqlite3_finalize(dead_stmt);
        }
    }

    /* Export DB path for children */
    setenv("DSCO_IPC_DB", g_ipc.db_path, 1);

    g_ipc.ready = true;
    return true;
}

void ipc_shutdown(void) {
    if (!g_ipc.ready) return;

    /* Fail any tasks we had claimed but didn't finish */
    {
        const char *fail_sql =
            "UPDATE tasks SET status='failed', result='agent shutdown', completed_at=? "
            "WHERE assigned_to=? AND status IN ('assigned','running')";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(g_ipc.db, fail_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_double(stmt, 1, now_ts());
            sqlite3_bind_text(stmt, 2, g_ipc.self_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    ipc_set_status(IPC_AGENT_DONE, "");
    sqlite3_close(g_ipc.db);
    g_ipc.db = NULL;
    g_ipc.ready = false;
}

const char *ipc_self_id(void) { return g_ipc.self_id; }
const char *ipc_db_path(void) { return g_ipc.db_path; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Agent Registry
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *status_str(ipc_agent_status_t s) {
    switch (s) {
        case IPC_AGENT_STARTING: return "starting";
        case IPC_AGENT_IDLE:     return "idle";
        case IPC_AGENT_WORKING:  return "working";
        case IPC_AGENT_DONE:     return "done";
        case IPC_AGENT_ERROR:    return "error";
        case IPC_AGENT_DEAD:     return "dead";
    }
    return "unknown";
}

static ipc_agent_status_t parse_status(const char *s) {
    if (!s) return IPC_AGENT_DEAD;
    if (strcmp(s, "starting") == 0) return IPC_AGENT_STARTING;
    if (strcmp(s, "idle") == 0)     return IPC_AGENT_IDLE;
    if (strcmp(s, "working") == 0)  return IPC_AGENT_WORKING;
    if (strcmp(s, "done") == 0)     return IPC_AGENT_DONE;
    if (strcmp(s, "error") == 0)    return IPC_AGENT_ERROR;
    return IPC_AGENT_DEAD;
}

bool ipc_register(const char *parent_id, int depth, const char *role,
                  const char *toolkit) {
    if (!g_ipc.ready) return false;

    const char *sql =
        "INSERT OR REPLACE INTO agents (id, parent_id, pid, depth, status, role, "
        "toolkit, started_at, last_heartbeat) "
        "VALUES (?, ?, ?, ?, 'idle', ?, ?, ?, ?)";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    double now = now_ts();
    sqlite3_bind_text(stmt, 1, g_ipc.self_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, parent_id ? parent_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, getpid());
    sqlite3_bind_int(stmt, 4, depth);
    sqlite3_bind_text(stmt, 5, role ? role : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, toolkit ? toolkit : "*", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 7, now);
    sqlite3_bind_double(stmt, 8, now);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ipc_set_status(ipc_agent_status_t status, const char *current_task) {
    if (!g_ipc.ready) return false;

    const char *sql =
        "UPDATE agents SET status=?, current_task=?, last_heartbeat=? WHERE id=?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, status_str(status), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, current_task ? current_task : "", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, now_ts());
    sqlite3_bind_text(stmt, 4, g_ipc.self_id, -1, SQLITE_STATIC);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ipc_heartbeat(void) {
    if (!g_ipc.ready) return false;

    const char *sql = "UPDATE agents SET last_heartbeat=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_double(stmt, 1, now_ts());
    sqlite3_bind_text(stmt, 2, g_ipc.self_id, -1, SQLITE_STATIC);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

int ipc_list_agents(ipc_agent_info_t *out, int max) {
    if (!g_ipc.ready) return 0;

    const char *sql = "SELECT id, parent_id, pid, depth, status, role, "
                      "current_task, toolkit, started_at, last_heartbeat "
                      "FROM agents ORDER BY depth, started_at";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
        ipc_agent_info_t *a = &out[count];
        snprintf(a->id, sizeof(a->id), "%s",
                 (const char *)sqlite3_column_text(stmt, 0));
        snprintf(a->parent_id, sizeof(a->parent_id), "%s",
                 sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1) : "");
        a->pid = sqlite3_column_int(stmt, 2);
        a->depth = sqlite3_column_int(stmt, 3);
        a->status = parse_status((const char *)sqlite3_column_text(stmt, 4));
        snprintf(a->role, sizeof(a->role), "%s",
                 sqlite3_column_text(stmt, 5) ? (const char *)sqlite3_column_text(stmt, 5) : "");
        snprintf(a->current_task, sizeof(a->current_task), "%s",
                 sqlite3_column_text(stmt, 6) ? (const char *)sqlite3_column_text(stmt, 6) : "");
        snprintf(a->toolkit, sizeof(a->toolkit), "%s",
                 sqlite3_column_text(stmt, 7) ? (const char *)sqlite3_column_text(stmt, 7) : "*");
        a->started_at = sqlite3_column_double(stmt, 8);
        a->last_heartbeat = sqlite3_column_double(stmt, 9);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

bool ipc_get_agent(const char *agent_id, ipc_agent_info_t *out) {
    ipc_agent_info_t agents[1];
    if (!g_ipc.ready) return false;

    const char *sql = "SELECT id, parent_id, pid, depth, status, role, "
                      "current_task, toolkit, started_at, last_heartbeat "
                      "FROM agents WHERE id=?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        snprintf(out->id, sizeof(out->id), "%s",
                 (const char *)sqlite3_column_text(stmt, 0));
        snprintf(out->parent_id, sizeof(out->parent_id), "%s",
                 sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1) : "");
        out->pid = sqlite3_column_int(stmt, 2);
        out->depth = sqlite3_column_int(stmt, 3);
        out->status = parse_status((const char *)sqlite3_column_text(stmt, 4));
        snprintf(out->role, sizeof(out->role), "%s",
                 sqlite3_column_text(stmt, 5) ? (const char *)sqlite3_column_text(stmt, 5) : "");
        snprintf(out->current_task, sizeof(out->current_task), "%s",
                 sqlite3_column_text(stmt, 6) ? (const char *)sqlite3_column_text(stmt, 6) : "");
        snprintf(out->toolkit, sizeof(out->toolkit), "%s",
                 sqlite3_column_text(stmt, 7) ? (const char *)sqlite3_column_text(stmt, 7) : "*");
        out->started_at = sqlite3_column_double(stmt, 8);
        out->last_heartbeat = sqlite3_column_double(stmt, 9);
        found = true;
    }
    sqlite3_finalize(stmt);
    (void)agents;
    return found;
}

bool ipc_agent_alive(const char *agent_id) {
    ipc_agent_info_t info;
    if (!ipc_get_agent(agent_id, &info)) return false;
    if (info.status == IPC_AGENT_DONE || info.status == IPC_AGENT_ERROR ||
        info.status == IPC_AGENT_DEAD) return false;
    return (now_ts() - info.last_heartbeat) < IPC_STALE_SEC;
}

int ipc_reap_dead_agents(double stale_s) {
    if (!g_ipc.ready) return 0;
    const char *sql = "UPDATE agents SET status='dead' "
                      "WHERE status IN ('starting','running','idle') "
                      "AND last_heartbeat > 0 AND (?1 - last_heartbeat) > ?2";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_double(stmt, 1, now_ts());
    sqlite3_bind_double(stmt, 2, stale_s);
    int rc = sqlite3_step(stmt);
    int changed = (rc == SQLITE_DONE) ? sqlite3_changes(g_ipc.db) : 0;
    sqlite3_finalize(stmt);
    return changed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Messaging
 * ═══════════════════════════════════════════════════════════════════════════ */

bool ipc_send(const char *to_agent, const char *topic, const char *body) {
    if (!g_ipc.ready) return false;

    const char *sql =
        "INSERT INTO messages (from_agent, to_agent, topic, body, created_at) "
        "VALUES (?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, g_ipc.self_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, to_agent ? to_agent : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, topic ? topic : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, body ? body : "", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 5, now_ts());

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static int recv_messages(const char *extra_where, const char *bind1,
                         ipc_message_t *out, int max) {
    if (!g_ipc.ready) return 0;

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, from_agent, to_agent, topic, body, created_at "
             "FROM messages WHERE read_at IS NULL AND "
             "(to_agent=? OR to_agent='') %s "
             "ORDER BY created_at LIMIT %d",
             extra_where ? extra_where : "", max);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, g_ipc.self_id, -1, SQLITE_STATIC);
    if (bind1) sqlite3_bind_text(stmt, 2, bind1, -1, SQLITE_STATIC);

    int count = 0;
    int ids_cap = max < 256 ? max : 256;
    int ids[256];
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max && count < ids_cap) {
        ipc_message_t *m = &out[count];
        m->id = sqlite3_column_int(stmt, 0);
        ids[count] = m->id;
        snprintf(m->from_agent, sizeof(m->from_agent), "%s",
                 (const char *)sqlite3_column_text(stmt, 1));
        snprintf(m->to_agent, sizeof(m->to_agent), "%s",
                 sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2) : "");
        snprintf(m->topic, sizeof(m->topic), "%s",
                 sqlite3_column_text(stmt, 3) ? (const char *)sqlite3_column_text(stmt, 3) : "");
        const char *body = (const char *)sqlite3_column_text(stmt, 4);
        m->body = body ? safe_strdup(body) : safe_strdup("");
        m->created_at = sqlite3_column_double(stmt, 5);
        m->read = false;
        count++;
    }
    sqlite3_finalize(stmt);

    /* Mark as read */
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            const char *mark = "UPDATE messages SET read_at=? WHERE id=?";
            sqlite3_stmt *ms;
            if (sqlite3_prepare_v2(g_ipc.db, mark, -1, &ms, NULL) == SQLITE_OK) {
                sqlite3_bind_double(ms, 1, now_ts());
                sqlite3_bind_int(ms, 2, ids[i]);
                sqlite3_step(ms);
                sqlite3_finalize(ms);
            }
        }
    }

    return count;
}

int ipc_recv(ipc_message_t *out, int max) {
    return recv_messages(NULL, NULL, out, max);
}

int ipc_recv_topic(const char *topic, ipc_message_t *out, int max) {
    return recv_messages("AND topic=?", topic, out, max);
}

int ipc_unread_count(void) {
    if (!g_ipc.ready) return 0;

    const char *sql =
        "SELECT COUNT(*) FROM messages WHERE read_at IS NULL "
        "AND (to_agent=? OR to_agent='')";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, g_ipc.self_id, -1, SQLITE_STATIC);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Task Queue
 * ═══════════════════════════════════════════════════════════════════════════ */

int ipc_task_submit(const char *description, int priority, int parent_task_id) {
    if (!g_ipc.ready) return -1;

    const char *sql =
        "INSERT INTO tasks (created_by, priority, parent_task_id, status, "
        "description, created_at) VALUES (?, ?, ?, 'pending', ?, ?)";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, g_ipc.self_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, priority);
    sqlite3_bind_int(stmt, 3, parent_task_id);
    sqlite3_bind_text(stmt, 4, description, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 5, now_ts());

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int id = (int)sqlite3_last_insert_rowid(g_ipc.db);
    sqlite3_finalize(stmt);
    return id;
}

bool ipc_task_claim(ipc_task_t *out) {
    if (!g_ipc.ready) return false;

    /* Atomically claim highest-priority pending task */
    const char *sql =
        "UPDATE tasks SET assigned_to=?, status='assigned', started_at=? "
        "WHERE id = ("
        "  SELECT id FROM tasks WHERE status='pending' "
        "  ORDER BY priority DESC, created_at ASC LIMIT 1"
        ") RETURNING id, created_by, parent_task_id, priority, description, created_at";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, g_ipc.self_id, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, now_ts());

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->id = sqlite3_column_int(stmt, 0);
        snprintf(out->assigned_to, sizeof(out->assigned_to), "%s", g_ipc.self_id);
        snprintf(out->created_by, sizeof(out->created_by), "%s",
                 (const char *)sqlite3_column_text(stmt, 1));
        out->parent_task_id = sqlite3_column_int(stmt, 2);
        out->priority = sqlite3_column_int(stmt, 3);
        out->status = IPC_TASK_ASSIGNED;
        snprintf(out->description, sizeof(out->description), "%s",
                 (const char *)sqlite3_column_text(stmt, 4));
        out->result = NULL;
        out->created_at = sqlite3_column_double(stmt, 5);
        out->started_at = now_ts();
        out->completed_at = 0;
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool ipc_task_start(int task_id) {
    if (!g_ipc.ready) return false;
    const char *sql = "UPDATE tasks SET status='running', started_at=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_double(stmt, 1, now_ts());
    sqlite3_bind_int(stmt, 2, task_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ipc_task_complete(int task_id, const char *result) {
    if (!g_ipc.ready) return false;
    const char *sql =
        "UPDATE tasks SET status='done', result=?, completed_at=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, result ? result : "", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, now_ts());
    sqlite3_bind_int(stmt, 3, task_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ipc_task_fail(int task_id, const char *error) {
    if (!g_ipc.ready) return false;
    const char *sql =
        "UPDATE tasks SET status='failed', result=?, completed_at=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, error ? error : "", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, now_ts());
    sqlite3_bind_int(stmt, 3, task_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

int ipc_task_list(const char *assigned_to, ipc_task_t *out, int max) {
    if (!g_ipc.ready) return 0;

    const char *sql_all =
        "SELECT id, assigned_to, created_by, parent_task_id, priority, "
        "status, description, result, created_at, started_at, completed_at "
        "FROM tasks ORDER BY priority DESC, created_at";
    const char *sql_filter =
        "SELECT id, assigned_to, created_by, parent_task_id, priority, "
        "status, description, result, created_at, started_at, completed_at "
        "FROM tasks WHERE assigned_to=? ORDER BY priority DESC, created_at";

    sqlite3_stmt *stmt;
    const char *sql = assigned_to ? sql_filter : sql_all;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    if (assigned_to) sqlite3_bind_text(stmt, 1, assigned_to, -1, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
        ipc_task_t *t = &out[count];
        t->id = sqlite3_column_int(stmt, 0);
        snprintf(t->assigned_to, sizeof(t->assigned_to), "%s",
                 sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1) : "");
        snprintf(t->created_by, sizeof(t->created_by), "%s",
                 sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2) : "");
        t->parent_task_id = sqlite3_column_int(stmt, 3);
        t->priority = sqlite3_column_int(stmt, 4);
        const char *st = (const char *)sqlite3_column_text(stmt, 5);
        if (st && strcmp(st, "pending") == 0)  t->status = IPC_TASK_PENDING;
        else if (st && strcmp(st, "assigned") == 0) t->status = IPC_TASK_ASSIGNED;
        else if (st && strcmp(st, "running") == 0)  t->status = IPC_TASK_RUNNING;
        else if (st && strcmp(st, "done") == 0)     t->status = IPC_TASK_DONE;
        else if (st && strcmp(st, "failed") == 0)   t->status = IPC_TASK_FAILED;
        else t->status = IPC_TASK_PENDING;
        snprintf(t->description, sizeof(t->description), "%s",
                 sqlite3_column_text(stmt, 6) ? (const char *)sqlite3_column_text(stmt, 6) : "");
        const char *res = (const char *)sqlite3_column_text(stmt, 7);
        t->result = res ? safe_strdup(res) : NULL;
        t->created_at = sqlite3_column_double(stmt, 8);
        t->started_at = sqlite3_column_double(stmt, 9);
        t->completed_at = sqlite3_column_double(stmt, 10);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

int ipc_task_pending_count(void) {
    if (!g_ipc.ready) return 0;
    const char *sql = "SELECT COUNT(*) FROM tasks WHERE status='pending'";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int ipc_task_requeue_stale(double timeout_s) {
    if (!g_ipc.ready) return 0;
    const char *sql = "UPDATE tasks SET status='pending', assigned_to='', started_at=0 "
                      "WHERE status IN ('assigned','running') "
                      "AND started_at > 0 AND (?1 - started_at) > ?2";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_double(stmt, 1, now_ts());
    sqlite3_bind_double(stmt, 2, timeout_s);
    int rc = sqlite3_step(stmt);
    int changed = (rc == SQLITE_DONE) ? sqlite3_changes(g_ipc.db) : 0;
    sqlite3_finalize(stmt);
    return changed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scratchpad
 * ═══════════════════════════════════════════════════════════════════════════ */

bool ipc_scratch_put(const char *key, const char *value) {
    if (!g_ipc.ready) return false;

    const char *sql =
        "INSERT OR REPLACE INTO scratchpad (key, value, agent_id, updated_at) "
        "VALUES (?, ?, ?, ?)";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, g_ipc.self_id, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, now_ts());

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

char *ipc_scratch_get(const char *key) {
    if (!g_ipc.ready) return NULL;

    const char *sql = "SELECT value FROM scratchpad WHERE key=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    char *val = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        val = v ? safe_strdup(v) : NULL;
    }
    sqlite3_finalize(stmt);
    return val;
}

bool ipc_scratch_del(const char *key) {
    if (!g_ipc.ready) return false;
    const char *sql = "DELETE FROM scratchpad WHERE key=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

int ipc_scratch_keys(const char *prefix, char keys[][IPC_MAX_KEY], int max) {
    if (!g_ipc.ready) return 0;

    const char *sql = prefix && prefix[0]
        ? "SELECT key FROM scratchpad WHERE key LIKE ? || '%' ORDER BY key"
        : "SELECT key FROM scratchpad ORDER BY key";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_ipc.db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    if (prefix && prefix[0]) {
        sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
        const char *k = (const char *)sqlite3_column_text(stmt, 0);
        if (k) snprintf(keys[count], IPC_MAX_KEY, "%s", k);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Convenience
 * ═══════════════════════════════════════════════════════════════════════════ */

int ipc_poll(void) {
    int flags = 0;
    if (ipc_unread_count() > 0) flags |= 1;
    if (ipc_task_pending_count() > 0) flags |= 2;
    return flags;
}

int ipc_status_json(char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"self\":");
    jbuf_append_json_str(&b, g_ipc.self_id);

    /* Agents */
    ipc_agent_info_t agents[64];
    int agent_count = ipc_list_agents(agents, 64);
    jbuf_append(&b, ",\"agents\":[");
    for (int i = 0; i < agent_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        jbuf_append(&b, "{\"id\":");
        jbuf_append_json_str(&b, agents[i].id);
        jbuf_append(&b, ",\"parent\":");
        jbuf_append_json_str(&b, agents[i].parent_id);
        jbuf_append(&b, ",\"depth\":");
        jbuf_append_int(&b, agents[i].depth);
        jbuf_append(&b, ",\"status\":");
        jbuf_append_json_str(&b, status_str(agents[i].status));
        jbuf_append(&b, ",\"role\":");
        jbuf_append_json_str(&b, agents[i].role);
        jbuf_append(&b, ",\"task\":");
        jbuf_append_json_str(&b, agents[i].current_task);
        jbuf_append(&b, ",\"alive\":");
        jbuf_append(&b, ipc_agent_alive(agents[i].id) ? "true" : "false");
        jbuf_append(&b, "}");
    }

    /* Unread / pending counts */
    jbuf_append(&b, "],\"unread_messages\":");
    jbuf_append_int(&b, ipc_unread_count());
    jbuf_append(&b, ",\"pending_tasks\":");
    jbuf_append_int(&b, ipc_task_pending_count());
    jbuf_append(&b, "}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}
