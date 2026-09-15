#define _DARWIN_C_SOURCE 1
#define _XOPEN_SOURCE 700
#include "buffer_store.h"
#include "crypto.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <sqlite3.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
static char output[300000], fixture[PATH_MAX], dbpath[PATH_MAX];
static yyjson_doc *response;
static void check(int pass,const char *message) {
    checks++;
    if(!pass){fprintf(stderr,"FAIL %u: %s\n%s\n",checks,message,output);exit(1);}
}
static yyjson_val *root(void){return yyjson_doc_get_root(response);}
static yyjson_val *buffer(void){return yyjson_obj_get(root(),"buffer");}
static const char *value(const char *key){return yyjson_get_str(yyjson_obj_get(buffer(),key));}
static void copy_value(const char *key,char *out,size_t cap){const char *v=value(key);check(v&&strlen(v)<cap,key);snprintf(out,cap,"%s",v);}
static bool call(const char *request){
    if(response)yyjson_doc_free(response);
    bool ok=tool_buffer(request,output,sizeof(output));
    response=yyjson_read(output,strlen(output),0);
    check(response&&yyjson_is_obj(root()),"result must be valid JSON object");
    check(yyjson_get_bool(yyjson_obj_get(root(),"ok"))==ok,"return value agrees with ok");
    return ok;
}
static void success(const char *request){check(call(request),request);}
static void failure(const char *request,const char *error){check(!call(request),request);check(!strcmp(yyjson_get_str(yyjson_obj_get(root(),"error")),error),error);}
static void request(jbuf_t *b,const char *action,const char *name,const char *field,const char *v,const char *revision,const char *rid){
    jbuf_init(b,256);jbuf_append(b,"{\"action\":");jbuf_append_json_str(b,action);
    if(name){jbuf_append(b,",\"name\":");jbuf_append_json_str(b,name);}
    if(field){jbuf_append_char(b,',');jbuf_append_json_str(b,field);jbuf_append_char(b,':');jbuf_append_json_str(b,v);}
    if(revision){jbuf_append(b,",\"expected_revision\":");jbuf_append_json_str(b,revision);}
    if(rid){jbuf_append(b,",\"request_id\":");jbuf_append_json_str(b,rid);}
    jbuf_append_char(b,'}');
}
static void write_file(const char *path,const char *data){
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);check(fd>=0,"open fixture file");
    size_t n=strlen(data);check(write(fd,data,n)==(ssize_t)n,"write fixture file");check(!close(fd),"close fixture file");
}
static void text_is(const char *name,const char *expected){jbuf_t b;request(&b,"read",name,NULL,NULL,NULL,NULL);success(b.data);jbuf_free(&b);check(!strcmp(yyjson_get_str(yyjson_obj_get(root(),"text")),expected),"text matches");}
static void exec_sql(const char *query){sqlite3 *db=NULL;check(sqlite3_open(dbpath,&db)==SQLITE_OK,"open test metadata");check(sqlite3_exec(db,query,NULL,NULL,NULL)==SQLITE_OK,"test metadata operation");check(sqlite3_close(db)==SQLITE_OK,"close test metadata");}
static int cleanup(const char *path,const struct stat *st,int type,struct FTW *f){(void)st;(void)type;(void)f;return remove(path);}

