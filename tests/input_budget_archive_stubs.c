#include "context_archive.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
char *input_budget_fixture_bytes;
size_t input_budget_fixture_size;
bool input_budget_fixture_fail;
bool context_archive_store_verified_ex(const void *p,size_t n,const ctx_put_opts_t *o,ctxkey_t *k,const char **stage) {
    (void)o;
    if(input_budget_fixture_fail){*stage="write_failed";return false;}
    free(input_budget_fixture_bytes);input_budget_fixture_bytes=malloc(n+1);
    if(!input_budget_fixture_bytes){*stage="write_failed";return false;}
    memcpy(input_budget_fixture_bytes,p,n);input_budget_fixture_bytes[n]=0;input_budget_fixture_size=n;
    memset(k,0,sizeof(*k));k->kind=CTX_KIND_BLOB;strcpy(k->id,"fixture");*stage="none";return true;
}
int ctxkey_format(const ctxkey_t *k,char *out,size_t cap){(void)k;return snprintf(out,cap,"ck:blob:fixture");}
