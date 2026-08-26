#!/usr/bin/env python3
"""Report the terminal geometry the banner renderer relies on.

Run this in each terminal (iTerm2, kitty, Terminal.app) and compare. It prints
what TIOCGWINSZ reports plus what the terminal answers to CSI 16 t (cell size in
device pixels) and CSI 14 t (text-area size in device pixels)."""
import os, sys, fcntl, termios, struct, select, tty

def winsz():
    b = fcntl.ioctl(sys.stdout.fileno(), termios.TIOCGWINSZ,
                    struct.pack("HHHH", 0, 0, 0, 0))
    r, c, xp, yp = struct.unpack("HHHH", b)
    return r, c, xp, yp

def query(seq, fd):
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        os.write(fd, seq.encode())
        buf = b""
        while select.select([fd], [], [], 0.4)[0]:
            ch = os.read(fd, 1)
            if not ch:
                break
            buf += ch
            if ch == b"t":
                break
        return buf
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

r, c, xp, yp = winsz()
cell_w = xp // c if c and xp else 0
cell_h = yp // r if r and yp else 0
ss = 2 if (cell_w or 9) <= 12 else 1
print(f"TERM_PROGRAM = {os.environ.get('TERM_PROGRAM')}  TERM = {os.environ.get('TERM')}")
print(f"TIOCGWINSZ   : cols={c} rows={r} xpixel={xp} ypixel={yp}")
print(f"derived cell : {cell_w}x{cell_h} px   supersample(ss)={ss}")
if sys.stdin.isatty():
    print(f"CSI 16 t (cell px)      -> {query(chr(27)+'[16t', sys.stdin.fileno())!r}")
    print(f"CSI 14 t (text-area px) -> {query(chr(27)+'[14t', sys.stdin.fileno())!r}")
else:
    print("stdin not a tty; skipped CSI 16t/14t queries")
