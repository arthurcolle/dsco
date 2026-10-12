#include <assert.h>
#include <unistd.h>
#include "../src/context_fabric.c"

static int scalar(sqlite3 *db,const char *sql) {
    sqlite3_stmt *s=NULL;assert(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK);
    assert(sqlite3_step(s)==SQLITE_ROW);int n=sqlite3_column_int(s,0);sqlite3_finalize(s);return n;
}
int main(void) {
    char base[]="/tmp/dsco-fts-index-XXXXXX";int fd=mkstemp(base);assert(fd>=0);close(fd);unlink(base);
    assert(!setenv("DSCO_CONTEXT_DB",base,1));
    char path[1024];snprintf(path,sizeof(path),"%s.fts",base);
    sqlite3 *old=NULL;assert(sqlite3_open(path,&old)==SQLITE_OK);
    assert(sqlite3_exec(old,"CREATE VIRTUAL TABLE ctx_fts USING fts5(ckey UNINDEXED,source,tags,body);"
        "INSERT INTO ctx_fts VALUES('legacy','test','','oldbody');",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(old);
    sqlite3 *db=ctx_fts_open_at(NULL);assert(db);
    assert(scalar(db,"SELECT count(*) FROM ctx_fts_keys WHERE ckey='legacy'")==1);
    ctx_fts_index(db,"legacy","test","","newbody");
    assert(scalar(db,"SELECT count(*) FROM ctx_fts WHERE ckey='legacy'")==1);
    assert(scalar(db,"SELECT count(*) FROM ctx_fts WHERE ctx_fts MATCH 'newbody'")==1);
    assert(scalar(db,"SELECT count(*) FROM ctx_fts WHERE ctx_fts MATCH 'oldbody'")==0);
    char key[64];for(int i=0;i<10000;i++) {snprintf(key,sizeof(key),"key-%d",i);ctx_fts_index(db,key,"test","","stablebody");}
    assert(scalar(db,"SELECT count(*) FROM ctx_fts")==10001);
    sqlite3_stmt *plan=NULL;assert(sqlite3_prepare_v2(db,"EXPLAIN QUERY PLAN DELETE FROM ctx_fts WHERE rowid=(SELECT fts_rowid FROM ctx_fts_keys WHERE ckey=?1) AND ckey=?1",-1,&plan,NULL)==SQLITE_OK);
    bool lookup=false,rowid_lookup=false;
    while(sqlite3_step(plan)==SQLITE_ROW) {
        const char *detail=(const char*)sqlite3_column_text(plan,3);
        if(strstr(detail,"SEARCH ctx_fts_keys USING INDEX")) lookup=true;
        if(strstr(detail,"VIRTUAL TABLE INDEX 0:=")) rowid_lookup=true;
    }
    sqlite3_finalize(plan);assert(lookup && rowid_lookup);
    assert(sqlite3_exec(db,"CREATE TRIGGER fail_map BEFORE INSERT ON ctx_fts_keys BEGIN SELECT RAISE(ABORT,'fixture'); END;",NULL,NULL,NULL)==SQLITE_OK);
    ctx_fts_index(db,"legacy","test","","failedbody");
    assert(scalar(db,"SELECT count(*) FROM ctx_fts WHERE ctx_fts MATCH 'newbody'")==1);
    assert(scalar(db,"SELECT count(*) FROM ctx_fts WHERE ctx_fts MATCH 'failedbody'")==0);
    sqlite3_close(db);db=ctx_fts_open_at(NULL);assert(db);
    assert(scalar(db,"SELECT count(*) FROM ctx_fts_keys")==10001);sqlite3_close(db);
    unlink(path);snprintf(path,sizeof(path),"%s.fts-wal",base);unlink(path);snprintf(path,sizeof(path),"%s.fts-shm",base);unlink(path);
    puts("PASS FTS: legacy migration, indexed rowid lookup, 10000 inserts, replacement, transactional rollback, reopen");
}
