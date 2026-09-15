#!/usr/bin/env python3
"""Offline exact catalog parsing and native request verbosity regression."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
provider = (ROOT / 'src/provider.c').read_text()
fixture_ast = ast.parse((ROOT / 'tests/test_codex_reasoning_replay.py').read_text())
head = next(ast.literal_eval(n.value) for n in fixture_ast.body
            if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'HEAD' for t in n.targets))
# The replay HEAD includes a guarded no-op grounding helper: this fixture has
# no tool payload, and adding one must fail rather than obscure verbosity deltas.
head = re.sub(r'const char \*codex_cache_default_(?:effort|verbosity)\([^\n]+\n', '', head)
head += '\n#include "codex_cache.h"\n'
head += '#include "' + str(ROOT / 'src/codex_cache.c') + '"\n'


def function(name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;{]*\)\s*\{', provider, re.M)
    assert match, name
    return provider[match.start():provider.index('\n}\n', match.start()) + 3]


main = r'''
static void release_catalog(codex_catalog_t*c){
 atomic_store(&g_catalog,NULL);
 for(int i=0;i<c->count;i++){
  codex_model_t*m=&c->models[i];free((char*)m->info.model_id);free(m->display_name);
  free(m->default_reasoning_level);free(m->visibility);free(m->norm);
 }
 free(c->models);free(c);
}
int main(void){
 assert(!codex_cache_default_verbosity("gpt-5.5"));
 assert(!codex_cache_default_verbosity(NULL));
 const struct{const char*fields,*expected;} cases[]={
  {"\"support_verbosity\":true,\"default_verbosity\":\"low\"","low"},
  {"\"support_verbosity\":true,\"default_verbosity\":\"medium\"","medium"},
  {"\"support_verbosity\":true,\"default_verbosity\":\"high\"","high"},
  {"\"support_verbosity\":false,\"default_verbosity\":\"low\"",NULL},
  {"\"default_verbosity\":\"low\"",NULL},
  {"\"support_verbosity\":true",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":null",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":1",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":\"LOW\"",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":\"verbose\"",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":\"\"",NULL},
  {"\"support_verbosity\":\"true\",\"default_verbosity\":\"low\"",NULL},
  {"\"support_verbosity\":1,\"default_verbosity\":\"low\"",NULL},
  {"\"support_verbosity\":null,\"default_verbosity\":\"low\"",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":\" low\"",NULL},
  {"\"support_verbosity\":truejunk,\"default_verbosity\":\"low\"",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":\"low\\u0000high\"",NULL},
  {"\"support_verbosity\":true,\"default_verbosity\":\"\\u006cow\"","low"},
 };
 session_state_t session={0};strcpy(session.model,"fixture-catalog-model");strcpy(session.effort,"high");
 char*baseline=chatgpt_native_build_request(NULL,NULL,&session,100,NULL);
 assert(!strstr(baseline,"\"text\":{\"verbosity\""));
 for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  jbuf_t data;jbuf_init(&data,512);
  jbuf_append(&data,"{\"models\":[{\"slug\":\"fixture-catalog-model\",\"supported_in_api\":true,\"default_reasoning_level\":\"high\",");
  jbuf_append(&data,cases[i].fields);jbuf_append(&data,"}]}");
  codex_catalog_t*c=catalog_from_json(data.data);assert(c&&c->count==1);publish(c);
  const char*v=codex_cache_default_verbosity(session.model);
  assert((v&&cases[i].expected&&!strcmp(v,cases[i].expected))||(!v&&!cases[i].expected));
  const char*prefixed=codex_cache_default_verbosity("openai/fixture-catalog-model");
  assert((!v&&!prefixed)||(v&&prefixed&&!strcmp(v,prefixed)));
  assert(!codex_cache_default_verbosity("unknown-model"));
  assert(!strcmp(codex_cache_default_effort(session.model),"high"));
  assert(codex_cache_model_supported(session.model));
  char*request=chatgpt_native_build_request(NULL,NULL,&session,100,NULL);
  assert(json_is_valid_container(request));
  if(v){
   char*text=json_get_raw(request,"text");assert(text);char*actual=json_get_str(text,"verbosity");
   assert(actual&&!strcmp(actual,v));free(actual);free(text);
   jbuf_t insertion;jbuf_init(&insertion,64);jbuf_append(&insertion,",\"text\":{\"verbosity\":");
   jbuf_append_json_str(&insertion,v);jbuf_append(&insertion,"}");
   char*where=strstr(request,insertion.data);assert(where);
   memmove(where,where+insertion.len,strlen(where+insertion.len)+1);
   assert(!strcmp(request,baseline));jbuf_free(&insertion);
  }else assert(!strcmp(request,baseline));
  free(request);release_catalog(c);jbuf_free(&data);
 }
 free(baseline);
 puts("PASS: 18 catalog cases, no-cache/unknown/prefix lookup, exact request-only field delta; effort/model retained");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-catalog-verbosity-') as temp:
    temp = Path(temp)
    source = temp / 'test.c'
    source.write_text(head + ''.join(function(name) for name in [
        'chatgpt_append_tool_items', 'chatgpt_append_message_item', 'chatgpt_native_build_request']) + main)
    subprocess.run(['cc', '-std=c11', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L',
                    '-fsanitize=address,undefined', '-g', '-I', str(ROOT / 'include'),
                    str(source), str(ROOT / 'src/json_util.c'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
