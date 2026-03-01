#include "baseline.h"
#include "json_util.h"

#include <sqlite3.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    sqlite3 *db;
    bool ready;
    char db_path[PATH_MAX];
    char instance_id[128];
    char parent_instance_id[128];
} baseline_state_t;

static baseline_state_t g_baseline = {0};

static const char *safe_str(const char *s) {
    return s ? s : "";
}

static void sanitize_token(char *s) {
    for (; *s; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.')) {
            *s = '_';
        }
    }
}

static bool mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool ensure_parent_dir(const char *file_path) {
    char tmp[PATH_MAX];
    size_t n = strlen(file_path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, file_path, n + 1);

    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    if (tmp[0] == '\0') return true;

    return mkdir_p(tmp);
}

static void resolve_db_path(char *out, size_t out_len) {
    const char *override = getenv("DSCO_BASELINE_DB");
    if (override && override[0]) {
        snprintf(out, out_len, "%s", override);
        return;
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, out_len, "%s/.dsco/baseline.db", home);
    } else {
        snprintf(out, out_len, ".dsco/baseline.db");
    }
}

static void generate_instance_id(char *out, size_t out_len) {
    char host[64] = "localhost";
    if (gethostname(host, sizeof(host) - 1) != 0) {
        snprintf(host, sizeof(host), "localhost");
    }
    host[sizeof(host) - 1] = '\0';
    sanitize_token(host);

    struct timeval tv;
    gettimeofday(&tv, NULL);

    snprintf(out, out_len, "%s-%ld-%d-%06d",
             host, (long)tv.tv_sec, (int)getpid(), (int)tv.tv_usec);
}

static bool exec_sql(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_baseline.db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "baseline: sqlite error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static bool ensure_schema(void) {
    const char *schema =
        "CREATE TABLE IF NOT EXISTS instances ("
        "  instance_id TEXT PRIMARY KEY,"
        "  parent_instance_id TEXT,"
        "  pid INTEGER NOT NULL,"
        "  model TEXT,"
        "  mode TEXT,"
        "  started_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        "  ended_at TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  instance_id TEXT NOT NULL,"
        "  ts TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        "  ts_epoch REAL NOT NULL DEFAULT ((julianday('now') - 2440587.5) * 86400.0),"
        "  category TEXT NOT NULL,"
        "  title TEXT NOT NULL,"
        "  detail TEXT,"
        "  metadata_json TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_events_instance_time ON events(instance_id, ts_epoch DESC);"
        "CREATE INDEX IF NOT EXISTS idx_events_time ON events(ts_epoch DESC);";

    return exec_sql(schema);
}

bool baseline_start(const char *model, const char *mode) {
    if (g_baseline.ready) return true;

    memset(&g_baseline, 0, sizeof(g_baseline));
    resolve_db_path(g_baseline.db_path, sizeof(g_baseline.db_path));

    if (!ensure_parent_dir(g_baseline.db_path)) {
        fprintf(stderr, "baseline: failed to create parent directory for %s\n", g_baseline.db_path);
        return false;
    }

    if (sqlite3_open(g_baseline.db_path, &g_baseline.db) != SQLITE_OK) {
        fprintf(stderr, "baseline: failed to open db %s: %s\n",
                g_baseline.db_path,
                sqlite3_errmsg(g_baseline.db));
        if (g_baseline.db) sqlite3_close(g_baseline.db);
        g_baseline.db = NULL;
        return false;
    }

    sqlite3_busy_timeout(g_baseline.db, 3000);
    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA synchronous=NORMAL;");
    exec_sql("PRAGMA temp_store=MEMORY;");

    if (!ensure_schema()) {
        sqlite3_close(g_baseline.db);
        g_baseline.db = NULL;
        return false;
    }

    generate_instance_id(g_baseline.instance_id, sizeof(g_baseline.instance_id));

    const char *parent = getenv("DSCO_PARENT_INSTANCE_ID");
    if (parent && parent[0]) {
        snprintf(g_baseline.parent_instance_id,
                 sizeof(g_baseline.parent_instance_id), "%s", parent);
    } else {
        g_baseline.parent_instance_id[0] = '\0';
    }

    sqlite3_stmt *st = NULL;
    const char *ins =
        "INSERT INTO instances(instance_id, parent_instance_id, pid, model, mode)"
        " VALUES(?1, ?2, ?3, ?4, ?5);";

    if (sqlite3_prepare_v2(g_baseline.db, ins, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "baseline: prepare failed: %s\n", sqlite3_errmsg(g_baseline.db));
        sqlite3_close(g_baseline.db);
        g_baseline.db = NULL;
        return false;
    }

    sqlite3_bind_text(st, 1, g_baseline.instance_id, -1, SQLITE_TRANSIENT);
    if (g_baseline.parent_instance_id[0])
        sqlite3_bind_text(st, 2, g_baseline.parent_instance_id, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 2);
    sqlite3_bind_int(st, 3, (int)getpid());
    if (model && model[0]) sqlite3_bind_text(st, 4, model, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 4);
    if (mode && mode[0]) sqlite3_bind_text(st, 5, mode, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 5);

    if (sqlite3_step(st) != SQLITE_DONE) {
        fprintf(stderr, "baseline: insert instance failed: %s\n", sqlite3_errmsg(g_baseline.db));
        sqlite3_finalize(st);
        sqlite3_close(g_baseline.db);
        g_baseline.db = NULL;
        return false;
    }
    sqlite3_finalize(st);

    g_baseline.ready = true;
    setenv("DSCO_INSTANCE_ID", g_baseline.instance_id, 1);

    baseline_log("lifecycle", "session_start", mode, NULL);
    return true;
}

