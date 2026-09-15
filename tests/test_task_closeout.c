/* Real closeout + conversation mechanics; only registry/gate/journal are fakes.
 * The gate fake verifies routing/tier preservation, not real authority policy. */
#include "task_closeout.h"
#include "tools.h"
#include "json_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const tool_def_t defs[] = {{.name="held"}, {.name="temporary"}};
static bool loaded[2], external[2], denied, journal_ok = true;
static int calls, appends, overflow;
const tool_def_t *tools_get_all(int *n) { *n=2; return defs; }
int tools_loaded_builtin_count(void) { return overflow ? 129 : loaded[0]+loaded[1]; }
int tools_loaded_builtin_indices(int *out, int max) {
    int n=0; for(int i=0;i<2 && n<max;i++) if(loaded[i]) out[n++]=i; return n;
}
int tools_loaded_external_count(void) { return external[0]+external[1]; }
bool tools_is_builtin_loaded(const char *name) {
    for(int i=0;i<2;i++) if(!strcmp(name,defs[i].name)) return loaded[i]; return false;
}
bool tools_is_external_loaded(const char *name) {
    return !strcmp(name,"held-external") ? external[0] :
           !strcmp(name,"temporary-external") ? external[1] : false;
}
external_tool_snapshot_t tools_external_snapshot(void) {
    external_tool_snapshot_t s={.items=calloc(2,sizeof(external_tool_t)),.count=2}; assert(s.items);
    strcpy(s.items[0].name,"held-external"); strcpy(s.items[1].name,"temporary-external");
    for(int i=0;i<2;i++) s.items[i].loaded=external[i]; return s;
}
void tools_external_snapshot_free(external_tool_snapshot_t *s) { free(s->items); }
bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *out,size_t len) {
    assert(!strcmp(name,"evict_tools") && !strcmp(tier,"trusted")); calls++;
    assert(!strstr(input,"all"));
    if(denied) { snprintf(out,len,"{\"error\":\"denied\"}"); return false; }
    if(strstr(input,"temporary-external")) external[1]=false;
    else { assert(strstr(input,"temporary")); loaded[1]=false; }
    snprintf(out,len,"{\"evicted\":1}"); return true;
}
const char *chronicle_run_id(void) { return "test-run"; }
bool chronicle_journal_append(const char *kind,const char *payload,bool durable) {
    assert(!strcmp(kind,"task.closeout.v1") && durable); appends++;
    assert(!strstr(payload,"held-external") && !strstr(payload,"temporary"));
    assert(strstr(payload,"\"training_eligible\":false")); return journal_ok;
}
static void reset(void) {
    loaded[0]=external[0]=true; loaded[1]=external[1]=false;
    denied=false; journal_ok=true; calls=appends=overflow=0; unsetenv("DSCO_TASK_CLOSEOUT");
}
static conversation_t fixture(int count) {
    conversation_t c={0}; c.count=count; c.msgs=calloc((size_t)count,sizeof(message_t)); assert(c.msgs);
    for(int i=0;i<count;i++) {
        message_t *m=&c.msgs[i]; m->role=ROLE_USER; m->content_count=1;
        m->content=calloc(1,sizeof(msg_content_t)); assert(m->content);
        m->content[0].type=strdup("text"); m->content[0].text=strdup("retain user constraint");
    }
    c.msgs[1].role=ROLE_ASSISTANT;
    free(c.msgs[1].content[0].type); c.msgs[1].content[0].type=strdup("tool_use");
    c.msgs[1].content[0].tool_name=strdup("generic");
    c.msgs[1].content[0].tool_id=strdup("test-id");
    c.msgs[1].content[0].tool_input=strdup("{}");
    free(c.msgs[2].content[0].type); c.msgs[2].content[0].type=strdup("tool_result");
    c.msgs[2].content[0].tool_name=strdup("generic");
    c.msgs[2].content[0].tool_id=strdup("test-id");
    free(c.msgs[2].content[0].text);
    char *text=malloc(2100); assert(text); memset(text,'x',2000); text[2000]=0;
    memcpy(text,"artifact.txt\n",13); strcat(text,"\n[key=ck:test:recoverable]");
    c.msgs[2].content[0].text=text;
    return c;
}
int main(void) {
    char report[2048]; task_closeout_t s;
    reset(); task_closeout_begin(&s); loaded[1]=external[1]=true;
    assert(task_closeout_finish(&s,"trusted",true,0,report,sizeof(report)));
    assert(calls==2 && loaded[0] && external[0] && !loaded[1] && !external[1]);
    assert(strstr(report,"\"schemas_evicted\":2") && appends==1);
    assert(task_closeout_finish(&s,"trusted",true,0,report,sizeof(report)) && calls==2 && appends==1);
    for(int mode=0;mode<2;mode++) {
        reset(); task_closeout_begin(&s); loaded[1]=true;
        assert(task_closeout_finish(&s,"trusted",mode==1,mode,report,sizeof(report)));
        assert(calls==0 && loaded[1] && strstr(report,"deferred"));
    }
    reset(); task_closeout_begin(&s); loaded[1]=true; denied=true;
    assert(!task_closeout_finish(&s,"trusted",true,0,report,sizeof(report)) && calls==1 && loaded[1]);
    reset(); task_closeout_begin(&s); loaded[1]=true; journal_ok=false;
    assert(!task_closeout_finish(&s,"trusted",true,0,report,sizeof(report)) && !loaded[1]);
    reset(); overflow=1; task_closeout_begin(&s);
    assert(!task_closeout_finish(&s,"trusted",true,0,report,sizeof(report)) && calls==0);
    reset(); setenv("DSCO_TASK_CLOSEOUT","0",1); task_closeout_begin(&s); loaded[1]=true;
    assert(task_closeout_finish(&s,"trusted",true,0,report,sizeof(report)) && calls==0 && appends==0);
    unsetenv("DSCO_TASK_CLOSEOUT");

    conversation_t c=fixture(9);
    assert(task_closeout_compact(&c,"{\"aggressive\":false,\"keep_recent\":2}",report,sizeof(report)));
    assert(!strcmp(c.msgs[1].content[0].type,"tool_use") && strstr(report,"\"aggressive\":false"));
    assert(strstr(report,"\"changed\":true") && conv_validate_tool_call_integrity(&c,false).ok);
    assert(task_closeout_compact(&c,"{\"aggressive\":true,\"keep_recent\":2}",report,sizeof(report)));
    assert(!strcmp(c.msgs[1].content[0].type,"text") && strstr(report,"\"tool_turns_compacted\":1"));
    assert(!strcmp(c.msgs[8].content[0].text,"retain user constraint")); conv_free(&c);

    c=fixture(3);
    assert(task_closeout_compact(&c,"{\"aggressive\":true,\"keep_recent\":2}",report,sizeof(report)));
    assert(strstr(report,"\"changed\":false") && !strcmp(c.msgs[1].content[0].type,"tool_use"));
    const char *bad[]={"{", "[]", "{\"aggressive\":\"false\"}", "{\"keep_recent\":null}",
        "{\"keep_recent\":-1}","{\"max_result_chars\":1000001}","{\"max_result_chars\":1.5}",
        "{\"aggressive\":false,\"aggressive\":true}","{\"extra\":1}","{\"keep_recent\":true}"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        assert(!task_closeout_compact(&c,bad[i],report,sizeof(report)));
        assert(!strcmp(c.msgs[1].content[0].type,"tool_use"));
    }
    assert(!task_closeout_compact(NULL,"{}",report,sizeof(report)));
    assert(!task_closeout_compact(&c,"{}",report,16)); conv_free(&c);
    int budgets[]={100,384,800};
    for(size_t i=0;i<sizeof(budgets)/sizeof(*budgets);i++) {
        c=fixture(9); conv_trim_old_results(&c,2,budgets[i]);
        const char *text=c.msgs[2].content[0].text;
        assert(strlen(text)<=(size_t)budgets[i] && strstr(text,"[key=ck:test:recoverable]"));
        char *once=strdup(text);
        for(int j=0;j<5;j++) conv_trim_old_results(&c,2,budgets[i]);
        assert(!strcmp(once,c.msgs[2].content[0].text)); free(once); conv_free(&c);
    }
    puts("PASS: scoped builtin/external eviction, preserved leases, deferral, denial, journal failure, overflow, opt-out, idempotence; typed compaction, tail integrity, bounded stable trimming and recovery keys");
}
