#include "result_spill.h"
#include "context_fabric.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *stored;
static int fault;
ctx_broker_t *ctx_broker_default(void) { return (ctx_broker_t *)1; }
bool ctx_put(ctx_broker_t *b,const void *p,size_t n,const ctx_put_opts_t *o,ctxkey_t *key,bool *d) {
    (void)b;(void)o;(void)d; memset(key,0,sizeof(*key));
    if(fault==1)return false;free(stored);stored=strndup(p,n);return stored!=NULL;
}
char *ctx_get(ctx_broker_t *b,const ctxkey_t *k,size_t *n) {
    (void)b;(void)k;if(fault==2)return NULL;*n=strlen(stored);
    char *s=strdup(stored);if(fault==3)(*n)--;if(fault==4)s[0]='!';return s;
}
int ctxkey_format(const ctxkey_t *k,char *out,size_t len) {
    (void)k;return snprintf(out,len,"ck:blob:owned-spill");
}
int main(void) {
    setenv("DSCO_RESULT_SPILL","1",1);
    char *original=malloc(50001),*result=malloc(60000);assert(original && result);
    memset(original,'x',50000);original[50000]=0;memcpy(original+25000,"MID_EXACT",9);
    for(fault=1;fault<=4;fault++) {
        strcpy(result,original);assert(!result_spill(result,60000,"owned",0));assert(!strcmp(result,original));
    }
    fault=0;
    const char *retrieval[]={"ctx_get","context_recall"};
    for(size_t i=0;i<sizeof(retrieval)/sizeof(*retrieval);i++) {
        strcpy(result,original);assert(!result_spill(result,60000,retrieval[i],0));assert(!strcmp(result,original));
    }
    fault=0;strcpy(result,original);assert(result_spill(result,60000,"owned",0));
    assert(strstr(result,"context_recall key=ck:blob:owned-spill"));
    assert(!strstr(result,"MID_EXACT") && !strcmp(stored,original));
    char label[1001];memset(label,'L',1000);label[1000]=0;
    strcpy(result,original);assert(!result_spill(result,60000,label,0));assert(!strcmp(result,original));
    setenv("DSCO_RESULT_SPILL","0",1);strcpy(result,original);
    assert(!result_spill(result,60000,"owned",0) && !strcmp(result,original));
    free(result);free(original);free(stored);
    puts("PASS spill: verified exact bytes, failed put/missing/truncated/corrupt readback retains originals, overlong label safe, explicit opt-out");
}