void baseline_stop(void) {
    if (!g_baseline.ready || !g_baseline.db) return;

    baseline_log("lifecycle", "session_end", NULL, NULL);

    sqlite3_stmt *st = NULL;
    const char *upd = "UPDATE instances SET ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE instance_id = ?1;";
    if (sqlite3_prepare_v2(g_baseline.db, upd, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, g_baseline.instance_id, -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
        sqlite3_finalize(st);
    }

    sqlite3_close(g_baseline.db);
    g_baseline.db = NULL;
    g_baseline.ready = false;
}

bool baseline_log(const char *category, const char *title,
                  const char *detail, const char *metadata_json) {
    if (!g_baseline.ready || !g_baseline.db) return false;

    sqlite3_stmt *st = NULL;
    const char *ins =
        "INSERT INTO events(instance_id, category, title, detail, metadata_json)"
        " VALUES(?1, ?2, ?3, ?4, ?5);";

    if (sqlite3_prepare_v2(g_baseline.db, ins, -1, &st, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, g_baseline.instance_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, (category && category[0]) ? category : "event", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, (title && title[0]) ? title : "untitled", -1, SQLITE_TRANSIENT);
    if (detail) sqlite3_bind_text(st, 4, detail, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 4);
    if (metadata_json) sqlite3_bind_text(st, 5, metadata_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 5);

    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

const char *baseline_instance_id(void) {
    return g_baseline.ready ? g_baseline.instance_id : "";
}

const char *baseline_db_path(void) {
    return g_baseline.db_path[0] ? g_baseline.db_path : "";
}

static void html_escape_append(jbuf_t *b, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '&': jbuf_append(b, "&amp;"); break;
            case '<': jbuf_append(b, "&lt;"); break;
            case '>': jbuf_append(b, "&gt;"); break;
            case '"': jbuf_append(b, "&quot;"); break;
            case '\'': jbuf_append(b, "&#39;"); break;
            default: jbuf_append_char(b, (char)*p); break;
        }
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t out = 0;
    if (dst_len == 0) return;

    for (size_t i = 0; src && src[i] && out + 1 < dst_len; i++) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (src[i] == '+') dst[out++] = ' ';
        else dst[out++] = src[i];
    }
    dst[out] = '\0';
}

static void extract_query_param(const char *uri, const char *key,
                                char *out, size_t out_len) {
    out[0] = '\0';
    if (!uri || !key) return;

    const char *q = strchr(uri, '?');
    if (!q || !*++q) return;

    size_t key_len = strlen(key);
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t seg_len = amp ? (size_t)(amp - q) : strlen(q);
        const char *eq = memchr(q, '=', seg_len);

        if (eq) {
            size_t klen = (size_t)(eq - q);
            if (klen == key_len && strncmp(q, key, key_len) == 0) {
                size_t vlen = seg_len - (klen + 1);
                char enc[512];
                if (vlen >= sizeof(enc)) vlen = sizeof(enc) - 1;
                memcpy(enc, eq + 1, vlen);
                enc[vlen] = '\0';
                url_decode(enc, out, out_len);
                return;
            }
        }

        if (!amp) break;
        q = amp + 1;
    }
}

