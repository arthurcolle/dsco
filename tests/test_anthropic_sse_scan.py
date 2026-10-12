#!/usr/bin/env python3
"""Exercise production SSE framing against the previous byte-scanning callback."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = r'''
static size_t reference(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    sse_state_t *s = userdata;
    if (g_interrupted || s->repdet_tripped) return 0;
    if (g_stream_heartbeat) tui_stream_heartbeat_recv(g_stream_heartbeat, total);
    const char *p = ptr;
    size_t start = 0;
    for (size_t i = 0; i <= total; i++) {
        if (g_interrupted || s->repdet_tripped) return 0;
        if (i == total || p[i] == '\n' || p[i] == '\r') {
            if (i > start) jbuf_append_len(&s->line_buf, p + start, i - start);
            if (i < total && p[i] == '\n' && s->line_buf.len > 0) {
                sse_process_line(s, s->line_buf.data);
                jbuf_reset(&s->line_buf);
            }
            start = i + 1;
        }
    }
    return total;
}
'''
HARNESS = r'''
#include "json_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {jbuf_t line_buf, output; bool repdet_tripped;} sse_state_t;
static volatile int g_interrupted;
static size_t heartbeat_bytes;
static void *g_stream_heartbeat = &heartbeat_bytes;
static void tui_stream_heartbeat_recv(void *ptr, size_t n) {*(size_t *)ptr += n;}
static void sse_process_line(sse_state_t *s, const char *line) {
    jbuf_append(&s->output, line); jbuf_append_char(&s->output, '\n');
    if (!strcmp(line, "interrupt")) g_interrupted = 1;
    if (!strcmp(line, "repeat")) s->repdet_tripped = true;
}
static void init(sse_state_t *s) {
    memset(s, 0, sizeof(*s)); jbuf_init(&s->line_buf, 16); jbuf_init(&s->output, 16);
}
static void cleanup(sse_state_t *s) {jbuf_free(&s->line_buf); jbuf_free(&s->output);}
'''
MAIN = r'''
static void check(const char *data, size_t n, size_t chunk, int interrupted) {
    sse_state_t a,b; init(&a); init(&b);
    for (size_t i=0; i<n || (n==0 && i==0); i+=chunk) {
        size_t len=n-i<chunk?n-i:chunk;
        g_interrupted=interrupted; heartbeat_bytes=0;
        size_t x=reference((void *)(data+i),1,len,&a), beats=heartbeat_bytes;
        int cancelled=g_interrupted;
        g_interrupted=interrupted; heartbeat_bytes=0;
        size_t y=stream_write_cb((void *)(data+i),1,len,&b);
        assert(x==y && cancelled==g_interrupted && beats==heartbeat_bytes);
        assert(a.repdet_tripped==b.repdet_tripped);
        assert(a.output.len==b.output.len && !memcmp(a.output.data,b.output.data,a.output.len));
        assert(a.line_buf.len==b.line_buf.len && !memcmp(a.line_buf.data,b.line_buf.data,a.line_buf.len));
        if (!x || !n) break;
    }
    cleanup(&a); cleanup(&b); g_interrupted=0;
}
static double bench(size_t (*fn)(void*,size_t,size_t,void*),char *p,size_t n,int count) {
    sse_state_t s; init(&s); g_interrupted=0;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<count;i++) {
        assert(fn(p,1,n,&s)==n); jbuf_reset(&s.line_buf); jbuf_reset(&s.output);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1); cleanup(&s);
    return ((t1.tv_sec-t0.tv_sec)*1e9+t1.tv_nsec-t0.tv_nsec)/count;
}
int main(void) {
    const char *fixtures[]={"", "data: first\r\n\r\ndata: second\n\nlast",
        "a\rb\rc\r", "interrupt\nignored\n", "repeat\nignored\n", "interrupt\n", "repeat\n"};
    for (size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);i++)
        for (size_t chunk=1;chunk<=strlen(fixtures[i])+1;chunk++) {
            check(fixtures[i],strlen(fixtures[i]),chunk,0);
            check(fixtures[i],strlen(fixtures[i]),chunk,1);
        }
    unsigned rng=12345;
    for (size_t n=0;n<1024;n++) {
        char *p=malloc(n+1); assert(p);
        for (size_t i=0;i<n;i++) {
            rng=rng*1664525u+1013904223u;
            p[i]="abc\r\n\0\xff"[(rng>>16)%7];
        }
        p[n]=0; check(p,n,rng%71+1,0); free(p);
    }
    for (size_t n=4088;n<=4104;n++) {
        char *p=malloc(n); assert(p); memset(p,'x',n);
        p[n-1]='\n';check(p,n,n,0);check(p,n,4096,0);free(p);
    }
    char *p=malloc(65536); assert(p);memset(p,'x',65536);
    for (size_t chunk=1;chunk<=65536;chunk=chunk*3+1)check(p,65536,chunk,0);
    puts("{\"correctness\":\"PASS: framing, CRLF, lone CR, embedded NUL, exact allocations, span boundaries, interrupt and repetition abort\"}");
    const size_t sizes[]={128,1024,4096,65536};
    for (size_t k=0;k<sizeof(sizes)/sizeof(sizes[0]);k++) {
        size_t n=sizes[k];memset(p,'x',65536);p[n-1]='\n';
        for (int sample=0;sample<7;sample++) {
            double old,new;
            if (sample%2) {new=bench(stream_write_cb,p,n,2000);old=bench(reference,p,n,2000);}
            else {old=bench(reference,p,n,2000);new=bench(stream_write_cb,p,n,2000);}
            printf("{\"bytes\":%zu,\"sample\":%d,\"before_ns\":%.3f,\"after_ns\":%.3f}\n",n,sample,old,new);
        }
    }
    free(p);return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (ROOT / 'src/llm.c').read_text()
    start = source.index('static size_t stream_write_cb(')
    callback = source[start:source.index('\nstatic size_t stream_header_cb(', start)]
    with tempfile.TemporaryDirectory(prefix='anthropic-sse-scan-') as temp:
        c, binary = Path(temp)/'scan.c', Path(temp)/'scan'
        c.write_text(HARNESS + REFERENCE + callback + MAIN)
        argv = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2' if args.sanitize else '-O3',
                '-I'+str(ROOT/'include'), str(c), str(ROOT/'src/json_util.c'), '-o', str(binary)]
        if args.sanitize:
            argv += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(argv, check=True)
        run = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
    rows = [json.loads(line) for line in run.stdout.splitlines()]
    summary = {}
    for size in sorted({r['bytes'] for r in rows if 'bytes' in r}):
        samples = [r for r in rows if r.get('bytes') == size]
        before = statistics.median(r['before_ns'] for r in samples)
        after = statistics.median(r['after_ns'] for r in samples)
        summary[size] = {'n': len(samples), 'before_ns': before, 'after_ns': after,
                         'speedup': before/after}
    result = {'correctness': rows[0]['correctness'], 'sanitize': args.sanitize,
              'source_sha256': hashlib.sha256(callback.encode()).hexdigest(),
              'scope': 'Production framing callback; event handler and heartbeat are test observers. No model inference.',
              'summary': summary, 'samples': rows[1:]}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'samples'}, indent=2))


if __name__ == '__main__':
    main()
