#!/usr/bin/env python3
"""Recover oversized tool arguments on the real Codex lane in owned PTYs.

Uses a private local Responses SSE server, HOME and context database. No paid
inference or existing terminal/GUI surfaces are accessed. Native mode saves
the compositor raster and proves graphics transport, without claiming OCR.
"""
import argparse
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time

from test_buffer_slash import TerminalText
from test_input_budget_binary import budget_events
from test_stream_completion import event

SYSTEM = "OWNED_CONTEXT_SYSTEM. Preserve the user's instructions and finish the requested task."
USER = "OWNED_CONTEXT_USER: write both owned files exactly once, recover old evidence, then finish."
FINAL = "OWNED_CONTEXT_RECOVERED_AND_FINISHED"
CALLS = ("owned_first_write", "owned_second_write", "owned_archive_recall")
TOOLS = {"write_file", "context_evict", "context_recall", "context_compact", "context_status"}


def full_events(root):
    rows = []
    for path in (root / "chronicle" / "events").rglob("*.jsonl"):
        for line in path.read_text().splitlines():
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    return rows


def tool_calls(request):
    return {item["call_id"]: item for item in request["input"]
            if item.get("type") == "function_call"}


def verify_protocol(request):
    calls = [item["call_id"] for item in request["input"]
             if item.get("type") == "function_call"]
    results = [item["call_id"] for item in request["input"]
               if item.get("type") == "function_call_output"]
    assert len(calls) == len(set(calls)) and sorted(calls) == sorted(results), (calls, results)


def saved_blocks(autosave):
    return [block for message in (autosave or {}).get("messages", [])
            for block in message.get("content", [])]


