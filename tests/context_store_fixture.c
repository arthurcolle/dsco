#include "context_archive.h"
#include "jev_compaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *saved;
ctx_broker_t *ctx_broker_default(void){return (ctx_broker_t *)1;}
bool ctx_put(ctx_broker_t *b,const void *p,size_t n,const ctx_put_opts_t *o,ctxkey_t *key,bool *dedup){(void)b;(void)dedup;memset(key,0,sizeof(*key));key->kind=o->kind;strcpy(key->id,"fixture");free(saved);saved=strndup(p,n);return saved!=NULL;}
char *ctx_get(ctx_broker_t *b,const ctxkey_t *key,size_t *n){(void)b;(void)key;if(!saved)return NULL;*n=strlen(saved);return strdup(saved);}
int ctxkey_format(const ctxkey_t *key,char *out,size_t n){return snprintf(out,n,"ck:%s:fixture",key->kind==CTX_KIND_TOOL?"tool":"msg");}
bool ctxkey_parse(const char *s,ctxkey_t *key){(void)s;(void)key;return false;}
void jev_compaction_shadow(const jev_compaction_candidate_t *c,size_t n,char *out,size_t len){(void)c;(void)n;snprintf(out,len,"{}");}
bool chronicle_event(const char *a,const char *b,const char *c,const char *d,const char *e,const char *f,const char *g,const char *h){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return false;}
