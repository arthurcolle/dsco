#include "trace_kg.h"
#include "trace_kg_store.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* Set only around emission of the source event; never propagated to entities. */
static _Thread_local yyjson_val *event_metadata;
/* JSON serialization is structural, never interpolation of untrusted text. */
static int emit_record(FILE *out, const char *kind, const char *id, const char *type,
                const char *from, const char *to, const char *evidence, const char *label) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) return 1;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_str(d, o, "schema", "dsco.trace-kg.v1");
    yyjson_mut_obj_add_str(d, o, "kind", kind);
    yyjson_mut_obj_add_str(d, o, "id", id);
    yyjson_mut_obj_add_str(d, o, "type", type);
    if (label) yyjson_mut_obj_add_str(d, o, "label", label);
    yyjson_mut_obj_add_str(d, o, "projection", "telemetry.v3");
    if (from) yyjson_mut_obj_add_str(d, o, "from", from);
    if (to) yyjson_mut_obj_add_str(d, o, "to", to);
    if (evidence) {
        yyjson_mut_obj_add_str(d, o, "evidence", evidence);
        yyjson_mut_obj_add_str(d, o, "epistemic_status", "observed_telemetry");
    }
    if (event_metadata && label) {
        const char *keys[] = {"timeout_origin", "execution_status", "failure_class", "wrapper_tool_name", "requested_tool_name", "tool_name"};
        for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
            const char *v=yyjson_get_str(yyjson_obj_get(event_metadata,keys[i]));
            if (v) yyjson_mut_obj_add_str(d,o,keys[i],v);
        }
        yyjson_val *ok=yyjson_obj_get(event_metadata,"ok");
        if (yyjson_is_bool(ok)) yyjson_mut_obj_add_bool(d,o,"ok",yyjson_get_bool(ok));
        yyjson_val *v=yyjson_obj_get(event_metadata,"timeout");
        if (yyjson_is_bool(v)) yyjson_mut_obj_add_bool(d,o,"timeout",yyjson_get_bool(v));
    }
    char *s = yyjson_mut_write(d, 0, NULL);
    int rc = !s || fprintf(out, "%s\n", s) < 0;
    free(s); yyjson_mut_doc_free(d); return rc;
}

static int emit(FILE *out, const char *kind, const char *id, const char *type,
                const char *from, const char *to, const char *evidence) {
    return emit_record(out, kind, id, type, from, to, evidence, NULL);
}

/* Length-prefixed components make namespace and tuple boundaries unambiguous. */
static void identity(const char *kind, const char *scope, const char *value, char id[65]) {
    sha256_ctx_t h; unsigned char digest[32];
    const char *parts[] = {"dsco.trace-kg.v1", kind, scope, value};
    sha256_init(&h);
    for (size_t i = 0; i < 4; i++) {
        char n[32]; size_t len = strlen(parts[i]);
        int used = snprintf(n, sizeof(n), "%zu:", len);
        sha256_update(&h, (const unsigned char *)n, (size_t)used);
        sha256_update(&h, (const unsigned char *)parts[i], len);
    }
    sha256_final(&h, digest);
    for (int i = 0; i < 32; i++) snprintf(id + i * 2, 3, "%02x", digest[i]);
}

static int edge(FILE *out, const char *type, const char *from, const char *to,
                const char *evidence) {
    char pair[130], id[65];
    snprintf(pair, sizeof(pair), "%s:%s", from, to);
    identity(type, evidence, pair, id);
    return emit(out, "edge", id, type, from, to, evidence);
}

static const char *text(sqlite3_stmt *s, int col) {
    const unsigned char *v = sqlite3_column_text(s, col);
    return v ? (const char *)v : "";
}

