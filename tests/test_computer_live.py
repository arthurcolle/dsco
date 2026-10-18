#!/usr/bin/env python3
"""Prove the computer tool's screenshot coordinate space on the real display.

    python3 tests/test_computer_live.py --binary ./dsco

Clicks and types into an owned fixture using coordinates expressed in the
model-facing screenshot space, so a scaling error on a high-resolution display
misses the target. It captures the screenshot before input to prove the flow
gate keeps local UI actions usable, then checks zoom output. This drives the
real pointer briefly; the pointer position and frontmost app are restored.
"""
from __future__ import annotations

import argparse
import base64
import json
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from test_harness_surfaces_mcp import MCP, ROOT, require
from test_app_control_live import Fixture, frontmost_pid


def jpeg_size(data: bytes) -> tuple[int, int]:
    i = 2
    while i < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
    raise AssertionError("no JPEG frame header")


def activate(pid: int) -> None:
    subprocess.run(["/usr/bin/osascript", "-e",
                    f'tell application "System Events" to set frontmost of (first process whose unix id is {pid}) to true'],
                   capture_output=True, timeout=10)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "dsco")
    args = parser.parse_args()
    require(sys.platform == "darwin", "requires macOS")
    evidence: dict = {}
    with tempfile.TemporaryDirectory(prefix="dsco-computer-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        built = subprocess.run(["cc", "-fobjc-arc", "-framework", "Cocoa",
                                str(ROOT / "tests" / "app_control_fixture.m"), "-o", str(executable)],
                               capture_output=True, text=True, timeout=60)
        require(built.returncode == 0, built.stderr[-800:])
        with MCP(args.binary.resolve(), directory, {"DSCO_VISION_GROUND": "0"}) as mcp:
            front_before = frontmost_pid()
            start, _, _ = mcp.call("computer", {"action": "cursor_position"})
            shot_w, shot_h = start["screenshot_size"]
            disp_w, disp_h = start["display"]
            evidence.update(display=[disp_w, disp_h], screenshot_size=[shot_w, shot_h])
            require(shot_w <= 1280 and shot_h <= 768, f"screenshot space too large: {shot_w}x{shot_h}")
            require(abs(shot_w / shot_h - disp_w / disp_h) < 0.01, "screenshot aspect differs from display")

            shot, content, _ = mcp.call("computer", {"action": "screenshot"})
            images = [i for i in content["content"] if i.get("type") == "image"]
            require(len(images) == 1 and images[0].get("mimeType") == "image/jpeg", "screenshot not attached as JPEG")
            raw = base64.b64decode(images[0]["data"])
            require(jpeg_size(raw) == (shot_w, shot_h), f"screenshot is {jpeg_size(raw)}, expected {shot_w}x{shot_h}")
            evidence["screenshot_bytes"] = len(raw)

            fixture = Fixture(executable)
            pid = fixture.identity["pid"]
            try:
                time.sleep(0.4)
                seen, _, _ = mcp.call("app_control", {"action": "observe", "pid": pid})
                wx, wy, _, _ = seen["window_frame"]
                frames = {e.get("label"): e["frame"] for e in seen["elements"] if "frame" in e}

                def to_shot(frame):
                    fx, fy, fw, fh = frame
                    px, py = wx + fx + fw / 2, wy + fy + fh / 2
                    return [round(px * shot_w / disp_w), round(py * shot_h / disp_h)]

                target = to_shot(frames["Apply Changes"])
                mcp.call("computer", {"action": "left_click", "coordinate": target, "screenshot": False})
                fixture.expect(lambda e: e.get("control") == "apply", "click via screenshot coordinates")
                evidence["click_shot_coordinate"] = target

                field = to_shot(frames["Recipient name"])
                mcp.call("computer", {"action": "left_click", "coordinate": field, "screenshot": False})
                time.sleep(0.2)
                mcp.call("computer", {"action": "type", "text": "hi π", "screenshot": False})
                fixture.expect(lambda e: e.get("event") == "text" and e.get("text") == "hi π", "typed text")
                evidence["type"] = True

                mcp.call("computer", {"action": "mouse_move", "coordinate": target, "duration": 0,
                                      "screenshot": False})
                pos, _, _ = mcp.call("computer", {"action": "cursor_position"})
                require(abs(pos["x"] - target[0]) <= 1 and abs(pos["y"] - target[1]) <= 1,
                        f"mouse_move/cursor_position round trip: {target} -> {pos}")
                evidence["move_round_trip"] = [pos["x"], pos["y"]]

                restore_pt = start["display_point"]
                back = [round(restore_pt[0] * shot_w / disp_w), round(restore_pt[1] * shot_h / disp_h)]
                mcp.call("computer", {"action": "mouse_move", "coordinate": back, "duration": 0, "screenshot": False})
            finally:
                fixture.close()
                if front_before > 0:
                    activate(front_before)

            region = [0, 0, shot_w // 4, shot_h // 4]
            zoom, content, _ = mcp.call("computer", {"action": "zoom", "region": region})
            images = [i for i in content["content"] if i.get("type") == "image"]
            require(len(images) == 1, "zoom image missing")
            zw, zh = jpeg_size(base64.b64decode(images[0]["data"]))
            require((zw, zh) == tuple(zoom["image_size"]), "zoom size mismatch")
            require(zw > region[2], f"zoom did not add resolution: {zw} <= {region[2]}")
            evidence["zoom"] = {"region": region, "image": [zw, zh]}
    print("PASS: computer tool high-res coordinate space: click/type/move via screenshot pixels, JPEG screenshot size, zoom")
    print(json.dumps(evidence, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {str(error)[:1500]}", file=sys.stderr)
        raise SystemExit(1)
