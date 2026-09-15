#!/usr/bin/env python3
"""Real full-profile one-shot phases using an immediate loopback SSE response."""
import argparse
import hashlib
import fcntl
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0));return s.getsockname()[1]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,default=Path('./dsco'))
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--rounds',type=int,default=3)
    parser.add_argument('--sample',action='store_true')
    parser.add_argument('--user-state',action='store_true')
    parser.add_argument('--background-services',action='store_true')
    parser.add_argument('--native-codex',action='store_true')
    parser.add_argument('--normal-env',action='store_true')
    args=parser.parse_args()
    assert not args.normal_env or args.user_state, '--normal-env requires --user-state'
    assert not (args.user_state and args.background_services), 'Avoid locking the operator refresh worker; test controls separately'
    binary=args.binary.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    requests=[];telemetry=[]
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):pass
        def do_POST(self):
            arrived=time.perf_counter();body=self.rfile.read(int(self.headers.get('Content-Length','0')))
            parsed=json.loads(body)
            if self.path=='/trust':
                telemetry.append({'arrived':arrived,'bytes':len(body)})
                self.send_response(200);self.send_header('Content-Length','2');self.end_headers()
                self.wfile.write(b'{}');self.wfile.flush();return
            row={'arrived':arrived,'path':self.path,'model':parsed.get('model'),'bytes':len(body),
                 'fixture_auth':self.headers.get('Authorization')=='Bearer fixture-only'}
            event={'id':'fixture','choices':[{'index':0,'delta':{'content':'READY'},'finish_reason':None}]}
            final={'id':'fixture','choices':[{'index':0,'delta':{},'finish_reason':'stop'}],
                   'usage':{'prompt_tokens':1,'completion_tokens':1,'cost':0}}
            payload=('data: '+json.dumps(event)+'\n\ndata: '+json.dumps(final)+'\n\ndata: [DONE]\n\n').encode()
            if args.native_codex:
                payload=('data: '+json.dumps({'type':'response.output_text.delta','delta':'READY'})+'\n\n' +
                         'data: '+json.dumps({'type':'response.completed','response':{'status':'completed','usage':{'input_tokens':1,'output_tokens':1}}})+'\n\n').encode()
            self.send_response(200);self.send_header('Content-Type','text/event-stream')
            self.send_header('Content-Length',str(len(payload)));self.send_header('Connection','close');self.end_headers()
            self.wfile.write(payload);self.wfile.flush();row['final_sent']=time.perf_counter();requests.append(row)
            self.close_connection=True
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    records=[]
    try:
        for i in range(args.rounds):
            with tempfile.TemporaryDirectory(prefix='dsco-oneshot-lifecycle-') as tmp:
                env={'HOME':tmp,'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','TMPDIR':tmp,'TERM':'dumb','LANG':'en_US.UTF-8',
                     'OPENAI_API_KEY':'fixture-only','OPENAI_API_BASE':f'http://127.0.0.1:{server.server_port}/v1',
                     'DSCO_ENV_FILE':'/dev/null','DSCO_PRICING_OFFLINE':'1','DSCO_DISABLE_DEFAULT_FALLBACKS':'1',
                     'DSCO_DISABLE_PROVIDER_FABRIC_AUTO':'1','DSCO_DURABLE_AUTOWAKE':'0','DSCO_MCP_HEADLESS':'0',
                     'DSCO_TOOLMGMT':'0','DSCO_TRUST_OPT_OUT':'1','DSCO_PERF':'json',
                     'DSCO_SECURE_STORE_NO_PROMPT':'1','DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT':'1',
                     'DSCO_MESH_PORT':str(free_port()),'DSCO_HTTP_PORT':str(free_port()),
                     'DSCO_BASELINE_DB':str(out/f'{i}-baseline.sqlite'),'DSCO_CHRONICLE_DIR':str(out/f'{i}-chronicle')}
                if args.normal_env:
                    env={**os.environ,**env}
                    env.pop('DSCO_ENV_FILE',None)
                    for key in ['DSCO_MCP_SERVER','DSCO_MCP_SERVERS','DSCO_DURABLE_AGENT_ID','DSCO_SUBAGENT']:
                        env.pop(key,None)
                lock=None
                if args.user_state:
                    env['HOME']=str(Path.home())
                if args.background_services:
                    lock_path=Path(tmp)/'.dsco/model_catalog_refresh.lock';lock_path.parent.mkdir(parents=True,exist_ok=True)
                    lock=lock_path.open('w');fcntl.flock(lock,fcntl.LOCK_EX)
                    env.pop('DSCO_PRICING_OFFLINE')
                if args.background_services or args.user_state:
                    env.pop('DSCO_TRUST_OPT_OUT')
                    env['DSCO_TRUST_URL']=f'http://127.0.0.1:{server.server_port}/trust'
                if args.native_codex:
                    env.update(DSCO_CHATGPT_OAUTH_TOKEN='fixture-only',DSCO_CHATGPT_ACCOUNT_ID='fixture-lifecycle',
                               DSCO_CHATGPT_BASE_URL=f'http://127.0.0.1:{server.server_port}/responses')
                command=[str(binary),'--profile','full','--provider','openai-codex' if args.native_codex else 'openai','-m','gpt-6-astra' if args.native_codex else 'fixture-model','-p','Return READY.']
                index=len(requests);start=time.perf_counter();realstart=time.time()
                p=subprocess.Popen(command,cwd=tmp,env=env,stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                sampler=None
                if args.sample:
                    sampler=subprocess.Popen(['sample',str(p.pid),'3','1','-file',str(out/f'{i}-sample.txt')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                try:stdout,stderr=p.communicate(timeout=15)
                except subprocess.TimeoutExpired:p.kill();p.communicate();raise
                end=time.perf_counter();realend=time.time()
                (out/f'{i}-stdout.txt').write_text(stdout);(out/f'{i}-stderr.txt').write_text(stderr)
                if sampler:sampler.wait(timeout=5)
                if lock:
                    fcntl.flock(lock,fcntl.LOCK_UN);lock.close()
                assert p.returncode==0,(p.returncode,(out/f'{i}-stderr.txt').read_text()[-2000:])
                assert len(requests)==index+1,requests[index:]
                request=requests[index];assert request['fixture_auth'] and request['model']==('gpt-6-astra' if args.native_codex else 'fixture-model')
                assert 'READY' in (out/f'{i}-stdout.txt').read_text()
                marks=[]
                for line in (out/f'{i}-stderr.txt').read_text().splitlines():
                    if line.startswith('{'):
                        try:v=json.loads(line)
                        except ValueError:continue
                        if v.get('type')=='startup':marks.append(v)
                row={'round':i,'wall_ms':(end-start)*1000,'realtime_ms':(realend-realstart)*1000,
                     'start_to_request_ms':(request['arrived']-start)*1000,'response_to_exit_ms':(end-request['final_sent'])*1000,
                     'request':request,'startup_marks':marks,'exit_code':p.returncode}
                records.append(row);print(json.dumps(row),flush=True)
    finally:
        server.shutdown();server.server_close()
        (out/'results.json').write_text(json.dumps({'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
             'scope':'full startup/one-shot completion, immediate local fixture response; no inference',
             'user_state':args.user_state,'background_services':args.background_services,'native_codex':args.native_codex,'normal_env':args.normal_env,
             'telemetry_requests':telemetry,'records':records},indent=2)+'\n')


if __name__=='__main__':main()
