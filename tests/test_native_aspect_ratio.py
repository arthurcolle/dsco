#!/usr/bin/env python3
"""Check real compositor framebuffer proportions through owned PTYs; no GUI or LLM."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", default="./dsco")
parser.add_argument("--output")
args = parser.parse_args()
binary = Path(args.binary).resolve(strict=True)
results = []
# Width cap, height cap, minimum width/height, odd Retina, normal default zoom.
cases = [(2560,1080,1), (1080,2560,1), (240,800,1), (1280,120,1),
         (2325,1682,2), (1280,800,1)]
with tempfile.TemporaryDirectory(prefix="dsco-aspect-") as directory:
    for pw, ph, dpr in cases:
        snapshot = Path(directory) / f"{pw}x{ph}.ppm"
        env = dict(os.environ)
        env.pop("DSCO_PIXEL_TUI_ZOOM", None)
        env.update(TERM="xterm-kitty", DSCO_KITTY_GRAPHICS="force",
                   DSCO_PIXEL_TUI="1", DSCO_PIXEL_TUI_DPR=str(dpr),
                   DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_NO_SUPERVISE="1",
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot))
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", max(1,ph//20), max(1,pw//10), pw, ph))
        proc = subprocess.Popen([str(binary), "--compositor-stream-bench", "32"],
                                env=env, stdin=slave, stdout=subprocess.PIPE, stderr=slave)
        os.close(slave)
        try:
            deadline = time.monotonic()+15
            while proc.poll() is None and time.monotonic()<deadline:
                if select.select([master], [], [], .05)[0]:
                    try:
                        os.read(master, 262144)
                    except OSError:
                        break
            summary, _ = proc.communicate(timeout=3)
            assert proc.returncode == 0, (pw, ph, proc.returncode, summary)
            perf = json.loads(summary)
            with snapshot.open("rb") as stream:
                assert stream.readline() == b"P6\n"
                w, h = map(int, stream.readline().split())
                assert stream.readline() == b"255\n"
                assert len(stream.read()) == w*h*3
            # At most one output pixel of rounding error on either axis.
            error = abs(w*ph-h*pw)
            assert error <= max(pw,ph)*dpr, (pw,ph,w,h,"distorted framebuffer")
            if (pw,ph) in ((2325,1682),(1280,800)):
                assert (w,h) == (pw,ph), ("native pixels changed",pw,ph,w,h)
            assert perf["frames"]["failed"] == 0, perf
            results.append(dict(terminal=[pw,ph], framebuffer=[w,h], dpr=dpr,
                                frames=perf["frames"]))
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
receipt = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
               cases=results)
if args.output:
    Path(args.output).write_text(json.dumps(receipt, indent=2)+"\n")
print(json.dumps(receipt, indent=2))
