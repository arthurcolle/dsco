#!/usr/bin/env python3
"""Compile exact production watchdog code; benchmark and check wakeup semantics."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r"""
#define _DEFAULT_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
volatile sig_atomic_t g_interrupted = 0;
static void tool_telemetry_reset(void) {}
static int dsco_tool_grace_period_s(void) { return 1; }
#ifdef WATCHDOG_WAKE_TESTS
static int s_test_cond_failure, s_test_thread_failure, s_test_supervisor_done;
static int test_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    return s_test_cond_failure ? ENOMEM : pthread_cond_init(c, a);
}
static int test_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *v) {
    return s_test_thread_failure ? EAGAIN : pthread_create(t, a, f, v);
}
#define pthread_cond_init test_cond_init
#define pthread_create test_thread_create
#endif
"""

def production(source, header):
    text = source.read_text()
    begin = text.index('_Thread_local volatile int tl_tool_cancelled = 0;')
    end = text.index('/* Per-tool timeout overrides', begin)
    watchdog = text[begin:end]
    hdr = header.read_text()
    begin = hdr.index('typedef struct {', hdr.index('/* ── Tool execution watchdog'))
    end = hdr.index('/* Cooperative cancel flag', begin)
    return PREFIX + hdr[begin:end] + watchdog + (ROOT / 'bench/watchdog_bench.c').read_text(), watchdog

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--before-source', type=Path)
    p.add_argument('--before-header', type=Path)
    p.add_argument('--pairs', type=int, default=40)
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    sources = {'after': (ROOT/'src/tools.c', ROOT/'include/tools.h')}
    if a.before_source:
        assert a.before_header, '--before-header is required with --before-source'
        sources['before'] = (a.before_source, a.before_header)
    evidence = {}
    for name, paths in sources.items():
        unit, section = production(*paths)
        cpath = out / f'{name}.c'; cpath.write_text(unit)
        cmd = cc + ['-O2', '-std=c11', '-pthread', str(cpath), '-o', str(out/name)]
        subprocess.run(cmd, check=True, timeout=30)
        evidence[name] = {'source_path': str(paths[0]), 'header_path': str(paths[1]),
                         'production_section_sha256': hashlib.sha256(section.encode()).hexdigest(),
                         'compile_argv': cmd}
    fixture = out/'read-fixture'; fixture.write_bytes(b'W'*768)
    samples = []
    for pair in range(a.pairs):
        names = list(sources)
        if pair % 2: names.reverse()
        for name in names:
            run = subprocess.run([str(out/name), str(fixture)], check=True, capture_output=True, text=True, timeout=5)
            samples.append({'pair': pair, 'variant': name, **json.loads(run.stdout)})
    (out/'samples.json').write_text(json.dumps(samples, indent=2)+'\n')
    summary = {'scope': 'Exact production watchdog section around a 1 ms filesystem-read workload. No LLM, governance, or full dispatch included.', 'pairs': a.pairs, 'builds': evidence}
    for name in sources:
        vals = [s for s in samples if s['variant'] == name]
        summary[name] = {f'{metric}_p50_ms': statistics.median(v[f'{metric}_ms'] for v in vals) for metric in ('total', 'stop', 'work')}
        summary[name]['total_p95_ms'] = sorted(v['total_ms'] for v in vals)[max(0, int(.95*len(vals))-1)]
    if 'before' in sources:
        summary['median_reduction_percent'] = 100*(1-summary['after']['total_p50_ms']/summary['before']['total_p50_ms'])
    cmd = cc + ['-O1', '-g', '-std=c11', '-pthread', '-DWATCHDOG_WAKE_TESTS', '-fsanitize=address,undefined', str(out/'after.c'), '-o', str(out/'regressions')]
    subprocess.run(cmd, check=True, timeout=30)
    run = subprocess.run([str(out/'regressions'), '--test'], capture_output=True, text=True, timeout=25, check=True)
    (out/'regressions.log').write_text(run.stdout+run.stderr)
    summary['verification'] = json.loads(run.stdout)
    summary['verification']['sanitizers'] = ['address', 'undefined']
    (out/'results.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2))

if __name__ == '__main__':
    main()
