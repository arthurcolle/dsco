#!/usr/bin/env python3
"""Exercise real terminal input while the Tool Management MCP is stalled."""
import argparse
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time


def run(binary, sync):
    contacted = threading.Event()
    release = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            contacted.set()
            release.wait(20)
            if 'id' not in body:
                self.send_response(202)
                self.end_headers()
                return
            result = ({'protocolVersion': '2024-11-05', 'capabilities': {'tools': {}},
                       'serverInfo': {'name': 'slow-tool-management', 'version': '1'}}
                      if body['method'] == 'initialize' else
                      {'tools': [{'name': 'search_tools', 'description': 'Search test tools',
                                  'inputSchema': {'type': 'object', 'properties': {}}}]})
            payload = json.dumps({'jsonrpc': '2.0', 'id': body['id'], 'result': result}).encode()
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='dsco-mcp-input-') as tmp:
            config = Path(tmp, '.dsco')
            config.mkdir()
            (config / 'mcp.json').write_text(json.dumps({'mcpServers': {
                'tool-management': {'url': f'http://127.0.0.1:{server.server_port}/mcp'}
            }}))
            env = dict(HOME=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin', TMPDIR=tmp,
                       TERM='xterm-256color', LANG='en_US.UTF-8', DSCO_ENV_FILE='/dev/null',
                       DSCO_NO_AUTO_SUPERVISE='1', DSCO_PRICING_OFFLINE='1',
                       DSCO_KITTY_GRAPHICS='0', DSCO_BANNER='0', DSCO_TUI_COMPOSER='1',
                       DSCO_DURABLE_AUTOWAKE='0', DSCO_DISABLE_DEFAULT_FALLBACKS='1',
                       DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_DISABLE_SHARED_HOME_OAUTH='1',
                       OPENAI_API_KEY='fixture-only', DSCO_MCP_TIMEOUT_MS='20000',
                       DSCO_MCP_SYNC=str(sync))
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 100, 0, 0))
            proc = subprocess.Popen([str(binary), '--tui', '--profile', 'lite',
                                     '--provider', 'openai', '-m', 'gpt-4.1'],
                                    cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
                                    start_new_session=True)
            os.close(slave)
            output = bytearray()

            def pump():
                if select.select([master], [], [], .02)[0]:
                    try:
                        output.extend(os.read(master, 262144))
                    except OSError:
                        pass

            def wait_for(predicate, message, timeout=3):
                deadline = time.monotonic() + timeout
                while not predicate() and time.monotonic() < deadline:
                    pump()
                assert predicate(), (message, bytes(output[-1500:]))

            def command(text, marker):
                offset = len(output)
                start = time.monotonic()
                os.write(master, text.encode() + b'\r')
                wait_for(lambda: marker in output[offset:], f'{text} blocked on MCP')
                return round(time.monotonic() - start, 3)

            try:
                wait_for(contacted.is_set, 'MCP connection never started', timeout=8)
                # The HTTP handler stays blocked until we explicitly release it.
                latency = command('/version', b'(built ')
                command('/mcp', b'loading in background')
                command('/mcp reload', b'loading in background')
                command('/version', b'(built ')
                # Preserve a partially typed command across loader completion.
                offset = len(output)
                os.write(master, b'/vers')
                release.set()
                until = time.monotonic() + .5
                while time.monotonic() < until:
                    pump()
                os.write(master, b'ion\r')
                wait_for(lambda: b'(built ' in output[offset:],
                         'loader completion lost the partially typed command')
                # Poll status until the completed catalog is published.
                deadline = time.monotonic() + 5
                while b'MCP tools registered' not in output and time.monotonic() < deadline:
                    os.write(master, b'/mcp\r')
                    until = time.monotonic() + .15
                    while time.monotonic() < until:
                        pump()
                assert b'MCP tools registered' in output, 'catalog did not become available'
                assert b'search_tools' in output, 'discovered tool missing'
                # A subsequent reload must return to input while the server stalls again.
                release.clear()
                contacted.clear()
                command('/mcp reload', b'reloading in background')
                wait_for(contacted.is_set, 'reload never contacted MCP')
                command('/version', b'(built ')
                os.write(master, b'/quit\r')
                wait_for(lambda: proc.poll() is not None, 'quit waited for MCP', timeout=5)
                assert proc.returncode == 0, proc.returncode
                return {'sync': sync, 'input_seconds_while_loading': latency, 'passed': True}
            finally:
                release.set()
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait(timeout=3)
                os.close(master)
    finally:
        release.set()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('./dsco'))
    parser.add_argument('--sync', type=int, choices=(0, 1))
    args = parser.parse_args()
    print(json.dumps([run(args.binary.resolve(), value)
                      for value in ([args.sync] if args.sync is not None else [0, 1])]))
