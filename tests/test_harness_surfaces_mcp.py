#!/usr/bin/env python3
"""Exercise real governed harness surfaces over the shipped MCP transport.

    python3 tests/test_harness_surfaces_mcp.py [--binary ./dsco]

No LLM requests, user browser profiles, live Kitty lifecycle actions, desktop
permission prompts, HOME overrides or keychain operations. Chrome is an owned
headless/offline instance. Files, PTYs and subprocesses belong to this test.
Environment changes apply only to the newly launched MCP subprocesses.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib


ROOT = Path(__file__).resolve().parents[1]
TOOLSETS = "core,terminal,desktop,browser"


def require(condition: object, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class MCP:
    def __init__(self, binary: Path, directory: Path, overrides: dict | None = None):
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("DSCO_ALLOW_") or key in {
                "DSCO_GOV_BYPASS", "DSCO_GOV_MODEL", "DSCO_MCP_GOV_PINNED"
            }:
                env.pop(key)
        env.update(DSCO_GOV_MODEL="standard", DSCO_PRICING_OFFLINE="1")
        env.update(overrides or {})
        require(env.get("HOME") == os.environ.get("HOME"), "test must preserve HOME")
        self.errors = tempfile.TemporaryFile(dir=directory)
        self.proc = subprocess.Popen(
            [str(binary), "mcp", "serve", "--toolsets", TOOLSETS, "--tier", "trusted"],
            cwd=directory, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errors, start_new_session=True,
        )
        self.pending = bytearray()
        self.sequence = 0
        self.responses = 0
        self.owned_groups: set[int] = set()
        try:
            reply = self.rpc("initialize", {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "harness-surfaces-regression", "version": "1"},
            })
            require(reply.get("result", {}).get("serverInfo", {}).get("name") == "dsco",
                    "real MCP initialize did not identify dsco")
            self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            self.ping()
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def send(self, value: dict) -> None:
        require(self.proc.poll() is None, "MCP process exited before request")
        self.proc.stdin.write(json.dumps(value, ensure_ascii=False).encode() + b"\n")
        self.proc.stdin.flush()

    def rpc(self, method: str, params: dict | None = None, timeout: float = 35) -> dict:
        self.sequence += 1
        request = {"jsonrpc": "2.0", "id": self.sequence, "method": method}
        if params is not None:
            request["params"] = params
        self.send(request)
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            require(remaining > 0, f"MCP timeout during {method}")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            require(ready, f"MCP timeout during {method}")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            require(chunk, f"MCP stdout closed during {method}")
            self.pending.extend(chunk)
            require(len(self.pending) <= 32 * 1024 * 1024, "MCP response exceeded test bound")
        line, _, remaining = self.pending.partition(b"\n")
        self.pending = bytearray(remaining)
        try:
            reply = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise AssertionError(f"non-JSON output corrupted MCP during {method}") from exc
        require(reply.get("jsonrpc") == "2.0" and reply.get("id") == self.sequence,
                f"unexpected MCP frame during {method}")
        self.responses += 1
        return reply

    def ping(self) -> None:
        require(self.rpc("ping").get("result") == {}, "MCP ping framing failed")

    def call(self, tool: str, args: dict, expected: bool = True) -> tuple[dict, dict, str]:
        reply = self.rpc("tools/call", {"name": tool, "arguments": args})
        envelope = reply.get("result", {})
        text = "\n".join(block.get("text", "") for block in envelope.get("content", [])
                         if block.get("type") == "text")
        if "error" in reply:
            text = str(reply["error"].get("message", "MCP error"))
        ok = "error" not in reply and not envelope.get("isError", False)
        action = args.get("action", args.get("command", ""))
        require(ok == expected,
                f"{tool} {action}: expected {'success' if expected else 'denial'}, "
                f"received {'success' if ok else text[:360]}")
        try:
            body = json.loads(text)
        except json.JSONDecodeError:
            body = {"text": text}
        if not isinstance(body, dict):
            body = {"value": body}
        if expected:
            require(body.get("ok", True) is not False, f"{tool} {action} reported failure")
        return body, envelope, text

    def track(self, pid: int) -> None:
        require(pid > 1 and pid != os.getpgrp(), "invalid owned process identity")
        self.owned_groups.add(pid)

    def close(self) -> None:
        if not self.proc.stdin.closed:
            self.proc.stdin.close()
        forced = False
        try:
            self.proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            forced = True
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        # Fallback is restricted to process groups returned by our spawn calls.
        # Successful close removes each group from this set immediately.
        if forced:
            for group in self.owned_groups:
                try:
                    os.killpg(group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        self.proc.stdout.close()
        self.errors.close()
        require(not forced and self.proc.returncode == 0,
                "MCP failed to exit cleanly and release its owned sessions")


def png_fixture() -> bytes:
    def chunk(name: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data) & 0xFFFFFFFF)
    width, height = 16, 12
    rows = b"".join(b"\0" + bytes((30, 160, 190)) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def image_block(envelope: dict, expected: bytes | None = None) -> bytes:
    images = [block for block in envelope.get("content", []) if block.get("type") == "image"]
    require(len(images) == 1, "expected exactly one real MCP image content block")
    require(images[0].get("mimeType") == "image/png", "unexpected MCP image MIME type")
    data = base64.b64decode(images[0]["data"], validate=True)
    require(data.startswith(b"\x89PNG\r\n\x1a\n"), "MCP image is not a PNG")
    if expected is not None:
        require(data == expected, "view_image changed the owned fixture bytes")
    return data


def collect_text(mcp: MCP, session: str, needle: str) -> str:
    text = ""
    for _ in range(8):
        body, _, _ = mcp.call("pty_session", {
            "action": "read", "session_id": session, "timeout_ms": 500,
        })
        raw = base64.b64decode(body["output_base64"], validate=True)
        require(raw.decode() == body["output"], "PTY UTF-8 text/base64 disagreement")
        text += body["output"]
        if needle in text:
            return text
    raise AssertionError(f"PTY did not produce fixture marker {needle!r}")


def verify_pty(mcp: MCP, directory: Path) -> dict:
    helper = directory / "pty_fixture.py"
    helper.write_text('''import fcntl, os, struct, sys, termios, tty
tty.setraw(0)
os.write(1, b"PTY-READY\\n")
line = bytearray()
while True:
    data = os.read(0, 1)
    if not data or data == b"\\x04": break
    if data != b"\\n":
        line.extend(data)
        continue
    if line == b"size":
        rows, cols, _, _ = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\\0" * 8))
        os.write(1, ("SIZE:%dx%d\\n" % (cols, rows)).encode())
    else:
        os.write(1, b"ECHO:" + line + b"\\n")
    line.clear()
''')
    body, _, _ = mcp.call("pty_session", {
        "action": "spawn", "command": sys.executable,
        "args": ["-u", str(helper)], "cwd": str(directory), "ttl_seconds": 30,
    })
    session, pid = body["session_id"], body["pid"]
    mcp.track(pid)
    require(body["surface_id"] == "pty:" + session, "PTY surface identity mismatch")
    collect_text(mcp, session, "PTY-READY")
    listing, _, _ = mcp.call("pty_session", {"action": "list"})
    require(any(item["session_id"] == session for item in listing["sessions"]), "live PTY missing from registry")
    mcp.call("pty_session", {"action": "write", "session_id": session, "input": "Astra π 🦉\n"})
    collect_text(mcp, session, "ECHO:Astra π 🦉")
    mcp.call("pty_session", {"action": "resize", "session_id": session, "cols": 142, "rows": 46})
    mcp.call("pty_session", {"action": "write", "session_id": session, "input": "size\n"})
    collect_text(mcp, session, "SIZE:142x46")
    body, _, _ = mcp.call("pty_session", {"action": "wait", "session_id": session, "timeout_ms": 10})
    require(body["timed_out"] and not body["reaped"], "observational wait killed the PTY")
    mcp.call("pty_session", {"action": "write", "session_id": session, "input": "\x04"})
    body, _, _ = mcp.call("pty_session", {"action": "wait", "session_id": session, "timeout_ms": 2000})
    require(body["eof"] and body["reaped"] and body["exit_code"] == 0, "PTY EOF or exit code lost")
    body, _, _ = mcp.call("pty_session", {"action": "read", "session_id": session, "offset": 0})
    require("Astra π 🦉" in body["output"] and "SIZE:142x46" in body["output"], "PTY replay lost output")
    mcp.call("pty_session", {"action": "close", "session_id": session})
    mcp.owned_groups.discard(pid)
    body, _, _ = mcp.call("pty_session", {"action": "spawn", "command": "/bin/sleep", "args": ["30"], "ttl_seconds": 30})
    mcp.track(body["pid"])
    closed, _, _ = mcp.call("pty_session", {"action": "close", "session_id": body["session_id"]})
    require(closed["reaped"] and closed["state"] == "closed", "PTY cancellation did not reap child")
    mcp.owned_groups.discard(body["pid"])
    return {"unicode": True, "resize": [142, 46], "eof": True, "cancel_reaped": True}


def verify_browser(mcp: MCP) -> dict:
    body, _, _ = mcp.call("browser_session", {"action": "launch", "headless": True, "offline": True})
    session, tab = body["session_id"], body["result"]["tabs"][0]["tab_id"]
    state, _, _ = mcp.call("browser_session", {"action": "status"})
    require(state["result"]["owned"] and state["result"]["offline"] and state["result"]["headless"],
            "browser did not launch owned, offline and headless")
    pid = state["result"]["process_id"]
    mcp.track(pid)

    def call(action: str, **kwargs):
        args = {"action": action, "session_id": session, **kwargs}
        if action in {"navigate", "snapshot", "type", "click", "evaluate", "screenshot"}:
            args["tab_id"] = tab
        return mcp.call("browser_session", args)

    html = '''<!doctype html><html><head><title>MCP owned fixture</title></head><body>
<h1>Harness surface proof</h1><label for="name">Name</label><input id="name">
<button id="go" onclick="document.getElementById('answer').textContent='Hello '+document.getElementById('name').value">Go</button>
<output id="answer"></output><p>Unicode: café 中文 🦉</p></body></html>'''
    url = "data:text/html;charset=utf-8;base64," + base64.b64encode(html.encode()).decode()
    body, _, _ = call("navigate", url=url)
    require(body["result"]["title"] == "MCP owned fixture", "data fixture navigation failed")
    body, _, _ = call("snapshot")
    dom = body["result"]["dom"]
    require("Harness surface proof" in dom["text"], "browser DOM text missing")
    require(any(item["selector"] == "#name" for item in dom["elements"]), "browser DOM locator missing")
    require(any(item.get("role") == "button" and item.get("name") == "Go"
                for item in body["result"]["accessibility"]), "browser accessibility button missing")
    call("type", selector="#name", text="Astra 中文")
    call("click", selector="#go")
    body, _, _ = call("evaluate", expression="document.querySelector('#answer').textContent")
    require(body["result"] == "Hello Astra 中文", "browser type/click postcondition failed")
    _, envelope, _ = call("screenshot")
    png = image_block(envelope)
    width, height = struct.unpack(">II", png[16:24])
    require(width > 100 and height > 100 and len(png) > 1000, "browser screenshot is not a real viewport")
    call("close")
    mcp.owned_groups.discard(pid)
    state, envelope, _ = mcp.call("browser_session", {"action": "status"})
    require(not state["result"]["active"], "owned browser survived close")
    require(not any(block.get("type") == "image" for block in envelope["content"]), "image content leaked into next MCP response")
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError("owned Chrome PID was not reaped")
    return {"dom": True, "accessibility": True, "type_click": True, "png_bytes": len(png), "viewport": [width, height]}


def verify_denials(binary: Path, directory: Path, synthetic: Path) -> dict:
    checks = 0
    for setting, tool, arguments in [
        ("DSCO_ALLOW_RUN", "pty_session", {"action": "spawn", "command": "/bin/echo", "args": ["denied-probe"]}),
        ("DSCO_ALLOW_WRITE", "pty_session", {"action": "spawn", "command": "/bin/echo", "args": ["denied-probe"]}),
        ("DSCO_ALLOW_SECRETS", "kitty_remote", {"command": "get-text", "to": "unix:" + str(directory / "no-kitty.sock")}),
    ]:
        with MCP(binary, directory, {setting: "0"}) as mcp:
            _, _, text = mcp.call(tool, arguments, expected=False)
            require(setting in text, f"denial did not identify {setting}")
            status, _, _ = mcp.call("desktop", {"action": "status"})
            require(status["permission_prompted"] is False, "desktop status prompted during lockdown")
            mcp.ping()
            checks += 1
    # A known fixture supplies each taint leg; no actual credential is read.
    # Its .aws/credentials-shaped path intentionally exercises the classifier.
    with MCP(binary, directory) as mcp:
        body, _, _ = mcp.call("pty_session", {"action": "spawn", "command": "/bin/echo", "args": ["owned-untrusted-fixture"]})
        mcp.track(body["pid"])
        mcp.call("pty_session", {"action": "wait", "session_id": body["session_id"], "timeout_ms": 2000})
        # Close before adding the private-data leg; thereafter exec must deny.
        mcp.call("pty_session", {"action": "close", "session_id": body["session_id"]})
        mcp.owned_groups.discard(body["pid"])
        mcp.call("read_file", {"path": str(synthetic)})
        _, _, text = mcp.call("pty_session", {"action": "spawn", "command": "/bin/echo", "args": ["must-be-denied"]}, expected=False)
        require("DSCO_ALLOW_EXFIL" in text or "trifecta" in text.lower(), "trifecta egress denial missing")
        status, _, _ = mcp.call("desktop", {"action": "status"})
        require(status["permission_prompted"] is False, "status failed after taint")
        mcp.ping()
        checks += 1
    return {"hard_denials": checks - 1, "trifecta_denial": True}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "dsco")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    started = time.monotonic()
    evidence = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    with tempfile.TemporaryDirectory(prefix="dsco-harness-mcp-") as temporary:
        directory = Path(temporary)
        image_bytes = png_fixture()
        image_path = directory / 'owned"image.png'
        image_path.write_bytes(image_bytes)
        cli_env = os.environ.copy()
        cli_env["DSCO_TRUST_TIER"] = "trusted"
        for flag in ("--tool-exec", "--tool-exec-raw"):
            cli = subprocess.run([str(binary), flag, "view_image", json.dumps({"path": str(image_path)})],
                                 env=cli_env, capture_output=True, timeout=20)
            require(cli.returncode == 0, f"{flag} image call failed")
            body = json.loads(cli.stdout)
            require(body.get("ok"), f"{flag} image call returned an error")
            image_block(body, image_bytes)
        evidence["direct_cli_image_paths"] = ["--tool-exec", "--tool-exec-raw"]
        synthetic = directory / ".aws" / "credentials"
        synthetic.parent.mkdir()
        synthetic.write_text("Synthetic test fixture only; contains no credentials.\n")
        with MCP(binary, directory) as mcp:
            listing = mcp.rpc("tools/list").get("result", {}).get("tools", [])
            tools = {tool["name"]: tool for tool in listing}
            for name in ("surface", "kitty_remote", "kitten", "pty_session", "desktop", "browser_session", "view_image"):
                require(name in tools, f"shipped MCP toolset is missing {name}")
                require(tools[name].get("inputSchema", {}).get("type") == "object", f"{name} has no object schema")
            for name, actions in {
                "pty_session": {"spawn", "read", "write", "resize", "wait", "close"},
                "browser_session": {"launch", "snapshot", "type", "click", "evaluate", "screenshot", "close"},
                "desktop": {"status", "list", "snapshot", "focus", "move", "resize"},
            }.items():
                advertised = tools[name]["inputSchema"]["properties"]["action"]["enum"]
                require(actions.issubset(advertised), f"{name} action schema is incomplete")
            evidence["advertised_tools"] = len(tools)
            body, _, _ = mcp.call("kitten", {"command": "show-key", "args": ["--help"], "timeout_seconds": 5})
            require(body.get("exit_code") == 0 and "show-key" in body.get("output", ""), "captured kitten help failed")
            mcp.ping()
            status, _, _ = mcp.call("desktop", {"action": "status"})
            require(status.get("permission_prompted") is False, "desktop status requested permission")
            evidence["desktop"] = {key: status.get(key) for key in ("platform", "supported", "permission_prompted")}
            _, envelope, _ = mcp.call("view_image", {"path": str(image_path)})
            image_block(envelope, image_bytes)
            evidence["view_image_bytes"] = len(image_bytes)
            evidence["pty"] = verify_pty(mcp, directory)
            print("PASS: discovery, captured kitten framing, desktop status, real image content and persistent PTY", flush=True)
            evidence["browser"] = verify_browser(mcp)
            mcp.ping()
            evidence["main_session_responses"] = mcp.responses
            print("PASS: owned offline browser DOM/AX, type, click, evaluate, MCP PNG and cleanup", flush=True)
        evidence["governance"] = verify_denials(binary, directory, synthetic)
        print("PASS: RUN/WRITE/SECRETS hard denials and offline synthetic trifecta", flush=True)
    evidence["elapsed_seconds"] = round(time.monotonic() - started, 3)
    evidence["keychain_operations"] = 0
    print(json.dumps(evidence, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {str(error)[:600]}", file=sys.stderr)
        raise SystemExit(1)
