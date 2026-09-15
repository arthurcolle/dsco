#!/usr/bin/env python3
"""Compile actual session initialization and native model serialization; no inference."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
llm=(ROOT/'src/llm.c').read_text();provider=(ROOT/'src/provider.c').read_text()
def function(source,name):
 m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;{]*\)\s*\{',source,re.M)
 assert m,name
 return source[m.start():source.index('\n}\n',m.start())+3]
headers=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "llm.h"
#include "config.h"
#include "provider.h"
#include "provider_profiles.h"
#include "json_util.h"
const model_info_t *codex_cache_lookup(const char *name){(void)name;return NULL;}
const model_info_t *openrouter_cache_lookup(const char *name){
 static model_info_t live={.alias="qwen-3.8-27b",.model_id="qwen/qwen3.8-27b",.context_window=131072};
 return !strcmp(name,"qwen-3.8-27b")?&live:NULL;
}
void uuid_v4(char out[37]){snprintf(out,37,"offline-fixture");}
dsco_trust_tier_t session_trust_tier_from_string(const char *s,bool *ok){(void)s;*ok=false;return DSCO_TRUST_STANDARD;}
int provider_build_default_fallback_models(const char *m,char out[][128],int max){(void)m;(void)out;(void)max;return 0;}
const char *provider_profile_canonical_name(const char *s){return s;}
const provider_profile_t *provider_profile_find(const char *s){(void)s;return NULL;}
typedef int openai_data_t;
static const char *openai_extra_params_json_for_provider(const char *s){(void)s;return NULL;}
static bool provider_is_sakana(provider_t*p){(void)p;return false;}
static bool abliteration_is_provider(const char*s){(void)s;return false;}
static bool openai_conversation_has_media(conversation_t*c){(void)c;return false;}
static bool abliteration_model_supports_vision(const char*s){(void)s;return false;}
'''
functions='\n'.join(function(llm,n) for n in ('llm_env_truthy','llm_env_falsy','llm_model_value_is_blank_default','llm_effective_model','session_model_for_provider','session_state_init','session_state_init_for_provider'))
functions+='\n'+'\n'.join(function(provider,n) for n in ('provider_model_has_prefix','provider_model_strip_explicit_openrouter_prefix','provider_strip_slash_namespace','provider_strip_profile_namespace','provider_request_model_id'))
wire=function(provider,'openai_build_request');wire=wire[:wire.index('    bool direct_openai =')]+'''    jbuf_append(&b,"}");return b.data;
}
'''
main=r'''
int main(void){
 setenv("DSCO_DISABLE_DEFAULT_FALLBACKS","1",1);
 session_state_t s;provider_t p={.name="cerebras"};
 session_state_init_for_provider(&s,"qwen-3.8-27b","cerebras");
 assert(!strcmp(s.model,"qwen-3.8-27b"));
 char *body=openai_build_request(&p,NULL,&s,1,NULL);char *model=json_get_str(body,"model");
 assert(model&&!strcmp(model,"qwen-3.8-27b"));free(model);free(body);
 session_state_init_for_provider(&s,"future-exact-id-20260905","cerebras");
 assert(!strcmp(s.model,"future-exact-id-20260905"));
 session_state_init_for_provider(&s,"qwen38-groq","groq");
 assert(!strcmp(s.model,"qwen/qwen3.8-27b"));
 session_state_init(&s,"qwen-3.8-27b");
 assert(!strcmp(s.model,"qwen/qwen3.8-27b"));
 puts("PASS: explicit provider session and native request preserve exact IDs; exact aliases and unpinned lookup retained");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-explicit-model-') as directory:
 p=Path(directory);(p/'test.c').write_text(headers+functions+wire+main)
 subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'include'),
 str(p/'test.c'),str(ROOT/'src/json_util.c'),str(ROOT/'src/env_config.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={'PATH':'/usr/bin:/bin'})
