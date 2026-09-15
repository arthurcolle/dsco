#!/usr/bin/env python3
"""Actual native binary + local mock provider + owned sleeping tool, no UI window."""
import argparse, fcntl, hashlib, http.server, json, os, pty, re, select, signal
import struct, subprocess, tempfile, termios, threading, time
from pathlib import Path

def run(binary, output, external_1080p=False):
    with tempfile.TemporaryDirectory(prefix='dsco-native-cadence-') as tmp:
        marker=Path(tmp)/'tool-started'
        requests=[]; errors=[]; frames=[]; raw=bytearray(); pending=bytearray(); reasoning_started=[]
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_POST(self):
                try:
                    assert self.path=='/v1/chat/completions',self.path
                    req=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    names=[t.get('function',t).get('name') for t in req.get('tools',[])]
                    result=any(m.get('role')=='tool' and 'NATIVE_TOOL_DONE' in json.dumps(m)
                               for m in req.get('messages',[]))
                    requests.append({'bash_disclosed':'bash' in names,'actual_result':result,
                        'last_user':str(next((m.get('content') for m in reversed(req.get('messages',[])) if m.get('role')=='user'),''))[-1800:]})
                    assert len(requests)<=3,requests
                    if len(requests)==1:
                        assert 'bash' in names,names
                        reasoning_started.append(time.monotonic())
                        time.sleep(1.5)  # No input, provider bytes, wheel, or tools to drive repaint.
                        delta={'tool_calls':[{'index':0,'id':'native_owned_sleep','type':'function',
                            'function':{'name':'bash','arguments':json.dumps({'command':
                                'printf running > '+str(marker)+'; sleep 3; printf NATIVE_TOOL_DONE',
                                'timeout':5})}}]}
                        finish='tool_calls'
                    else:
                        assert result,'real owned tool result was not received'
                        delta={'content':'The owned bash animation test completed successfully. Output: NATIVE_TOOL_DONE. NATIVE_TOOL_CADENCE_FINISHED'};finish='stop'
                    events=[{'id':'native-fixture','choices':[{'index':0,'delta':delta,'finish_reason':None}]},
                            {'id':'native-fixture','choices':[{'index':0,'delta':{},'finish_reason':finish}],
                             'usage':{'prompt_tokens':10,'completion_tokens':10,'cost':0}}]
                    body=(''.join('data: '+json.dumps(e)+'\n\n' for e in events)+'data: [DONE]\n\n').encode()
                    self.send_response(200);self.send_header('Content-Type','text/event-stream')
                    self.send_header('Content-Length',str(len(body)));self.end_headers()
                    self.wfile.write(body);self.wfile.flush()
                except Exception as exc:
                    errors.append(repr(exc));self.close_connection=True
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        env=dict(HOME=tmp,TMPDIR=tmp,PATH='/usr/bin:/bin:/usr/sbin:/sbin',TERM='xterm-kitty',
            LANG='en_US.UTF-8',OPENAI_API_KEY='fixture-only',
            OPENAI_API_BASE=f'http://127.0.0.1:{server.server_port}/v1',
            DSCO_ENV_FILE='/dev/null',DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',
            DSCO_DISABLE_DEFAULT_FALLBACKS='1',DSCO_AUTO_FALLBACK='0',DSCO_DYNAMIC_FAILOVER='0',
            DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',DSCO_NO_AUTO_SUPERVISE='1',DSCO_NO_SUPERVISE='1',
            DSCO_KITTY_AGENT_WINDOWS='0',DSCO_PIXEL_TUI='1',DSCO_KITTY_GRAPHICS='1',
            DSCO_PIXEL_TUI_DPR='2',DSCO_MCP_HEADLESS='0',DSCO_BANNER='0',
            DSCO_KITTY_BANNER='0',DSCO_TOOL_PROXY='1',DSCO_ALLOW_NET='0',DSCO_GOAL_NO_AUTORUN='1',DSCO_AUTO_GOAL='0',
            DSCO_SYSTEM_PROMPT='Owned local fixture. Execute the requested owned tool.',DSCO_HARD_TURN_CEILING='3')
        if external_1080p:
            env['DSCO_PIXEL_TUI_DPR']='1'
            env['DSCO_PIXEL_TUI_ZOOM']='1.25'
        master,slave=pty.openpty()
        geometry=(54,192,1920,1080) if external_1080p else (35,112,2240,1400)
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',*geometry))
        proc=None; sent=False;quit_sent=False;started_tool=None;finished_at=None
        started=time.monotonic(); first_frame=None
        try:
            proc=subprocess.Popen([str(binary),'--profile','worker','--native','-i','--provider','openai','-m','gpt54'],
                cwd=tmp,env=env,stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
            os.close(slave);slave=-1
            while time.monotonic()-started<23:
                now=time.monotonic()
                if select.select([master],[],[],.01)[0]:
                    try:data=os.read(master,262144)
                    except OSError:data=b''
                    raw.extend(data);pending.extend(data)
                    assert len(raw)<30*1024*1024,'bounded PTY output'
                    while True:
                        at=pending.find(b'\x1b_G')
                        if at<0:
                            pending[:]=pending[-2:];break
                        end=pending.find(b'\x1b\\',at+3)
                        if end<0:
                            del pending[:at];break
                        packet=bytes(pending[at+3:end]);del pending[:end+2]
                        header=packet.split(b';',1)[0].decode('ascii','replace')
                        fields=dict(v.split('=',1) for v in header.split(',') if '=' in v)
                        action=fields.get('a','')
                        if action in ('t','T','f') and int(fields.get('s','0'))>0 and int(fields.get('v','0'))>0:
                            first_frame=first_frame or now
                            frames.append({'at':round(now-started,4),'a':action,
                                'y':int(fields.get('y','0')),
                                'w':int(fields.get('s','0')),'h':int(fields.get('v','0'))})
                if not sent and first_frame and now-first_frame>.6:
                    os.write(master,b'Run the owned bash animation test.\r');sent=True
                if marker.exists() and started_tool is None:started_tool=now
                if len(requests)>=2 and finished_at is None:finished_at=now
                if finished_at and now-finished_at>1.2 and not quit_sent:
                    os.write(master,b'/quit\r');quit_sent=True
                if proc.poll() is not None:break
            if proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=3)
                raise AssertionError('native fixture did not exit normally')
            assert proc.returncode==0,proc.returncode
            assert not errors,errors
            assert started_tool is not None,'real bash tool never started'
            assert len(requests)>=2 and requests[-1]['actual_result'],requests
            reasoning_lo=reasoning_started[0]-started+.3
            reasoning_hi=reasoning_started[0]-started+1.3
            reasoning_frames=[r for r in frames if reasoning_lo<=r['at']<=reasoning_hi and r['y']<200]
            lo=started_tool-started+.4;hi=started_tool-started+2.6
            live=[r for r in frames if lo<=r['at']<=hi]
            patches=[r for r in live if r['a']=='f']
            gaps=[b['at']-a['at'] for a,b in zip(patches,patches[1:])]
            result={'binary':str(binary),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                'requests':requests,'owned_tool_ran':True,'external_1080p':external_1080p,'patches_during_2_2s_tool_window':len(patches),
                'reasoning_frames_without_input_or_provider_bytes':len(reasoning_frames),
                'max_patch_gap_ms':round(max(gaps,default=0)*1000,3),
                'median_patch_gap_ms':round(sorted(gaps)[len(gaps)//2]*1000,3) if gaps else None,
                'patch_dimensions':sorted({(r['w'],r['h']) for r in patches}),
                'full_uploads_during_window':len(live)-len(patches),'pty_bytes':len(raw),
                'exit_code':proc.returncode,'visual_window_opened':False,'provider_inference':False}
            output.write_text(json.dumps(result,indent=2)+'\n')
            assert len(reasoning_frames)>=4,result
            assert len(patches)>=35,result
            assert max(gaps,default=99)<.3,result
            print(json.dumps(result))
        finally:
            if proc and proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM)
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
            if slave>=0:os.close(slave)
            os.close(master);server.shutdown();server.server_close();thread.join(timeout=2)
            # Strip graphic payloads; save only bounded diagnostic text on failed cases.
            if errors or not output.exists():
                clean=re.sub(rb'\x1b_G.*?\x1b\\',b'[graphics]',bytes(raw),flags=re.S)
                output.with_suffix('.diagnostic.txt').write_bytes(clean[-12000:])
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--binary',default='./dsco')
    parser.add_argument('--output',required=True)
    parser.add_argument('--external-1080p',action='store_true');args=parser.parse_args()
    run(Path(args.binary).resolve(),Path(args.output).resolve(),args.external_1080p)