static void split_path(const char *uri, char *path, size_t path_len) {
    if (!uri || path_len == 0) return;
    const char *q = strchr(uri, '?');
    size_t n = q ? (size_t)(q - uri) : strlen(uri);
    if (n >= path_len) n = path_len - 1;
    memcpy(path, uri, n);
    path[n] = '\0';
}

static void resolve_instance_filter(const char *uri, const char *default_filter,
                                    char *out, size_t out_len) {
    char from_query[256];
    extract_query_param(uri, "instance", from_query, sizeof(from_query));
    if (from_query[0]) {
        snprintf(out, out_len, "%s", from_query);
    } else if (default_filter && default_filter[0]) {
        snprintf(out, out_len, "%s", default_filter);
    } else {
        out[0] = '\0';
    }
}

static void jbuf_append_long(jbuf_t *b, long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", v);
    jbuf_append(b, tmp);
}

static bool build_events_json(jbuf_t *b, const char *instance_filter) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, ts, instance_id, category, title, COALESCE(detail,''), COALESCE(metadata_json,'') "
        "FROM events "
        "WHERE (?1 IS NULL OR instance_id = ?1) "
        "ORDER BY ts_epoch DESC "
        "LIMIT 500;";

    if (sqlite3_prepare_v2(g_baseline.db, sql, -1, &st, NULL) != SQLITE_OK) {
        return false;
    }
    if (instance_filter && instance_filter[0])
        sqlite3_bind_text(st, 1, instance_filter, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 1);

    jbuf_append(b, "{\"instance_filter\":");
    if (instance_filter && instance_filter[0]) jbuf_append_json_str(b, instance_filter);
    else jbuf_append(b, "null");
    jbuf_append(b, ",\"events\":[");

    bool first = true;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) jbuf_append(b, ",");
        first = false;

        long id = (long)sqlite3_column_int64(st, 0);
        const char *ts = (const char *)sqlite3_column_text(st, 1);
        const char *inst = (const char *)sqlite3_column_text(st, 2);
        const char *cat = (const char *)sqlite3_column_text(st, 3);
        const char *title = (const char *)sqlite3_column_text(st, 4);
        const char *detail = (const char *)sqlite3_column_text(st, 5);
        const char *meta = (const char *)sqlite3_column_text(st, 6);

        jbuf_append(b, "{\"id\":");
        jbuf_append_long(b, id);
        jbuf_append(b, ",\"ts\":");
        jbuf_append_json_str(b, safe_str(ts));
        jbuf_append(b, ",\"instance_id\":");
        jbuf_append_json_str(b, safe_str(inst));
        jbuf_append(b, ",\"category\":");
        jbuf_append_json_str(b, safe_str(cat));
        jbuf_append(b, ",\"title\":");
        jbuf_append_json_str(b, safe_str(title));
        jbuf_append(b, ",\"detail\":");
        jbuf_append_json_str(b, safe_str(detail));
        jbuf_append(b, ",\"metadata\":");
        jbuf_append_json_str(b, safe_str(meta));
        jbuf_append(b, "}");
    }

    jbuf_append(b, "]}");
    sqlite3_finalize(st);
    return true;
}

