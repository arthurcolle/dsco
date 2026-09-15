"""Compile the actual fan-out implementation with deterministic pthread faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ParallelHandles(unittest.TestCase):
    def test_partial_thread_creation_failure(self):
        source = (ROOT / 'src/toolmgmt.c').read_text()
        start = source.index('typedef struct {\n    tm_call_t *calls;')
        end = source.index('/* ── Dynamic tool registration', start)
        implementation = source[start:end]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
typedef unsigned long pthread_t;
typedef struct { const char *tool; const char *args_json; int timeout_ms; char *result; long status; } tm_call_t;
static int attempt, failure_mask, live;
static bool handles[16];
static void dsco_http_global_init(void) {}
static char *tm_exec(const char *t,const char *a,int ms,long *st) { (void)t;(void)a;(void)ms; *st=200;return NULL; }
static int pthread_create(pthread_t *h,void *attr,void *(*fn)(void *),void *arg) {
    (void)attr; int i=attempt++;
    if (failure_mask & (1<<i)) return 11;
    *h=(unsigned long)i+1; handles[*h]=true; live++;
    fn(arg); return 0;
}
static int pthread_join(pthread_t h,void *ret) {
    (void)ret;
    assert(h>0 && h<16 && handles[h]); handles[h]=false; live--; return 0;
}
'''
        main = r'''
int main(void) {
  for (failure_mask=0; failure_mask<16; failure_mask++) {
    tm_call_t calls[4]={{0}}; attempt=0; live=0;
    assert(toolmgmt_parallel(calls,4,4)==4); assert(live==0);
  }
  assert(toolmgmt_parallel(NULL,0,4)==0);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c = Path(d)/'probe.c'; binary = Path(d)/'probe'
            c.write_text(harness + implementation + main)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
            # Verify the regression test rejects the original indexing bug.
            c.write_text(harness + implementation.replace('pthread_create(&th[spawned]', 'pthread_create(&th[i]') + main)
            subprocess.run(['cc','-std=c11',str(c),'-o',str(binary)],check=True)
            self.assertNotEqual(subprocess.run([str(binary)],capture_output=True).returncode,0)

if __name__ == '__main__': unittest.main()
