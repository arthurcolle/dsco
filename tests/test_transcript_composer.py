#!/usr/bin/env python3
"""Owned PTY only: Ctrl-T opens/closes review without submitting or changing draft."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import termios
import time

binary=Path(__file__).resolve().parents[1]/'build/transcript_composer_fixture'
pid,fd=pty.fork()
if pid==0:
    os.environ.update(TERM='xterm-256color',DSCO_PIXEL_TUI='0',DSCO_TUI_ANIMATIONS='0')
    os.execl(str(binary),str(binary))
fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',30,100,0,0))
log=bytearray()
def pump(seconds):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if select.select([fd],[],[],.05)[0]:
            try: data=os.read(fd,65536)
            except OSError: break
            if not data: break
            log.extend(data)
def until(marker):
    end=time.monotonic()+5
    while marker not in log and time.monotonic()<end:pump(.1)
    assert marker in log,(marker,bytes(log[-1000:]))
try:
    pump(.5)
    os.write(fd,b'preserve-draft');pump(.2)
    os.write(fd,b'\x14');until(b'TRANSCRIPT_FIXTURE_CODE')
    assert b'SUBMITTED:' not in log
    os.write(fd,b'\x14');pump(.2)
    assert b'\x1b[?1049h' in log and b'\x1b[?1049l' in log
    os.write(fd,b'-tail\r');until(b'SUBMITTED:preserve-draft-tail')
    waited,status=os.waitpid(pid,0);assert waited==pid and os.waitstatus_to_exitcode(status)==0
    print('PASS: actual composer Ctrl-T review, close, draft/cursor preservation, and submit')
finally:
    try: os.kill(pid,signal.SIGTERM)
    except ProcessLookupError:pass
    try:os.waitpid(pid,0)
    except ChildProcessError:pass
    os.close(fd)
