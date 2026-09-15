"""Verify the installed CLI's governed selected-MCP route against owned SSE."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

parser=argparse.ArgumentParser()
parser.add_argument('--binary',default='./dsco')
a=parser.parse_args()
binary=str(Path(a.binary).resolve(strict=True))
calls=[]
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args): pass
    def do_POST(self):
        q=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if 'id' not in q:
            self.send_response(202);self.send_header('Content-Length','0');self.end_headers();return
        method=q['method']
        if method=='initialize':
            result={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'owned-sse','version':'1'}}
        elif method=='tools/list':
            result={'tools':[{'name':'fixture_read','description':'Read an owned fixture value','inputSchema':{'type':'object','properties':{}}}]}
        elif method=='tools/call':
            calls.append(q['params']);result={'content':[{'type':'text','text':'OWNED_SSE_PROOF'}],'isError':False}
        else:result={}
        self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
        notify={'jsonrpc':'2.0','method':'notifications/progress','params':{}}
        reply={'jsonrpc':'2.0','id':q['id'],'result':result}
        for obj in (notify,reply):
            self.wfile.write(('data: '+json.dumps(obj)+'\n\n').encode());self.wfile.flush()
        time.sleep(4)  # Connection remains open after each complete response.
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='dsco-owned-sse-') as directory:
        root=Path(directory)
        (root/'.mcp.json').write_text(json.dumps({'mcpServers':{'owned-sse':{'url':f'http://127.0.0.1:{server.server_port}/mcp','transport':'http'}}}))
        env=os.environ.copy();env.update(DSCO_MCP_SERVER='owned-sse',DSCO_MCP_TIMEOUT_MS='1500',DSCO_PRICING_OFFLINE='1',DSCO_NO_AUTO_SUPERVISE='1')
        start=time.monotonic()
        p=subprocess.run([binary,'--tool-exec','mcp__owned-sse__fixture_read','{}'],cwd=root,env=env,capture_output=True,text=True,timeout=15)
        elapsed=time.monotonic()-start
        assert p.returncode==0,(p.stdout,p.stderr)
        result=json.loads(p.stdout)
        assert result['ok'] and 'OWNED_SSE_PROOF' in str(result['result']),result
        assert len(calls)==1,calls
        print(json.dumps({'ok':True,'binary':binary,'seconds':round(elapsed,3),'tool_executions':len(calls),'persistent_sse':True,'notifications_ignored':True}))
finally:
    server.shutdown();server.server_close()
