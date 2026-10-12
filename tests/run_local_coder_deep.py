#!/usr/bin/env python3
"""Real installed-agent integration tests against disposable Python/C projects.
No fabricated tool callbacks or model responses. Tests are hashed before model work.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
RUN = Path(os.environ.get('DSCO_TEST_REPORT_DIR', str(ROOT / 'reports/local-coder-deep'))) / ('coding-' + str(time.time_ns()))
RUN.mkdir(parents=True)
LAUNCHER = Path(os.environ.get('DSCO_TEST_LAUNCHER', str(Path.home() / '.local/bin/dsco-local-coder')))

CASES = [
    {
        'name': 'csv_parser', 'source': 'parser.py',
        'initial': '''def parse_inventory(text):
    rows = text.strip().split('\\n')
    return [{'name': row.split(',')[0], 'quantity': int(row.split(',')[1])} for row in rows[1:]]
''',
        'task': 'Fix parse_inventory(text) in parser.py to parse CSV with header name,quantity, quoted commas and quotes in names, blank lines and CRLF. Return a list of dicts with integer quantity; empty or whitespace-only input returns []. Preserve carriage returns inside quoted field values; do not normalize the whole input. Invalid or empty quantities must raise ValueError, not silently drop rows. Use only the Python standard library. Do not change tests.',
        'test': '''import unittest
from parser import parse_inventory
class Tests(unittest.TestCase):
 def test_plain(self): self.assertEqual(parse_inventory('name,quantity\\napple,2\\n'),[{'name':'apple','quantity':2}])
 def test_quoted_comma(self): self.assertEqual(parse_inventory('name,quantity\\n"bolt, steel",3\\n'),[{'name':'bolt, steel','quantity':3}])
 def test_quotes_crlf(self): self.assertEqual(parse_inventory('name,quantity\\r\\n"a ""quoted"" part",-2\\r\\n'),[{'name':'a "quoted" part','quantity':-2}])
 def test_embedded_cr_preserved(self): self.assertEqual(parse_inventory('name,quantity\\r\\n"carriage\\rreturn",7\\r\\n'),[{'name':'carriage\\rreturn','quantity':7}])
 def test_invalid_quantity_rejected(self):
  for value in ['not-an-int','']:
   with self.assertRaises(ValueError): parse_inventory('name,quantity\\nx,'+value+'\\n')
 def test_empty(self):
  for s in ['', '  \\n', 'name,quantity\\n']: self.assertEqual(parse_inventory(s),[])
 def test_blank_lines(self): self.assertEqual(parse_inventory('name,quantity\\na,1\\n\\nb,0\\n'),[{'name':'a','quantity':1},{'name':'b','quantity':0}])
if __name__=='__main__': unittest.main(verbosity=2)
''',
        'verify': [sys.executable, 'verify.py'],
    },
    {
        'name': 'jsonl_errors', 'source': 'events.py',
        'initial': '''import json
def load_events(text):
    return [json.loads(line) for line in text.split('\\n')]
''',
        'task': 'Fix load_events(text) in events.py. Ignore empty/whitespace lines; accept a final line without newline; require each JSON value to be an object. Invalid JSON or non-object records must raise ValueError containing the physical 1-based line number (for example line 3). Preserve Unicode. Do not change tests.',
        'test': '''import unittest
from events import load_events
class Tests(unittest.TestCase):
 def test_empty(self): self.assertEqual(load_events(' \\n\\n'),[])
 def test_last_line(self): self.assertEqual(load_events('{"a":1}'),[{'a':1}])
 def test_blank_unicode(self): self.assertEqual(load_events('\\n{"city":"東京"}\\n  \\n{"a":2}\\n'),[{'city':'東京'},{'a':2}])
 def test_physical_error_line(self):
  with self.assertRaisesRegex(ValueError,r'(?i)line\\s+3\\b'): load_events('{}\\n\\nnot json\\n')
 def test_non_objects(self):
  for s in ['[]','42','null','"text"']:
   with self.assertRaisesRegex(ValueError,r'(?i)line\\s+2\\b'): load_events('{}\\n'+s)
if __name__=='__main__': unittest.main(verbosity=2)
''',
        'verify': [sys.executable, 'verify.py'],
    },
    {
        'name': 'path_containment', 'source': 'paths.py',
        'initial': '''from pathlib import Path
def safe_child(root, relative):
    return Path(root) / relative
''',
        'task': 'Fix safe_child(root, relative) in paths.py. Return the resolved pathlib.Path of a strict descendant of root. Reject absolute input, traversal outside root, symlink escapes and root itself, with ValueError. Existing target files are not required. Do not read or write target file contents. Use only the standard library. Do not change tests.',
        'test': '''from pathlib import Path
import tempfile,unittest
from paths import safe_child
class Tests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(); self.base=Path(self.tmp.name); self.root=self.base/'work-root'; self.root.mkdir(); (self.base/'work-root-sibling').mkdir(); (self.root/'link').symlink_to(self.base/'work-root-sibling',target_is_directory=True)
 def tearDown(self): self.tmp.cleanup()
 def test_nested(self): self.assertEqual(safe_child(self.root,'a/b.txt'),self.root.resolve()/'a/b.txt')
 def test_normalized(self): self.assertEqual(safe_child(self.root,'a/../b.txt'),self.root.resolve()/'b.txt')
 def test_rejections(self):
  for p in ['.', '..', '../work-root-sibling/x', 'link/x', str(self.base/'outside')]:
   with self.subTest(path=p):
    with self.assertRaises(ValueError): safe_child(self.root,p)
if __name__=='__main__': unittest.main(verbosity=2)
''',
        'verify': [sys.executable, 'verify.py'],
    },
    {
        'name': 'c_ring_buffer', 'source': 'queue.c',
        'initial': '''#include "queue.h"
int queue_init(queue *q, unsigned cap) { if (!cap || cap > 8) return 0; q->head=q->tail=q->size=0; q->cap=cap; return 1; }
int queue_push(queue *q, int v) { if(q->size==q->cap) return 0; q->data[q->tail]=v; q->tail=(q->tail+1)&(q->cap-1); q->size++; return 1; }
int queue_pop(queue *q, int *v) { if(!q->size) return 0; *v=q->data[q->head]; q->head=(q->head+1)&(q->cap-1); q->size--; return 1; }
''',
        'header': '''#ifndef QUEUE_H
#define QUEUE_H
typedef struct { int data[8]; unsigned head,tail,size,cap; } queue;
int queue_init(queue*, unsigned); int queue_push(queue*,int); int queue_pop(queue*,int*);
#endif
''',
        'task': 'Fix queue.c so the ring buffer handles every capacity 1 through 8, including non-powers-of-two, with correct FIFO order through repeated wraparound. Keep the API and queue.h unchanged. Keep full/empty and invalid-capacity behavior. Do not change tests. Run python3 verify.py, which compiles real C code and executes it.',
        'c_test': '''#include "queue.h"
#include <assert.h>
#include <stdio.h>
#undef assert
#define assert(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\\n", __LINE__); return 1; } } while (0)
int main(void) { queue q; int v;
 assert(!queue_init(&q,0)); assert(!queue_init(&q,9));
 for(unsigned cap=1;cap<=8;cap++) { assert(queue_init(&q,cap)); assert(!queue_pop(&q,&v));
  for(int cycle=0;cycle<30;cycle++) {
   for(unsigned i=0;i<cap;i++) assert(queue_push(&q,cycle*10+(int)i));
   assert(!queue_push(&q,999));
   for(unsigned i=0;i<cap;i++) { assert(queue_pop(&q,&v)); assert(v==cycle*10+(int)i); }
   assert(!queue_pop(&q,&v));
   assert(queue_push(&q,123)); assert(queue_pop(&q,&v)); assert(v==123);
  }
 }
 puts("PASS: real C ring buffer, all capacities and wraparound"); return 0; }
''',
        'test': '''import subprocess,sys
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','queue.c','verify_queue.c','-o','verify_queue'],check=True)
subprocess.run(['./verify_queue'],check=True)
''',
        'verify': [sys.executable, 'verify.py'],
    },
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_case(case):
    directory = RUN / case['name']
    directory.mkdir()
    source = directory / case['source']
    source.write_text(case['initial'])
    test = directory / 'verify.py'
    test.write_text(case['test'])
    protected = [test]
    if 'header' in case:
        header = directory / 'queue.h'; header.write_text(case['header']); protected.append(header)
        ctest = directory / 'verify_queue.c'; ctest.write_text(case['c_test']); protected.append(ctest)
    protected_hashes = {str(p): sha(p) for p in protected}
    baseline = subprocess.run(case['verify'], cwd=directory, capture_output=True, text=True, timeout=15)
    (directory / 'baseline.log').write_text(baseline.stdout + baseline.stderr)
    assert baseline.returncode != 0, 'Fixture must demonstrate the real bug first'
    before = sha(source)
    prompt = (f'Actual coding task in {directory}. Read {case["source"]}, make the fix with real tools, '
              f'and run {" ".join(case["verify"])}. {case["task"]} '
              f'You may edit ONLY {source}. Do not modify verify.py, headers, tests or any file outside '
              'this directory. Do not install packages or use external services. Do not claim success '
              'until the actual command passes. Stop after reporting the observed result.')
    started = time.monotonic()
    logpath = directory / 'native-agent.log'
    with logpath.open('x') as log:
        proc = subprocess.Popen([str(LAUNCHER), '--directory', str(directory),
                                 '--verify', shlex.join(case['verify']), prompt],
                                stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        timed_out = False
        try:
            proc.wait(timeout=180)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(proc.pid, signal.SIGTERM)
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL); proc.wait(timeout=5)
    unchanged_tests = all(p.exists() and sha(p) == digest for name, digest in protected_hashes.items() for p in [Path(name)])
    verification = subprocess.run(case['verify'], cwd=directory, capture_output=True, text=True, timeout=15)
    (directory / 'independent-verification.log').write_text(verification.stdout + verification.stderr)
    trace = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', logpath.read_text(errors='replace'))
    result = {'case': case['name'], 'baseline_exit': baseline.returncode, 'agent_exit': proc.returncode,
              'timeout': timed_out, 'source_changed': sha(source) != before,
              'tests_unchanged': unchanged_tests, 'independent_test_exit': verification.returncode,
              'real_tool_calls': len(re.findall(r'►\s+\w+', trace)),
              'seconds': time.monotonic() - started, 'workspace': str(directory),
              'source_sha256': sha(source), 'protected_test_hashes': protected_hashes}
    result['passed'] = (not timed_out and result['source_changed'] and unchanged_tests
                        and verification.returncode == 0 and result['real_tool_calls'] > 0)
    p = directory / 'result.json'; p.write_text(json.dumps(result, indent=2) + '\n')
    assert json.loads(p.read_text()) == result
    print(json.dumps(result), flush=True)
    return result


if __name__ == '__main__':
    print('RUN=' + str(RUN), flush=True)
    # Independent files; two real native agent sessions stress shared serving safely.
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        results = list(executor.map(run_case, CASES))
    summary = {'cases': len(results), 'passed': sum(x['passed'] for x in results),
               'results': results, 'inference_concurrency': 2,
               'scope': 'Installed launcher and real native DSCO tools; actual file edits, Python tests and C compilation. Test files hashed and independently rerun. No mocked tool execution.'}
    p = RUN / 'summary.json'; p.write_text(json.dumps(summary, indent=2) + '\n')
    assert json.loads(p.read_text()) == summary
    print('SUMMARY=' + json.dumps(summary), flush=True)
