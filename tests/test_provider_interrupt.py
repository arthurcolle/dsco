#!/usr/bin/env python3
"""Local HTTP proof for production provider curl setup and write callbacks."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}\n', match.start()) + 3]


HEAD = r'''
#include <assert.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "json_util.h"
volatile int g_interrupted;
static double interrupted_at;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void *cancel(void *x){(void)x;struct timespec t={0,250000000};nanosleep(&t,NULL);interrupted_at=now();g_interrupted=1;return NULL;}
typedef struct {jbuf_t line_buf,text_accum,reasoning_accum;int output_block_count,announced_tool_count;bool stream_done,trace_events_enabled;unsigned trace_events[10];time_t credit_reset_at;} oai_sse_state_t;
typedef oai_sse_state_t chatgpt_sse_state_t;
static void oai_handle_sse_line(oai_sse_state_t*s,const char*line){if(strstr(line,"[DONE]"))s->stream_done=true;}
static void chatgpt_sse_process_line(chatgpt_sse_state_t*s,const char*line){if(strstr(line,"response.completed"))s->stream_done=true;}
static size_t provider_credit_header_cb(void*p,size_t a,size_t b,void*x){(void)p;(void)x;return a*b;}
typedef struct {const char *name;} fixture_provider;
typedef struct {const char *api_url;} fixture_data;
static bool provider_is_sakana(fixture_provider*p){(void)p;return false;}
static void *dcr_provider_find(const char*n){(void)n;return NULL;}
static long dcr_provider_stream_idle_timeout_ms(const char*n,long fallback){(void)n;return fallback;}
static long chatgpt_stream_idle_timeout_s(void){return 300;}
static bool provider_env_truthy(const char*s){return s&&!strcmp(s,"1");}
static _Thread_local long s_provider_subscription_queue_ms;
static void llm_debug_save_request(const char*s,int code){(void)s;(void)code;}
'''

MAIN = r'''
int main(int argc,char**argv){
 assert(argc==4);bool enabled=atoi(argv[3]);curl_global_init(CURL_GLOBAL_DEFAULT);
 CURL*curl=curl_easy_init();assert(curl);bool native=!strcmp(argv[2],"codex");
 const char *cases[]={"success","success","terminal","error","quiet","partial"};
 for(int i=0;i<6;i++){
  char url[1024];snprintf(url,sizeof(url),"%s/%s/%s",argv[1],argv[2],cases[i]);
  bool interrupt=i>=4;pthread_t thread;g_interrupted=0;interrupted_at=0;
  if(interrupt)assert(!pthread_create(&thread,NULL,cancel,NULL));
  bool done=false;long status=0,connections=0;double begin=now();
  CURLcode raw=native?run_codex(curl,url,&done,&status,&connections):run_openai(curl,url,&done,&status,&connections);
  double ended=now();if(interrupt)pthread_join(thread,NULL);
  CURLcode normalized=provider_stream_terminal_abort(raw,done)?CURLE_OK:raw;
  if(i<2){assert(raw==CURLE_OK&&status==200);if(i==1)assert(connections==0);}
  if(i==2)assert(done&&normalized==CURLE_OK&&status==200);
  if(i==3)assert(raw==CURLE_OK&&status==500&&!done);
  if(interrupt&&enabled){
   assert(raw==CURLE_ABORTED_BY_CALLBACK&&!done&&status==200);
   assert(ended-interrupted_at<1.8);
   assert(!provider_test_chatgpt_transport_retry(raw,status,i==5));
  }
  if(interrupt&&!enabled)assert(raw==CURLE_OK&&ended-interrupted_at>2.5);
  printf("{\"lane\":\"%s\",\"case\":\"%s\",\"interrupt_enabled\":%s,\"curl_code\":%d,\"http_status\":%ld,\"new_connections\":%ld,\"elapsed_ms\":%.3f,\"cancel_latency_ms\":%.3f}\n",argv[2],cases[i],enabled?"true":"false",raw,status,connections,(ended-begin)*1000,interrupt?(ended-interrupted_at)*1000:0.0);
 }
 curl_easy_cleanup(curl);curl_global_cleanup();return 0;
}
'''


def source_for(path):
    source = path.read_text()
    enabled = 'static void provider_stream_enable_interrupt(' in source
    pieces = [HEAD, function(source, 'provider_stream_terminal_abort'),
              function(source, 'provider_test_chatgpt_transport_retry')]
    if enabled:
        pieces.extend(function(source, name) for name in
                      ['provider_stream_progress_cb', 'provider_stream_enable_interrupt'])
    pieces.extend(function(source, name) for name in ['chatgpt_sse_write_cb', 'oai_sse_write_cb'])
    if 'static void chatgpt_trace_phase(' in source:
        trace_end = source.index('} chatgpt_trace_t;') + len('} chatgpt_trace_t;')
        trace_start = source.rfind('typedef struct {', 0, trace_end)
        pieces.append(source[trace_start:trace_end])
        pieces.extend(function(source, name) for name in ['chatgpt_trace_now_ms',
                      'chatgpt_trace_phase', 'chatgpt_trace_progress_cb',
                      'chatgpt_trace_header_cb', 'chatgpt_trace_write_cb'])
    for lane, name, url_expr in [('codex', 'chatgpt_native_stream_once', 'url'),
                                 ('openai', 'openai_stream', 'od->api_url')]:
        body = function(source, name)
        setup_start = body.index(f'    curl_easy_setopt(curl, CURLOPT_URL, {url_expr});')
        setup_end = body.index('    CURLcode res = curl_easy_perform(curl);', setup_start)
        setup = body[setup_start:setup_end]
        # Exact shipped curl options and callback. Authentication and SSE JSON
        # interpretation are fixtures: this test measures transport interruption.
        pieces.append(f'''
static CURLcode run_{lane}(CURL*curl,const char*url,bool*done,long*status,long*connections){{
 fixture_provider pv={{"fixture"}},*p=&pv;fixture_data data={{url}},*od=&data;
 const char *request_json="{{}}";struct curl_slist *hdrs=NULL;
 hdrs=curl_slist_append(hdrs,"Content-Type: application/json");
 hdrs=curl_slist_append(hdrs,"Accept: text/event-stream");
 {'chatgpt' if lane=='codex' else 'oai'}_sse_state_t state={{0}};jbuf_init(&state.line_buf,4096);
 curl_easy_reset(curl);
 {'provider_stream_enable_interrupt(curl);' if 'provider_stream_enable_interrupt(curl);' in body else ''}
 {setup}
 CURLcode res=curl_easy_perform(curl);*done=state.stream_done;
 curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,status);curl_easy_getinfo(curl,CURLINFO_NUM_CONNECTS,connections);
 curl_slist_free_all(hdrs);jbuf_free(&state.line_buf);return res;
}}
''')
    pieces.append(MAIN)
    return '\n'.join(pieces), enabled


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', '0')))
        lane, case = self.path.strip('/').split('/')
        terminal = b'data: {"type":"response.completed"}\n\n' if lane == 'codex' else b'data: [DONE]\n\n'
        body = terminal if case == 'terminal' else b'data: fixture\n\n'
        if case == 'error':
            body = b'{"error":{"message":"fixture failure"}}\n'
        if case == 'quiet':
            body = b'\n'
        self.send_response(500 if case == 'error' else 200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            if case == 'quiet':
                time.sleep(3)
            if case == 'partial':
                self.wfile.write(body[:-1]);self.wfile.flush();time.sleep(3)
                body = body[-1:]
            self.wfile.write(body);self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline-provider', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--repetitions', type=int, default=2)
    ap.add_argument('--trace', action='store_true')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    sources = {'candidate': ROOT / 'src/provider.c'}
    if args.baseline_provider:
        sources['baseline'] = args.baseline_provider
    results = []
    with Server(('127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True);thread.start()
        for variant, source in sources.items():
            generated, enabled = source_for(source)
            cfile = args.output / f'{variant}.c';binary = args.output / variant
            cfile.write_text(generated)
            subprocess.run(['cc', '-O2', '-std=c11', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L',
                            '-I', str(ROOT / 'include'), str(cfile), str(ROOT / 'src/json_util.c'),
                            '-lcurl', '-lpthread', '-o', str(binary)], check=True)
            for repetition in range(args.repetitions):
                for lane in ['codex', 'openai']:
                    run = subprocess.run([str(binary.resolve()), f'http://127.0.0.1:{server.server_port}',
                                          lane, str(int(enabled))], capture_output=True, text=True,
                                         timeout=15, check=True,
                                         env={**os.environ, 'DSCO_CHATGPT_TRACE': '1' if args.trace else '0'})
                    if args.trace and lane == 'codex' and 'static void chatgpt_trace_phase(' in source.read_text():
                        assert 'phase=request' in run.stderr and 'phase=headers' in run.stderr
                        assert 'phase=body' in run.stderr and 'phase=terminal' in run.stderr
                    for line in run.stdout.splitlines():
                        results.append(dict(json.loads(line), variant=variant, repetition=repetition))
                    (args.output / f'{variant}-{lane}-{repetition}.stderr').write_text(run.stderr)
        server.shutdown();thread.join()
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    (args.output / 'manifest.json').write_text(json.dumps({
        'scope': 'Real loopback HTTP/1.1 using extracted production curl setup/write callbacks; fixture SSE interpretation and auth; no inference.',
        'cancellation_after_ms': 250, 'quiet_server_hold_ms': 3000,
        'source_sha256_after_run': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name,path in sources.items()},
        'compiled_fixture_sha256': {name: hashlib.sha256((args.output / f'{name}.c').read_bytes()).hexdigest() for name in sources}
    }, indent=2) + '\n')
    print(json.dumps([r for r in results if r['case'] in ['quiet','partial']], indent=2))


if __name__ == '__main__':
    main()
