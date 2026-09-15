import os,pty,select,time,fcntl,termios,struct,re,signal,pathlib,codecs
binary=str(pathlib.Path(__file__).resolve().parents[1]/'build/tui_swarm_composer_fixture')
pid,fd=pty.fork()
if pid==0:
    os.environ.update(TERM='xterm-kitty',COLORTERM='truecolor',DSCO_KITTY_SIGNATURE='ᴀʀᴛʜᴜʀᴄᴏʟʟᴇ',LC_ALL='C')
    os.execl(binary,binary)
fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',32,110,1100,640))
log=bytearray()
def pump(seconds):
    stop=time.monotonic()+seconds
    while time.monotonic()<stop:
        if select.select([fd],[],[],max(0,min(.05,stop-time.monotonic())))[0]:
            try: b=os.read(fd,262144)
            except OSError: return
            if not b:return
            log.extend(b)
def send(b,wait=.2):
    os.write(fd,b);pump(wait)
try:
    # Cold debug builds can take longer than a fixed 600 ms to enter the
    # retained composer. Wait for the observable mount contract itself.
    mount_deadline=time.monotonic()+3.0
    while b'SWARM' not in log and time.monotonic()<mount_deadline:
        pump(.1)
    assert b'SWARM' in log, 'dock did not mount'
    assert '⧉'.encode() in log, 'Unicode unavailable under default C locale'
    if os.environ.get('DSCO_TUI_FIXTURE_COMPLETE'):
        completion_deadline=time.monotonic()+3.0
        compact_pattern=rb'\x1b\[23;1H[^\r\n]*SWARM'
        while (not re.search(compact_pattern, log) or
               b'0 active / 2 finished / 0 failed' not in log) and \
              time.monotonic()<completion_deadline:
            pump(.1)
        assert re.search(compact_pattern, log), 'completed dock did not shrink to six rows'
        assert b'0 active / 2 finished / 0 failed' in log, 'completion notice missing'
        print('PTY completion PASS: successful workers compact to six rows and report completion')
    before_typing = len(log)
    send(b'draft')
    if os.environ.get('DSCO_TUI_TEST_PROGRESS'):
        assert b'Swarm:' in log and b'started' in log, 'start toast missing'
        pulse_start = len(log)
        pump(15.2)
        pulse = log[pulse_start:]
        expected = b'no new output' if os.environ.get('DSCO_TUI_FIXTURE_STATIC') else b'output received'
        assert expected in pulse, '15-second activity toast missing'
        print('PTY progress PASS: real 15-second footer toast, truthful output status')
    if os.environ.get('DSCO_TUI_FIXTURE_STATIC'):
        typed = log[before_typing:]
        assert b'SWARM' not in typed, 'typing repainted unchanged worker cards'
        before_clear = len(log)
        send(b'\x0c')
        assert b'SWARM' in log[before_clear:], 'Ctrl+L did not restore retained cards'
        print('PTY retained PASS: unchanged cards skipped on typing; Ctrl+L restores full dock')
    send(b'\x07')
    assert b'FOCUSED' in log
    send(b'\x1b[1;2C') # shift-right moves
    send(b'\x1b[1;5D') # ctrl-left shrinks
    send(b'\t')
    send(b'z')
    send(b'z')
    send(b'\x1b[5~')
    send(b'\x1b') # back to preserved composer
    # Drag first worker title, using top=13 (32 - composer4 - dock16 + 1), title=14.
    send(b'\x1b[<0;7;14M\x1b[<32;10;15M\x1b[<0;10;15m')
    send(b'\x07') # return focus to composer
    for rows,cols in [(24,80),(40,140),(18,58),(32,110)]:
        fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,cols*10,rows*20))
        os.kill(pid,signal.SIGWINCH);pump(.25)
    if os.environ.get('DSCO_TUI_FIXTURE_COMPLETE'):
        start = len(log)
        send(b'\x07')
        assert b'HISTORY' in log[start:], 'Ctrl+G did not open completed history'
        start = len(log)
        send(b'x')
        assert b'1 dismissed' in log[start:], 'terminal card did not dismiss'
        start = len(log)
        send(b'h')
        restored = log[start:]
        assert b'HISTORY' in restored and b'dismissed' in restored, 'dismissed card lost from history'
        assert b'fixture stream' in restored, 'dismissal discarded output'
        send(b'h')
        send(b'\x07')
        print('PTY lifecycle PASS: history, dismissal, retained output, preserved draft')
    # Lifecycle controls returned focus to input; submission must preserve the draft.
    send(b'-preserved\r',.5)
    pump(.2)
    got,status=os.waitpid(pid,os.WNOHANG)
    assert got==pid, 'composer did not submit'
    assert os.waitstatus_to_exitcode(status)==0, 'draft changed during card interaction'
    assert b'SUBMITTED:draft-preserved' in log
    assert b'\x1b[?1002h' in log and b'\x1b[?1002l' in log, 'mouse mode not restored'
    pathlib.Path(os.environ.get('DSCO_TUI_TEST_CAPTURE', '/tmp/dsco-tui-dock-pty.ansi')).write_bytes(log)
    print('PTY PASS: real composer, live worker updates, C-locale Unicode, focus/move/resize/zoom/drag, four terminal resizes, retained draft, restored mouse mode')
finally:
    try:
        got,status=os.waitpid(pid,os.WNOHANG)
        if not got:os.kill(pid,signal.SIGKILL);os.waitpid(pid,0)
    except ChildProcessError:pass
    os.close(fd)
