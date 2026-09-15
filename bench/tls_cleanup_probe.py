#!/usr/bin/env python3
"""Exact native transport cleanup with system libcurl, verified local TLS and ALPN."""
import argparse
import json
from pathlib import Path
import re
import socket
import ssl
import subprocess
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
C=r'''
#include <curl/curl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
typedef struct {CURL *transport_curl;} provider_t;
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static size_t terminal_cb(char *p,size_t s,size_t n,void *ctx){(void)p;*(bool*)ctx=true;return 0;}
'''
MAIN=r'''
int main(int argc,char**argv){
 assert(argc==4);curl_global_init(CURL_GLOBAL_DEFAULT);
 for(int i=0;i<atoi(argv[3]);i++){
  CURL *c=curl_easy_init();provider_t p={c};bool terminal=false;
  curl_easy_setopt(c,CURLOPT_URL,argv[1]);curl_easy_setopt(c,CURLOPT_CAINFO,argv[2]);
  curl_easy_setopt(c,CURLOPT_POSTFIELDS,"{}");curl_easy_setopt(c,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_2TLS);
  curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,terminal_cb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&terminal);
  curl_easy_setopt(c,CURLOPT_TIMEOUT,5L);curl_easy_setopt(c,CURLOPT_NOPROXY,"*");
  double start=now_ms();CURLcode rc=curl_easy_perform(c);double done=now_ms();
  double curl_total=0;long status=0,http=0;curl_easy_getinfo(c,CURLINFO_TOTAL_TIME,&curl_total);
  curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_getinfo(c,CURLINFO_HTTP_VERSION,&http);
  if(!(provider_stream_terminal_abort(rc,terminal)&&status==200))fprintf(stderr,"curl rc=%d %s terminal=%d status=%ld http=%ld\n",rc,curl_easy_strerror(rc),terminal,status,http);
  assert(provider_stream_terminal_abort(rc,terminal)&&status==200);
  double clean_start=now_ms();chatgpt_finish_transport(&p,c,false,true);double clean_end=now_ms();
  assert(p.transport_curl==NULL);
  printf("{\"round\":%d,\"perform_ms\":%.6f,\"curl_total_ms\":%.6f,\"cleanup_ms\":%.6f,\"whole_ms\":%.6f,\"http_version\":%ld}\n",i,done-start,curl_total*1000,clean_end-clean_start,clean_end-start,http);
 }
 curl_global_cleanup();
}
'''


def exact(sock,n):
    data=b''
    while len(data)<n:
        more=sock.recv(n-len(data))
        if not more:raise EOFError()
        data+=more
    return data


def frame(kind,flags,stream,body=b''):
    return len(body).to_bytes(3,'big')+bytes([kind,flags])+stream.to_bytes(4,'big')+body


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=(ROOT/'src/provider.c').read_text();a=source.index('static void chatgpt_finish_transport(');b=source.index('\n}\n',a)+3
    aa=source.index('static bool provider_stream_terminal_abort(');bb=source.index('\n}\n',aa)+3
    (out/'probe.c').write_text(C+source[aa:bb]+source[a:b]+MAIN)
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2',str(out/'probe.c'),'-lcurl','-o',str(out/'probe')],check=True)
    subprocess.run(['/opt/homebrew/opt/openssl@3/bin/openssl','req','-x509','-newkey','rsa:2048','-nodes',
                    '-keyout',str(out/'key.pem'),'-out',str(out/'cert.pem'),'-days','1','-subj','/CN=localhost',
                    '-addext','subjectAltName=IP:127.0.0.1,DNS:localhost'],check=True,capture_output=True)
    rows=[];errors=[];threads=[]
    for mode in ['http/1.1','h2']:
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(out/'cert.pem',out/'key.pem')
        context.set_alpn_protocols([mode]);listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen()
        stop=threading.Event();listener.settimeout(.1)
        def connection(raw):
            try:
                with context.wrap_socket(raw,server_side=True) as conn:
                    conn.settimeout(5)
                    body=b'data: {"type":"response.completed"}\n\n'
                    if mode=='h2':
                        assert exact(conn,24)==b'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n'
                        conn.sendall(frame(4,0,0))
                        while True:
                            header=exact(conn,9);length=int.from_bytes(header[:3],'big');kind=header[3];flags=header[4];stream=int.from_bytes(header[5:],'big')&0x7fffffff
                            exact(conn,length)
                            if kind==4 and not flags&1:conn.sendall(frame(4,1,0))
                            if kind in [0,1] and stream and flags&1:break
                        conn.sendall(frame(1,4,stream,b'\x88')+frame(0,0,stream,body))
                    else:
                        data=b''
                        while b'\r\n\r\n' not in data:data+=conn.recv(4096)
                        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 100000\r\n\r\n'+body)
                    # Deliberately do not acknowledge TLS shutdown for two seconds.
                    time.sleep(2)
            except (EOFError,ConnectionError,ssl.SSLError):pass
            except Exception as e:errors.append(repr(e))
        def accept():
            while not stop.is_set():
                try:raw,_=listener.accept()
                except socket.timeout:continue
                except OSError:break
                t=threading.Thread(target=connection,args=(raw,),daemon=True);threads.append(t);t.start()
        worker=threading.Thread(target=accept,daemon=True);worker.start()
        try:
            r=subprocess.run([str(out/'probe'),f'https://127.0.0.1:{listener.getsockname()[1]}/responses',str(out/'cert.pem'),'4'],text=True,capture_output=True,timeout=25)
            (out/(mode.replace('/','-')+'.stdout')).write_text(r.stdout);(out/(mode.replace('/','-')+'.stderr')).write_text(r.stderr)
            assert r.returncode==0,(r.returncode,r.stderr)
            rows.extend({'protocol':mode,**json.loads(line)} for line in r.stdout.splitlines())
        finally:stop.set();listener.close();worker.join()
        for t in threads:t.join(timeout=3)
    assert not errors,errors
    (out/'results.json').write_text(json.dumps({'scope':'production cleanup helper, system libcurl, verified local TLS, stalled peer close-notify','records':rows,'errors':errors},indent=2)+'\n')
    print(json.dumps(rows,indent=2))


if __name__=='__main__':main()
