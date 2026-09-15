#!/usr/bin/env python3
"""Offline TLS HTTP/2 reproduction of native SSE terminal-abort connection reuse."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SERVER = r'''
const fs=require('fs'), http2=require('http2');
const server=http2.createSecureServer({key:fs.readFileSync(process.argv[2]),cert:fs.readFileSync(process.argv[3])});
let connections=0,requests=0;
const large=process.argv[4]==='large',openai=process.argv[5]==='openai';
server.on('session',s=>{connections++;s.on('error',()=>{});});
server.on('stream',(stream,headers)=>{
 requests++;let bytes=0;stream.on('error',()=>{});stream.on('data',b=>bytes+=b.length);
 stream.on('end',()=>{
  stream.respond({':status':200,'content-type':'text/event-stream'});
  const body=openai?('data: '+JSON.stringify({choices:[{delta:{content:'x'.repeat(330000)},finish_reason:'stop'}]})+'\n\ndata: [DONE]\n\n'):large?['response.created','response.in_progress','response.completed'].map(type=>
    'event: '+type+'\ndata: '+JSON.stringify({type,response:{instructions:'x'.repeat(110000),status:type==='response.completed'?'completed':'in_progress'}})+'\n\n').join(''):
    'data: {"type":"response.completed"}\n\n';
  console.log(JSON.stringify({request:requests,connections,stream_id:stream.id,bytes,response_bytes:Buffer.byteLength(body),remote_window:stream.session.state.remoteWindowSize,local_window:stream.session.state.localWindowSize}));
  if(headers[':path']==='/ended')stream.end(body);
  else stream.write(body); // Open SSE stays open until client cancels its stream.
 });
});
server.listen(0,'127.0.0.1',()=>console.log(JSON.stringify({port:server.address().port})));
'''
HEAD = r'''
#include "json_util.h"
#include <curl/curl.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {jbuf_t line_buf;bool stream_done;} chatgpt_sse_state_t;
typedef struct {CURL *transport_curl;} provider_t;
static int cleanup_calls;
static void counted_cleanup(CURL*c){cleanup_calls++;curl_easy_cleanup(c);}
static void chatgpt_sse_process_line(chatgpt_sse_state_t*s,const char*p){if(!strncmp(p,"data:",5)&&strstr(p,"response.completed"))s->stream_done=true;}
'''
MAIN = r'''
int main(int argc,char**argv){
 assert(argc>=2&&argc<=4);bool large=argc>=3,consume=argc==4;int requests=large?10:40;
 jbuf_t request;jbuf_init(&request,52000);jbuf_append(&request,"{\"input\":\"");
 if(large)for(int i=0;i<51200;i++)jbuf_append_char(&request,'x');
 jbuf_append(&request,"\"}");
 curl_global_init(CURL_GLOBAL_DEFAULT);
 CURL *owned=curl_easy_init();assert(owned);chatgpt_finish_transport(NULL,owned,true,false);assert(cleanup_calls==1);
 if(FIXED_TRANSPORT){
  provider_t retained={.transport_curl=curl_easy_init()};assert(retained.transport_curl);
  chatgpt_finish_transport(&retained,retained.transport_curl,false,false);
  assert(cleanup_calls==1&&retained.transport_curl);
  chatgpt_finish_transport(&retained,retained.transport_curl,false,true);
  assert(cleanup_calls==2&&!retained.transport_curl);
 }
 provider_t provider={.transport_curl=curl_easy_init()};assert(provider.transport_curl);
 fprintf(stderr,"libcurl=%s\n",curl_version());
 for(int i=0;i<requests;i++){
  bool ended=consume||(!large&&i>=20);
  char url[256];snprintf(url,sizeof(url),"%s/%s",argv[1],ended?"ended":"held");
  chatgpt_sse_state_t state={0};jbuf_init(&state.line_buf,4096);
  if(!provider.transport_curl)provider.transport_curl=curl_easy_init();
  CURL *c=provider.transport_curl;assert(c);
  curl_easy_reset(c);
  curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_POSTFIELDS,large?request.data:"{}");
  curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,consume?drain_write_cb:chatgpt_sse_write_cb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&state);
  curl_easy_setopt(c,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_2TLS);
  /* Fixture-only self-signed TLS and bounded test deadline. */
  curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,0L);curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,0L);
  curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,3000L);
  CURLcode rc=curl_easy_perform(c);long status=0,connections=0,version=0;double total=0;curl_off_t downloaded=0;
  curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_getinfo(c,CURLINFO_NUM_CONNECTS,&connections);
  curl_easy_getinfo(c,CURLINFO_HTTP_VERSION,&version);curl_easy_getinfo(c,CURLINFO_TOTAL_TIME,&total);
  curl_easy_getinfo(c,CURLINFO_SIZE_DOWNLOAD_T,&downloaded);
  fprintf(stderr,"request=%d status=%ld http_version=%ld curl_code=%d terminal=%d downloaded=%lld ms=%.3f\n",i+1,status,version,rc,state.stream_done,(long long)downloaded,total*1000);
  chatgpt_finish_transport(&provider,c,false,provider_stream_terminal_abort(rc,state.stream_done));
  assert(status==200&&version==CURL_HTTP_VERSION_2_0&&state.stream_done);
  assert(rc==CURLE_OK||provider_stream_terminal_abort(rc,state.stream_done));
  if(!large)assert(total<1.0);
  if(large)assert(downloaded>300000);
  if(consume){assert(rc==CURLE_OK);if(i>0)assert(connections==0);}
  printf("{\"request\":%d,\"server_end_stream\":%s,\"curl_code\":%d,\"terminal\":true,\"new_connections\":%ld,\"elapsed_ms\":%.3f,\"downloaded_bytes\":%lld,\"request_bytes\":%zu}\n",i+1,ended?"true":"false",rc,connections,total*1000,(long long)downloaded,large?request.len:2);
  fflush(stdout);jbuf_free(&state.line_buf);
 }
 if(provider.transport_curl)curl_easy_cleanup(provider.transport_curl);
 curl_global_cleanup();jbuf_free(&request);return 0;
}
'''


def function(source, name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start()
    return source[start:source.index('\n}\n', start) + 3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--large', action='store_true', help='Ten 50 KiB requests and three 110 KB events per response.')
    ap.add_argument('--provider-source', type=Path, default=ROOT / 'src/provider.c')
    ap.add_argument('--consume-terminal', action='store_true', help='Control: drain the terminal chunk to normal HTTP completion.')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--openai', action='store_true', help='Exercise the OpenAI-compatible framing and transport cleanup.')
    args = ap.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if args.consume_terminal and not args.large:
        ap.error('--consume-terminal requires --large')
    source = args.provider_source.read_text()
    cfile = output / 'client.c'
    cleanup = (function(source, 'chatgpt_finish_transport') if 'static void chatgpt_finish_transport(' in source else
               'static void chatgpt_finish_transport(provider_t*p,CURL*c,bool owned,bool aborted){(void)p;(void)aborted;if(owned)curl_easy_cleanup(c);}\n')
    drain = '''static size_t drain_write_cb(void*p,size_t z,size_t n,void*s){
size_t consumed=chatgpt_sse_write_cb(p,z,n,s);
return !consumed&&((chatgpt_sse_state_t*)s)->stream_done?z*n:consumed;
}\n'''
    head = HEAD
    callback_name = 'chatgpt_sse_write_cb'
    if args.openai:
        head += '''
typedef provider_t openai_data_t;
#define oai_sse_state_t chatgpt_sse_state_t
static void oai_handle_sse_line(oai_sse_state_t*s,const char*p){if(!strcmp(p,"data: [DONE]"))s->stream_done=true;}
#define chatgpt_sse_write_cb oai_sse_write_cb
'''
        callback_name = 'oai_sse_write_cb'
        stream = function(source, 'openai_stream')
        begin = stream.index('    curl_slist_free_all(hdrs);') + len('    curl_slist_free_all(hdrs);')
        end = stream.index('    result.http_status', begin)
        cleanup = 'static void chatgpt_finish_transport(provider_t*p,CURL*curl,bool owned_curl,bool terminal_aborted){openai_data_t*od=p;' + stream[begin:end].replace('od->curl', 'od->transport_curl') + '}\n'
    fixed = ('terminal_aborted' in stream[begin:end] if args.openai else 'static void chatgpt_finish_transport(' in source)
    client_main = MAIN.replace('curl_easy_reset(c);', 'curl_easy_reset(c);\n  curl_easy_setopt(c,CURLOPT_BUFFERSIZE,4096L);') if args.openai else MAIN
    cfile.write_text(head + '#define FIXED_TRANSPORT ' + str(int(fixed)) + '\n' +
                    function(source, 'provider_stream_terminal_abort') + function(source, callback_name) +
                    '#define curl_easy_cleanup counted_cleanup\n' + cleanup + '\n#undef curl_easy_cleanup\n' + drain + client_main)
    serverfile = output / 'server.js'
    serverfile.write_text(SERVER)
    binary = output / 'client'
    subprocess.run(['cc', '-O2', *(['-fsanitize=address,undefined', '-g', '-fno-omit-frame-pointer'] if args.sanitize else []), '-I' + str(ROOT / 'include'), str(cfile),
                    str(ROOT / 'src/json_util.c'), '-lcurl', '-o', str(binary)], check=True)
    key, cert = output / 'fixture-key.pem', output / 'fixture-cert.pem'
    subprocess.run([shutil.which('openssl'), 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(cert), '-days', '1', '-subj', '/CN=localhost'],
                   capture_output=True, check=True)
    proc = subprocess.Popen([shutil.which('node'), str(serverfile), str(key), str(cert), 'large' if args.large else 'small', 'openai' if args.openai else 'native'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        import select
        assert select.select([proc.stdout], [], [], 5)[0], 'fixture did not start'
        startup = proc.stdout.readline()
        port = json.loads(startup)['port']
        cp = subprocess.run([str(binary), f'https://127.0.0.1:{port}', *(['large'] if args.large else []),
                             *(['consume'] if args.consume_terminal else [])], capture_output=True,
                            text=True, timeout=35 if args.large else 15)
        (output / 'client.stdout').write_text(cp.stdout)
        (output / 'client.stderr').write_text(cp.stderr)
    finally:
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=3)
        (output / 'server.stdout').write_text(stdout)
        (output / 'server.stderr').write_text(stderr)
        key.unlink(missing_ok=True)
    rows = [json.loads(line) for line in cp.stdout.splitlines()]
    result = {'passed': cp.returncode == 0, 'process_exit_code': cp.returncode,
              'requests': len(rows), 'new_connections': sum(r['new_connections'] for r in rows),
              'max_elapsed_ms': max((r['elapsed_ms'] for r in rows), default=0),
              'curl_codes': sorted(set(r['curl_code'] for r in rows)),
              'large_flow_control_fixture': args.large,
              'consumed_terminal_control': args.consume_terminal,
              'terminal_transport_retirement': fixed,
              'dialect': 'openai' if args.openai else 'native',
              'sanitizers': args.sanitize,
              'total_request_bytes': sum(r['request_bytes'] for r in rows),
              'total_downloaded_bytes': sum(r['downloaded_bytes'] for r in rows),
              'fixture_sha256': hashlib.sha256(cfile.read_bytes()).hexdigest(),
              'scope': 'Exact production framing/terminal normalization, fixture event parser, TLS HTTP/2 loopback'}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    assert cp.returncode == 0, cp.stderr


if __name__ == '__main__':
    main()
