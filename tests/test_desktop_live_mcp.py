#!/usr/bin/env python3
"""Prove native desktop mutation against one owned Cocoa fixture via real MCP.

    python3 tests/test_desktop_live_mcp.py --binary ./dsco
    python3 tests/test_desktop_live_mcp.py --binary ./dsco --type-text 'Astra π 🦉'

Consecutive mutations are immediate to catch CG/AX compositor races.

The optional typing check must target the fixture's verified focused window.
Only this fixture is listed, inspected or mutated. No screenshots, permission
requests, HOME overrides or keychain operations occur. Temporary build output
and fixture cleanup are confined to resources this test creates.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

from test_harness_surfaces_mcp import MCP, ROOT, require


class Fixture:
    def __init__(self, executable: Path, directory: Path):
        self.errors = tempfile.TemporaryFile(dir=directory)
        self.proc = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.errors)
        self.pending = bytearray()
        self.restoration = None
        try:
            self.identity = self.read_event(10)
        except BaseException as error:
            if self.proc.poll() is None:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
            self.errors.seek(0)
            detail = self.errors.read().decode(errors="replace")[-1200:]
            self.proc.stdout.close()
            self.errors.close()
            raise AssertionError(f"fixture startup: {error}; exit={self.proc.returncode}; {detail}") from error
        require(self.identity.get("pid") == self.proc.pid and self.identity.get("window_id", 0) > 0,
                "fixture did not report its own PID/window identity")

    def read_event(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            require(remaining > 0, "fixture event timeout")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            require(ready, "fixture event timeout")
            chunk = os.read(self.proc.stdout.fileno(), 8192)
            require(chunk, "fixture exited without expected event")
            self.pending.extend(chunk)
        line, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        return json.loads(line)

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.stdin.write(b"quit\n")
            self.proc.stdin.flush()
            self.proc.stdin.close()
            try:
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    try:
                        event = self.read_event(max(0.01, deadline - time.monotonic()))
                    except AssertionError:
                        # Bare AppKit may terminate without a delegate ack.
                        # Process reap below is the authoritative cleanup check.
                        break
                    if event.get("event") == "closed":
                        self.restoration = event
                        break
                self.proc.wait(timeout=3)
            except (AssertionError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait(timeout=3)
                raise
        self.proc.stdout.close()
        self.errors.close()
        require(self.proc.returncode == 0, "owned Cocoa fixture did not exit cleanly")


def near(actual: dict, expected: dict) -> bool:
    return all(abs(actual.get(key, float("inf")) - value) <= 1.0 for key, value in expected.items())


def roles(node: dict):
    yield node.get("role")
    for child in node.get("children", []):
        yield from roles(child)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "dsco")
    parser.add_argument("--type-text", default=None)
    args = parser.parse_args()
    require(sys.platform == "darwin", "live desktop fixture requires macOS")
    binary = args.binary.resolve(strict=True)
    evidence = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    with tempfile.TemporaryDirectory(prefix="dsco-desktop-live-") as temporary:
        directory = Path(temporary)
        executable = directory / "desktop-fixture"
        built = subprocess.run(["cc", "-fobjc-arc", "-Wall", "-Wextra", "-framework", "Cocoa",
                                str(ROOT / "tests" / "desktop_fixture.m"), "-o", str(executable)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        require(built.returncode == 0, "Cocoa fixture compilation failed: " + built.stderr.decode()[-1200:])
        with MCP(binary, directory) as mcp:
            status, _, _ = mcp.call("desktop", {"action": "status"})
            require(status.get("permission_prompted") is False, "status prompted for permission")
            require(status.get("accessibility_trusted") is True, "existing accessibility permission is required; test did not request it")
            fixture = Fixture(executable, directory)
            try:
                target = {"pid": fixture.identity["pid"], "window_id": fixture.identity["window_id"]}
                evidence["fixture"] = target
                for attempt in range(20):
                    listing, _, _ = mcp.call("desktop", {"action": "list", "pid": target["pid"], "include_titles": True, "limit": 8})
                    if any(window["window_id"] == target["window_id"] for window in listing["windows"]):
                        break
                    time.sleep(0.1)
                require(all(window["pid"] == target["pid"] for window in listing["windows"]), "desktop list escaped fixture PID filter")
                require(any(window["window_id"] == target["window_id"] for window in listing["windows"]), "fixture window not listed")
                inspected, _, _ = mcp.call("desktop", {"action": "inspect", **target})
                require(inspected.get("accessibility_available") and inspected.get("movable") and inspected.get("resizable"), "fixture AX window not resolved")
                initial = inspected["window"]["bounds"]
                snapshot, _, _ = mcp.call("desktop", {"action": "snapshot", **target, "max_depth": 4, "max_nodes": 32})
                require("AXTextField" in set(roles(snapshot["tree"])), "fixture AX text field missing")
                focused, _, _ = mcp.call("desktop", {"action": "focus", **target})
                require(focused.get("verified"), "fixture focus was not verified")
                moved = {"x": initial["x"] + 24, "y": initial["y"] + 18}
                result, _, _ = mcp.call("desktop", {"action": "move", **target, **moved})
                require(result.get("verified") and near(result["observed_bounds"], moved), "fixture move postcondition failed")
                resized = {"width": initial["width"] + 42, "height": initial["height"] + 28}
                result, _, _ = mcp.call("desktop", {"action": "resize", **target, **resized})
                require(result.get("verified") and near(result["observed_bounds"], resized), "fixture resize postcondition failed")
                result, _, _ = mcp.call("desktop", {"action": "set_bounds", **target, **initial})
                require(result.get("verified") and near(result["observed_bounds"], initial), "fixture set_bounds restore failed")
                inspected, _, _ = mcp.call("desktop", {"action": "inspect", **target})
                require(inspected.get("accessibility_available") and near(inspected["window"]["bounds"], initial), "independent fixture bounds readback failed")
                evidence.update(list=True, inspect=True, snapshot_nodes=snapshot["nodes"], focus=True,
                                move=True, resize=True, set_bounds=True, bounds_restored=True)
                if args.type_text is not None:
                    require(len(args.type_text.encode()) <= 256 and "\n" not in args.type_text,
                            "typing fixture input must be short single-line text")
                    mcp.call("desktop", {"action": "focus", **target})
                    checked, _, _ = mcp.call("desktop", {"action": "inspect", **target})
                    require(checked.get("focused") is True, "refusing to type without exact fixture focus")
                    _, content, _ = mcp.call("computer", {"action": "type", "text": args.type_text, "screenshot": False})
                    require(not any(item.get("type") == "image" for item in content["content"]), "computer ignored screenshot:false")
                    deadline = time.monotonic() + 4
                    while True:
                        event = fixture.read_event(max(0.01, deadline - time.monotonic()))
                        if event.get("event") == "text" and event.get("text") == args.type_text:
                            break
                        require(time.monotonic() < deadline, "typed text did not reach owned fixture exactly")
                    evidence["typed_text_verified"] = args.type_text
                mcp.ping()
                evidence["json_rpc_responses"] = mcp.responses
            finally:
                fixture.close()
                evidence["fixture_cleanup"] = fixture.restoration
    evidence["keychain_operations"] = 0
    print("PASS: real MCP native fixture list/inspect/AX/focus/move/resize/set_bounds and cleanup")
    print(json.dumps(evidence, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {str(error)[:1200]}", file=sys.stderr)
        raise SystemExit(1)
