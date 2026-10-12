#!/usr/bin/env python3
"""Offline real-binary proof that useful work survives the former 30s wall."""
import argparse
import concurrent.futures
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time


class SlowHttp(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        time.sleep(35)
        body = b"HTTP_PAST_30_OK"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--artifact-dir", required=True)
    args = p.parse_args()
    binary = str(pathlib.Path(args.binary).resolve())
    artifacts = pathlib.Path(args.artifact_dir).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    rows = []
    with tempfile.TemporaryDirectory(prefix="dsco-deadline-binary-") as temporary:
        root = pathlib.Path(temporary)
        home = root / "home"
        home.mkdir()
        env = {
            "PATH": "/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin",
            "HOME": str(home), "DSCO_ENV_FILE": "/dev/null",
            "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_NO_SUPERVISE": "1",
            "DSCO_PRICING_OFFLINE": "1", "DSCO_SECURE_STORE_NO_PROMPT": "1",
            "DSCO_DISABLE_SHARED_HOME_OAUTH": "1",
            "DSCO_DISABLE_CODEX_OAUTH_DISCOVERY": "1", "DSCO_TOOLMGMT": "0",
            "DSCO_PIXEL_TUI": "0", "DSCO_MCP_SYNC": "1", "TERM": "dumb",
        }
        peer = root / "peer.py"
        peer.write_text('''import json,sys,time
for line in sys.stdin:
    request=json.loads(line)
    if 'id' not in request: continue
    method=request['method']
    if method=='initialize': result={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'fixture','version':'1'}}
    elif method=='tools/list': result={'tools':[{'name':'slow','description':'Offline delayed operation','inputSchema':{'type':'object','properties':{'delay':{'type':'integer'}}}}]}
    elif method=='tools/call':
        time.sleep(request['params'].get('arguments',{}).get('delay',35))
        result={'content':[{'type':'text','text':'MCP_PAST_30_OK'}]}
    else: result={}
    print(json.dumps({'jsonrpc':'2.0','id':request['id'],'result':result}),flush=True)
''')
        (root / ".mcp.json").write_text(json.dumps({"mcpServers": {
            "deadline-fixture": {"command": sys.executable, "args": [str(peer)]}}}))
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowHttp)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        def call(tool, inputs, expected, extra=None, must_succeed=True):
            started = time.monotonic()
            proc = subprocess.run(
                [binary, "--tool-exec-raw", tool, json.dumps(inputs)],
                cwd=root, env=dict(env, **(extra or {})), capture_output=True,
                text=True, timeout=55,
            )
            elapsed = time.monotonic() - started
            (artifacts / (tool.replace("/", "_") + ("-failure" if not must_succeed else "") + ".log")).write_text(proc.stdout + proc.stderr)
            if must_succeed:
                assert proc.returncode == 0 and expected in proc.stdout, (tool, proc.returncode, proc.stdout, proc.stderr)
                assert elapsed >= 34, (tool, elapsed)
                assert "[killed:" not in proc.stdout
            else:
                assert proc.returncode != 0 and elapsed < 5, (tool, elapsed, proc.stdout, proc.stderr)
            return {"tool": tool, "elapsed_s": round(elapsed, 3), "exit_code": proc.returncode,
                    "passed": True, "explicit_timeout": not must_succeed}

        tasks = [
            ("run_command", {"command": "sleep 35; printf COMMAND_PAST_30_OK"}, "COMMAND_PAST_30_OK", None),
            ("dsco-python-3x", {"code": "import time\nprint('STARTED',flush=True)\ntime.sleep(35)\nprint('PYTHON_QUIET_GAP_OK')"}, "PYTHON_QUIET_GAP_OK", None),
            ("http_request", {"url": f"http://127.0.0.1:{httpd.server_port}/slow"}, "HTTP_PAST_30_OK", None),
            ("mcp__deadline-fixture__slow", {"delay": 35}, "MCP_PAST_30_OK", {"DSCO_MCP_SERVER": "deadline-fixture"}),
        ]
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(call, *task) for task in tasks]
                for future in futures:
                    rows.append(future.result())
            rows.append(call("run_command", {"command": "sleep 5", "timeout_seconds": 1}, "", must_succeed=False))
            rows.append(call("mcp__deadline-fixture__slow", {"delay": 5}, "",
                extra={"DSCO_MCP_SERVER": "deadline-fixture", "DSCO_MCP_CALL_TIMEOUT_MS": "750"}, must_succeed=False))
        finally:
            httpd.shutdown()
    result = {"schema": "dsco.timeout_binary.v1", "binary": binary, "checks": rows,
              "paid_requests": 0, "provider_requests": 0}
    (artifacts / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
