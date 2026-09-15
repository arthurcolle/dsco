#include "execution_layer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int calls, checks;
static bool tool_ok = true, input_ok = true;
void uuid_v4(char *out) { strcpy(out, "00000000-0000-4000-8000-000000000001"); }
void baseline_log(const char *a,const char *b,const char *c,const char *d) {(void)a;(void)b;(void)c;(void)d;}
bool tools_validate_input(const char *n,const char *i,char *e,size_t z) {
    (void)n;(void)i; if(!input_ok) snprintf(e,z,"bad input");return input_ok;
}
static bool execute(const char *n,const char *i,const char *t,char *r,size_t z) {
    (void)n;(void)i;(void)t; calls++;snprintf(r,z,"tool-output");return tool_ok;
}
static bool verify(const char *n,const char *i,const char *r,void *context,char *e,size_t z) {
    assert(!strcmp(n,"test_tool"));assert(!strcmp(i,"{}"));assert(!strcmp(r,"tool-output"));
    checks++;bool ok=*(bool *)context;if(!ok)snprintf(e,z,"artifact differs");return ok;
}
int main(void) {
    execution_intent_t i={.execute=execute,.input_json="{}"};strcpy(i.tool_name,"test_tool");
    execution_receipt_t r;char out[128],last[2048];bool verified=true;
    assert(execution_submit(&i,&r,out,sizeof(out)) && r.executed && !r.verified);
    i.verify=verify;i.verify_context=&verified;
    assert(execution_submit(&i,&r,out,sizeof(out)) && r.verified && checks==1);
    tool_ok=false;assert(!execution_submit(&i,&r,out,sizeof(out)) && !r.verified && checks==1);
    tool_ok=true;verified=false;
    assert(!execution_submit(&i,&r,out,sizeof(out)) && !r.verified && r.status==EXEC_STATUS_FAILED);
    assert(!strcmp(r.denial_reason,"artifact differs"));
    assert(execution_last_receipt_json(last,sizeof(last)) && strstr(last,"artifact differs"));
    input_ok=false;int before=calls;
    assert(!execution_submit(&i,&r,out,sizeof(out)) && !r.executed && calls==before);
    assert(!execution_submit(&i,&r,NULL,0));
    puts("PASS: verifier context, postcondition failure, gate rejection, no-verifier and tool failure");
}
