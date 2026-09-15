#include "trace_kg_store.h"
#include "trace_kg.h"
#include "../vendor/yyjson.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int ingest(const char *source, const char *target) {
    struct stat a,b;
    if (stat(source,&a) != 0 || (stat(target,&b)==0 && a.st_dev==b.st_dev && a.st_ino==b.st_ino)) {
        fprintf(stderr,"trace-kg: source missing or destination aliases source\n"); return 1;
    }
    FILE *f=tmpfile();
    if (!f) return 1;
    /* Complete a consistent source snapshot before touching destination. */
    if (trace_kg_export(source,NULL,f)) { fclose(f); return 1; }
    rewind(f);
    sqlite3 *db=NULL; sqlite3_stmt *put=NULL; int rc=1;
    if (sqlite3_open(target,&db)!=SQLITE_OK) goto done;
    sqlite3_busy_timeout(db,2000);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS kg_assertions("
        "id TEXT NOT NULL, evidence TEXT NOT NULL, kind TEXT NOT NULL, type TEXT NOT NULL,"
        "from_id TEXT,to_id TEXT,label TEXT,record TEXT NOT NULL,PRIMARY KEY(id,evidence));"
        "CREATE INDEX IF NOT EXISTS kg_type ON kg_assertions(type);"
        "CREATE INDEX IF NOT EXISTS kg_from ON kg_assertions(from_id);"
        "CREATE INDEX IF NOT EXISTS kg_evidence ON kg_assertions(evidence);",
        NULL,NULL,NULL)!=SQLITE_OK) goto done;
    /* Validate the entire destination, including disjoint legacy records. */
    sqlite3_stmt *version=NULL;
    if (sqlite3_prepare_v2(db,"SELECT count(*) FROM kg_assertions WHERE NOT json_valid(record) OR COALESCE(json_extract(record,'$.projection'),'')<>'telemetry.v3'",-1,&version,NULL)!=SQLITE_OK) goto done;
    int valid=sqlite3_step(version)==SQLITE_ROW && sqlite3_column_int64(version,0)==0;
    sqlite3_finalize(version);
    if (!valid) { fprintf(stderr,"trace-kg: projection conflict: rebuild into a new graph\n"); goto done; }
    if (sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS kg_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO kg_metadata VALUES('projection','telemetry.v3');",NULL,NULL,NULL)!=SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,"SELECT value FROM kg_metadata WHERE key='projection'",-1,&version,NULL)!=SQLITE_OK) goto done;
    valid=sqlite3_step(version)==SQLITE_ROW && strcmp((const char *)sqlite3_column_text(version,0),"telemetry.v3")==0;
    sqlite3_finalize(version);
    if (!valid) { fprintf(stderr,"trace-kg: projection conflict: rebuild into a new graph\n"); goto done; }
    /* Exact replay only. */
    if (sqlite3_exec(db,"CREATE TRIGGER IF NOT EXISTS kg_conflict BEFORE INSERT ON kg_assertions "
        "WHEN EXISTS(SELECT 1 FROM kg_assertions WHERE id=NEW.id AND evidence=NEW.evidence AND record<>NEW.record) "
        "BEGIN SELECT RAISE(ABORT,'projection/evidence conflict: rebuild into a new graph'); END;",NULL,NULL,NULL)!=SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,"INSERT INTO kg_assertions VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(id,evidence) DO NOTHING",-1,&put,NULL)!=SQLITE_OK) goto done;
    char *line=NULL; size_t cap=0; ssize_t n; long long added=0;
    while ((n=getline(&line,&cap,f))>=0) {
        yyjson_doc *d=yyjson_read(line,(size_t)n,0);
        yyjson_val *o=d?yyjson_doc_get_root(d):NULL;
        const char *keys[]={"id","evidence","kind","type","from","to","label"};
        int bad=0;
        for (int i=0;i<7;i++) {
            const char *v=yyjson_get_str(yyjson_obj_get(o,keys[i]));
            if (i<4 && !v) bad=1;
            if (v) sqlite3_bind_text(put,i+1,v,-1,SQLITE_TRANSIENT);
            else sqlite3_bind_null(put,i+1);
        }
        sqlite3_bind_text(put,8,line,(int)n,SQLITE_TRANSIENT);
        if (bad || sqlite3_step(put)!=SQLITE_DONE) {
            yyjson_doc_free(d); free(line); goto done;
        }
        added+=sqlite3_changes(db);
        sqlite3_reset(put); sqlite3_clear_bindings(put); yyjson_doc_free(d);
    }
    free(line);
    if (ferror(f) || sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK) goto done;
    printf("{\"schema\":\"dsco.trace-kg.ingest.v1\",\"assertions_added\":%lld}\n",added);
    rc=0;
done:
    if (rc) {
        fprintf(stderr,"trace-kg: ingest failed: %s\n",db?sqlite3_errmsg(db):"open failed");
        if (db) sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    }
    sqlite3_finalize(put); sqlite3_close(db); fclose(f); return rc;
}

