#!/usr/bin/env python3
"""Real question-tool input handoff and recovery in owned ANSI/Kitty PTYs.

Uses a loopback Responses fixture, real tool dispatch, and isolated state;
no provider inference, desktop automation, or remote terminal control.
"""
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


def run(binary, mode, case, followup, output):
    requests = []
    errors = []
    transcript = bytearray()
    result = {"mode": mode, "case": case, "followup": followup}
    response_ready = threading.Event()
    with tempfile.TemporaryDirectory(prefix="dsco-question-pty-") as tmp:
        root = Path(tmp)
        (root / ".dsco").mkdir()
        (root / ".dsco/company-account-policy.json").write_text(
            '{"registration_required":false}\n')
        (root / ".dsco/company-account-policy.json").chmod(0o600)
        (root / "read.txt").write_text("BATCH_READ_OK")
        question = {"question": "QUESTION_INPUT_FIXTURE", "header": "Input",
                    "id": "choice", "options": [{"label": "First", "value": "first"},
                                                     {"label": "Second", "value": "second"}]}

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                try:
                    body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(body)
                    if len(requests) == 1 and case == "draft":
                        assert response_ready.wait(5), "draft setup timed out"
                    data = event({"type": "response.created", "response": {"id": "fixture"}})
                    if len(requests) == 1:
                        calls = [("AskUserQuestion", {"questions": [question]}, "question-call")]
                        if case == "batch":
                            calls.insert(0, ("read_file", {"path": str(root / "read.txt")},
                                             "read-call"))
                        for index, (name, arguments, ident) in enumerate(calls):
                            item = {"type": "function_call", "id": "item-" + ident,
                                    "name": name, "call_id": ident,
                                    "arguments": json.dumps(arguments)}
                            for kind in ("added", "done"):
                                data += event({"type": "response.output_item." + kind,
                                               "output_index": index, "item": item})
                    else:
                        data += event({"type": "response.output_text.delta",
                                       "delta": "QUESTION_RECOVERED"})
                    data += event({"type": "response.completed", "response": {
                        "status": "completed", "usage": {"input_tokens": 10,
                                                               "output_tokens": 5}}})
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(data.encode())))
                    self.end_headers()
                    self.wfile.write(data.encode())
                except Exception as exc:
                    errors.append(repr(exc))

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = {"HOME": tmp, "TMPDIR": tmp, "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
               "LANG": "en_US.UTF-8", "TERM": "xterm-kitty", "TERM_PROGRAM": "kitty",
               "COLORTERM": "truecolor", "KITTY_WINDOW_ID": "1",
               "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
               "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_NO_SUPERVISE": "1",
               "DSCO_KITTY_AGENT_WINDOWS": "0", "DSCO_DURABLE_AUTOWAKE": "0",
               "DSCO_AUTO_GOAL": "0", "DSCO_DISABLE_DEFAULT_FALLBACKS": "1",
               "DSCO_DISABLE_PROVIDER_FABRIC_AUTO": "1",
               "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_MCP_HEADLESS": "0",
               "DSCO_TUI_COMPOSER": "1", "DSCO_TUI_FOLLOWUP": str(followup),
               "DSCO_PIXEL_TUI_ANIMATIONS": "0", "DSCO_TUI_ANIMATIONS": "0",
               "DSCO_TOOL_ALLOWLIST": "AskUserQuestion,read_file",
               "DSCO_CHATGPT_OAUTH_TOKEN": "fixture-only",
               "DSCO_CHATGPT_ACCOUNT_ID": "fixture-account",
               "DSCO_CHATGPT_BASE_URL": f"http://127.0.0.1:{server.server_port}/responses"}
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 100, 1000, 640))
        process = subprocess.Popen([str(binary), "--" + mode, "--profile", "lite",
                                    "--provider", "openai-codex", "-m", "gpt-6-astra",
                                    "--trust-tier", "trusted", "--approval-mode", "never"],
                                   env=env, cwd=tmp, stdin=slave, stdout=slave, stderr=slave,
                                   start_new_session=True)
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

        def await_condition(predicate, description, timeout=8):
            started = time.monotonic()
            while not predicate() and time.monotonic() - started < timeout:
                pump(.03)
                assert process.poll() is None, (description, process.returncode)
            assert predicate(), (description, transcript[-2000:])

        try:
            await_condition(lambda: b"\x1b[?2004h" in transcript, "initial composer")
            pump(.2)
            os.write(master, b"Ask the fixture question.\r")
            if case == "draft":
                await_condition(lambda: len(requests) == 1, "fixture request")
                pump(.2)
                os.write(master, b"DRAFT_BEFORE_DIALOG")
                pump(.2)
                response_ready.set()
            await_condition(lambda: b"QUESTION_INPUT_FIXTURE" in transcript,
                            "question dialog")
            pump(.2)
            if case in ("custom", "custom-cancel", "ctrl-c"):
                os.write(master, b"3")
                await_condition(lambda: b"\x1b[96m> " in transcript or
                                b"\x1b[36m> " in transcript, "free-text prompt")
                pump(.1)
                os.write(master, b"custom answer\r" if case == "custom" else
                         b"\x03" if case == "ctrl-c" else b"\x1b")
                if case == "custom":
                    pump(.2)
                    os.write(master, b"\r")
            elif case == "cancel":
                os.write(master, b"\x1b")
            elif case == "signal":
                os.kill(process.pid, signal.SIGINT)
            else:
                # Arrow selection followed by review/submit exercises the raw
                # reader, including a distinct later composer after the tool.
                os.write(master, b"\x1b[B\r")
                pump(.2)
                os.write(master, b"\r")
            await_condition(lambda: len(requests) >= 2, "dialog did not return to model")
            tools = [item for item in requests[1].get("input", [])
                     if item.get("type") == "function_call_output"]
            answer = next(json.loads(item["output"]) for item in tools
                          if item.get("call_id") == "question-call")
            cancelled = case in ("cancel", "custom-cancel", "ctrl-c", "signal")
            assert answer["status"] == ("cancel" if cancelled else "submit"), answer
            if not cancelled:
                expected = "custom answer" if case == "custom" else "second"
                assert answer["answers"][0]["value"] == expected, answer
            if case == "batch":
                assert any("BATCH_READ_OK" in item["output"] for item in tools), tools
            pump(.3)
            os.write(master, b"-ok\r" if case == "draft" else
                     b"After the dialog, reply briefly.\r")
            await_condition(lambda: len(requests) >= 3, "composer did not recover")
            if case == "draft":
                assert "DRAFT_BEFORE_DIALOG-ok" in json.dumps(requests[2]), "draft lost"
            pump(.3)
            os.write(master, b"/quit\r")
            until = time.monotonic() + 5
            while process.poll() is None and time.monotonic() < until:
                pump(.03)
            assert process.poll() == 0, ("/quit failed after question", process.poll())
            assert not errors, errors
            result.update(passed=True, status=answer["status"], requests=len(requests),
                          composer_recovered=True, clean_exit=True)
            return result
        except Exception as exc:
            result.update(passed=False, error=repr(exc))
            raise
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)
            os.close(master)
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
            stem = f"{mode}-{case}-followup{followup}"
            (output / (stem + ".ansi")).write_bytes(transcript)
            (output / (stem + ".json")).write_text(json.dumps(result, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("tui", "native"))
    parser.add_argument("--case", choices=("single", "batch", "custom", "cancel",
                                           "custom-cancel", "ctrl-c", "draft", "signal"))
    parser.add_argument("--followup", type=int, choices=(0, 1))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve(strict=True)
    results = []
    for mode in ([args.mode] if args.mode else ("tui", "native")):
        for followup in ([args.followup] if args.followup is not None else (1, 0)):
            for case in ([args.case] if args.case else
                         ("single", "batch", "custom", "cancel", "custom-cancel", "ctrl-c",
                          "draft", "signal")):
                if case == "draft" and not followup:
                    continue  # Retained drafts require the enabled composer.
                result = run(binary, mode, case, followup, args.output)
                results.append(result)
                print(json.dumps(result), flush=True)
    print(json.dumps({"passed": True, "scenarios": len(results),
                      "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
