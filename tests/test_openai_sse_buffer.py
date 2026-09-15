#!/usr/bin/env python3
"""Differential-test and benchmark exact OpenAI SSE framing callbacks offline."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HEAD = r'''
#include "json_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {jbuf_t line_buf,output;bool stream_done,got_error,record;int calls;} oai_sse_state_t;
static void oai_handle_sse_line(oai_sse_state_t*s,const char*p){
 s->calls++;
 if(s->record){jbuf_append(&s->output,p);jbuf_append_char(&s->output,'\n');}
 if(!strcmp(p,"data: [DONE]"))s->stream_done=true;
 if(!strcmp(p,"data: error"))s->got_error=true;
}
static void init(oai_sse_state_t*s,bool record){memset(s,0,sizeof(*s));s->record=record;jbuf_init(&s->line_buf,16);jbuf_init(&s->output,16);}
static void cleanup(oai_sse_state_t*s){jbuf_free(&s->line_buf);jbuf_free(&s->output);}
'''
MAIN = r'''
static void check(char*p,size_t n,size_t chunk){
 oai_sse_state_t a,b;init(&a,true);init(&b,true);
 assert(reference(NULL,0,1,&a)==oai_sse_write_cb(NULL,0,1,&b));
 for(size_t i=0;i<n;i+=chunk){
  size_t len=n-i<chunk?n-i:chunk;
  size_t x=reference(p+i,1,len,&a),y=oai_sse_write_cb(p+i,1,len,&b);
  assert(x==y&&a.stream_done==b.stream_done&&a.got_error==b.got_error&&a.calls==b.calls);
  assert(a.output.len==b.output.len&&a.line_buf.len==b.line_buf.len);
  if(a.stream_done)break;
 }
 assert(!memcmp(a.output.data,b.output.data,a.output.len));
 assert(!memcmp(a.line_buf.data,b.line_buf.data,a.line_buf.len));
 cleanup(&a);cleanup(&b);
}
static double bench(size_t(*fn)(void*,size_t,size_t,void*),char*p,size_t n,size_t chunk){
 oai_sse_state_t s;init(&s,false);int iterations=(16*1024*1024)/n;
 clock_t begin=clock();
 for(int k=0;k<iterations;k++){
  for(size_t i=0;i<n;i+=chunk){size_t z=n-i<chunk?n-i:chunk;assert(fn(p+i,1,z,&s)==z);}
  jbuf_reset(&s.line_buf);
 }
 double ns=(double)(clock()-begin)/CLOCKS_PER_SEC*1e9/iterations;
 cleanup(&s);return ns;
}
int main(int argc,char**argv){
 (void)argv;
 char fixture[]="\r\n\ndata: {\"delta\":\"hello\"}\r\ndata: next\rpart\n\ndata: error\n\ndata: [DONE]\nignored\n";
 for(size_t n=0;n<sizeof(fixture);n++)for(size_t c=1;c<=sizeof(fixture);c++)check(fixture,n,c);
 unsigned rng=12345;char fuzz[1024];const char alphabet[]="abc\r\n\0";
 for(int trial=0;trial<1000;trial++){
  for(size_t i=0;i<sizeof(fuzz);i++){rng=rng*1664525u+1013904223u;fuzz[i]=alphabet[(rng>>16)%6];}
  check(fuzz,sizeof(fuzz),(rng%71)+1);
 }
 size_t max=1024*1024;char*large=malloc(max);assert(large);memset(large,'x',max);large[max-1]='\n';
 for(size_t c=1;c<max*2;c=c*3+1)check(large,max,c);
 puts("{\"equivalence_passed\":true,\"coverage\":\"all truncations and chunk splits, empty chunks, CRLF, lone CR, NUL, error forwarding, terminal return, randomized input, fragmented 1MiB line\"}");
 if(argc==1){
  struct{const char*name;size_t n,chunk;int pattern;} cases[]={
   {"128B-line",128,128,0},{"16KiB-line",16384,16384,0},
   {"1MiB-line-128B-fragments",1048576,128,0},
   {"1MiB-line-16KiB-fragments",1048576,16384,0},
   {"16KiB-many-events",16384,16384,1},{"16KiB-CRLF-events",16384,16384,2},
   {"16KiB-many-CR",16384,16384,3}};
  for(size_t k=0;k<sizeof(cases)/sizeof(cases[0]);k++){
   size_t n=cases[k].n;memset(large,'x',n);large[n-1]='\n';
   for(size_t i=0;i<n;i++){
    if(cases[k].pattern==1&&i%64==63)large[i]='\n';
    if(cases[k].pattern==2&&i%64==62)large[i]='\r';
    if(cases[k].pattern==2&&i%64==63)large[i]='\n';
    if(cases[k].pattern==3&&i%2==0)large[i]='\r';
   }
   check(large,n,cases[k].chunk);
   for(int sample=0;sample<5;sample++){
    double old,new;
    if(sample%2){new=bench(oai_sse_write_cb,large,n,cases[k].chunk);old=bench(reference,large,n,cases[k].chunk);}
    else{old=bench(reference,large,n,cases[k].chunk);new=bench(oai_sse_write_cb,large,n,cases[k].chunk);}
    printf("{\"case\":\"%s\",\"sample\":%d,\"before_ns\":%.1f,\"after_ns\":%.1f}\n",cases[k].name,sample,old,new);
   }
  }
 }
 free(large);return 0;
}
'''


def callback(source):
    start = source.index('static size_t oai_sse_write_cb(')
    return source[start:source.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-provider', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    baseline = args.baseline_provider.read_text()
    candidate = (ROOT / 'src/provider.c').read_text()
    fixture = output / 'openai_sse.c'
    fixture.write_text(HEAD + callback(baseline).replace('oai_sse_write_cb(', 'reference(', 1)
                       + callback(candidate) + MAIN)
    commands = []
    for variant, flags in [('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']),
                           ('optimized', ['-O3'])]:
        binary = output / variant
        command = ['cc', *flags, '-I' + str(ROOT / 'include'), str(fixture),
                   str(ROOT / 'src/json_util.c'), '-o', str(binary)]
        subprocess.run(command, capture_output=True, text=True, check=True)
        cp = subprocess.run([str(binary), *(['check'] if variant == 'sanitized' else [])],
                            capture_output=True, text=True, timeout=45)
        (output / (variant + '.stdout')).write_text(cp.stdout)
        (output / (variant + '.stderr')).write_text(cp.stderr)
        assert cp.returncode == 0, cp.stderr
        commands.append(command)
    rows = [json.loads(line) for line in cp.stdout.splitlines()][1:]
    summary = []
    for case in dict.fromkeys(row['case'] for row in rows):
        samples = [row for row in rows if row['case'] == case]
        before = statistics.median(row['before_ns'] for row in samples)
        after = statistics.median(row['after_ns'] for row in samples)
        summary.append({'case': case, 'before_median_ns': before, 'after_median_ns': after,
                        'speedup': before / after})
    result = {'passed': True, 'summary': summary, 'compile_commands': commands,
              'baseline_callback_sha256': hashlib.sha256(callback(baseline).encode()).hexdigest(),
              'candidate_callback_sha256': hashlib.sha256(callback(candidate).encode()).hexdigest(),
              'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest()}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