static bool build_timeline_html(jbuf_t *b, const char *instance_filter) {
    jbuf_append(b,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>dsco timeline</title>"
        "<style>"
        "body{font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Helvetica,Arial;margin:0;padding:24px;background:#f6f8fb;color:#142033;}"
        "h1{margin:0 0 8px 0;font-size:24px;}"
        "p{margin:6px 0 12px 0;color:#51607a;}"
        ".card{background:#fff;border:1px solid #d9e1ee;border-radius:12px;padding:14px 16px;margin:12px 0;box-shadow:0 2px 8px rgba(12,24,48,.05);}"
        ".row{display:flex;gap:12px;align-items:baseline;flex-wrap:wrap;}"
        ".chip{display:inline-block;padding:2px 8px;border-radius:999px;background:#e8eef9;color:#1b3f76;font-size:12px;}"
        ".muted{color:#6b7890;font-size:13px;}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#f7f9fc;border:1px solid #e1e7f0;border-radius:8px;padding:8px;margin:8px 0 0 0;font-size:12px;}"
        "table{width:100%;border-collapse:collapse;font-size:13px;}"
        "th,td{padding:8px;border-bottom:1px solid #e2e8f2;text-align:left;vertical-align:top;}"
        "a{color:#1146a8;text-decoration:none;}"
        "a:hover{text-decoration:underline;}"
        "input{padding:7px 10px;border:1px solid #c9d5e8;border-radius:8px;min-width:320px;}"
        "button{padding:7px 12px;border-radius:8px;border:1px solid #1f4a93;background:#1f4a93;color:#fff;cursor:pointer;}"
        "</style></head><body>");

    jbuf_append(b, "<h1>DSCO Temporal Baseline</h1>");
    jbuf_append(b, "<p>SQLite: <code>");
    html_escape_append(b, baseline_db_path());
    jbuf_append(b, "</code></p>");

    jbuf_append(b, "<form method='get' class='card'><div class='row'>");
    jbuf_append(b, "<label for='instance'><strong>Instance filter</strong></label>");
    jbuf_append(b, "<input id='instance' name='instance' placeholder='all instances' value='");
    html_escape_append(b, instance_filter ? instance_filter : "");
    jbuf_append(b, "'>");
    jbuf_append(b, "<button type='submit'>Apply</button>");
    jbuf_append(b, "<a href='/' class='muted'>clear filter</a>");
    jbuf_append(b, "<a href='/events.json' class='muted'>events.json</a>");
    jbuf_append(b, "</div></form>");

    jbuf_append(b, "<div class='card'><h2 style='margin-top:0'>Instances</h2><table>");
    jbuf_append(b, "<thead><tr><th>Instance</th><th>Parent</th><th>PID</th><th>Mode</th><th>Model</th><th>Started</th><th>Ended</th></tr></thead><tbody>");

    sqlite3_stmt *inst = NULL;
    const char *inst_sql =
        "SELECT instance_id, COALESCE(parent_instance_id,''), pid, COALESCE(mode,''), COALESCE(model,''), started_at, COALESCE(ended_at,'') "
        "FROM instances ORDER BY started_at DESC LIMIT 200;";

    if (sqlite3_prepare_v2(g_baseline.db, inst_sql, -1, &inst, NULL) == SQLITE_OK) {
        while (sqlite3_step(inst) == SQLITE_ROW) {
            const char *iid = (const char *)sqlite3_column_text(inst, 0);
            const char *parent = (const char *)sqlite3_column_text(inst, 1);
            int pid = sqlite3_column_int(inst, 2);
            const char *mode = (const char *)sqlite3_column_text(inst, 3);
            const char *model = (const char *)sqlite3_column_text(inst, 4);
            const char *started = (const char *)sqlite3_column_text(inst, 5);
            const char *ended = (const char *)sqlite3_column_text(inst, 6);

            char pid_buf[32];
            snprintf(pid_buf, sizeof(pid_buf), "%d", pid);

            jbuf_append(b, "<tr><td><a href='/?instance=");
            html_escape_append(b, safe_str(iid));
            jbuf_append(b, "'>");
            html_escape_append(b, safe_str(iid));
            jbuf_append(b, "</a></td><td>");
            html_escape_append(b, safe_str(parent));
            jbuf_append(b, "</td><td>");
            html_escape_append(b, pid_buf);
            jbuf_append(b, "</td><td>");
            html_escape_append(b, safe_str(mode));
            jbuf_append(b, "</td><td>");
            html_escape_append(b, safe_str(model));
            jbuf_append(b, "</td><td>");
            html_escape_append(b, safe_str(started));
            jbuf_append(b, "</td><td>");
            html_escape_append(b, safe_str(ended));
            jbuf_append(b, "</td></tr>");
        }
    }
    sqlite3_finalize(inst);

    jbuf_append(b, "</tbody></table></div>");

    jbuf_append(b, "<div class='card'><h2 style='margin-top:0'>Timeline Events</h2>");

    sqlite3_stmt *ev = NULL;
    const char *ev_sql =
        "SELECT ts, instance_id, category, title, COALESCE(detail,'') "
        "FROM events "
        "WHERE (?1 IS NULL OR instance_id = ?1) "
        "ORDER BY ts_epoch DESC "
        "LIMIT 500;";

    if (sqlite3_prepare_v2(g_baseline.db, ev_sql, -1, &ev, NULL) == SQLITE_OK) {
        if (instance_filter && instance_filter[0])
            sqlite3_bind_text(ev, 1, instance_filter, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(ev, 1);

        while (sqlite3_step(ev) == SQLITE_ROW) {
            const char *ts = (const char *)sqlite3_column_text(ev, 0);
            const char *iid = (const char *)sqlite3_column_text(ev, 1);
            const char *cat = (const char *)sqlite3_column_text(ev, 2);
            const char *title = (const char *)sqlite3_column_text(ev, 3);
            const char *detail = (const char *)sqlite3_column_text(ev, 4);

            jbuf_append(b, "<div class='card'>");
            jbuf_append(b, "<div class='row'><span class='chip'>");
            html_escape_append(b, safe_str(cat));
            jbuf_append(b, "</span><strong>");
            html_escape_append(b, safe_str(title));
            jbuf_append(b, "</strong></div>");
            jbuf_append(b, "<div class='muted'>");
            html_escape_append(b, safe_str(ts));
            jbuf_append(b, " · instance ");
            jbuf_append(b, "<a href='/?instance=");
            html_escape_append(b, safe_str(iid));
            jbuf_append(b, "'>");
            html_escape_append(b, safe_str(iid));
            jbuf_append(b, "</a></div>");
            if (detail && detail[0]) {
                jbuf_append(b, "<pre>");
                html_escape_append(b, detail);
                jbuf_append(b, "</pre>");
            }
            jbuf_append(b, "</div>");
        }
    }
    sqlite3_finalize(ev);

    jbuf_append(b, "</div>");
    jbuf_append(b, "</body></html>");
    return true;
}

static void send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

static void send_response(int fd, const char *status, const char *ctype, const char *body) {
    size_t body_len = body ? strlen(body) : 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, body_len);
    if (n < 0) return;
    send_all(fd, hdr, (size_t)n);
    if (body_len > 0) send_all(fd, body, body_len);
}

