"""Exercise the production truncation boundary with large escaped swarm JSON."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run():
    source = (ROOT / 'src/tools.c').read_text()
    start = source.index('static void ctx_persist_and_truncate(')
    end = source.index('\n}\n', start) + 3
    function = source[start:end]
    stubs = r'''
#include "json_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool g_ctx_inline_truncation = true;
static void *g_tools_vfs;
static int generic_calls;
static bool ctx_is_internal_tool(const char *name) { (void)name; return false; }
static int ctx_inline_budget(void) { return 25000; }
static void sha256_hex(const uint8_t *s, size_t n, char *h) { (void)s; (void)n; *h=0; }
static void vfs_result_put(void *v, const char *t, const char *h, const char *r, int ttl) {
 (void)v;(void)t;(void)h;(void)r;(void)ttl;
}
static size_t ctx_truncate_json(const char *s, size_t n, char *o, size_t z) {
 (void)s;(void)n;generic_calls++;snprintf(o,z,"{}");return 2;
}
static size_t ctx_truncate_generic(const char *s, size_t n, const char *k, char *o, size_t z) {
 (void)k;return ctx_truncate_json(s,n,o,z);
}
'''
    main = r'''
int main(int argc, char **argv) {
 char input[65536], result[65536]; size_t n=fread(input,1,sizeof(input)-1,stdin);input[n]=0;
 memcpy(result,input,n+1);
  char args[100];snprintf(args,sizeof(args),"{\"action\":\"%s\"}",argc>2?argv[2]:"collect");
 ctx_persist_and_truncate(argc>1?argv[1]:"swarm", args, true, result, sizeof(result));
 if ((argc==1 || !strcmp(argv[1],"swarm")) && (generic_calls || strcmp(result,input))) return 2;
 if (argc>1 && strcmp(argv[1],"swarm") && !generic_calls) return 3;
 fputs(result,stdout);return 0;
}
'''
    payload = {'group_id': 7, 'complete': False, 'results': [
        {'id': i, 'status': 'streaming', 'budget_accounted_usd': None,
         'output': '[...truncated 12000 bytes; full output in swarm artifact...]\n' + ('"\\\n' * 2200)}
        for i in range(3)], 'artifact_path': '.swarm'}
    encoded = json.dumps(payload)
    assert len(encoded) > 25000
    with tempfile.TemporaryDirectory() as directory:
        c = Path(directory) / 'probe.c'; exe = Path(directory) / 'probe'
        c.write_text(stubs + function + main)
        subprocess.run(['cc', '-std=c11', '-D_DARWIN_C_SOURCE', '-Iinclude', str(c),
                        'src/json_util.c', '-o', str(exe)], cwd=ROOT, check=True)
        result = subprocess.run([str(exe)], input=encoded, text=True, capture_output=True, check=True)
        assert json.loads(result.stdout) == payload
        health = {'schema': 'dsco.swarm_health.v1', 'workers': [
            {'id': i, 'provider': 'provider', 'model': 'quoted"' * 20,
             'last_observed_output_seconds_ago': None, 'stall_candidate': None,
             'age_seconds': 1000} for i in range(128)]}
        assert len(json.dumps(health)) > 25000
        result = subprocess.run([str(exe), 'swarm', 'health'], input=json.dumps(health),
                                text=True, capture_output=True, check=True)
        assert json.loads(result.stdout) == health
        subprocess.run([str(exe), 'read_file'], input=encoded, text=True, capture_output=True, check=True)
    print('PASS: large escaped swarm collect/health JSON remains intact; other tools retain truncation')


if __name__ == '__main__':
    run()
