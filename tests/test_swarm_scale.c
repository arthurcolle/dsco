#include "swarm_scale.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int calls;
static bool allow=true, large_reply, partial_reply, valid_large_reply;
static char *forwarded;
/* A spy, not an alternative executor: production links the real tier gate. */
bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *result,size_t cap) {
    assert(!strcmp(name,"swarm") && !strcmp(tier,"sandbox")); calls++;
    free(forwarded);forwarded=strdup(input);
    if (partial_reply) snprintf(result,cap,"{\"group_id\":7,\"agents_spawned\":1,\"agent_ids\":[9]}");
    else if (valid_large_reply) {
        int n=snprintf(result,cap,"{\"group_id\":7,\"agent_ids\":[4,9],\"payload\":\"");
        assert(n>0 && cap>(size_t)n+3); memset(result+n,'x',cap-(size_t)n-3);
        result[cap-3]='"';result[cap-2]='}';result[cap-1]='\0';
    }
    else if (large_reply) { memset(result,'x',cap-1);result[cap-1]='\0'; }
    else snprintf(result,cap,allow ? "{\"group_id\":7,\"agents_spawned\":2,\"agent_ids\":[4,9]}" : "{\"error\":\"capability denied\"}");
    return allow;
}
static yyjson_doc *run(const char *s,bool expected) {
    static char out[131072];
    bool ok=swarm_scale_execute(s,"sandbox",512,out,sizeof(out));
    if(ok!=expected)fprintf(stderr,"%s\n",out);
    assert(ok==expected);yyjson_doc *d=yyjson_read(out,strlen(out),0);assert(d);return d;
}
static void bad(const char *s) { int before=calls;yyjson_doc *d=run(s,false);assert(calls==before);assert(yyjson_obj_get(yyjson_doc_get_root(d),"error"));yyjson_doc_free(d); }
static double now(void) { struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return t.tv_sec+t.tv_nsec/1e9; }
int main(void) {
    bad("{}");bad("{\"pure_tasks\":false,\"tasks\":[\"a\"]}");
    bad("{\"pure_tasks\":true,\"pure_tasks\":false,\"tasks\":[\"a\"]}");
    bad("{\"pure_tasks\":true,\"tasks\":[\"a\\u0000b\"]}");
    bad("{\"pure_tasks\":true,\"tasks\":[{\"task\":\"a\",\"task\":\"b\"}]}");
    bad("{\"pure_tasks\":true,\"tasks\":[{\"task\":\"a\",\"executor\":\"codex\"}]}");
    bad("{\"pure_tasks\":true,\"tasks\":[\"\"]}");
    bad("{\"pure_tasks\":true,\"tasks\":[42]}");
    bad("{\"pure_tasks\":true,\"tasks\":[],\"dry_run\":true}");
    bad("{\"pure_tasks\":true,\"tasks\":[\"a\"],\"coordinator\":\"sum\"}");
    bad("{\"pure_tasks\":true,\"tasks\":[\"a\"],\"dry_run\":1}");
    const char *request="{\"action\":\"scale\",\"pure_tasks\":true,\"dry_run\":true,\"budget\":0.25,\"system_prompt\":\"immutable input\",\"tasks\":[{\"task\":\"a\",\"provider\":\"openai-codex\",\"model\":\"m1\"},{\"task\":\"a\",\"provider\":\"openai-codex\",\"model\":\"m1\"},{\"task\":\"a\",\"provider\":\"openai-codex\",\"model\":\"m2\"},\"a\",\"a\"]}";
    yyjson_doc *d=run(request,true);yyjson_val *r=yyjson_doc_get_root(d);
    assert(yyjson_get_uint(yyjson_obj_get(r,"logical_tasks"))==5);
    assert(yyjson_get_uint(yyjson_obj_get(r,"unique_tasks"))==3);
    yyjson_val *aliases=yyjson_obj_get(r,"logical_to_unique_task");int expected[]={0,0,1,2,2};
    for(int i=0;i<5;i++)assert(yyjson_get_uint(yyjson_arr_get(aliases,i))==(unsigned)expected[i]);
    yyjson_val *plan=yyjson_obj_get(r,"planned_request");assert(!strcmp(yyjson_get_str(yyjson_obj_get(plan,"action")),"create"));
    assert(yyjson_get_real(yyjson_obj_get(plan,"budget"))==.25);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(plan,"system_prompt")),"immutable input"));
    assert(!yyjson_obj_get(plan,"pure_tasks") && !yyjson_obj_get(plan,"dry_run"));
    assert(yyjson_arr_size(yyjson_obj_get(plan,"tasks"))==3 && calls==0);yyjson_doc_free(d);
    const char *execute="{\"pure_tasks\":true,\"tasks\":[\"a\",\"a\",\"b\"]}";
    d=run(execute,true);assert(calls==1);
    yyjson_val *refs=yyjson_obj_get(yyjson_doc_get_root(d),"logical_worker_ids");
    assert(yyjson_arr_size(refs)==3 && yyjson_get_sint(yyjson_arr_get(refs,0))==4 && yyjson_get_sint(yyjson_arr_get(refs,1))==4 && yyjson_get_sint(yyjson_arr_get(refs,2))==9);
    yyjson_doc_free(d);
    yyjson_doc *sent=yyjson_read(forwarded,strlen(forwarded),0);assert(sent);
    assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(sent),"tasks"))==2);yyjson_doc_free(sent);
    allow=false;d=run(execute,false);assert(calls==2);yyjson_doc_free(d);allow=true;
    char small[64];int before=calls;
    assert(!swarm_scale_execute(execute,"sandbox",512,small,sizeof(small)) && calls==before);
    char out[8192];assert(!swarm_scale_execute(execute,"sandbox",1,out,sizeof(out)) && calls==before);
    large_reply=true;assert(swarm_scale_execute(execute,"sandbox",512,out,sizeof(out)) && calls==before+1);
    d=yyjson_read(out,strlen(out),0);assert(d && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d),"execution_response_omitted")));yyjson_doc_free(d);large_reply=false;
    partial_reply=true; d=run(execute,true);
    assert(yyjson_is_null(yyjson_obj_get(yyjson_doc_get_root(d),"logical_worker_ids")));
    assert(yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d),"result_reconciliation_required")));
    yyjson_doc_free(d);partial_reply=false;
    valid_large_reply=true;assert(swarm_scale_execute(execute,"sandbox",512,out,sizeof(out)));
    d=yyjson_read(out,strlen(out),0);assert(d);r=yyjson_doc_get_root(d);
    assert(yyjson_get_sint(yyjson_obj_get(r,"group_id"))==7 && yyjson_is_true(yyjson_obj_get(r,"reconcile_before_retry")));
    assert(yyjson_arr_size(yyjson_obj_get(r,"logical_worker_ids"))==3);
    yyjson_doc_free(d);valid_large_reply=false;
    /* A family with U=sqrt(N) unique jobs: exact dispatch-count savings, not a bill. */
    const int sizes[]={16,64,256,1024,4096};const int roots[]={4,8,16,32,64};
    for(int k=0;k<5;k++) {
        jbuf_t b;jbuf_init(&b,1024);jbuf_append(&b,"{\"pure_tasks\":true,\"dry_run\":true,\"tasks\":[");
        for(int i=0;i<sizes[k];i++)jbuf_appendf(&b,"%s\"immutable job %d\"",i?",":"",i%roots[k]);
        jbuf_append(&b,"]}");double start=now();d=run(b.data,true);double elapsed=(now()-start)*1000;
        r=yyjson_doc_get_root(d);assert(yyjson_get_uint(yyjson_obj_get(r,"unique_tasks"))==(unsigned)roots[k]);
        assert(yyjson_get_uint(yyjson_obj_get(r,"reused_tasks"))==(unsigned)(sizes[k]-roots[k]));
        printf("{\"logical_tasks\":%d,\"unique_dispatches\":%d,\"dispatch_ratio\":%.6f,\"plan_ms\":%.3f,\"billed_cost\":null}\n",sizes[k],roots[k],(double)roots[k]/sizes[k],elapsed);
        yyjson_doc_free(d);jbuf_free(&b);
    }
    /* Unique tasks cannot be compressed: preserve the linear floor. */
    d=run("{\"pure_tasks\":true,\"dry_run\":true,\"tasks\":[\"a\",\"b\",\"c\",\"d\"]}",true);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(d),"unique_tasks"))==4);yyjson_doc_free(d);
    free(forwarded);puts("PASS: exact reuse, pin preservation, gated dispatch, no preflight side effects, bounded receipts, no invented unique-work savings");return 0;
}
