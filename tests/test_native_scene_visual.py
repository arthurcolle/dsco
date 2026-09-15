#!/usr/bin/env python3
"""Capture only the explicitly created Kitty fixture's exact native window."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
from test_buffer_views_mcp import Client

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    parser.add_argument('--fixture', default='build/test_pixel_scene_lifetime')
    parser.add_argument('--output', default='reports/buffers-20260906')
    parser.add_argument('--no-patch', action='store_true')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve()); fixture=str(Path(args.fixture).resolve(strict=True))
    output=Path(args.output).resolve(); output.mkdir(parents=True,exist_ok=True)
    state=Path(tempfile.mkdtemp(prefix='dsco-visual-',dir='/tmp')); status=state/'scene-status.json'
    c=Client(binary,state); surface=None
    def remote(command, **kwargs):
        response=c.rpc('tools/call',dict(name='kitty_remote',arguments=dict(command=command,to=started['to'],**kwargs)))
        assert not response.get('isError'), response
        return json.loads(response['content'][0]['text'])
    def wait(predicate):
        end=time.monotonic()+5
        while time.monotonic()<end:
            current=json.loads(status.read_text()) if status.exists() else {}
            if predicate(current): return current
            time.sleep(.1)
        raise AssertionError(current)
    def capture(name):
        # macOS can return a stale backing image for an inactive Kitty window.
        # Raise only the owned fixture before inspecting its rendered pixels.
        assert c.surface('focus', surface_id=surface)['verified']
        time.sleep(.4)
        path=output/f'native-scene-{name}.png'
        subprocess.run(['/usr/sbin/screencapture','-x','-o','-l',str(native_id),str(path)],check=True,timeout=5)
        assert path.stat().st_size>1000
        return str(path)
    try:
        started=c.surface('start',command='/usr/bin/env',args=['DSCO_PIXEL_SCENE_FIXTURE_STATUS='+str(status),*(['DSCO_PIXEL_TUI_PATCH=0'] if args.no_patch else []),fixture,'--live'],visible=True)
        assert started['verified'], started
        surface=started['surfaces'][0]['surface_id']
        windows=json.loads(remote('ls')['output'])
        assert len(windows)==1 and len(windows[0]['tabs'])==1
        pane=windows[0]['tabs'][0]['windows'][0]
        assert any(fixture in p['cmdline'] for p in pane['foreground_processes']), pane
        native_id=windows[0]['platform_window_id']
        initial=wait(lambda s:s.get('placed') and not s.get('dirty'))
        captures=[capture('retained')]
        c.surface('send_text',surface_id=surface,text='m')
        menu=wait(lambda s:s.get('menu')==1 and not s.get('placed'))
        captures.append(capture('menu'))
        c.surface('send_text',surface_id=surface,text='m')
        restored=wait(lambda s:s.get('menu')==0 and s.get('placed'))
        captures.append(capture('restored'))
        remote('resize-os-window',args=['--match=id:'+str(pane['id']),'--width=120','--height=36','--unit=cells'])
        resized=wait(lambda s:s.get('cols',0)>initial['cols'] and s.get('rows',0)>initial['rows'] and s.get('placed'))
        assert resized['scene_id']!=initial['scene_id'] and resized['input']=='Draft remains usable'
        captures.append(capture('resized'))
        c.surface('send_text',surface_id=surface,text='t')
        phase=wait(lambda s:s.get('phase')==1 and s.get('placed') and s.get('base_id'))
        captures.append(capture('phase'))
        for record in (initial,menu,restored,resized,phase): record.pop('input_hex',None)
        print(json.dumps(dict(ok=True,binary=binary,binary_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(Path(fixture).read_bytes()).hexdigest(),state=str(state),native_window_id=native_id,surface_id=surface,initial=initial,menu=menu,restored=restored,resized=resized,phase=phase,captures=captures)))
    finally:
        if surface: c.surface('close',surface_id=surface)
        c.close()
if __name__=='__main__':main()
