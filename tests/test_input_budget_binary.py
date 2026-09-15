#!/usr/bin/env python3
"""Verify interactive wire budgets with a real binary and owned local SSE/PTys.

No inference, paid API, GUI control, or user terminal is involved. Child state
and its autosave are isolated in a temporary directory. Native mode checks
emitted graphics and saves its raster; it does not OCR or infer visible text.
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
import shutil
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time

from test_buffer_slash import TerminalText


SYSTEM = "OWNED_BUDGET_SYSTEM. Follow the user request."
USER = "OWNED_BUDGET_USER: read the owned fixture file, then finish."
RECOVERY = "OWNED_BUDGET_RECOVERED_USER: reply briefly."
CALL = "owned_budget_read_call"
MID = "OWNED_BUDGET_MIDDLE_MUST_REMAIN_IN_LOCAL_HISTORY"


def budget_events(directory):
    events = []
    for path in Path(directory, "chronicle", "events").rglob("*.jsonl"):
        for line in path.read_text().splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if event.get("event_type") == "request.input_budget":
                events.append(event["payload"])
    return events


def final_assistant_saved(autosave):
    if not autosave:
        return False
    return any(message.get("role") == "assistant" and
               any(block.get("type") == "text" and
                   "OWNED_BUDGET_FINISHED" in (block.get("text") or "")
                   for block in message.get("content", []))
               for message in autosave.get("messages", []))


def run_case(binary, native, case, evidence):
    requests, errors = [], []
    state = {"final_reply_sent": False}
    mode = "native" if native else "tui"
    with tempfile.TemporaryDirectory(prefix=f"dsco-budget-{mode}-{case}-") as tmp:
        root = Path(tmp)
        sample = root / "observation.txt"
        sample.write_text("OWNED_BUDGET_START\n" + ("x" * 95 + "\n") * 500 +
                          MID + "\n" + ("y" * 95 + "\n") * 500 + "OWNED_BUDGET_END\n")
        snapshot = root / "native.ppm"
        rejected_snapshot = root / "native-rejection.ppm"

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    assert self.path == "/v1/chat/completions", self.path
                    request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(request)
                    if case in ("projection", "capacity") and len(requests) == 1:
                        delta = {"tool_calls": [{"index": 0, "id": CALL, "type": "function",
                                 "function": {"name": "read_file", "arguments":
                                              json.dumps({"path": str(sample)})}}]}
                        finish = "tool_calls"
                    else:
                        delta = {"content": "OWNED_BUDGET_FINISHED"}
                        finish = "stop"
                    chunks = [dict(id="owned-budget", choices=[dict(index=0, delta=delta,
                                   finish_reason=None)]),
                              dict(id="owned-budget", choices=[dict(index=0, delta={},
                                   finish_reason=finish)],
                                   usage=dict(prompt_tokens=10, completion_tokens=5, cost=0))]
                    payload = ("".join("data: " + json.dumps(chunk) + "\n\n"
                                       for chunk in chunks) + "data: [DONE]\n\n").encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    if finish == "stop":
                        state["final_reply_sent"] = True
                except Exception as exc:
                    errors.append(repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        env = dict(HOME=tmp, TMPDIR=tmp, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   TERM="xterm-kitty" if native else "xterm-256color", COLORTERM="truecolor",
                   LANG="en_US.UTF-8", OPENAI_API_KEY="owned-local-fixture",
                   OPENAI_API_BASE=f"http://127.0.0.1:{server.server_port}/v1",
                   DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1",
                   DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_DISABLE_DEFAULT_FALLBACKS="1",
                   DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_NO_AUTO_SUPERVISE="1",
                   DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0",
                   DSCO_MCP_HEADLESS="0", DSCO_BANNER="0", DSCO_KITTY_BANNER="0",
                   DSCO_TOOL_PROXY="1", DSCO_TOOL_ALLOWLIST="read_file", DSCO_AUTO_GOAL="0",
                   DSCO_AUTO_COMPACT="0", DSCO_TUI_ANIMATIONS="0",
                   DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_PIXEL_TUI_DPR="1",
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot),
                   DSCO_CHRONICLE_DIR=str(root / "chronicle"), DSCO_CHRONICLE_MODE="full-local",
                   DSCO_SYSTEM_PROMPT=SYSTEM + (" z" * (50000 if case == "capacity" else 30000)
                                               if case in ("system", "capacity") else ""),
                   DSCO_HARD_TURN_CEILING="4")
        if not native:
            env["DSCO_KITTY_GRAPHICS"] = "0"
        if case == "projection":
            env["DSCO_MAX_INPUT_TOKENS"] = "32768"
        elif case != "capacity":
            env["DSCO_MAX_INPUT_TOKENS"] = "8000"
        initial = "OWNED_PROTECTED_USER " + "雪" * 4000 if case in ("user", "auto") else USER
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 1200, 800))
        proc = subprocess.Popen([str(binary), "--profile", "worker",
                                 "--native" if native else "--tui", "-i",
                                 "--provider", "openai", "-m", "fixture-model",
                                 "--trust-tier", "trusted", "--approval-mode", "never"],
                                cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
                                start_new_session=True)
        os.close(slave)
        text_filter, transcript, raw_tail = TerminalText(), bytearray(), bytearray()
        started = time.monotonic()
        ready_at = rejection_at = clear_at = finished_at = response_received_at = None
        sent = recovered = quit_sent = False
        retained = None
        events = []
        try:
            while time.monotonic() - started < 35:
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
                    assert len(transcript) < 4 * 1024 * 1024, "PTY text limit"
                now = time.monotonic()
                if ready_at and not sent and now - ready_at > .15:
                    # A paste keeps the Unicode rejection fixture within the
                    # real composer cap without repainting once per byte.
                    payload = b"\x1b[200~" + initial.encode() + b"\x1b[201~\r"
                    for offset in range(0, len(payload), 4096):
                        os.write(master, payload[offset:offset + 4096])
                    sent = True
                events = budget_events(root)
                rejected = any(not event["admitted"] for event in events)
                if rejected:
                    rejection_at = rejection_at or now
                    if not recovered:
                        assert not requests, "protected input reached provider"
                autosave = root / ".dsco" / "sessions" / "_autosave.json"
                if autosave.exists():
                    try:
                        retained = json.loads(autosave.read_text())
                    except ValueError:
                        pass
                if rejection_at and now - rejection_at > .5 and clear_at is None:
                    if native and snapshot.exists():
                        shutil.copyfile(snapshot, rejected_snapshot)
                    if case in ("user", "auto"):
                        assert retained and initial in json.dumps(retained, ensure_ascii=False), \
                            "rejected user input not retained locally"
                        os.write(master, b"/input-budget auto\r" if case == "auto" else b"/clear\r")
                        clear_at = now
                    elif not quit_sent:
                        os.write(master, b"/quit\r")
                        quit_sent = True
                        clear_at = now
                if case in ("user", "auto") and clear_at and not recovered and now - clear_at > .4:
                    os.write(master, (RECOVERY + "\r").encode())
                    recovered = True
                expected_requests = 2 if case in ("projection", "capacity") else 1
                response_received = len(requests) == expected_requests and state["final_reply_sent"]
                if response_received:
                    response_received_at = response_received_at or now
                    assert final_assistant_saved(retained) or now - response_received_at < 3, \
                        "idle autosave omitted final assistant response before the next input"
                completed = response_received and final_assistant_saved(retained)
                if completed and case != "system":
                    finished_at = finished_at or now
                    if now - finished_at > .5 and not quit_sent:
                        os.write(master, b"/quit\r")
                        quit_sent = True
                if proc.poll() is not None:
                    break
            assert proc.poll() == 0 and quit_sent, (case, mode, proc.poll(), events,
                                                   bytes(transcript[-3000:]))
            assert not errors, errors
            events = budget_events(root)
            admitted = [event for event in events if event["admitted"]]
            assert len(admitted) == len(requests), (events, len(requests))
            assert all(event["after_tokens"] <= event["limit"] for event in admitted), events
            if case == "capacity":
                assert len(requests) == 2 and len(events) == 2, (len(requests), events)
                assert all(event["limit"] > 32768 for event in events), events
                assert events[0]["before_tokens"] > 32768, "protected instructions must exceed old fixed default"
                assert events[1]["before_tokens"] > 32768 and not events[1]["reduced_fields"], events
                assert MID in json.dumps(requests[1]), "model capacity default discarded tool evidence"
                assert all(SYSTEM in json.dumps(req) and USER in json.dumps(req) for req in requests)
                assert final_assistant_saved(retained)
            elif case == "projection":
                assert len(requests) == 2 and len(events) == 2, (len(requests), events)
                assert all(event["limit"] == 32768 for event in events), events
                assert events[1]["before_tokens"] > 32768 and events[1]["reduced_fields"] > 0, events
                messages = requests[1]["messages"]
                output = next(m for m in messages if m.get("role") == "tool")
                assert output["tool_call_id"] == CALL and "request input budget" in output["content"]
                assert MID not in output["content"], "wire result was not shortened"
                assert retained and MID in json.dumps(retained), "full local result missing"
                assert final_assistant_saved(retained), "idle autosave lost final assistant response"
                assert all(SYSTEM in json.dumps(req) and USER in json.dumps(req) for req in requests)
                call = next(call for message in messages for call in message.get("tool_calls", []))
                assert call["id"] == CALL and json.loads(call["function"]["arguments"])["path"] == str(sample)
            else:
                assert events and not events[0]["admitted"] and events[0]["limit"] == 8000, events
                if not native:
                    assert b"protected input exceeds request budget" in transcript, bytes(transcript[-3000:])
                if case in ("user", "auto"):
                    assert recovered and len(requests) == 1
                    assert RECOVERY in json.dumps(requests[0])
                    assert ("雪" in json.dumps(requests[0], ensure_ascii=False)) == (case == "auto")
                    if case == "auto":
                        assert events[-1]["limit"] > 32768, events
                else:
                    assert not requests
            if native:
                assert text_filter.graphics and snapshot.exists(), "native renderer did not produce a raster"
            result = dict(mode=mode, case=case, requests=len(requests), events=events,
                          clean_quit=True, native_graphics=text_filter.graphics,
                          idle_final_assistant_saved=final_assistant_saved(retained) if requests else None)
            return result
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
            os.close(master)
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=3)
            if evidence:
                target = evidence / f"{mode}-{case}"
                target.mkdir(parents=True, exist_ok=True)
                (target / "requests.json").write_text(json.dumps(requests, indent=2, ensure_ascii=False))
                (target / "events.json").write_text(json.dumps(events, indent=2))
                (target / "transcript.txt").write_bytes(transcript)
                if retained:
                    (target / "autosave.json").write_text(json.dumps(retained, ensure_ascii=False))
                for source in (snapshot, rejected_snapshot):
                    if source.exists():
                        shutil.copyfile(source, target / source.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./dsco")
    parser.add_argument("--mode", choices=("tui", "native", "both"), default="both")
    parser.add_argument("--case", choices=("capacity", "projection", "user", "auto", "system", "all"), default="all")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    binary = Path(args.binary).resolve(strict=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    modes = (False, True) if args.mode == "both" else (args.mode == "native",)
    cases = ("capacity", "projection", "user", "auto", "system") if args.case == "all" else (args.case,)
    for native in modes:
        for case in cases:
            result = run_case(binary, native, case, args.output_dir)
            result["binary_sha256"] = digest
            assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest, "binary changed during test"
            print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
