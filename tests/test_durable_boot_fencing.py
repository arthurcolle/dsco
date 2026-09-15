#!/usr/bin/env python3
"""Real CLI boot ownership before a local fixture provider is called; no inference."""
import argparse
import http.server
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    args=parser.parse_args();binary=args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='dsco-boot-fence-') as tmp:
        db=Path(tmp)/'bus.sqlite';requests=[]
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                with sqlite3.connect(db) as sql:
                    state=sql.execute('SELECT status,generation,claim_pid FROM tasks').fetchall()
                requests.append({'model':body['model'],'state':state})
                delta={'id':'fixture','choices':[{'index':0,'delta':{'content':'READY'},'finish_reason':None}]}
                final={'id':'fixture','choices':[{'index':0,'delta':{},'finish_reason':'stop'}],
                       'usage':{'prompt_tokens':1,'completion_tokens':1,'cost':0}}
                data=('data: '+json.dumps(delta)+'\n\n'+'data: '+json.dumps(final)+'\n\ndata: [DONE]\n\n').encode()
                self.send_response(200);self.send_header('Content-Type','text/event-stream')
                self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        server.daemon_threads=True
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        env={'HOME':tmp,'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','TMPDIR':tmp,'TERM':'dumb',
             'OPENAI_API_KEY':'fixture-only','OPENAI_API_BASE':f'http://127.0.0.1:{server.server_port}/v1',
             'DSCO_ENV_FILE':'/dev/null','DSCO_PRICING_OFFLINE':'1',
             'DSCO_DISABLE_DEFAULT_FALLBACKS':'1','DSCO_DISABLE_PROVIDER_FABRIC_AUTO':'1',
             'DSCO_DURABLE_AUTOWAKE':'0','DSCO_AGENTS_DB':str(db),'DSCO_IPC_DB':str(db),
             'DSCO_MCP_HEADLESS':'0'}
        try:
            for args in [['agents','create','boot-worker'],['agents','create','controller'],
                         ['agents','task','--from','controller','--to','boot-worker','Return READY.']]:
                p=subprocess.run([str(binary),*args],env=env,cwd=tmp,capture_output=True,text=True,timeout=10)
                assert p.returncode==0,(p.stdout,p.stderr)
            with sqlite3.connect(db) as sql:task=sql.execute('SELECT id FROM tasks').fetchone()[0]
            env.update(DSCO_SUBAGENT='1',DSCO_DURABLE_AGENT_ID='boot-worker',
                       DSCO_DURABLE_BOOT_TASK_ID=str(task),DSCO_DURABLE_BOOT_TASK='Return READY.',
                       DSCO_DURABLE_POLL_MS='100',DSCO_DURABLE_IDLE_EXIT_MS='1000')
            command=[str(binary),'--profile','worker','--provider','openai','-m','fixture-model','-p','Return READY.']
            p=subprocess.run(command,env=env,cwd=tmp,capture_output=True,text=True,timeout=20)
            assert p.returncode==0,(p.stdout,p.stderr)
            assert len(requests)==1,requests
            assert requests[0]['state'][0][:2]==('running',1) and requests[0]['state'][0][2]>0,requests
            with sqlite3.connect(db) as sql:
                state=sql.execute('SELECT status,generation,result FROM tasks').fetchone()
            assert state==('done',1,'READY'),state
            again=subprocess.run(command,env=env,cwd=tmp,capture_output=True,text=True,timeout=10)
            assert again.returncode!=0 and 'no longer claimable' in again.stderr,(again.stdout,again.stderr)
            assert len(requests)==1,'duplicate boot reached the provider'
            print('PASS: real durable CLI claims boot before provider execution and records its fenced result')
            print('PASS: replaying a completed boot is rejected before another provider request')
        finally:
            server.shutdown();server.server_close();thread.join(timeout=3)

if __name__=='__main__':main()