int baseline_serve_http(int port, const char *default_instance_filter) {
    if (!g_baseline.ready || !g_baseline.db) {
        return -1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("timeline socket");
        return -1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("timeline bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 32) != 0) {
        perror("timeline listen");
        close(server_fd);
        return -1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "http://127.0.0.1:%d", port);
    baseline_log("server", "timeline_server_start", msg, NULL);

    fprintf(stderr,
            "\nTimeline server running on %s\n"
            "Instance: %s\n"
            "DB: %s\n"
            "Press Ctrl+C to stop.\n\n",
            msg,
            baseline_instance_id(),
            baseline_db_path());

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) break;
            continue;
        }

        char req[8192];
        ssize_t nr = recv(client_fd, req, sizeof(req) - 1, 0);
        if (nr <= 0) {
            close(client_fd);
            continue;
        }
        req[nr] = '\0';

        char method[16] = {0};
        char uri[2048] = {0};
        if (sscanf(req, "%15s %2047s", method, uri) != 2) {
            send_response(client_fd, "400 Bad Request", "text/plain", "bad request\n");
            close(client_fd);
            continue;
        }

        if (strcmp(method, "GET") != 0) {
            send_response(client_fd, "405 Method Not Allowed", "text/plain", "method not allowed\n");
            close(client_fd);
            continue;
        }

        char path[1024];
        char instance_filter[256];
        split_path(uri, path, sizeof(path));
        resolve_instance_filter(uri, default_instance_filter,
                                instance_filter, sizeof(instance_filter));

        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            jbuf_t html;
            jbuf_init(&html, 16384);
            if (build_timeline_html(&html, instance_filter)) {
                send_response(client_fd, "200 OK", "text/html", html.data ? html.data : "");
            } else {
                send_response(client_fd, "500 Internal Server Error", "text/plain", "failed to render timeline\n");
            }
            jbuf_free(&html);
        } else if (strcmp(path, "/events.json") == 0) {
            jbuf_t json;
            jbuf_init(&json, 16384);
            if (build_events_json(&json, instance_filter)) {
                send_response(client_fd, "200 OK", "application/json", json.data ? json.data : "{}");
            } else {
                send_response(client_fd, "500 Internal Server Error", "application/json", "{\"error\":\"failed to query events\"}");
            }
            jbuf_free(&json);
        } else if (strcmp(path, "/health") == 0) {
            send_response(client_fd, "200 OK", "text/plain", "ok\n");
        } else {
            send_response(client_fd, "404 Not Found", "text/plain", "not found\n");
        }

        close(client_fd);
    }

    close(server_fd);
    baseline_log("server", "timeline_server_stop", NULL, NULL);
    return 0;
}
