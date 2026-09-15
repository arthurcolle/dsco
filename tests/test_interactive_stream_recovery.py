#!/usr/bin/env python3
"""Real --tui/--native failure then follow-up in owned PTYs, with local Responses."""
import argparse
import fcntl
import hashlib
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

from test_stream_completion import event


def run(binary, mode, case, output):
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
            requests.append(body)
            self.send_response(400 if len(requests) == 1 and case == "rejection" else 200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            if len(requests) == 1:
                data = '{"error":{"message":"fixture rejection"}}' if case == "rejection" else event({"type": "response.output_text.delta", "delta": "PARTIAL-KEPT"})
            else:
                data = event({"type": "response.output_text.delta", "delta": "RECOVERED-SECOND"})
                data += event({"type": "response.completed", "response": {"status": "completed", "usage": {"input_tokens": 7, "output_tokens": 5}}})
            self.wfile.write(data.encode())
            self.wfile.flush()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    transcript = bytearray()
    result = {"mode": mode, "case": case, "requests": requests}
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-interactive-recovery-") as tmp:
            env = {"HOME": tmp, "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "TMPDIR": tmp,
                   "TERM": "xterm-kitty", "TERM_PROGRAM": "kitty", "KITTY_WINDOW_ID": "1",
                   "COLORTERM": "truecolor", "LANG": "en_US.UTF-8",
                   "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
                   "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_KITTY_AGENT_WINDOWS": "0",
                   "DSCO_TUI_COMPOSER": "1", "DSCO_DURABLE_AUTOWAKE": "0",
                   "DSCO_DISABLE_DEFAULT_FALLBACKS": "1", "DSCO_DISABLE_PROVIDER_FABRIC_AUTO": "1",
                   "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_MCP_HEADLESS": "0",
                   "DSCO_CHATGPT_OAUTH_TOKEN": "fixture-only", "DSCO_CHATGPT_ACCOUNT_ID": "fixture-account",
                   "OPENAI_API_KEY": "fixture-only", "OPENAI_API_BASE": f"http://127.0.0.1:{server.server_port}/v1",
                   "DSCO_CHATGPT_BASE_URL": f"http://127.0.0.1:{server.server_port}/responses"}
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 100, 1000, 640))
            command = [str(binary), mode, "--profile", "lite", "--provider", "openai-codex", "-m", "gpt-5.6"]
            process = subprocess.Popen(command, env=env, cwd=tmp, stdin=slave, stdout=slave,
                                       stderr=slave, start_new_session=True)
            os.close(slave)
            os.set_blocking(master, False)

            def pump(seconds):
                until = time.monotonic() + seconds
                while time.monotonic() < until:
                    if select.select([master], [], [], min(.03, max(0, until-time.monotonic())))[0]:
                        try:
                            chunk = os.read(master, 262144)
                        except OSError:
                            return
                        if not chunk:
                            return
                        transcript.extend(chunk)
                    assert len(transcript) < 64 * 1024 * 1024, "PTY output exceeded bound"

            def await_requests(count):
                started = time.monotonic()
                while len(requests) < count and time.monotonic() - started < 12:
                    pump(.05)
                    assert process.poll() is None, ("CLI exited", process.returncode)
                assert len(requests) >= count, ("request not sent", count, transcript[-2000:])

            try:
                pump(2)
                if case == "partial":
                    os.write(master, b"/fallback gpt-4.1\r")
                    pump(.3)
                os.write(master, b"Remember message RETAIN-FIRST. Reply briefly.\r")
                await_requests(1)
                pump(1.2)
                assert len(requests) == 1, "failed stream was automatically replayed"
                os.write(master, b"Return RECOVERED-SECOND.\r")
                await_requests(2)
                pump(1.0)
                second = json.dumps(requests[1])
                result["first_prompt_retained"] = "RETAIN-FIRST" in second
                result["failure_record_retained"] = "Turn incomplete:" in second
                result["partial_text_retained"] = "PARTIAL-KEPT" in second
                assert result["first_prompt_retained"], "failed first user prompt was deleted"
                assert result["failure_record_retained"], "failed response was silently accepted or lost"
                if case == "partial":
                    assert result["partial_text_retained"], "visible partial response was discarded"
                assert len(requests) == 2, "unexpected inference replay"
                os.write(master, b"/quit\r")
                started = time.monotonic()
                while process.poll() is None and time.monotonic() - started < 5:
                    pump(.05)
                assert process.poll() == 0, "composer failed to accept /quit after recovery"
                result["passed"] = True
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
                os.close(master)
    except Exception as exc:
        result["passed"] = False
        result["error"] = repr(exc)
        raise
    finally:
        server.shutdown()
        server.server_close()
        stem = mode.lstrip("-") + "-" + case
        (output / f"{stem}.ansi").write_bytes(transcript)
        (output / f"{stem}.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("tui", "native"))
    parser.add_argument("--case", choices=("partial", "rejection"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    results = [run(binary, "--" + mode, case, args.output)
               for mode in ([args.mode] if args.mode else ["tui", "native"])
               for case in ([args.case] if args.case else ["partial", "rejection"])]
    print(json.dumps({"passed": True, "scenarios": len(results),
                      "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
