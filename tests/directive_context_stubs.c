#include "context_fabric.h"
ctx_broker_t *ctx_broker_default(void){return 0;}
bool ctx_put(ctx_broker_t*b,const void*d,size_t n,const ctx_put_opts_t*o,ctxkey_t*k,bool*x){(void)b;(void)d;(void)n;(void)o;(void)k;(void)x;return false;}
char *ctx_get(ctx_broker_t*b,const ctxkey_t*k,size_t*n){(void)b;(void)k;(void)n;return 0;}
bool ctxkey_parse(const char*s,ctxkey_t*k){(void)s;(void)k;return false;}
int ctxkey_format(const ctxkey_t*k,char*out,size_t n){(void)k;if(n)*out=0;return 0;}
