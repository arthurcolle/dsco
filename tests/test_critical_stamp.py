#!/usr/bin/env python3
"""Exercise critical-stop stamps without credentials or external inference."""
import argparse
import http.server
import os
from pathlib import Path
import subprocess
import tempfile
import threading

from test_stream_completion import response

ROOT = Path(__file__).resolve().parents[1]


def test_format():
    with tempfile.TemporaryDirectory(prefix="dsco-stamp-format-") as tmp:
        fixture = Path(tmp) / "stamp.c"
        fixture.write_text('''#include "critical_stamp.h"
int main(void) {
    critical_stamp_print(stdout, "bad\\n\\033[31merror", "partial", "retry");
    return 0;
}
''')
        binary = Path(tmp) / "stamp"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                        str(fixture), str(ROOT / "src/critical_stamp.c"), "-o", str(binary)], check=True)
        text = subprocess.check_output([binary]).decode()
        assert text.count("[!! CRITICAL ERROR !!]") == 1, text
        assert "Error: bad  [31merror" in text and "\033" not in text, text
        assert "Impact: partial" in text and "Next: retry" in text, text


def test_headless(binary):
    class Handler(http.server.BaseHTTPRequestHandler):
        fail = False

        def log_message(self, *_):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            self.send_response(400 if self.fail else 200)
            self.send_header("Content-Type", "application/json" if self.fail else "text/event-stream")
            self.end_headers()
            self.wfile.write(b'{"error":{"message":"fixture rejection"}}' if self.fail
                             else response("responses", "success"))

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-stamp-headless-") as tmp:
            dsco_dir = Path(tmp) / ".dsco"
            dsco_dir.mkdir(mode=0o700)
            policy = dsco_dir / "company-account-policy.json"
            policy.write_text('{"registration_required":false}\n')
            policy.chmod(0o600)
            env = {"HOME": tmp, "TMPDIR": tmp, "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                   "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
                   "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_DURABLE_AUTOWAKE": "0",
                   "DSCO_DISABLE_DEFAULT_FALLBACKS": "1", "DSCO_DISABLE_PROVIDER_FABRIC_AUTO": "1",
                   "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_MCP_HEADLESS": "0",
                   "DSCO_CHATGPT_OAUTH_TOKEN": "fixture-only", "DSCO_CHATGPT_ACCOUNT_ID": "fixture-account",
                   "OPENAI_API_KEY": "fixture-only", "OPENAI_API_BASE": f"http://127.0.0.1:{server.server_port}/v1",
                   "DSCO_CHATGPT_BASE_URL": f"http://127.0.0.1:{server.server_port}/responses"}
            cmd = [str(binary), "--profile", "lite", "--provider", "openai-codex",
                   "-m", "gpt-5.6", "-p", "Say hi."]
            for fail in (True, False):
                Handler.fail = fail
                result = subprocess.run(cmd, env=env, cwd=tmp, capture_output=True,
                                        text=True, timeout=22)
                err = result.stderr
                assert (result.returncode != 0) == fail, (fail, result.returncode, err[-1200:])
                assert err.count("[!! CRITICAL ERROR !!]") == int(fail), (fail, err[-1200:])
                if fail:
                    assert "fixture rejection" in err and "Impact: " in err and "Next: " in err, err[-1500:]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    test_format()
    test_headless(args.binary.resolve())
    print("critical stamp: format, headless failure, headless success passed")
