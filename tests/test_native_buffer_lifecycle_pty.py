#!/usr/bin/env python3
"""Real native app, owned invisible PTY: Tab, saved undo after refresh, reflow.
No model calls, external windows, clipboard access, or existing user buffers.
"""
import argparse,fcntl,hashlib,json,os,pty,re,select,signal,struct,subprocess,tempfile,termios,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def run(binary,trace_output=None):
    with tempfile.TemporaryDirectory(prefix='dsco-buffer-lifecycle-') as directory:
        env=dict(HOME=directory,TMPDIR=directory,PATH='/usr/bin:/bin:/usr/sbin:/sbin',LANG='en_US.UTF-8',
            TERM='xterm-kitty',TERM_PROGRAM='kitty',OPENAI_API_KEY='fixture-only',OPENAI_API_BASE='http://127.0.0.1:1/v1',
            DSCO_ENV_FILE='/dev/null',DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',
            DSCO_NO_AUTO_SUPERVISE='1',DSCO_NO_SUPERVISE='1',DSCO_KITTY_AGENT_WINDOWS='0',
            DSCO_PIXEL_TUI='1',DSCO_KITTY_GRAPHICS='1',DSCO_PIXEL_TUI_DPR='1',
            DSCO_BANNER='0',DSCO_KITTY_BANNER='0',DSCO_ALLOW_NET='0',DSCO_GOV_MODEL='standard',
            DSCO_ALLOW_READ='1',DSCO_ALLOW_WRITE='1',DSCO_ALLOW_RUN='0',DSCO_ALLOW_CONTROL='0',
            DSCO_AUTO_GOAL='0',DSCO_GOAL_NO_AUTORUN='1',DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',
            DSCO_AUTO_FALLBACK='0',DSCO_DISABLE_DEFAULT_FALLBACKS='1',DSCO_DYNAMIC_FAILOVER='0',
            DSCO_SYSTEM_PROMPT='Owned local slash-command test; no inference requested.')
        paths={}
        long_text=''.join('row%02d\n'%i for i in range(60))
        for name,text in [('A','alpha'),('B','beta'),('Long',long_text)]:
            p=subprocess.run([str(binary),'buffer','create',json.dumps({'name':name,'kind':'scratch','content':text})],cwd=directory,env=env,capture_output=True,timeout=20,check=True)
            r=json.loads(p.stdout);assert r.get('ok'),r
            path=Path(r['buffer']['content_path']);assert path.resolve().is_relative_to(Path(directory).resolve());paths[name]=path
        master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',35,112,1120,700))
        proc=subprocess.Popen([str(binary),'--profile','worker','--native','-i','--provider','openai','-m','gpt-5.6-luna'],cwd=directory,env=env,stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
        os.close(slave);output=bytearray()
        def pump(seconds):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                if select.select([master],[],[],.02)[0]:
                    try:data=os.read(master,262144)
                    except OSError:return
                    output.extend(data);assert len(output)<64*1024*1024
        def send(data):
            assert os.write(master,data)==len(data);pump(.3)
        def wait_text(name,expected,label):
            end=time.monotonic()+5
            while paths[name].read_text()!=expected and time.monotonic()<end:pump(.05)
            assert paths[name].read_text()==expected,label
        try:
            end=time.monotonic()+8
            while b'\x1b_G' not in output and time.monotonic()<end:pump(.05)
            assert b'\x1b_G' in output,'native framebuffer started';pump(.6)
            send(b'/ui trace 30s\r');assert list(Path(directory).glob('dsco-ui-*.jsonl'))
            send(b'/buffer edit A\r');pump(.4)
            send(b'/buffer edit B\r');pump(.4)
            send(b'\x07');send(b'\t')  # Activate B, then stable-ID cycle to A.
            suffix='-tab-π';send(b'\x1b[200~'+suffix.encode()+b'\x1b[201~')
            assert paths['A'].read_text()=='alpha' and paths['B'].read_text()=='beta','no implicit save'
            send(b'\x13');wait_text('A','alpha'+suffix,'Tab must direct typing and Ctrl-S to selected buffer A')
            assert paths['B'].read_text()=='beta','other buffer untouched'
            send(b'\x1b');send(b'/windows refresh 1\r')
            send(b'\x07');send(b'\x1a');send(b'\x13')  # Re-focus; undo retained across no-op refresh.
            wait_text('A','alpha','saved undo history must survive unchanged refresh')
            send(b'\x1b');send(b'/buffer edit Long\r');pump(.4);send(b'\x07')
            fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',18,70,700,360));os.kill(proc.pid,signal.SIGWINCH);pump(1)
            send(b'\x1b');send(b'/ui trace stop\r');pump(.4)
            trace=next(Path(directory).glob('dsco-ui-*.jsonl'))
            end=time.monotonic()+3
            while trace.stat().st_size==0 and time.monotonic()<end:pump(.05)
            records=[json.loads(x) for x in trace.read_text().splitlines()]
            if trace_output:
                trace_output.parent.mkdir(parents=True,exist_ok=True);raw=trace.read_bytes();trace_output.write_bytes(raw);assert trace_output.read_bytes()==raw
            assert records[0]['event']=='begin' and records[-1]['event']=='end'
            last={};seen=False
            for r in records:
                if r.get('id')=='window/3' and r.get('op')!='remove':
                    last.update({k:v[1] for k,v in r.get('changes',{}).items()});seen=True
            assert seen,'long buffer appears in actual compositor trace'
            title=int(re.search(r'NATIVE_WINDOW_TITLE_HEIGHT\s*(?:=\s*)?(\d+)',(ROOT/'include/native_windows.h').read_text()).group(1))
            available=max(1,(last['height']-title-38)//18)
            assert last['cursor']==len(long_text.encode()) and last['scroll']<=60<last['scroll']+available,('trailing-newline caret visible after terminal reflow',last,available)
            frames=[r for r in records if r.get('event')=='frame']
            assert frames and all(0<=r['frame_ms']<60000 for r in frames),'no fabricated machine-uptime frame latency'
            assert paths['Long'].read_text()==long_text,'reflow does not change canonical text'
            send(b'/quit\r');proc.wait(timeout=8);assert proc.returncode==0
            if trace_output:
                trace_output.parent.mkdir(parents=True,exist_ok=True);raw=trace.read_bytes();trace_output.write_bytes(raw);assert trace_output.read_bytes()==raw
            return {'ok':True,'binary':str(binary),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                'tab_routes_only_selected_buffer':True,'no_implicit_save':True,'undo_survives_refresh':True,
                'trailing_newline_caret_visible_after_terminal_resize':True,'unrelated_buffer_preserved':True,
                'canonical_save_verified':True,'clean_quit':True,'trace_frames':len(frames),
                'max_frame_ms':max(r['frame_ms'] for r in frames),'pty_bytes':len(output),'trace':str(trace_output) if trace_output else None}
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM)
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=3)
            os.close(master)
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--binary',type=Path,default=ROOT/'dsco');ap.add_argument('--trace-output',type=Path);ap.add_argument('--report',type=Path);a=ap.parse_args()
    result=run(a.binary.resolve(),a.trace_output);raw=json.dumps(result,indent=2)+'\n'
    if a.report:a.report.write_text(raw);assert a.report.read_text()==raw
    print(raw,end='')
