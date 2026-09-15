/* Immutable prompt revisions backed by the native context fabric. Branch heads
 * use SQLite transactions + compare-and-swap; no evaluator or instruction-tier
 * changes. The CLI is local-principal access, not a network authorization gate. */
#include "prompt_branch.h"
#include "context_fabric.h"
#include "crypto.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PB_LIMIT (256U * 1024U)
#define PB_TEXT (64U * 1024U)
static const char *pb_string(yyjson_val *o, const char *k) {
    yyjson_val *v = yyjson_obj_get(o, k);
    const char *s = yyjson_get_str(v);
    return s && strlen(s) == yyjson_get_len(v) ? s : NULL;
}
static bool pb_name(const char *s) {
    if (!s || !*s || strlen(s) > 80) return false;
    for (; *s; s++) if (!( (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')) return false;
    return true;
}
static bool pb_hash(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (; *s; s++) if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f')) return false;
    return true;
}
static int pb_error(const char *code, const char *message) {
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"code\":"); jbuf_append_json_str(&b, code);
    jbuf_append(&b, ",\"error\":"); jbuf_append_json_str(&b, message); jbuf_append(&b, "}");
    puts(b.data); jbuf_free(&b); return 1;
}
static sqlite3_stmt *pb_stmt(sqlite3 *db, const char *sql, const char *a, const char *b) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (a) sqlite3_bind_text(s, 1, a, -1, SQLITE_TRANSIENT);
    if (b) sqlite3_bind_text(s, 2, b, -1, SQLITE_TRANSIENT);
    return s;
}
static bool pb_head(sqlite3 *db, const char *doc, const char *branch, char out[65]) {
    sqlite3_stmt *s = pb_stmt(db, "SELECT head FROM prompt_branch_heads WHERE document=? AND name=?", doc, branch);
    bool ok = s && sqlite3_step(s) == SQLITE_ROW;
    if (ok) snprintf(out, 65, "%s", sqlite3_column_text(s, 0));
    sqlite3_finalize(s); return ok;
}
static char *pb_revision(sqlite3 *db, const char *doc, const char *id) {
    sqlite3_stmt *s = pb_stmt(db, "SELECT metadata FROM prompt_branch_revisions WHERE document=? AND id=?", doc, id);
    char *out = s && sqlite3_step(s) == SQLITE_ROW ? safe_strdup((const char *)sqlite3_column_text(s, 0)) : NULL;
    sqlite3_finalize(s); return out;
}
static bool pb_event(sqlite3 *db, const char *doc, const char *branch, const char *action,
                      const char *before, const char *after, const char *author) {
    sqlite3_stmt *s = pb_stmt(db, "INSERT INTO prompt_branch_events(document,branch,action,before_head,after_head,author) VALUES(?,?,?,?,?,?)", doc, branch);
    if (!s) return false;
    sqlite3_bind_text(s, 3, action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, before, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, after, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, author, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE; sqlite3_finalize(s); return ok;
}
static void pb_pair(jbuf_t *b, const char *key, const char *value, bool comma) {
    if (comma) jbuf_append(b, ",");
    jbuf_append_json_str(b, key); jbuf_append(b, ":"); jbuf_append_json_str(b, value);
}
static bool pb_save(sqlite3 *db, const char *doc, const char *parent, const char *merge,
                     const char *key, const char *author, const char *message, char id[65]) {
    jbuf_t b; jbuf_init(&b, 512); jbuf_append(&b, "{");
    pb_pair(&b, "schema", "dsco.prompt-revision.v1", false);
    pb_pair(&b, "document", doc, true); pb_pair(&b, "parent", parent, true);
    pb_pair(&b, "merge_parent", merge, true); pb_pair(&b, "ctxkey", key, true);
    pb_pair(&b, "author", author, true); pb_pair(&b, "message", message, true);
    jbuf_append(&b, "}"); sha256_hex((const unsigned char *)b.data, b.len, id);
    sqlite3_stmt *s = pb_stmt(db, "INSERT OR IGNORE INTO prompt_branch_revisions(document,id,metadata) VALUES(?,?,?)", doc, id);
    if (s) sqlite3_bind_text(s, 3, b.data, -1, SQLITE_TRANSIENT);
    bool ok = s && sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s); jbuf_free(&b); return ok;
}
static bool pb_set(sqlite3 *db, const char *doc, const char *branch, const char *head, bool create) {
    sqlite3_stmt *s = pb_stmt(db, create ?
        "INSERT INTO prompt_branch_heads(document,name,head) VALUES(?,?,?)" :
        "UPDATE prompt_branch_heads SET head=?3 WHERE document=?1 AND name=?2", doc, branch);
    if (s) sqlite3_bind_text(s, 3, head, -1, SQLITE_TRANSIENT);
    bool ok = s && sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(s); return ok;
}
static int pb_run(yyjson_val *root, sqlite3 *db, ctx_broker_t *broker) {
    const char *action = pb_string(root, "action"), *doc = pb_string(root, "document");
    const char *branch = pb_string(root, "branch"); if (!branch) branch = "main";
    const char *author = pb_string(root, "author"); if (!author) author = "local-principal";
    const char *message = pb_string(root, "message"); if (!message) message = "";
    if (!action || !pb_name(doc) || !pb_name(branch) || !*author || strlen(author) > 120 || strlen(message) > 1024)
        return pb_error("invalid", "action, document and valid names are required; author/message are bounded");
    if (!strcmp(action, "list")) {
        sqlite3_stmt *s = pb_stmt(db, "SELECT name,head FROM prompt_branch_heads WHERE document=? ORDER BY name LIMIT 257", doc, NULL);
        if (!s) return pb_error("storage", "list failed");
        jbuf_t b; jbuf_init(&b, 512); jbuf_append(&b, "{\"ok\":true,\"branches\":[");
        int count = 0, rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            if (count++) jbuf_append(&b, ",");
            jbuf_append(&b, "{"); pb_pair(&b, "name", (const char *)sqlite3_column_text(s, 0), false);
            pb_pair(&b, "head", (const char *)sqlite3_column_text(s, 1), true); jbuf_append(&b, "}");
        }
        sqlite3_finalize(s); jbuf_append(&b, "],\"limit\":257}");
        if (rc == SQLITE_DONE) puts(b.data);
        jbuf_free(&b); return rc == SQLITE_DONE ? 0 : pb_error("storage", "list failed");
    }
    char head[65] = "", target[65] = "", key[CTX_KEY_STR_MAX] = "";
    if (!strcmp(action, "get") || !strcmp(action, "history")) {
        const char *revision = pb_string(root, "revision");
        if (revision) { if (!pb_hash(revision)) return pb_error("invalid", "invalid revision"); snprintf(head, sizeof(head), "%s", revision); }
        else if (!pb_head(db, doc, branch, head)) return pb_error("not_found", "branch not found");
        jbuf_t out; jbuf_init(&out, 512); jbuf_append(&out, "{\"ok\":true,");
        bool history = !strcmp(action, "history");
        if (history) jbuf_append(&out, "\"history\":[");
        bool more = false;
        for (int i = 0; i < (history ? 100 : 1); i++) {
            char *metadata = pb_revision(db, doc, head);
            if (!metadata) { jbuf_free(&out); return pb_error("not_found", "revision not found"); }
            char computed[65]; sha256_hex((const unsigned char *)metadata, strlen(metadata), computed);
            if (strcmp(computed, head)) { free(metadata); jbuf_free(&out); return pb_error("integrity", "revision hash mismatch"); }
            yyjson_doc *r = yyjson_read(metadata, strlen(metadata), 0);
            yyjson_val *v = r ? yyjson_doc_get_root(r) : NULL;
            const char *ck = pb_string(v, "ctxkey"), *parent = pb_string(v, "parent");
            if (!ck || !parent) { yyjson_doc_free(r); free(metadata); jbuf_free(&out); return pb_error("integrity", "invalid revision metadata"); }
            if (history) { if (i) jbuf_append(&out, ","); jbuf_append(&out, "{"); }
            pb_pair(&out, "revision", head, false); jbuf_append(&out, ",\"metadata\":"); jbuf_append(&out, metadata);
            if (!history) {
                ctxkey_t parsed; size_t n = 0;
                char *text = ctxkey_parse(ck, &parsed) ? ctx_get(broker, &parsed, &n) : NULL;
                if (text) sha256_hex((const unsigned char *)text, n, computed);
                if (!text || strcmp(computed, parsed.id) || strlen(text) != n) {
                    free(text); yyjson_doc_free(r); free(metadata); jbuf_free(&out); return pb_error("integrity", "content missing or corrupt");
                }
                pb_pair(&out, "content", text, true); free(text);
            } else jbuf_append(&out, "}");
            more = *parent != 0;
            snprintf(head, sizeof(head), "%s", parent);
            yyjson_doc_free(r); free(metadata);
            if (!more) break;
        }
        if (history) jbuf_append(&out, more ? "],\"truncated\":true" : "],\"truncated\":false");
        jbuf_append(&out, "}"); puts(out.data); jbuf_free(&out); return 0;
    }
    bool init = !strcmp(action, "init"), fork = !strcmp(action, "fork"), commit = !strcmp(action, "commit"), promote = !strcmp(action, "promote");
    if (!init && !fork && !commit && !promote) return pb_error("invalid", "unknown action");
    if ((init || promote) && strcmp(branch, "main")) return pb_error("invalid", "init and promote target main");
    if ((commit || fork) && !strcmp(branch, "main")) return pb_error("protected", "main changes only through explicit promote; commit to a branch");
    const char *expected = pb_string(root, "expected"), *text = pb_string(root, "content");
    if (!init && !pb_hash(expected)) return pb_error("invalid", "expected revision required");
    if ((init || commit) && (!text || strlen(text) > PB_TEXT)) return pb_error("invalid", "content must be a string <=64 KiB");
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return pb_error("busy", "writer transaction unavailable");
    const char *error = "storage", *detail = "transaction failed";
    bool exists = pb_head(db, doc, branch, head), ok = false;
    if (init && exists) { error = "conflict"; detail = "document already exists"; goto done; }
    if (fork && exists) { error = "conflict"; detail = "branch already exists"; goto done; }
    if ((commit || promote) && (!exists || strcmp(head, expected))) { error = "conflict"; detail = "stale branch head"; goto done; }
    char merge[65] = "";
    if (fork || promote) {
        const char *from = pb_string(root, "from");
        if (!pb_name(from) || (promote && !strcmp(from, "main")) || !pb_head(db, doc, from, target)) {
            error = "not_found"; detail = "source branch not found"; goto done;
        }
        const char *source_expected = promote ? pb_string(root, "source_expected") : expected;
        if (!pb_hash(source_expected) || strcmp(target, source_expected)) { error = "conflict"; detail = "stale source head"; goto done; }
        const char *revision = pb_string(root, "revision");
        if (fork && revision) { if (!pb_hash(revision)) { error = "invalid"; detail = "invalid revision"; goto done; } snprintf(target, sizeof(target), "%s", revision); }
        char *metadata = pb_revision(db, doc, target);
        if (!metadata) { error = "not_found"; detail = "source revision not found"; goto done; }
        char computed[65]; sha256_hex((const unsigned char *)metadata, strlen(metadata), computed);
        if (strcmp(computed, target)) { free(metadata); error = "integrity"; detail = "source revision hash mismatch"; goto done; }
        yyjson_doc *r = yyjson_read(metadata, strlen(metadata), 0);
        const char *ck = pb_string(r ? yyjson_doc_get_root(r) : NULL, "ctxkey");
        if (ck && strlen(ck) < sizeof(key)) snprintf(key, sizeof(key), "%s", ck);
        yyjson_doc_free(r); free(metadata);
        ctxkey_t parsed; size_t bytes = 0;
        char *source_text = ctxkey_parse(key, &parsed) ? ctx_get(broker, &parsed, &bytes) : NULL;
        if (source_text) sha256_hex((const unsigned char *)source_text, bytes, computed);
        bool intact = source_text && !strcmp(computed, parsed.id) && strlen(source_text) == bytes;
        free(source_text);
        if (!intact) { error = "integrity"; detail = "source content missing or corrupt"; goto done; }
        snprintf(merge, sizeof(merge), "%s", target);
    }
    if (init || commit) {
        ctx_put_opts_t opts = {.kind = CTX_KIND_BLOB, .source = "prompt-branch", .tags = "prompt,versioned", .embed = 0};
        ctxkey_t k;
        if (!ctx_put(broker, text, strlen(text), &opts, &k, NULL) || ctxkey_format(&k, key, sizeof(key)) < 0 || !ctx_pin(broker, &k)) {
            detail = "context fabric put/pin failed"; goto done;
        }
    }
    if (!fork && !pb_save(db, doc, head, merge, key, author, message, target)) goto done;
    if (!pb_set(db, doc, branch, target, init || fork) || !pb_event(db, doc, branch, action, head, target, author)) goto done;
    ok = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
 done:
    if (!ok) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return pb_error(error, detail); }
    jbuf_t out; jbuf_init(&out, 256); jbuf_append(&out, "{\"ok\":true,");
    pb_pair(&out, "document", doc, false); pb_pair(&out, "branch", branch, true);
    pb_pair(&out, "revision", target, true); pb_pair(&out, "ctxkey", key, true);
    jbuf_append(&out, "}"); puts(out.data); jbuf_free(&out); return 0;
}
int prompt_branch_cli(void) {
    /* stdin avoids private prompt contents in process arguments. */
    char *input = malloc(PB_LIMIT + 2);
    if (!input) return pb_error("storage", "allocation failed");
    size_t n = fread(input, 1, PB_LIMIT + 1, stdin); input[n] = 0;
    if (ferror(stdin) || n > PB_LIMIT) { free(input); return pb_error("invalid", "request exceeds 256 KiB or read failed"); }
    yyjson_doc *request = yyjson_read(input, n, 0); free(input);
    yyjson_val *root = request ? yyjson_doc_get_root(request) : NULL;
    bool valid = yyjson_is_obj(root);
    size_t i, count; yyjson_val *k, *v;
    yyjson_obj_foreach(root, i, count, k, v) {
        const char *name = yyjson_get_str(k);
        if (!name || strlen(name) != yyjson_get_len(k) || !yyjson_is_str(v) || strlen(yyjson_get_str(v)) != yyjson_get_len(v)) valid = false;
        const char *allowed[] = {"action","document","branch","author","message","content","expected","from","revision","source_expected"};
        bool known = false; for (size_t a = 0; a < sizeof(allowed)/sizeof(*allowed); a++) if (name && !strcmp(name, allowed[a])) known = true;
        if (!known) valid = false;
        yyjson_obj_iter it = yyjson_obj_iter_with(root); yyjson_val *other; int matches = 0;
        while ((other = yyjson_obj_iter_next(&it))) if (name && yyjson_equals_str(other, name)) matches++;
        if (matches != 1) valid = false;
    }
    if (!valid) { yyjson_doc_free(request); return pb_error("invalid", "expected a string-valued JSON object without duplicate or unknown fields"); }
    const char *path = getenv("DSCO_PROMPT_BRANCH_DB"), *context_path = getenv("DSCO_CONTEXT_DB");
    char default_path[1024];
    if (!path || !*path) {
        const char *home = getenv("HOME");
        if (!home || snprintf(default_path, sizeof(default_path), "%s/.dsco/context/prompt-branches.db", home) >= (int)sizeof(default_path)) {
            yyjson_doc_free(request); return pb_error("storage", "HOME missing or path too long");
        }
        path = default_path;
    }
    if (strlen(path) > 1000 || (context_path && !strcmp(path, context_path))) { yyjson_doc_free(request); return pb_error("invalid", "branch and context databases must be separate, bounded paths"); }
    mode_t old_mask = umask(0077);
    ctx_broker_t *broker = ctx_broker_open(context_path && *context_path ? context_path : NULL);
    sqlite3 *db = NULL;
    if (!broker || sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        sqlite3_close(db); ctx_broker_close(broker); umask(old_mask); yyjson_doc_free(request); return pb_error("storage", "cannot open stores; create the configured parent directory first");
    }
    sqlite3_busy_timeout(db, 5000);
    const char *schema = "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS prompt_branch_revisions(document TEXT NOT NULL,id TEXT NOT NULL,metadata TEXT NOT NULL,created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(document,id));"
        "CREATE TABLE IF NOT EXISTS prompt_branch_heads(document TEXT NOT NULL,name TEXT NOT NULL,head TEXT NOT NULL,PRIMARY KEY(document,name));"
        "CREATE TABLE IF NOT EXISTS prompt_branch_events(seq INTEGER PRIMARY KEY,document TEXT NOT NULL,branch TEXT NOT NULL,action TEXT NOT NULL,before_head TEXT NOT NULL,after_head TEXT NOT NULL,author TEXT NOT NULL,created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK ? pb_run(root, db, broker) : pb_error("storage", "schema unavailable");
    sqlite3_close(db); ctx_broker_close(broker); yyjson_doc_free(request); umask(old_mask); return rc;
}
