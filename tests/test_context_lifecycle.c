/* Offline lifecycle proof with the real broker, VFS, archive, admission and FTS. */
#include "../src/context_fabric.c"
#include "context_archive.h"
#include "input_budget.h"
#include "config.h"
#include "jev_compaction.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <unistd.h>
#include <time.h>
const model_info_t *openrouter_cache_lookup(const char *name){(void)name;return NULL;}
const model_info_t *codex_cache_lookup(const char *name){(void)name;return NULL;}
void jev_compaction_shadow(const jev_compaction_candidate_t *items,size_t n,char *out,size_t len){(void)items;(void)n;snprintf(out,len,"{}");}
bool chronicle_event(const char *a,const char *b,const char *c,const char *d,const char *e,const char *f,const char *g,const char *h){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return false;}
static bool embedding_fixture;
static int embedding_calls;
static ctx_broker_t *embedding_lock_fixture;
float *tools_embed_text(const char *text,int *dim){
    if(embedding_lock_fixture){assert(!pthread_mutex_trylock(&embedding_lock_fixture->mu));pthread_mutex_unlock(&embedding_lock_fixture->mu);}
    if(!embedding_fixture){*dim=0;return NULL;}
    embedding_calls++;*dim=2;float *v=malloc(2*sizeof(float));assert(v);
    v[0]=1;v[1]=strstr(text,"tailneedle_unique_fact")?1:0;return v;
}
static void ready(ctx_broker_t *b) {
    for(int i=0;i<5000;i++) {
        int pending,failed;ctx_search_coverage(b,&pending,&failed);
        if(!pending){assert(!failed);return;}
        struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    assert(!"historical full-text migration timed out");
}
static void pages(ctx_broker_t *b,ctxkey_t key,const char *original,size_t start,size_t end) {
    char page[512];size_t cursor=start;int count=0;
    for(;;) {
        assert(context_archive_page(b,&key,page,sizeof(page)));
        char *body=strchr(page,'\n');assert(body);*body++=0;
        yyjson_doc *d=yyjson_read(page,strlen(page),0);assert(d);yyjson_val *r=yyjson_doc_get_root(d);
        yyjson_val *range=yyjson_obj_get(r,"range");
        size_t a=(size_t)yyjson_get_uint(yyjson_arr_get(range,0)),z=(size_t)yyjson_get_uint(yyjson_arr_get(range,1));
        assert(a==cursor && z>=a && z<=end);
        assert(yyjson_get_uint(yyjson_obj_get(r,"requested_end"))==end);
        const char *encoding=yyjson_get_str(yyjson_obj_get(r,"encoding"));
        if(!strcmp(encoding,"utf-8")){assert(strlen(body)==z-a && !memcmp(body,original+a,z-a));}
        else {unsigned char decoded[512];size_t n=base64_decode(body,strlen(body),decoded,sizeof(decoded));assert(n==z-a && !memcmp(decoded,original+a,n));}
        cursor=z;++count;
        if(yyjson_get_bool(yyjson_obj_get(r,"complete"))){yyjson_doc_free(d);break;}
        assert(z>a);assert(ctxkey_parse(yyjson_get_str(yyjson_obj_get(r,"next_key")),&key));
        yyjson_doc_free(d);assert(count<1000);
    }
    assert(cursor==end);
}
int main(void) {
    char dir[]="/tmp/dsco-lifecycle-XXXXXX";assert(mkdtemp(dir));
    char path[1024],other[1024];snprintf(path,sizeof(path),"%s/store.db",dir);
    snprintf(other,sizeof(other),"%s/unrelated.db",dir);setenv("DSCO_CONTEXT_DB",other,1);
    ctx_broker_t *b=ctx_broker_open(path);assert(b);ready(b);
    /* Facts at the tail must remain searchable, not just recoverable by a known key. */
    size_t n=30000;char *text=malloc(n+1);assert(text);memset(text,' ',n);text[n]=0;
    memcpy(text,"HEAD_EVIDENCE",13);memcpy(text+20000,"tailneedle_unique_fact",22);
    embedding_fixture=true;int dim=0;float *all=ctx_embed_fulltext(text,n,&dim);
    assert(all && dim==2 && embedding_calls==4 && all[1]>0);free(all);embedding_fixture=false;
    ctx_put_opts_t opts={.kind=CTX_KIND_TOOL,.source="fixture",.embed=0};ctxkey_t key;bool dedup;
    assert(ctx_put(b,text,n,&opts,&key,&dedup) && !dedup);
    ctx_search_hit_t hits[8];assert(ctx_search(b,"tailneedle_unique_fact",-1,8,hits)==1);
    assert(!strcmp(hits[0].key.id,key.id));pages(b,key,text,0,n);
    embedding_lock_fixture=b;assert(ctx_search(b,"tailneedle_unique_fact",-1,8,hits)==1);embedding_lock_fixture=NULL;
    sqlite3 *saved_fts=b->fts;b->fts=NULL;
    assert(ctx_search(b,"tailneedle_unique_fact",-1,8,hits)==1);
    for(int i=0;i<1100;i++) {
        char cid[128];snprintf(cid,sizeof(cid),"fallback-%04d",i);
        assert(vfs_kv_put_str(b->vfs,CTX_BUCKET_META,cid,"{\"kind\":3,\"preview\":\"\"}"));
        assert(vfs_kv_put_str(b->vfs,CTX_BUCKET_BLOB,cid,"unrelated evidence"));
    }
    int key_count=0;char **fallback_keys=vfs_kv_keys(b->vfs,CTX_BUCKET_META,&key_count);assert(key_count>1024);
    const char *last=fallback_keys[key_count-1];
    assert(vfs_kv_put_str(b->vfs,CTX_BUCKET_BLOB,last,"fallback_tail_beyond_old_scan_cap"));
    assert(ctx_search(b,"fallback_tail_beyond_old_scan_cap",-1,8,hits)==1);
    for(int i=0;i<key_count;i++){assert(vfs_kv_delete(b->vfs,CTX_BUCKET_META,fallback_keys[i]));assert(vfs_kv_delete(b->vfs,CTX_BUCKET_BLOB,fallback_keys[i]));free(fallback_keys[i]);}free(fallback_keys);
    b->fts=saved_fts;assert(ctx_put(b,text,n,&opts,&key,&dedup));
    key.slice_kind=CTX_SLICE_BYTES;key.slice_start=19000;key.slice_end=21000;pages(b,key,text,19000,21000);
    key.slice_kind=CTX_SLICE_NONE;
    /* Metadata-only and corrupted dedup hits must be repaired, never blessed. */
    assert(vfs_kv_delete(b->vfs,CTX_BUCKET_BLOB,key.id));assert(!ctx_get(b,&key,NULL));
    assert(ctx_put(b,text,n,&opts,&key,&dedup) && !dedup);
    assert(vfs_kv_put_str(b->vfs,CTX_BUCKET_BLOB,key.id,"CORRUPTION"));assert(!ctx_get(b,&key,NULL));
    assert(ctx_put(b,text,n,&opts,&key,&dedup) && !dedup);
    assert(ctx_put(b,text,n,&opts,&key,&dedup) && dedup);
    /* Simulate the pre-repair prefix index, then restart. */
    assert(sqlite3_exec(b->fts,"UPDATE ctx_fts SET body='HEAD_EVIDENCE';DELETE FROM ctx_fts_full",NULL,NULL,NULL)==SQLITE_OK);
    ctx_broker_close(b);b=ctx_broker_open(path);assert(b);ready(b);
    assert(ctx_search(b,"tailneedle_unique_fact",-1,8,hits)==1);pages(b,key,text,0,n);
    char fts_path[1200];snprintf(fts_path,sizeof(fts_path),"%s.fts",path);assert(!access(fts_path,F_OK));
    snprintf(fts_path,sizeof(fts_path),"%s.fts",other);assert(access(fts_path,F_OK));
    /* Strict parsing never converts a malformed range to an unbounded load. */
    const char *bad[]={"ck:tool:x#q:0-2","ck:tool:x#b:5-3","ck:tool:x#b:-1-2","ck:tool:x#l:0-1","ck:tool:x#b:0-2junk","ck:tool:x#b:0","ck:tool:x#b:999999999999999999999999999-", "ck:tool:x#"};
    ctxkey_t parsed;for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!ctxkey_parse(bad[i],&parsed));
    assert(ctxkey_parse("ck:tool:x#b:4-",&parsed) && parsed.slice_end==-1);
    assert(ctxkey_parse("ck:tool:x#b:4--1",&parsed) && parsed.slice_end==-1);
    /* Unicode and arbitrary bytes are recoverable through every page. */
    char unicode[3001];for(int i=0;i<1000;i++)memcpy(unicode+i*3,"雪",3);unicode[3000]=0;
    assert(ctx_put(b,unicode,3000,&opts,&parsed,NULL));pages(b,parsed,unicode,0,3000);
    parsed.slice_kind=CTX_SLICE_BYTES;parsed.slice_start=1;parsed.slice_end=20;pages(b,parsed,unicode,1,20);
    char binary[2000];for(int i=0;i<2000;i++)binary[i]=(char)(i%256);
    assert(ctx_put(b,binary,sizeof(binary),&opts,&parsed,NULL));pages(b,parsed,binary,0,sizeof(binary));
    char lines[2001];for(int i=0;i<1000;i++)memcpy(lines+i*2,"x\n",2);lines[2000]=0;
    assert(ctx_put(b,lines,2000,&opts,&parsed,NULL));parsed.slice_kind=CTX_SLICE_LINES;parsed.slice_start=50;parsed.slice_end=500;
    pages(b,parsed,lines,98,1000);
    free(text);ctx_broker_close(b);
    /* Actual admission projection stores all bytes in the persistent broker. */
    setenv("DSCO_CONTEXT_DB",path,1);setenv("DSCO_MAX_INPUT_TOKENS","3000",1);
    text=malloc(60001);assert(text);memset(text,'x',60000);text[60000]=0;
    jbuf_t request;jbuf_init(&request,61000);jbuf_append(&request,"{\"input\":[{\"role\":\"user\",\"content\":\"KEEP_OBJECTIVE\"},{\"type\":\"function_call_output\",\"call_id\":\"owned\",\"output\":");jbuf_append_json_str(&request,text);jbuf_append(&request,"}]}");
    input_budget_result_t admission;assert(input_budget_apply(&request.data,200000,256,&admission));
    yyjson_doc *d=yyjson_read(request.data,strlen(request.data),0);assert(d);
    const char *receipt=yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(d),"input"),1),"output"));
    const char *begin=strstr(receipt,"key=");assert(begin);begin+=4;const char *finish=strchr(begin,';');assert(finish);
    char handle[CTX_KEY_STR_MAX];assert((size_t)(finish-begin)<sizeof(handle));memcpy(handle,begin,(size_t)(finish-begin));handle[finish-begin]=0;
    assert(ctxkey_parse(handle,&parsed));size_t size=0;char *stored=ctx_get(ctx_broker_default(),&parsed,&size);
    assert(stored && size==60000 && !memcmp(stored,text,size));free(stored);free(text);yyjson_doc_free(d);jbuf_free(&request);
    /* Legacy trimming/media APIs preserve full blocks, including all media fields. */
    conversation_t conv;conv_init(&conv);conv_add_user_text(&conv,"KEEP_ROOT_OBJECTIVE");
    conv_add_assistant_tool_use(&conv,"legacy","read_file","{}");
    char old[12001];memset(old,'z',12000);old[12000]=0;
    conv_add_tool_result(&conv,"legacy",old,false);
    conv_add_user_text(&conv,"KEEP_LATEST");conv_add_assistant_text(&conv,"RECENT");
    conv_trim_old_results(&conv,2,16);
    assert(strstr(conv.msgs[2].content[0].text,"key=ck:msg:") && conv_validate_tool_call_integrity(&conv,false).ok);
    char *once=strdup(conv.msgs[2].content[0].text);
    conv_trim_old_results(&conv,2,16);assert(!strcmp(once,conv.msgs[2].content[0].text));free(once);conv_free(&conv);
    conv_init(&conv);conv_add_user_text(&conv,"KEEP_IMAGE_INSTRUCTION");conv_add_user_text(&conv,"media");
    msg_content_t *media=&conv.msgs[1].content[0];free(media->type);media->type=strdup("image");
    media->image_data=strdup("ALL_IMAGE_BYTES");media->image_media_type=strdup("image/png");media->image_url=strdup("image-fixture");
    conv_add_user_text(&conv,"KEEP_LATEST");conv_add_assistant_text(&conv,"RECENT");
    conv_strip_binaries(&conv,2);assert(!strcmp(conv.msgs[0].content[0].text,"KEEP_IMAGE_INSTRUCTION"));
    assert(!strcmp(media->type,"text") && strstr(media->text,"Verified context archive"));
    begin=strstr(media->text,"key=")+4;finish=strchr(begin,';');memcpy(handle,begin,(size_t)(finish-begin));handle[finish-begin]=0;
    assert(ctxkey_parse(handle,&parsed));stored=ctx_get(ctx_broker_default(),&parsed,&size);
    assert(stored && strstr(stored,"ALL_IMAGE_BYTES") && strstr(stored,"image/png") && strstr(stored,"image-fixture"));free(stored);conv_free(&conv);
    post_compact_restore_t restore;post_compact_restore_init(&restore);
    post_compact_restore_track(&restore,"fixture.txt",old);
    assert(strlen(restore.files[0].content)==12000 && restore.total_tokens==restore.files[0].tokens);
    post_compact_restore_track(&restore,"fixture.txt","fresh");
    assert(restore.total_tokens==restore.files[0].tokens && !strcmp(restore.files[0].content,"fresh"));post_compact_restore_free(&restore);
    /* The older automatic tier must preserve human instructions and recover
     * when the former circuit-breaker failure count has been reached. */
    setenv("DSCO_MAX_INPUT_TOKENS","auto",1);setenv("DSCO_MAX_TOKENS","256",1);
    unsetenv("DSCO_JEV_COMPACTION");
    conv_init(&conv);conv_add_user_text(&conv,"KEEP_AUTO_ROOT");
    char *big=malloc(60001);assert(big);memset(big,'a',60000);big[60000]=0;
    conv_add_assistant_text(&conv,big);conv_add_assistant_text(&conv,big);free(big);
    conv_add_user_text(&conv,"KEEP_AUTO_CORRECTION");conv_add_assistant_text(&conv,"KEEP_AUTO_RECENT");
    session_state_t session={.compact_enabled=true,.context_window=20000,
        .turn_count=483,.user_turn_count=2};strcpy(session.model,"local/fixture");
    compact_config_t config;compact_config_init(&config);config.snip_keep_tail=2;
    config.consecutive_failures=config.max_failures;
    compact_result_t compact=conv_auto_compact(&conv,&session,&config);
    assert(compact.post_token_count<compact.pre_token_count && config.consecutive_failures==0);
    assert(!strcmp(conv.msgs[0].content[0].text,"KEEP_AUTO_ROOT"));
    assert(!strcmp(conv.msgs[conv.count-2].content[0].text,"KEEP_AUTO_CORRECTION"));
    assert(!strcmp(conv.msgs[conv.count-1].content[0].text,"KEEP_AUTO_RECENT"));
    assert(session.turn_count==483 && session.user_turn_count==2);
    char session_path[4096];snprintf(session_path,sizeof(session_path),"%s.session",path);
    assert(conv_save_ex(&conv,&session,session_path));
    conversation_t loaded;conv_init(&loaded);session_state_t resumed={0};
    assert(conv_load_ex(&loaded,&resumed,session_path));
    assert(resumed.turn_count==483 && resumed.user_turn_count==2);
    conv_free(&loaded);
    /* Explicit zero survives load; legacy files never reuse a prior slot's ordinal. */
    session.user_turn_count=0;assert(conv_save_ex(&conv,&session,session_path));
    resumed.user_turn_count=99;conv_init(&loaded);
    assert(conv_load_ex(&loaded,&resumed,session_path) && resumed.user_turn_count==0);
    conv_free(&loaded);
    FILE *legacy=fopen(session_path,"w");assert(legacy);
    fputs("{\"session\":{\"turn_count\":483},\"messages\":[]}",legacy);fclose(legacy);
    resumed.user_turn_count=99;conv_init(&loaded);
    assert(conv_load_ex(&loaded,&resumed,session_path));
    assert(resumed.turn_count==483 && resumed.user_turn_count==0);
    conv_free(&loaded);unlink(session_path);conv_free(&conv);
    char actual_path[4096],expected_path[4096];
    assert(realpath(vfs_database_path(ctx_broker_default()->vfs),actual_path) && realpath(path,expected_path));
    assert(!strcmp(actual_path,expected_path));
    ctx_broker_shutdown();
    puts("PASS lifecycle: full-tail search, precise paging, requested range bounds, Unicode/binary roundtrip, strict key rejection, stale-index restart migration, broker path isolation, damaged dedup repair, admission byte preservation");
    return 0;
}
