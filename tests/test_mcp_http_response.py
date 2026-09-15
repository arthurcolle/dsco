"""Loopback-only real libcurl transport regression; no email or remote actions."""
import http.server
import json
import subprocess
import threading
import time

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        if self.path == '/json':
            data=b'{"jsonrpc":"2.0","id":7,"result":{"ok":true}}'
            self.send_response(200)
            self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(data)))
            self.end_headers();self.wfile.write(data);return
        self.send_response(403 if self.path == '/denied' else 200)
        self.send_header('Content-Type','text/event-stream')
        self.end_headers()
        self.wfile.write(b'data: {"jsonrpc":"2.0","method":"notifications/progress"}\n\n')
        self.wfile.flush()
        data=b'data: {"jsonrpc":"2.0","id":7,"result":{"ok":true}}\n\n'
        for chunk in [data[:17],data[17:]]:
            self.wfile.write(chunk);self.wfile.flush();time.sleep(.02)
        time.sleep(3)  # Deliberately longer than the client's entire deadline.

server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
server.daemon_threads=True
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
    for path in ['/json','/sse','/denied']:
        start=time.monotonic()
        p=subprocess.run(['build/mcp_http_response_fixture',f'http://127.0.0.1:{server.server_port}{path}'],capture_output=True,text=True,timeout=4)
        elapsed=time.monotonic()-start
        if path == '/denied':
            assert p.returncode != 0, p.stdout
        else:
            assert p.returncode == 0, p.stderr
            assert json.loads(p.stdout)['result'] == {'ok':True}
            assert elapsed < 1.2, elapsed
        print(f'{path}: verified in {elapsed:.3f}s')
finally:
    server.shutdown();server.server_close()
