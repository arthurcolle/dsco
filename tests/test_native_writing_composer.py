#!/usr/bin/env python3
"""Real native input loop in an owned headless PTY; no desktop/Kitty opens."""
import fcntl,json,os,pty,select,signal,struct,tempfile,termios,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='dsco-native-writing-') as td:
    state=Path(td)/'state.json'
    pid,fd=pty.fork()
    if pid==0:
        os.chdir(root)
        os.environ.update(TERM='xterm-kitty',TERM_PROGRAM='kitty',DSCO_NO_ANIMATION='1')
        os.execv(str(root/'build/native_writing_composer_fixture'),['fixture',str(state)])
    reaped=False
    def pump(seconds=.12):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if select.select([fd],[],[],max(0,min(.03,end-time.monotonic())))[0]:
                try:
                    if not os.read(fd,262144):return
                except OSError:return
    def status():
        try:return json.loads(state.read_text())
        except (OSError,ValueError):return {}
    def wait(predicate,seconds=5):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            s=status()
            if predicate(s):return s
            pump(.03)
        raise AssertionError(status())
    def send(data):os.write(fd,data);pump()
    def mouse(x,y,release=False):
        return f'\x1b[<0;{int(x//10)+1};{int(y//20)+1}{"m" if release else "M"}'.encode()
    try:
        fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',32,110,1100,640))
        os.kill(pid,signal.SIGWINCH)
        wait(lambda s:len(s.get('windows',[]))==1);pump(.6)
        w=status()['windows'][0]
        x,y=w['x']+24,w['y']+70
        send(mouse(x,y)+mouse(x,y,True))
        assert status()['focused'],status()
        rect=tuple(w[k] for k in ('x','y','width','height'))
        send('trx_zπ🙂\nAB'.encode())
        send(b'\x7f\x1b[DB')
        send(b'\x1b[200~\tx\n_y\x1b[201~')
        expected='trx_zπ🙂\nB\tx\n_yA'
        s=wait(lambda s:s.get('windows',[{}])[0].get('text')==expected)
        assert s['returned']==0 and s['focused'],s
        send(b'\x1a')
        wait(lambda s:s.get('windows',[{}])[0].get('text')=='trx_zπ🙂\nBA')
        send(b'\x19')
        wait(lambda s:s.get('windows',[{}])[0].get('text')==expected)
        assert tuple(s['windows'][0][k] for k in ('x','y','width','height'))==rect,s
        send(b'\x01')
        wait(lambda s:s['windows'][0]['anchor']==0 and s['windows'][0]['cursor']==len(expected.encode()))
        send(b'\x1b');assert status()['focused'],status()
        send(b'\x1b[1;2D\x1b[1;2D')
        wait(lambda s:abs(s['windows'][0]['cursor']-s['windows'][0]['anchor'])==2)
        send(b'\x1b');assert status()['focused'],status()
        send(b'\x06trx\r')
        wait(lambda s:s['windows'][0]['anchor']==0 and s['windows'][0]['cursor']==3)
        send(b'\x1b')

        send(b'\x1b');send(b'chat-safe\r')
        final=wait(lambda s:s.get('phase')==3)
        assert final['submitted']=='chat-safe' and final['windows'][0]['text']==expected,final
        assert final['failures']==0,final
        end=time.monotonic()+5
        while time.monotonic()<end:
            child,code=os.waitpid(pid,os.WNOHANG)
            if child:
                reaped=True;assert os.waitstatus_to_exitcode(code)==0;break
            pump(.03)
        assert reaped,'owned fixture did not exit'
        print('PASS: native body click, typing t/r/x/z/underscores, Unicode, newline, backspace, arrows, literal tab/multiline paste, stable placement, Escape back to preserved chat (owned PTY only)')
    finally:
        if not reaped:
            try:os.kill(pid,signal.SIGKILL);os.waitpid(pid,0)
            except ProcessLookupError:pass
        os.close(fd)