int trace_kg_export(const char *database, const char *session, FILE *out) {
    sqlite3 *db = NULL; sqlite3_stmt *s = NULL; int rc = 1;
    if (!database || !out) return 1;
    if (sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    sqlite3_busy_timeout(db, 2000);
    /* One read transaction gives a consistent snapshot while the recorder runs. */
    if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) goto done;
    const char *sql =
        "SELECT event_id,session_id,trace_id,span_id,parent_span_id,event_type,payload_json "
        "FROM events WHERE (?1 IS NULL OR session_id=?1) "
        "ORDER BY session_id,seq,event_id";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) goto done;
    if (session) sqlite3_bind_text(s, 1, session, -1, SQLITE_TRANSIENT);
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *event = text(s,0), *sess = text(s,1), *trace = text(s,2);
        const char *span = text(s,3), *parent = text(s,4), *type = text(s,5);
        char eid[65], sid[65], tid[65], cid[65], pid[65], toolid[65];
        if (!*event || !*sess || !*type) goto done;
        identity("event", sess, event, eid); identity("session", "", sess, sid);
        yyjson_doc *metadata_doc = yyjson_read(text(s,6),strlen(text(s,6)),0);
        event_metadata = metadata_doc ? yyjson_doc_get_root(metadata_doc) : NULL;
        int event_failed = emit_record(out,"node",eid,type,NULL,NULL,eid,event);
        event_metadata = NULL;
        yyjson_doc_free(metadata_doc);
        if (event_failed || emit(out,"node",sid,"session",NULL,NULL,eid) || edge(out,"contains",sid,eid,eid)) goto done;
        if (*trace) {
            identity("trace",sess,trace,tid);
            if (emit(out,"node",tid,"trace",NULL,NULL,eid) || edge(out,"in_trace",eid,tid,eid)) goto done;
        }
        if (*span) {
            identity("span",sess,span,cid);
            if (emit(out,"node",cid,"span",NULL,NULL,eid) || edge(out,"in_span",eid,cid,eid)) goto done;
            if (*parent) {
                identity("span",sess,parent,pid);
                if (emit(out,"node",pid,"span",NULL,NULL,eid) || edge(out,"child_of",cid,pid,eid)) goto done;
            }
        }
        yyjson_doc *payload = yyjson_read(text(s,6), strlen(text(s,6)), 0);
        yyjson_val *p = payload ? yyjson_doc_get_root(payload) : NULL;
        /* Malformed payloads are represented as events, never silently promoted
         * into successful calls. Only typed tool events produce tool relations. */
        if (strcmp(type,"tool.call.created") == 0 || strcmp(type,"tool.call.completed") == 0) {
            const char *tool = yyjson_get_str(yyjson_obj_get(p,"tool_name"));
            int failed = 0;
            if (tool && *tool) {
                identity("tool","",tool,toolid);
                failed = emit_record(out,"node",toolid,"tool",NULL,NULL,eid,tool) || edge(out,"uses_tool",eid,toolid,eid);
            }
            yyjson_val *ok = yyjson_obj_get(p,"ok");
            if (!failed && strcmp(type,"tool.call.completed") == 0 && yyjson_is_bool(ok)) {
                char oid[65]; const char *outcome = yyjson_get_bool(ok) ? "reported_success" : "reported_failure";
                identity("outcome",eid,outcome,oid);
                failed = emit(out,"node",oid,outcome,NULL,NULL,eid) || edge(out,"reports",eid,oid,eid);
            }
            if (failed) { yyjson_doc_free(payload); goto done; }
        }
        yyjson_doc_free(payload);
    }
    if (step != SQLITE_DONE || fflush(out) != 0) goto done;
    rc = 0;
done:
    if (rc) fprintf(stderr,"trace-kg: export failed; discard partial output%s%s\n",db ? ": " : "",db ? sqlite3_errmsg(db) : "");
    sqlite3_finalize(s); sqlite3_close(db); return rc;
}

int trace_kg_cli(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "export") != 0)
        return trace_kg_store_cli(argc, argv);
    if (argc < 4 || argc > 5 || strcmp(argv[2],"export") != 0) {
        fprintf(stderr,"usage: dsco trace-kg export <chronicle.sqlite> [session-id]\n"); return 2;
    }
    return trace_kg_export(argv[3], argc == 5 ? argv[4] : NULL, stdout);
}
