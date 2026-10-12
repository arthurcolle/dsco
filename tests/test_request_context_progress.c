/* Exercise the real admission loop and archiver with an injectable store and
 * request envelope. Only the unused fallback builder is renamed: no network. */
#define llm_build_request_ex_for_credential fixture_unused_builder
#include "../src/request_context.c"
#undef llm_build_request_ex_for_credential
#include "context_eviction.h"
#include "context_fabric.h"
#include "jev_compaction.h"
#include <assert.h>

static conversation_t *active;
static char *saved;
static char admission[2048];
static int steps, builds, stored_count, failure_at;
char *fixture_unused_builder(conversation_t *c,session_state_t *s,int n,const char *k) {
    (void)c;(void)s;(void)n;(void)k;assert(0);return NULL;
}
void tools_set_active_conversation(void *c) { active=c; }
void tools_set_active_session(void *s) { (void)s; }
void tools_set_request_budget(int b,int a,int l) { assert(b>=a && a<=l); }
bool tools_execute_for_tier(const char *name,const char *args,const char *tier,char *out,size_t len) {
    assert(!strcmp(name,"context_evict") && !strcmp(tier,"trusted")); steps++;
    return context_evict(active,args,out,len);
}
bool chronicle_event(const char *type,const char *trace,const char *span,const char *parent,
                     const char *actor,const char *id,const char *payload,const char *sensitivity) {
    (void)trace;(void)span;(void)parent;(void)actor;(void)id;(void)sensitivity;
    if (!strcmp(type,"request.input_budget")) snprintf(admission,sizeof(admission),"%s",payload);
    return true;
}
void jev_compaction_shadow(const jev_compaction_candidate_t *items,size_t n,char *out,size_t len) {
    (void)items;(void)n;(void)out;(void)len;assert(0);
}
ctx_broker_t *ctx_broker_default(void) { return (ctx_broker_t *)1; }
bool ctx_put(ctx_broker_t *b,const void *p,size_t n,const ctx_put_opts_t *o,ctxkey_t *key,bool *d) {
    (void)b;(void)d; stored_count++;
    if (failure_at && stored_count>=failure_at) return false;
    memset(key,0,sizeof(*key));key->kind=o->kind;snprintf(key->id,sizeof(key->id),"fixture-%d",stored_count);
    free(saved);saved=strndup(p,n);return saved!=NULL;
}
int ctxkey_format(const ctxkey_t *k,char *out,size_t len) { return snprintf(out,len,"ck:msg:%s",k->id); }
char *ctx_get(ctx_broker_t *b,const ctxkey_t *key,size_t *n) { (void)b;(void)key;*n=strlen(saved);return strdup(saved); }
bool ctxkey_parse(const char *s,ctxkey_t *key) { (void)s;(void)key;return false; }
static char *build(provider_t *p,conversation_t *c,session_state_t *s,int n,const char *k) {
    (void)p;(void)s;(void)n;(void)k; builds++;
    jbuf_t out;jbuf_init(&out,4096);jbuf_append(&out,"{\"instructions\":\"FIXED_SYSTEM\",\"tools\":[{\"name\":\"FIXED_TOOL\"}],\"messages\":[");
    for (int i=0;i<c->count;i++) {
        if(i)jbuf_append(&out,",");
        jbuf_appendf(&out,"{\"role\":\"%s\",\"content\":",c->msgs[i].role==ROLE_USER?"user":"assistant");
        jbuf_append_json_str(&out,c->msgs[i].content[0].text);jbuf_append(&out,"}");
    }
    jbuf_append(&out,"]}");return out.data;
}
static conversation_t seed(void) {
    conversation_t c;conv_init(&c);conv_add_user_text(&c,"PRESERVED_OBJECTIVE");
    char text[7001];memset(text,'x',7000);text[7000]=0;
    for (int i=0;i<650;i++)conv_add_assistant_text(&c,text);
    conv_add_user_text(&c,"PRESERVED_LATEST");conv_add_assistant_text(&c,"PRESERVED_LATEST_EVIDENCE");return c;
}
int main(void) {
    unsetenv("DSCO_JEV_COMPACTION");setenv("DSCO_REQUEST_TARGET_BYTES","65536",1);
    setenv("DSCO_MAX_INPUT_TOKENS","2000000",1);
    session_state_t s={.compact_enabled=true,.context_window=2000000,.trust_tier=DSCO_TRUST_TRUSTED};
    provider_t p={.build_request=build};conversation_t c=seed();
    char *request=request_context_build(&p,&c,&s,"owned",256);assert(request);
    assert(steps==11 && builds==12 && stored_count==661);
    assert(json_get_int(admission,"archive_passes",0)==11);
    assert(json_get_bool(admission,"wire_target_satisfied",false));
    assert(strstr(request,"PRESERVED_OBJECTIVE") && strstr(request,"PRESERVED_LATEST_EVIDENCE"));
    assert(c.count==4 && strstr(c.msgs[1].content[0].text,"[Archived assistant response;"));
    assert(s.non_conv_overhead_tokens>=512);free(request);conv_free(&c);
    /* Partial storage failure rebuilds once but cannot retry another archive
     * batch or mutate the first unarchived response. */
    steps=builds=stored_count=0;failure_at=3;c=seed();
    request=request_context_build(&p,&c,&s,"owned",256);assert(request);
    assert(steps==1 && builds==2 && stored_count==3);
    assert(!json_get_bool(admission,"archive_ok",true));
    assert(strstr(admission,"archive_denied_or_failed"));
    assert(strlen(c.msgs[3].content[0].text)==7000);free(request);conv_free(&c);
    steps=builds=stored_count=0;failure_at=65;c=seed();
    request=request_context_build(&p,&c,&s,"owned",256);assert(request);
    assert(steps==1 && builds==2 && stored_count==65 && c.count==653);
    assert(strstr(c.msgs[1].content[0].text,"[Archived assistant response;"));
    assert(strlen(c.msgs[65].content[0].text)==7000);
    free(request);conv_free(&c);
    steps=builds=stored_count=0;failure_at=0;s.compact_enabled=false;c=seed();
    request=request_context_build(&p,&c,&s,"owned",256);assert(request);
    assert(steps==0 && builds==1 && stored_count==0 && strlen(c.msgs[1].content[0].text)==7000);
    assert(strstr(admission,"compaction_disabled"));free(request);conv_free(&c);
    free(saved);
    printf("PASS admission: 650 archives in 11 productive passes, actual-progress stop, partial failure rebuild without retry, overhead estimate\n");
}