def run_case(binary, native, evidence):
    mode = "native" if native else "tui"
    requests, errors = [], []
    state = {"archive_key": None, "final_sent": False, "first_mtime": None}
    result = {"mode": mode, "passed": False}
    transcript, raw_tail = bytearray(), bytearray()
    retained, events, proc = None, [], None
    with tempfile.TemporaryDirectory(prefix=f"dsco-context-recovery-{mode}-") as tmp:
        root = Path(tmp)
        # The isolated HOME has no user catalog. Seed only the local fixture's
        # advertised model so the native adapter retains the exact model ID.
        (root / ".dsco").mkdir()
        (root / ".dsco" / "codex_models.json").write_text(json.dumps({"models": [
            {"slug": "gpt-6-astra", "supported_in_api": True, "context_window": 272000,
             "default_reasoning_level": "low", "priority": 1}]}))
        files = [root / "first.txt", root / "second.txt"]
        contents = ["OWNED_FIRST_BEGIN\n" + "a" * 60000 + "\nOWNED_FIRST_END",
                    "OWNED_SECOND_BEGIN\n" + "b" * 60000 + "\nOWNED_SECOND_END"]
        args = [{"path": str(path), "content": content} for path, content in zip(files, contents)]
        snapshot = root / "native.ppm"

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                try:
                    assert self.path == "/responses", self.path
                    request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(request)
                    stage = len(requests)
                    assert stage <= 4, "provider request was unexpectedly replayed"
                    assert request["model"] == "gpt-6-astra", request["model"]
                    assert {tool["name"] for tool in request["tools"]} == TOOLS
                    encoded = json.dumps(request)
                    assert SYSTEM in encoded and USER in encoded, "user/system instructions changed"
                    verify_protocol(request)
                    calls = tool_calls(request)
                    if stage == 2:
                        assert json.loads(calls[CALLS[0]]["arguments"]) == args[0]
                        assert files[0].read_text() == contents[0]
                        state["first_mtime"] = files[0].stat().st_mtime_ns
                    if stage >= 3:
                        assert CALLS[0] not in calls, "old tool call was not evicted"
                        assert json.loads(calls[CALLS[1]]["arguments"]) == args[1], \
                            "latest tool arguments were changed"
                        assert files[0].stat().st_mtime_ns == state["first_mtime"], \
                            "first file was executed again during recovery"
                        assert files[1].read_text() == contents[1]
                    if stage == 3:
                        assert contents[0] not in encoded, "old argument payload remains in request"
                        match = re.search(r"key=(ck:tool:[A-Za-z0-9_-]+)", encoded)
                        assert match, "eviction did not leave a retrieval key"
                        state["archive_key"] = match[1]
                    data = event({"type": "response.created", "response": {"id": f"owned-{stage}"}})
                    if stage < 4:
                        tool_args = args[stage - 1] if stage < 3 else {"key": state["archive_key"]}
                        item = {"type": "function_call", "id": f"item-{stage}",
                                "name": "write_file" if stage < 3 else "context_recall",
                                "call_id": CALLS[stage - 1], "arguments": json.dumps(tool_args)}
                        data += event({"type": "response.output_item.added", "output_index": 0,
                                       "item": item})
                        data += event({"type": "response.output_item.done", "output_index": 0,
                                       "item": item})
                    else:
                        assert CALLS[2] in calls
                        recall = next(item for item in request["input"]
                                      if item.get("type") == "function_call_output" and
                                      item["call_id"] == CALLS[2])
                        assert state["archive_key"] in recall["output"], "recall result lost its key"
                        data += event({"type": "response.output_text.delta", "delta": FINAL})
                    data += event({"type": "response.completed", "response": {"status": "completed",
                                  "usage": {"input_tokens": 100, "output_tokens": 10,
                                            "input_tokens_details": {"cached_tokens": 0}}}})
                    payload = data.encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    self.wfile.flush()
                    if stage == 4:
                        state["final_sent"] = True
                except Exception as exc:
                    errors.append(repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        env = dict(HOME=tmp, TMPDIR=tmp, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   DSCO_MAX_INPUT_TOKENS="32768",
                   TERM="xterm-kitty" if native else "xterm-256color", COLORTERM="truecolor",
                   LANG="en_US.UTF-8", DSCO_CHATGPT_OAUTH_TOKEN="owned-fixture-only",
                   DSCO_CHATGPT_ACCOUNT_ID="owned-fixture-account",
                   DSCO_CHATGPT_BASE_URL=f"http://127.0.0.1:{server.server_port}/responses",
                   OPENAI_API_KEY="owned-fixture-only", DSCO_DISABLE_SHARED_HOME_OAUTH="1",
                   OPENAI_API_BASE=f"http://127.0.0.1:{server.server_port}/v1",
                   DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1",
                   DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_DISABLE_DEFAULT_FALLBACKS="1",
                   DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_NO_AUTO_SUPERVISE="1",
                   DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0", DSCO_DURABLE_AUTOWAKE="0",
                   DSCO_MCP_HEADLESS="0", DSCO_BANNER="0", DSCO_KITTY_BANNER="0",
                   DSCO_TOOL_PROXY="1", DSCO_TOOL_ALLOWLIST=",".join(sorted(TOOLS)),
                   DSCO_AUTO_GOAL="0", DSCO_AUTO_COMPACT="1", DSCO_TUI_ANIMATIONS="0",
                   DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_PIXEL_TUI_DPR="1",
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot),
                   DSCO_CONTEXT_DB=str(root / "context.db"),
                   DSCO_CHRONICLE_DIR=str(root / "chronicle"), DSCO_CHRONICLE_MODE="full-local",
                   DSCO_SYSTEM_PROMPT=SYSTEM, DSCO_HARD_TURN_CEILING="6")
        if not native:
            env["DSCO_KITTY_GRAPHICS"] = "0"
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 1200, 800))
        text_filter = TerminalText()
        ready_at = finished_at = final_sent_at = None
        sent = quit_sent = False
        try:
            proc = subprocess.Popen([str(binary), "--profile", "worker",
                                     "--native" if native else "--tui", "-i",
                                     "--provider", "openai-codex", "-m", "gpt-6-astra",
                                     "--trust-tier", "trusted", "--approval-mode", "never"],
                                    cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
                                    start_new_session=True)
            os.close(slave)
            started = time.monotonic()
            while time.monotonic() - started < 45:
                if select.select([master], [], [], .02)[0]:
                    try:
                        data = os.read(master, 262144)
                    except OSError:
                        data = b""
                    raw_tail.extend(data)
                    if b"\x1b[?2004h" in raw_tail:
                        ready_at = ready_at or time.monotonic()
                    del raw_tail[:-65536]
                    transcript.extend(text_filter.feed(data))
                    assert len(transcript) < 4 * 1024 * 1024, "PTY output limit"
                assert not errors, errors
                now = time.monotonic()
                if ready_at and not sent and now - ready_at > .15:
                    os.write(master, (USER + "\r").encode())
                    sent = True
                autosave = root / ".dsco" / "sessions" / "_autosave.json"
                if autosave.exists():
                    try:
                        retained = json.loads(autosave.read_text())
                    except ValueError:
                        pass
                final_saved = any(block.get("type") == "text" and FINAL in (block.get("text") or "")
                                  for block in saved_blocks(retained))
                if state["final_sent"]:
                    final_sent_at = final_sent_at or now
                    assert final_saved or now - final_sent_at < 3, "idle autosave omitted final answer"
                if final_saved:
                    finished_at = finished_at or now
                    if now - finished_at > .5 and not quit_sent:
                        os.write(master, b"/quit\r")
                        quit_sent = True
                if proc.poll() is not None:
                    break
            assert proc.poll() == 0 and quit_sent, (mode, proc.poll(), len(requests),
                                                   budget_events(root), bytes(transcript[-3000:]))
            assert len(requests) == 4 and state["final_sent"]
            events = budget_events(root)
            admitted = [row for row in events if row["admitted"]]
            rejected = [row for row in events if not row["admitted"]]
            assert len(admitted) == 4 and len(rejected) == 1, events
            assert all(row["limit"] == 32768 for row in events), "default cap changed"
            assert all(row["after_tokens"] <= row["limit"] for row in admitted), events
            assert rejected[0]["after_tokens"] > rejected[0]["limit"], events
            # Recovery must happen before request 3, with no rejected wire sent.
            assert events[2] == rejected[0] and events[3]["admitted"], events
            assert USER in json.dumps(retained), "user instruction lost from autosave"
            recall = next(block for block in saved_blocks(retained)
                          if block.get("type") == "tool_result" and block.get("tool_id") == CALLS[2])
            archive_text = recall["text"].split("\n", 1)[1]
            archived, _ = json.JSONDecoder().raw_decode(archive_text)
            old_call = next(block for block in archived[0]["content"] if block.get("type") == "tool_use")
            old_result = next(block for block in archived[1]["content"] if block.get("type") == "tool_result")
            assert old_call["tool_id"] == old_result["tool_id"] == CALLS[0]
            assert old_call["tool_name"] == "write_file" and json.loads(old_call["tool_input"]) == args[0]
            assert "verified write" in old_result["text"] and str(files[0]) in old_result["text"]
            executed = [row["payload"] for row in full_events(root) if row.get("event_type") == "tool.call.created"]
            assert [row["tool_id"] for row in executed if row["tool_name"] == "write_file"] == list(CALLS[:2]), executed
            for path, content in zip(files, contents):
                assert path.read_text() == content
            if native:
                assert text_filter.graphics and snapshot.exists(), "native raster was not emitted"
            result.update(passed=True, requests=len(requests), events=events,
                          archive_key=state["archive_key"], archived_content_exact=True,
                          original_writes_executed_once=True, latest_arguments_preserved=True,
                          idle_final_saved=True, clean_quit=True, native_graphics=text_filter.graphics)
            return result
        except Exception as exc:
            result["error"] = repr(exc)
            raise
        finally:
            if proc and proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
            os.close(master)
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=3)
            if evidence:
                target = evidence / mode
                target.mkdir(parents=True, exist_ok=True)
                (target / "result.json").write_text(json.dumps(result, indent=2))
                (target / "requests.json").write_text(json.dumps(requests, indent=2))
                (target / "events.json").write_text(json.dumps(budget_events(root), indent=2))
                (target / "transcript.txt").write_bytes(transcript)
                if retained:
                    (target / "autosave.json").write_text(json.dumps(retained))
                if snapshot.exists():
                    shutil.copyfile(snapshot, target / snapshot.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("./dsco"))
    parser.add_argument("--mode", choices=("tui", "native", "both"), default="both")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    for native in (False, True) if args.mode == "both" else (args.mode == "native",):
        result = run_case(binary, native, args.output_dir)
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest, "binary changed during test"
        result["binary_sha256"] = digest
        print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
