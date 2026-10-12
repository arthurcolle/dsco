#!/usr/bin/env python3
"""Cold Codex defaults follow config; live catalog priorities and pins survive."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include <assert.h>
#include <stdio.h>
#include "codex_cache.c"
static void clear_catalog(void) {
 codex_catalog_t *c=atomic_exchange(&g_catalog,NULL);
 if(!c)return;
 for(int i=0;i<c->count;i++) {
  codex_model_t *m=&c->models[i];
  free((char*)m->info.model_id);free(m->display_name);
  free(m->default_reasoning_level);free(m->visibility);free(m->norm);
 }
 free(c->models);free(c);
}
int main(void) {
 assert(!strcmp(codex_cache_default_model(),DEFAULT_MODEL));
 assert(!strcmp(DEFAULT_MODEL,"gpt-5.6-luna"));
 assert(codex_cache_model_supported(DEFAULT_MODEL));
 assert(!strcmp(codex_cache_default_effort(NULL),"medium"));
 codex_catalog_t*c=catalog_from_json("{\"models\":["
  "{\"slug\":\"hidden-new-model\",\"priority\":1,\"visibility\":\"hide\",\"supported_in_api\":true},"
  "{\"slug\":\"unavailable-new-model\",\"priority\":2,\"visibility\":\"list\",\"supported_in_api\":false},"
  "{\"slug\":\"future-visible-model\",\"priority\":3,\"visibility\":\"list\",\"supported_in_api\":true,\"default_reasoning_level\":\"high\"},"
  "{\"slug\":\"gpt-5.6-luna\",\"priority\":8,\"visibility\":\"list\",\"supported_in_api\":true}]}");
 assert(c);publish(c);
 assert(codex_cache_lookup("future-visible-model")->context_window == 0);
 assert(!strcmp(codex_cache_default_model(),"future-visible-model"));
 assert(!strcmp(codex_cache_default_effort("future-visible-model"),"high"));
 assert(codex_cache_model_supported("openai/future-visible-model"));
 assert(!codex_cache_model_supported("unavailable-new-model"));
 clear_catalog();
 c=catalog_from_json("{\"models\":[{\"slug\":\"hidden\",\"visibility\":\"hide\",\"supported_in_api\":true}]}");
 assert(c);publish(c);assert(!strcmp(codex_cache_default_model(),DEFAULT_MODEL));
 clear_catalog();
 puts("PASS: cold/hidden-only fallback follows Luna; future catalog priority wins; unsupported/hidden defaults excluded");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-current-model-') as td:
    td=Path(td); src=td/'test.c';src.write_text(SOURCE)
    subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L',
       '-fsanitize=address,undefined','-g','-I',str(ROOT/'include'),'-I',str(ROOT/'src'),
       str(src),str(ROOT/'src/json_util.c'),'-lpthread','-o',str(td/'test')],check=True)
    subprocess.run([str(td/'test')],check=True)
swarm=(ROOT/'src/swarm.c').read_text()
assert 'snprintf(e->codex_model, sizeof(e->codex_model), "%s", DEFAULT_MODEL);' in swarm
assert 'snprintf(e->codex_model, sizeof(e->codex_model), "gpt-5.5");' not in swarm
print('PASS: executor metadata uses central default, not a stale independent model literal')