static int query(const char *path, const char *command, const char *event) {
    sqlite3 *db=NULL; sqlite3_stmt *s=NULL; int rc=1;
    if (sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK) goto done;
    sqlite3_busy_timeout(db,2000);
    const char *sql;
    if (!strcmp(command,"summary")) {
        sql="SELECT json_object('wrapper',COALESCE(json_extract(e.record,'$.wrapper_tool_name'),json_extract(e.record,'$.tool_name'),'unknown'),"
            "'requested_target',COALESCE(json_extract(e.record,'$.requested_tool_name'),(SELECT CASE WHEN count(DISTINCT start.id)=1 THEN max(json_extract(start.record,'$.requested_tool_name')) ELSE 'ambiguous' END FROM kg_assertions span JOIN kg_assertions other ON other.to_id=span.to_id AND other.kind='edge' AND other.type='in_span' JOIN kg_assertions start ON start.id=other.from_id AND start.kind='node' AND start.type='tool.call.created' AND start.id=start.evidence WHERE span.from_id=e.id AND span.kind='edge' AND span.type='in_span' HAVING count(DISTINCT start.id)>0),'unknown'),"
            "'timeout_origin',COALESCE(json_extract(e.record,'$.timeout_origin'),'unknown'),"
            "'failure_class',COALESCE(json_extract(e.record,'$.failure_class'),'unknown'),"
            "'evidence',e.id) AS record FROM kg_assertions e WHERE e.kind='node' AND e.id=e.evidence AND e.type='tool.call.completed' "
            "AND EXISTS(SELECT 1 FROM kg_assertions f WHERE f.evidence=e.id AND f.kind='node' AND f.id<>f.evidence AND f.type='reported_failure') ORDER BY e.id";
    } else if (!strcmp(command,"failures")) {
        /* Return evidence-backed failures, not denominator-free reliability claims. */
        sql="SELECT DISTINCT a.record FROM kg_assertions a WHERE a.evidence IN "
            "(SELECT evidence FROM kg_assertions WHERE kind='node' AND id<>evidence AND type='reported_failure') "
            "ORDER BY a.evidence,a.kind,a.id";
    } else {
        /* Accept a graph event ID or original Chronicle event ID. */
        sql="SELECT DISTINCT a.record FROM kg_assertions a WHERE a.evidence=?1 OR a.evidence IN "
            "(SELECT id FROM kg_assertions WHERE kind='node' AND label=?1) "
            "ORDER BY a.evidence,a.kind,a.id";
    }
    char *grouped=NULL;
    if (!strcmp(command,"summary")) {
        grouped=sqlite3_mprintf("WITH failures AS (%s) SELECT json_object('wrapper',json_extract(record,'$.wrapper'),'requested_target',json_extract(record,'$.requested_target'),'timeout_origin',json_extract(record,'$.timeout_origin'),'failure_class',json_extract(record,'$.failure_class'),'count',count(*),'evidence',json_group_array(json_extract(record,'$.evidence'))) FROM failures GROUP BY json_extract(record,'$.wrapper'),json_extract(record,'$.requested_target'),json_extract(record,'$.timeout_origin'),json_extract(record,'$.failure_class')",sql);
        if (!grouped) goto done;
        sql=grouped;
    }
    int prepared=sqlite3_prepare_v2(db,sql,-1,&s,NULL);
    sqlite3_free(grouped);
    if (prepared!=SQLITE_OK) goto done;
    if (event) sqlite3_bind_text(s,1,event,-1,SQLITE_TRANSIENT);
    int step;
    while ((step=sqlite3_step(s))==SQLITE_ROW) {
        const char *record=(const char *)sqlite3_column_text(s,0);
        if (!record || fputs(record,stdout)==EOF) goto done;
        if (!strcmp(command,"summary") && fputc('\n',stdout)==EOF) goto done;
    }
    if (step!=SQLITE_DONE || fflush(stdout)) goto done;
    rc=0;
done:
    if (rc) fprintf(stderr,"trace-kg: query failed: %s\n",db?sqlite3_errmsg(db):"open failed");
    sqlite3_finalize(s); sqlite3_close(db); return rc;
}

int trace_kg_store_cli(int argc,char **argv) {
    if (argc==6 && !strcmp(argv[2],"ingest") && !strcmp(argv[4],"--into"))
        return ingest(argv[3],argv[5]);
    if (argc==5 && !strcmp(argv[2],"failures") && !strcmp(argv[4],"--summary")) return query(argv[3],"summary",NULL);
    if (argc==4 && !strcmp(argv[2],"failures")) return query(argv[3],argv[2],NULL);
    if (argc==5 && !strcmp(argv[2],"explain")) return query(argv[3],argv[2],argv[4]);
    fprintf(stderr,"usage: dsco trace-kg ingest <chronicle.sqlite> --into <graph.sqlite>\n"
                   "       dsco trace-kg failures <graph.sqlite>\n"
                   "       dsco trace-kg explain <graph.sqlite> <event-id>\n");
    return 2;
}
