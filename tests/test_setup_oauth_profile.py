#!/usr/bin/env python3
"""Production setup-policy regression: saved rotating bearers are not reusable credentials."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/setup.c').read_text()
a=s.index('static const blocked_setup_env_t k_blocked_setup_env[] = {')
b=s.index('\n};',a)+3
parts=[s[a:b]]
for name in ['blocked_setup_env_entry','setup_env_key_is_blocked']:
 a=s.rfind('\n',0,s.index(name+'('))+1;b=s.index('\n}',a)+2;parts.append(s[a:b])
unit='''#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef struct {const char *name; const char *reason; bool report_live;} blocked_setup_env_t;
'''+ '\n'.join(parts)+'''
int main(void){
 assert(setup_env_key_is_blocked("CLAUDE_CODE_OAUTH_TOKEN"));
 assert(setup_env_key_is_blocked("DSCO_CLAUDE_CODE_OAUTH_TOKEN"));
 assert(!setup_env_key_is_blocked("ANTHROPIC_API_KEY"));
 assert(!setup_env_key_is_blocked("OPENROUTER_API_KEY"));
 assert(!setup_env_key_is_blocked("DSCO_FABRIC_SUBLANES"));
 assert(!blocked_setup_env_entry("CLAUDE_CODE_OAUTH_TOKEN")->report_live);
 puts("PASS: rotating OAuth profile snapshots blocked; API keys/routing preserved; bearer values not reported");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-setup-oauth-') as d:
 p=Path(d);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
# Both loading and writing use this same production predicate.
assert 'if (setup_env_key_is_blocked(l.items[i].key))' in s
assert 'kv_list_drop_blocked_setup_keys(&l)' in s
