#!/usr/bin/env python3
"""Key-to-completed-graphics-upload latency on an owned PTY; no GUI or inference."""
import argparse, fcntl, hashlib, json, os, pty, select, signal, statistics, struct
import subprocess, tempfile, termios, time
from pathlib import Path

def run(binary, zoom, dpr=1):
    with tempfile.TemporaryDirectory(prefix='dsco-key-latency-') as tmp:
        env = dict(HOME=tmp, TMPDIR=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
            TERM='xterm-kitty', LANG='en_US.UTF-8', OPENAI_API_KEY='fixture-only',
            DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
            DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',
            DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1', DSCO_KITTY_AGENT_WINDOWS='0',
            DSCO_PIXEL_TUI='1', DSCO_KITTY_GRAPHICS='1', DSCO_PIXEL_TUI_DPR=str(dpr),
            DSCO_PIXEL_TUI_ZOOM=str(zoom), DSCO_MCP_HEADLESS='0', DSCO_BANNER='0',
            DSCO_KITTY_BANNER='0', DSCO_GOAL_NO_AUTORUN='1', DSCO_AUTO_GOAL='0')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH',54,192,1920,1080))
        proc = subprocess.Popen([str(binary),'--profile','worker','--native','-i',
            '--provider','openai','-m','gpt54'], cwd=tmp, env=env, stdin=slave,
            stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        pending=bytearray(); current={}; completed=[]; wire=0
        def pump(timeout):
            nonlocal wire, current
            if not select.select([master],[],[],timeout)[0]: return
            data=os.read(master,262144); wire+=len(data); pending.extend(data)
            assert wire < 30*1024*1024, 'bounded PTY output'
            while True:
                start=pending.find(b'\x1b_G')
                if start<0: pending[:]=pending[-2:]; return
                end=pending.find(b'\x1b\\',start+3)
                if end<0: del pending[:start]; return
                packet=bytes(pending[start+3:end]); del pending[:end+2]
                fields=dict(x.split('=',1) for x in packet.split(b';',1)[0].decode().split(',') if '=' in x)
                if fields.get('a') in ('t','T','f') and 's' in fields and 'v' in fields:
                    current=fields
                if current and fields.get('m','0')=='0':
                    completed.append((time.monotonic(),current)); current={}
        try:
            deadline=time.monotonic()+15
            while not completed and time.monotonic()<deadline: pump(.02)
            assert completed, 'native compositor did not upload its initial frame'
            until=time.monotonic()+.3
            while time.monotonic()<until: pump(.01)
            samples=[]; dimensions=[]; start_wire=wire
            for key in b'editing latency proof abcdef':
                baseline=len(completed); started=time.monotonic(); os.write(master,bytes([key]))
                while len(completed)==baseline and time.monotonic()-started<2: pump(.002)
                assert len(completed)>baseline, 'keypress did not produce a graphics upload'
                at,fields=completed[baseline]
                samples.append((at-started)*1000)
                dimensions.append([int(fields.get('s','0')),int(fields.get('v','0'))])
                until=time.monotonic()+.025
                while time.monotonic()<until: pump(.002)
            return dict(binary=str(binary),sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                zoom=zoom,dpr=dpr,samples=len(samples),
                p50_ms=round(statistics.median(samples),3),
                p95_ms=round(sorted(samples)[int(.95*(len(samples)-1))],3),
                max_ms=round(max(samples),3),wire_bytes=wire-start_wire,
                key_latencies_ms=[round(value,3) for value in samples],
                patch_dimensions=sorted(set(map(tuple,dimensions))),gui_opened=False,inference=False)
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM)
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: os.killpg(proc.pid,signal.SIGKILL); proc.wait()
            os.close(master)

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--binary',default='./dsco');p.add_argument('--output')
    a=p.parse_args(); results=[run(Path(a.binary).resolve(),z,d) for z,d in
        ((.65,2),(.65,1),(1,1),(1.25,1),(1.5,1),(1.75,1),(2,1))]
    report=json.dumps(results,indent=2)+'\n'
    if a.output: Path(a.output).write_text(report)
    print(report)
