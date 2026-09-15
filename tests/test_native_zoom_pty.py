#!/usr/bin/env python3
"""Live /zoom reflow on an owned 1080p PTY, without a GUI or provider request."""
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

p = argparse.ArgumentParser()
p.add_argument('--binary', default='./dsco')
p.add_argument('--output', required=True)
a = p.parse_args()
binary = Path(a.binary).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix='dsco-zoom-') as tmp:
    env = dict(HOME=tmp, TMPDIR=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
               TERM='xterm-kitty', LANG='en_US.UTF-8', OPENAI_API_KEY='fixture-only',
               OPENAI_API_BASE='http://127.0.0.1:1/v1', DSCO_ENV_FILE='/dev/null',
               DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
               DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0',
               DSCO_DYNAMIC_FAILOVER='0', DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',
               DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
               DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PIXEL_TUI='1',
               DSCO_KITTY_GRAPHICS='force', DSCO_MCP_HEADLESS='0', DSCO_BANNER='0',
               DSCO_KITTY_BANNER='0', DSCO_TOOL_PROXY='1', DSCO_ALLOW_NET='0',
               DSCO_GOAL_NO_AUTORUN='1', DSCO_AUTO_GOAL='0',
               DSCO_PIXEL_TUI_ANIMATIONS='0',
               DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(Path(tmp)/'zoom.ppm'))
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH',54,192,1920,1080))
    proc = subprocess.Popen([str(binary),'--profile','worker','--native','-i',
                             '--provider','openai','-m','gpt54'], cwd=tmp, env=env,
                            stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    pending = bytearray()
    frames = []
    receipts = []
    def pump(seconds):
        end = time.monotonic()+seconds
        while time.monotonic()<end:
            if select.select([master],[],[],.02)[0]:
                try: data=os.read(master,262144)
                except OSError: return
                pending.extend(data)
                while True:
                    at=pending.find(b'\x1b_G')
                    if at<0:
                        pending[:]=pending[-2:]
                        break
                    stop=pending.find(b'\x1b\\',at+3)
                    if stop<0:
                        del pending[:at]
                        break
                    header=bytes(pending[at+3:stop]).split(b';',1)[0]
                    del pending[:stop+2]
                    fields=dict(v.split('=',1) for v in header.decode('ascii','replace').split(',') if '=' in v)
                    if fields.get('a') in ('t','T','f') and int(fields.get('s','0'))>0:
                        frames.append((int(fields['s']),int(fields.get('v','0'))))
    try:
        for command in (None, '/zoom 0.75', '/zoom 0.05', '/zoom 3',
                        '/zoom 150%', '/zoom 1', '/zoom auto', '/zoom +', '/zoom -'):
            expected=(1920,1080)
            before=len(frames)
            if command: os.write(master,command.encode()+b'\r')
            end=time.monotonic()+8
            while len(frames)==before and time.monotonic()<end and proc.poll() is None:
                pump(.1)
            assert len(frames)>before, (command,expected,frames[before:],proc.poll())
            pump(.8)
            with (Path(tmp)/'zoom.ppm').open('rb') as image:
                assert image.readline()==b'P6\n'
                assert tuple(map(int,image.readline().split()))==expected
            receipts.append(dict(command=command or 'startup', framebuffer=list(expected),
                                 snapshot_sha256=hashlib.sha256((Path(tmp)/'zoom.ppm').read_bytes()).hexdigest()))
        os.write(master,b'/quit\r')
        end=time.monotonic()+5
        while proc.poll() is None and time.monotonic()<end: pump(.1)
        assert proc.wait(timeout=2)==0
    finally:
        if proc.poll() is None: proc.terminate(); proc.wait(timeout=3)
        os.close(master)
    receipt=dict(binary=str(binary),sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                 terminal=[1920,1080],steps=receipts,exit_code=proc.returncode,
                 gui_opened=False,provider_inference=False)
    Path(a.output).write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))