extern char **environ;
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--retry-child")) {
        char out[4096];return tool_buffer("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"+\",\"request_id\":\"cross-process\"}",out,sizeof(out))?0:1;
    }
    char temp[]="/tmp/dsco-buffer-tests-XXXXXX";check(mkdtemp(temp)!=NULL,"private test root");
    check(realpath(temp,fixture)!=NULL,"resolve test root");
    char store[PATH_MAX];snprintf(store,sizeof(store),"%s/store",fixture);check(!setenv("DSCO_BUFFER_DIR",store,1),"set dedicated store");
    snprintf(dbpath,sizeof(dbpath),"%s/main/index.sqlite",store);
    check(!buffer_store_sensitive("{\"action\":\"list\"}"),"fresh listing not secret");
    check(!buffer_store_sensitive("{\"action\":\"create\",\"name\":\"fresh\"}"),"fresh create not secret");
    success("{\"action\":\"list\"}");check(yyjson_arr_size(yyjson_obj_get(root(),"buffers"))==0,"fresh list empty");
    struct stat st;check(lstat(store,&st)&&errno==ENOENT,"read-only listing creates no files");
    failure("{\"action\":\"nope\"}","invalid_action");check(lstat(store,&st)&&errno==ENOENT,"unknown action has no side effects");
    failure("{\"action\":\"create\",\"name\":\"bad\",\"content\":\"a\\u0000b\"}","invalid_input");
    failure("{\"action\":\"create\",\"action\":\"list\",\"name\":\"bad\"}","invalid_input");
    failure("{\"action\":\"create\",\"name\":\"bad\",\"path\":\"/tmp/nope\"}","invalid_input");
    failure("{\"action\":\"create\",\"name\":\"bad\",\"workspace\":\"../escape\"}","invalid_input");

    success("{\"action\":\"create\",\"name\":\"notes\",\"content\":\"hello 🌍\\n\",\"request_id\":\"create-1\"}");
    char id[37],path[PATH_MAX],revision[65];copy_value("buffer_id",id,sizeof(id));copy_value("content_path",path,sizeof(path));copy_value("revision",revision,sizeof(revision));
    check(!stat(path,&st)&&(st.st_mode&0777)==0600,"content is private regular file");
    check(!buffer_store_sensitive("{\"action\":\"inspect\",\"name\":\"notes\"}"),"ordinary selected buffer not secret");
    success("{\"request_id\":\"create-1\",\"content\":\"hello 🌍\\n\",\"name\":\"notes\",\"action\":\"create\"}");
    check(yyjson_get_bool(yyjson_obj_get(root(),"replayed"))&&!strcmp(id,value("buffer_id")),"create retry identity stable across field order");
    failure("{\"action\":\"create\",\"name\":\"other\",\"request_id\":\"create-1\"}","idempotency_conflict");
    failure("{\"action\":\"create\",\"name\":\"notes\"}","name_conflict");
    text_is("notes","hello 🌍\n");
    const char *encoded=yyjson_get_str(yyjson_obj_get(root(),"base64"));unsigned char decoded[64];size_t bytes=base64_decode(encoded,strlen(encoded),decoded,sizeof(decoded));
    check(bytes==strlen("hello 🌍\n")&&!memcmp(decoded,"hello 🌍\n",bytes),"base64 read is lossless UTF-8");
    success("{\"action\":\"read\",\"name\":\"notes\",\"offset\":6,\"max_bytes\":4}");
    check(!strcmp(yyjson_get_str(yyjson_obj_get(root(),"text")),"🌍")&&yyjson_get_uint(yyjson_obj_get(root(),"next_offset"))==10,"byte chunk boundaries");
    failure("{\"action\":\"read\",\"name\":\"notes\",\"offset\":7}","invalid_offset");
    failure("{\"action\":\"read\",\"name\":\"notes\",\"offset\":6,\"max_bytes\":1}","chunk_too_small");
    failure("{\"action\":\"read\",\"name\":\"notes\",\"max_bytes\":-1}","invalid_input");
    failure("{\"action\":\"write\",\"name\":\"notes\",\"content\":\"changed\"}","revision_required");

    jbuf_t b;request(&b,"write","notes","content","changed",revision,"write-1");success(b.data);success(b.data);jbuf_free(&b);
    check(yyjson_get_bool(yyjson_obj_get(root(),"replayed")),"write retry deduplicated despite old CAS");
    request(&b,"write","notes","content","stale",revision,NULL);failure(b.data,"revision_conflict");jbuf_free(&b);
    request(&b,"fork","notes","new_name","stale fork",revision,NULL);failure(b.data,"revision_conflict");jbuf_free(&b);
    failure("{\"action\":\"inspect\",\"name\":\"stale fork\"}","not_found");
    success("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"!\",\"request_id\":\"append-1\"}");
    success("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"!\",\"request_id\":\"append-1\"}");
    text_is("notes","changed!");
    exec_sql("UPDATE requests SET pending=1 WHERE id='append-1'");
    success("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"!\",\"request_id\":\"append-1\"}");
    check(yyjson_get_bool(yyjson_obj_get(root(),"replayed")),"recover committed content before metadata acknowledgment");
    text_is("notes","changed!");
    sqlite3 *pending_db=NULL;sqlite3_stmt *pending_stmt=NULL;char pending_stage[PATH_MAX];
    check(sqlite3_open(dbpath,&pending_db)==SQLITE_OK,"open pending recovery fixture");
    check(sqlite3_prepare_v2(pending_db,"SELECT stage FROM requests WHERE id='append-1'",-1,&pending_stmt,NULL)==SQLITE_OK&&sqlite3_step(pending_stmt)==SQLITE_ROW,"load staged replacement identity");
    snprintf(pending_stage,sizeof(pending_stage),"%s/main/%s.stage",store,sqlite3_column_text(pending_stmt,0));
    sqlite3_finalize(pending_stmt);sqlite3_close(pending_db);
    write_file(pending_stage,"changed!");write_file(path,"changed");
    exec_sql("UPDATE requests SET pending=1 WHERE id='append-1'");
    success("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"!\",\"request_id\":\"append-1\"}");
    text_is("notes","changed!");check(lstat(pending_stage,&st)&&errno==ENOENT,"recovery consumes committed stage");
    write_file(pending_stage,"changed!");write_file(path,"unrelated editor change");
    exec_sql("UPDATE requests SET pending=1 WHERE id='append-1'");
    failure("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"!\",\"request_id\":\"append-1\"}","pending_conflict");
    text_is("notes","unrelated editor change");
    write_file(path,"changed!");success("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"!\",\"request_id\":\"append-1\"}");
    pid_t child;char *child_argv[]={argv[0],"--retry-child",NULL};
    check(!posix_spawn(&child,argv[0],NULL,NULL,child_argv,environ),"spawn independent store reader");
    success("{\"action\":\"append\",\"name\":\"notes\",\"content\":\"+\",\"request_id\":\"cross-process\"}");
    int status;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process sees persistent retry identity");
    text_is("notes","changed!+");
    write_file(path,"external edit");success("{\"action\":\"inspect\",\"name\":\"notes\"}");
    char expected[65];sha256_hex((const uint8_t*)"external edit",13,expected);check(!strcmp(value("revision"),expected),"external edit refreshes revision");
    copy_value("revision",revision,sizeof(revision));
    struct stat dbstat1,dbstat2;check(!stat(dbpath,&dbstat1),"stat index before metadata lookup");
    text_is("notes","external edit");check(!buffer_store_sensitive("{\"action\":\"read\",\"name\":\"notes\"}"),"read sensitivity lookup");check(!stat(dbpath,&dbstat2),"stat index after metadata lookup");
    check(dbstat1.st_mtimespec.tv_sec==dbstat2.st_mtimespec.tv_sec&&dbstat1.st_mtimespec.tv_nsec==dbstat2.st_mtimespec.tv_nsec,"read actions do not update metadata");
    success("{\"action\":\"rename\",\"name\":\"notes\",\"new_name\":\"renamed\"}");check(!strcmp(id,value("buffer_id")),"rename keeps stable ID");
    request(&b,"inspect","wrong","buffer_id",id,NULL,NULL);failure(b.data,"selector_conflict");jbuf_free(&b);
    success("{\"action\":\"close\",\"name\":\"renamed\"}");check(yyjson_get_bool(yyjson_obj_get(buffer(),"closed")),"closed flag set");
    check(!stat(path,&st),"close preserves content file");text_is("renamed","external edit");
    failure("{\"action\":\"append\",\"name\":\"renamed\",\"content\":\"x\"}","buffer_closed");
    success("{\"action\":\"reopen\",\"name\":\"renamed\"}");
    success("{\"action\":\"create\",\"name\":\"secret\",\"sensitive\":true,\"kind\":\"log\",\"content\":\"private\"}");
    check(buffer_store_sensitive("{\"action\":\"inspect\",\"name\":\"secret\"}"),"sensitive selected lookup");
    check(buffer_store_sensitive("{\"action\":\"list\"}"),"listing sensitive metadata is classified");
    success("{\"action\":\"fork\",\"name\":\"secret\",\"new_name\":\"forked\",\"request_id\":\"fork-1\"}");
    check(yyjson_get_bool(yyjson_obj_get(buffer(),"sensitive")),"fork retains sensitivity");char forkid[37];copy_value("buffer_id",forkid,sizeof(forkid));
    success("{\"action\":\"fork\",\"name\":\"secret\",\"new_name\":\"forked\",\"request_id\":\"fork-1\"}");check(!strcmp(forkid,value("buffer_id")),"fork retry stable");
    text_is("forked","private");

    char source[PATH_MAX],alternate[PATH_MAX];snprintf(source,sizeof(source),"%s/source.txt",fixture);snprintf(alternate,sizeof(alternate),"%s/target.txt",fixture);
    write_file(source,"imported");
    jbuf_init(&b,256);jbuf_append(&b,"{\"action\":\"create\",\"name\":\"filebuf\",\"kind\":\"file\",\"source_path\":");jbuf_append_json_str(&b,source);jbuf_append_char(&b,'}');success(b.data);jbuf_free(&b);
    copy_value("revision",revision,sizeof(revision));request(&b,"write","filebuf","content","saved content",revision,NULL);success(b.data);jbuf_free(&b);
    char actual[64]={0};int fd=open(source,O_RDONLY);check(fd>=0&&read(fd,actual,sizeof(actual)-1)==8,"source still exists");close(fd);check(!strcmp(actual,"imported"),"import and buffer write preserve source");
    write_file(source,"external source change");failure("{\"action\":\"save\",\"name\":\"filebuf\"}","source_conflict");
    write_file(source,"imported");success("{\"action\":\"save\",\"name\":\"filebuf\"}");
    memset(actual,0,sizeof(actual));fd=open(source,O_RDONLY);check(fd>=0&&read(fd,actual,sizeof(actual)-1)==13,"read saved source");close(fd);check(!strcmp(actual,"saved content"),"save writes explicit source");
    write_file(alternate,"existing target");request(&b,"save","filebuf","path",alternate,NULL,NULL);failure(b.data,"source_conflict");jbuf_free(&b);
    sha256_hex((const uint8_t*)"existing target",15,expected);jbuf_init(&b,256);jbuf_append(&b,"{\"action\":\"save\",\"name\":\"filebuf\",\"path\":");jbuf_append_json_str(&b,alternate);jbuf_append(&b,",\"expected_source_revision\":");jbuf_append_json_str(&b,expected);jbuf_append_char(&b,'}');success(b.data);jbuf_free(&b);
    request(&b,"save","filebuf","path",path,NULL,NULL);failure(b.data,"forbidden_target");jbuf_free(&b);
    char prohibited[PATH_MAX];snprintf(prohibited,sizeof(prohibited),"%s/test.keychain-db",fixture);request(&b,"save","filebuf","path",prohibited,NULL,NULL);failure(b.data,"forbidden_target");jbuf_free(&b);check(lstat(prohibited,&st)&&errno==ENOENT,"no keychain target written");
    check(buffer_store_sensitive("{\"action\":\"create\",\"kind\":\"file\",\"name\":\"env\",\"source_path\":\"/tmp/.env\"}"),"credential source path classified before import");
    char bad[]={ 'a',(char)0xc0,(char)0xaf,0};write_file(path,bad);failure("{\"action\":\"read\",\"name\":\"renamed\"}","invalid_utf8");write_file(path,"restored");
    char tiny[80];check(!tool_buffer("{\"action\":\"inspect\",\"name\":\"renamed\"}",tiny,sizeof(tiny)),"small output refuses truncation");yyjson_doc *tiny_doc=yyjson_read(tiny,strlen(tiny),0);check(tiny_doc!=NULL,"small output still valid JSON");yyjson_doc_free(tiny_doc);

    char *large=malloc(1048577);check(large!=NULL,"allocate maximum content");memset(large,'x',1048576);large[1048576]=0;
    request(&b,"create","max buffer","content",large,NULL,NULL);success(b.data);jbuf_free(&b);
    check(yyjson_get_uint(yyjson_obj_get(buffer(),"bytes"))==1048576,"maximum 1 MiB content accepted");
    failure("{\"action\":\"append\",\"name\":\"max buffer\",\"content\":\"x\"}","content_too_large");
    success("{\"action\":\"read\",\"name\":\"max buffer\",\"max_bytes\":32768}");check(yyjson_get_uint(yyjson_obj_get(root(),"bytes"))==32768&&yyjson_get_bool(yyjson_obj_get(root(),"truncated")),"large reads are bounded and paginated");
    memset(large,'\n',1048576);request(&b,"create","escaped max","content",large,NULL,NULL);success(b.data);jbuf_free(&b);free(large);
    check(yyjson_get_uint(yyjson_obj_get(buffer(),"bytes"))==1048576,"encoded JSON expansion does not reduce content limit");

    exec_sql("UPDATE buffers SET sensitive='oops' WHERE name='renamed'");
    check(buffer_store_sensitive("{\"action\":\"inspect\",\"name\":\"renamed\"}"),"invalid selected metadata fails closed");
    failure("{\"action\":\"inspect\",\"name\":\"renamed\"}","metadata_corrupt");
    exec_sql("PRAGMA user_version=2");failure("{\"action\":\"list\"}","metadata_corrupt");check(buffer_store_sensitive("{\"action\":\"list\"}"),"corrupt listing fails closed, never mistaken for missing store");
    if(response)yyjson_doc_free(response);
    check(!nftw(fixture,cleanup,16,FTW_DEPTH|FTW_PHYS),"remove only dedicated test fixture");
    printf("buffer_store: %u checks passed\n",checks);return 0;
}
