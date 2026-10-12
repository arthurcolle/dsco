"""Actual child pipe -> cooperative provider tick -> live composer, no inference."""
import fcntl, os, pathlib, pty, select, signal, struct, termios, time
root = pathlib.Path(__file__).resolve().parents[1]
pid, fd = pty.fork()
if not pid:
    os.environ.update(TERM='xterm-256color', LC_ALL='C', DSCO_PIXEL_TUI='0')
    os.execl(str(root/'build/swarm_progress_fixture'), 'swarm_progress_fixture', 'tui', '16')
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 38, 120, 0, 0))
log = bytearray()
status = None
def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ready, _, _ = select.select([fd], [], [], min(.05, max(0, end-time.monotonic())))
        if ready:
            try: data=os.read(fd,65536)
            except OSError: break
            if not data: break
            log.extend(data)
try:
    pump(.12); os.write(fd,b'draft')
    deadline=time.monotonic()+8
    while b'ROOT_WAIT_COMPLETE' not in log and time.monotonic()<deadline: pump(.1)
    assert b'ROOT_WAIT_COMPLETE' in log, 'root wait did not complete through provider bridge'
    assert b'stderr-proof' in log and b'worker:' in log, 'real child output was not projected'
    assert b'failed' in log or b'error' in log, 'worker failure missing from projection'
    os.write(fd,b'-preserved\r'); pump(.5)
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        done, value=os.waitpid(pid,os.WNOHANG)
        if done: status=value; break
        pump(.05)
    assert status is not None and os.waitstatus_to_exitcode(status)==0, bytes(log[-2000:])
    assert b'SUBMITTED:draft-preserved' in log
    assert b'\x1b[?1000l' in log and b'\x1b[?1006l' in log
    print('PASS: real worker pipes, quiet provider wait, completion/failure, retained output and exact draft in real composer')
finally:
    capture=os.environ.get('DSCO_PROGRESS_PTY_CAPTURE')
    if capture:
        p=pathlib.Path(capture); p.parent.mkdir(parents=True,exist_ok=True); p.write_bytes(log)
        assert p.read_bytes()==log
    if status is None:
        try: os.kill(pid,signal.SIGKILL); os.waitpid(pid,0)
        except ProcessLookupError: pass
    os.close(fd)
