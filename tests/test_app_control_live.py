#!/usr/bin/env python3
"""Prove background app control against an owned, never-activated Cocoa fixture.

    python3 tests/test_app_control_live.py --binary ./dsco
    python3 tests/test_app_control_live.py --binary ./dsco --vision

Every action goes through real `dsco mcp serve`. The test asserts the user's
pointer position and frontmost application are unchanged afterwards, and that
each effect reached the fixture (stdout events), not whatever app has focus.

--vision also sends the fixture window image (synthetic content only) to
TypeSafe and needs TYPESAFE_API_KEY. That one call carries the screen pixels
off the machine, so the gate requires the operator override DSCO_ALLOW_EXFIL=1;
this flag sets it for the dsco subprocess only.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

from test_harness_surfaces_mcp import MCP, ROOT, require


def frontmost_pid() -> int:
    front =subprocess.run(["/usr/bin/lsappinfo", "front"], capture_output=True, text=True, timeout=5).stdout.strip()
    out = subprocess.run(["/usr/bin/lsappinfo", "info", "-only", "pid", front],
                         capture_output=True, text=True, timeout=5).stdout
    digits = "".join(ch for ch in out.split("=")[-1] if ch.isdigit())
    return int(digits) if digits else -1


class Fixture:
    def __init__(self, executable: Path):
        self.proc = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)
        self.pending = bytearray()
        self.identity = self.event(10)
        require(self.identity.get("pid") == self.proc.pid, "fixture identity mismatch")

    def event(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            require(remaining > 0, "fixture event timeout")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            require(ready, "fixture event timeout")
            chunk = os.read(self.proc.stdout.fileno(), 8192)
            require(chunk, "fixture exited")
            self.pending.extend(chunk)
        line, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        return json.loads(line)

    def expect(self, predicate, what: str, timeout: float = 4.0) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            try:
                event = self.event(max(0.01, deadline - time.monotonic()))
            except AssertionError as error:
                raise AssertionError(f"fixture never reported {what} ({error})") from error
            if predicate(event):
                return event
            require(time.monotonic() < deadline, f"fixture never reported {what}")

    def close(self) -> dict:
        closed = {}
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write(b"q\n")
                self.proc.stdin.flush()
                closed = self.expect(lambda e: e.get("event") == "closed", "close", 4)
            except (AssertionError, BrokenPipeError):
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)
        return closed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "dsco")
    parser.add_argument("--vision", action="store_true")
    args = parser.parse_args()
    require(sys.platform == "darwin", "requires macOS")
    if args.vision:
        require(os.environ.get("TYPESAFE_API_KEY"), "--vision needs TYPESAFE_API_KEY")
    evidence: dict = {}
    with tempfile.TemporaryDirectory(prefix="dsco-app-control-") as temporary:
        directory = Path(temporary)
        executable = directory / "app-control-fixture"
        built = subprocess.run(["cc", "-fobjc-arc", "-Wall", "-Wextra", "-framework", "Cocoa",
                                str(ROOT / "tests" / "app_control_fixture.m"), "-o", str(executable)],
                               capture_output=True, text=True, timeout=60)
        require(built.returncode == 0, "fixture build failed: " + built.stderr[-1200:])
        overrides = {"DSCO_VISION_GROUND": "1" if args.vision else "0"}
        if args.vision:
            overrides["DSCO_ALLOW_EXFIL"] = "1"
        with MCP(args.binary.resolve(), directory, overrides) as mcp:
            front_before = frontmost_pid()
            cursor_before, _, _ = mcp.call("computer", {"action": "cursor_position"})
            fixture = Fixture(executable)
            pid = fixture.identity["pid"]
            try:
                time.sleep(0.3)
                require(frontmost_pid() != pid, "fixture activated itself; test cannot prove background control")

                listing, _, _ = mcp.call("app_control", {"action": "apps"})
                require(any(w["pid"] == pid for w in listing["windows"]), "apps did not list fixture")

                seen, _, _ = mcp.call("app_control", {"action": "observe", "pid": pid})
                labels = {e.get("label") for e in seen["elements"]}
                require({"Apply Changes", "Send receipt"} <= labels, f"observe missed controls: {sorted(filter(None, labels))}")
                evidence["observed_elements"] = seen["count"]

                found, _, _ = mcp.call("app_control", {"action": "find", "pid": pid, "query": "Apply Changes button"})
                require(found["best"] and found["candidates"][0]["label"] == "Apply Changes", f"find: {found}")
                mcp.call("app_control", {"action": "press", "pid": pid, "element": found["best"]})
                fixture.expect(lambda e: e.get("control") == "apply", "Apply press")
                evidence["press"] = True

                found, _, _ = mcp.call("app_control", {"action": "find", "pid": pid, "query": "send receipt checkbox"})
                mcp.call("app_control", {"action": "press", "pid": pid, "element": found["best"]})
                fixture.expect(lambda e: e.get("event") == "checkbox" and e.get("state") in (True, 1), "checkbox toggle")
                evidence["checkbox"] = True

                found, _, _ = mcp.call("app_control", {"action": "find", "pid": pid, "query": "recipient name field"})
                field = found["best"]
                require(field, f"text field not found: {found}")
                result, _, _ = mcp.call("app_control", {"action": "set_value", "pid": pid, "element": field,
                                                        "text": "Ada Lovelace"})
                require(result.get("verified"), f"set_value not verified: {result}")
                evidence["set_value"] = result["value_after"]

                mcp.call("app_control", {"action": "menu", "pid": pid, "path": ["Fixture", "Reset Counter..."]})
                fixture.expect(lambda e: e.get("event") == "menu", "menu item")
                evidence["menu"] = True

                mcp.call("app_control", {"action": "focus", "pid": pid, "element": field})
                keyed, _, _ = mcp.call("app_control", {"action": "key", "pid": pid, "keys": "end"})
                require(keyed.get("verified") is False, "key must not claim unverified delivery")
                typed = " π 🦉"
                result, _, _ = mcp.call("app_control", {"action": "type", "pid": pid, "element": field, "text": typed})
                require(result.get("verified"), f"type not verified: {result}")
                event = fixture.expect(lambda e: e.get("event") == "text" and e.get("text") == "Ada Lovelace" + typed,
                                       "typed text in background", 6)
                evidence["typed_background"] = event["text"]

                # Window pixels may show secrets, so the gate treats a screenshot as
                # private-data access; later input would need DSCO_ALLOW_EXFIL=1.
                captured, content, _ = mcp.call("app_control", {"action": "screenshot", "pid": pid})
                require(captured.get("screenshot_attached") and
                        any(item.get("type") == "image" for item in content["content"]),
                        "app screenshot was not attached")
                evidence["screenshot_after_input"] = True

                if args.vision:
                    found, content, _ = mcp.call("app_control", {"action": "find", "pid": pid,
                                                                 "query": "the orange star icon"})
                    vision = found.get("vision")
                    require(isinstance(vision, dict) and vision.get("element"), f"vision fallback failed: {found}")
                    evidence["vision_confidence"] = vision["confidence"]
                    mcp.call("app_control", {"action": "press", "pid": pid, "element": vision["element"]["id"]})
                    fixture.expect(lambda e: e.get("control") == "star", "vision-located press")
                    evidence["vision_press"] = True

                cursor_after, _, _ = mcp.call("computer", {"action": "cursor_position"})
                front_after = frontmost_pid()
                require((cursor_after["x"], cursor_after["y"]) == (cursor_before["x"], cursor_before["y"]),
                        f"pointer moved: {cursor_before} -> {cursor_after}")
                require(front_after == front_before, f"frontmost app changed: {front_before} -> {front_after}")
                evidence.update(pointer_unchanged=True, focus_unchanged=True)
            finally:
                closed = fixture.close()
                evidence["fixture_was_frontmost_at_exit"] = closed.get("was_frontmost")
    require(evidence.get("fixture_was_frontmost_at_exit") in (False, 0), "fixture became frontmost")
    print("PASS: background app_control observe/find/press/checkbox/set_value/menu/focus/key/type"
          + ("/vision" if args.vision else "") + " with pointer and focus untouched")
    print(json.dumps(evidence, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {str(error)[:1500]}", file=sys.stderr)
        raise SystemExit(1)
