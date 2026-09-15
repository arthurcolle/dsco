"""Compile the actual callback; compare framing to its scalar reference offline."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = r'''
static size_t reference(void *ptr, size_t size, size_t nmemb, void *userdata) {
 size_t total=size*nmemb; chatgpt_sse_state_t *s=userdata; const char *p=ptr;
 for(size_t i=0;i<total;i++) {
  if(p[i]=='\n') { if(s->line_buf.len) {
   chatgpt_sse_process_line(s,s->line_buf.data); jbuf_reset(&s->line_buf);
   if(s->stream_done) return 0;
  }} else if(p[i]!='\r') jbuf_append_char(&s->line_buf,p[i]);
 } return total;
}
'''
HARNESS = r'''
#include "json_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { jbuf_t line_buf, output; int stream_done; } chatgpt_sse_state_t;
static void chatgpt_sse_process_line(chatgpt_sse_state_t *s,const char *p) {
 jbuf_append(&s->output,p); jbuf_append_char(&s->output,'\n');
 if(!strcmp(p,"[DONE]")) s->stream_done=1;
}
static void init(chatgpt_sse_state_t *s) { memset(s,0,sizeof(*s)); jbuf_init(&s->line_buf,16); jbuf_init(&s->output,16); }
static void cleanup(chatgpt_sse_state_t *s) { jbuf_free(&s->line_buf);jbuf_free(&s->output); }
'''
MAIN = r'''
static void check(char *p,size_t n,size_t chunk) {
 chatgpt_sse_state_t a,b;init(&a);init(&b);
 for(size_t i=0;i<n;i+=chunk) {
  size_t len=n-i<chunk?n-i:chunk;
  size_t x=reference(p+i,1,len,&a), y=chatgpt_sse_write_cb(p+i,1,len,&b);
  assert(x==y);assert(a.stream_done==b.stream_done);
  assert(a.output.len==b.output.len && !memcmp(a.output.data,b.output.data,a.output.len));
  assert(a.line_buf.len==b.line_buf.len && !memcmp(a.line_buf.data,b.line_buf.data,a.line_buf.len));
  if(a.stream_done)break;
 }
 cleanup(&a);cleanup(&b);
}
static double bench(size_t (*fn)(void*,size_t,size_t,void*),char *p,size_t n) {
 chatgpt_sse_state_t s;init(&s); clock_t t=clock();
 for(int i=0;i<20000;i++) {assert(fn(p,1,n,&s)==n);jbuf_reset(&s.line_buf);jbuf_reset(&s.output);}
 double ns=(double)(clock()-t)/CLOCKS_PER_SEC*1e9/20000;cleanup(&s);return ns;
}
int main(void) {
 char fixture[]="\r\n\ndata: {\"delta\":\"hello\"}\r\ndata: next\rpart\n\n[DONE]\nignored\n";
 for(size_t c=1;c<=sizeof(fixture);c++)check(fixture,sizeof(fixture)-1,c);
 unsigned rng=12345; char fuzz[1024];
 const char alphabet[]="abc\r\n\0";
 for(int trial=0;trial<1000;trial++) {
  for(size_t i=0;i<sizeof(fuzz);i++){rng=rng*1664525u+1013904223u;fuzz[i]=alphabet[(rng>>16)%6];}
  check(fuzz,sizeof(fuzz),(rng%71)+1);
 }
 char *large=malloc(65536);assert(large);memset(large,'x',65536);large[65535]='\n';
 for(size_t c=1;c<70000;c=c*3+1)check(large,65536,c);
 puts("framing equivalence: PASS (chunk splits, CRLF, lone CR, NUL, termination, randomized input)");
 for(size_t n=128;n<=65536;n*=8) {
  large[n-1]='\n';
  for(int sample=0;sample<5;sample++) {
   double old=bench(reference,large,n),new=bench(chatgpt_sse_write_cb,large,n);
   printf("bytes=%zu sample=%d before_ns=%.1f after_ns=%.1f speedup=%.2f\n",n,sample,old,new,old/new);
  }
 }
 free(large);return 0;
}
'''

class SSEBuffer(unittest.TestCase):
    def test_framing_and_benchmark(self):
        source = (ROOT / 'src/provider.c').read_text()
        start = source.index('static size_t chatgpt_sse_write_cb(')
        end = source.index('\n}\n', start) + 3
        reference = REFERENCE
        baseline_path = os.environ.get('DSCO_SSE_BASELINE_PROVIDER')
        if baseline_path:
            baseline = Path(baseline_path).read_text()
            baseline_start = baseline.index('static size_t chatgpt_sse_write_cb(')
            baseline_end = baseline.index('\n}\n', baseline_start) + 3
            reference = baseline[baseline_start:baseline_end].replace(
                'chatgpt_sse_write_cb(', 'reference(', 1)
        with tempfile.TemporaryDirectory() as d:
            c = Path(d) / 'sse.c'
            binary = Path(d) / 'sse'
            c.write_text(HARNESS + reference + source[start:end] + MAIN)
            subprocess.run(['cc', '-O3', '-I'+str(ROOT/'include'), str(c),
                            str(ROOT/'src/json_util.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
