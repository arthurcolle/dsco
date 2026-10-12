#!/usr/bin/env python3
"""Manual /compact archives prose below the automatic trigger, even when auto is off.

Private HOME, PTY, local SSE fixture and fake credentials only. No paid requests.
"""
import argparse
import hashlib
import sqlite3
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time

FIRST = 'OWNED_MANUAL_FIRST'
SECOND = 'OWNED_MANUAL_SECOND'
THIRD = 'OWNED_MANUAL_CONTINUE'
LONG = 'OWNED_MANUAL_BEGIN_' + 'z' * 40000 + '_OWNED_MANUAL_END'
FINAL = 'OWNED_MANUAL_FINISHED'

def run(binary, expected_noop, output, write_denied=False, restart=False, omit_usage=False, batch=0):
    requests, errors = [], []
    seeded_answers = set()
    root_key = None
    leaf_recalled = False
    with tempfile.TemporaryDirectory(prefix='dsco-manual-compaction-') as tmp:
        root = Path(tmp)
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def do_POST(self):
                nonlocal root_key, leaf_recalled
                try:
                    assert self.path == '/v1/chat/completions'
                    req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    requests.append(req); stage = len(requests)
                    messages = req['messages']; encoded = json.dumps(messages)
                    assert FIRST in encoded
                    if stage == 2:
                        assert LONG in encoded, 'automatic path altered the controlled history'
                    if stage >= 3:
                        assert SECOND in encoded and THIRD in encoded
                        if expected_noop: assert LONG in encoded, 'baseline unexpectedly compacted prose'
                        else:
                            assert '_OWNED_MANUAL_END' not in encoded, 'manual /compact left old prose intact'
                            key = re.search(r'key=(ck:msg:[A-Za-z0-9_-]+)', encoded)
                            assert key, 'manual compaction did not retain a retrieval key'
                    if stage == 3 and not expected_noop:
                        root_key=key[1]
                        delta = {'tool_calls': [{'index': 0, 'id': 'owned-recall', 'type': 'function',
                            'function': {'name': 'context_recall',
                                         'arguments': json.dumps({'key': key[1] + '#b:0-1024'})}}]}
                        finish = 'tool_calls'
                    elif stage >= 4 and not expected_noop:
                        recall = next(m for m in reversed(messages) if m.get('role') == 'tool' and m.get('tool_call_id','').startswith('owned-recall'))
                        body=recall['content']
                        if (not batch and 'OWNED_MANUAL_BEGIN_' in body) or (batch and re.search(r'"text"\s*:\s*"OWNED_BATCH_\d+_OWNED_MANUAL_BEGIN_',body)):
                            leaf_recalled=True;delta={'content':FINAL};finish='stop'
                        else:
                            assert batch, 'archived text is not retrievable'
                            handles=re.findall(r'key=(ck:msg:[A-Za-z0-9_-]+)',body)
                            assert len(handles)>=2, 'receipt index is not traversable through context_recall'
                            child=next(k for k in handles[1:] if k != handles[0])
                            delta={'tool_calls':[{'index':0,'id':f'owned-recall-{stage}','type':'function',
                                'function':{'name':'context_recall','arguments':json.dumps({'key':child+'#b:0-4096'})}}]}
                            finish='tool_calls'
                    else:
                        text = LONG if stage == 1 else 'OWNED_MANUAL_SHORT' if stage == 2 else FINAL
                        delta = {'content': text}; finish = 'stop'
                    chunks = [dict(id='owned-manual', choices=[dict(index=0, delta=delta, finish_reason=None)]),
                              dict(id='owned-manual', choices=[dict(index=0, delta={}, finish_reason=finish)],
                                   usage={'prompt_tokens': 20, 'completion_tokens': 10, 'cost': 0})]
                    if omit_usage: chunks[-1].pop("usage",None)
                    data = (''.join('data: ' + json.dumps(chunk) + '\n\n' for chunk in chunks) + 'data: [DONE]\n\n').encode()
                    self.send_response(200); self.send_header('Content-Type', 'text/event-stream')
                    self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
                except Exception as exc:
                    errors.append(repr(exc)); self.close_connection = True
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        env = dict(HOME=tmp, TMPDIR=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin', TERM='xterm-256color', LANG='en_US.UTF-8',
            OPENAI_API_KEY='owned-local-fixture', OPENAI_API_BASE=f'http://127.0.0.1:{server.server_port}/v1',
            DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
            DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0', DSCO_DYNAMIC_FAILOVER='0',
            DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
            DSCO_KITTY_AGENT_WINDOWS='0', DSCO_KITTY_GRAPHICS='0', DSCO_BANNER='0', DSCO_AUTO_GOAL='0',
            DSCO_TOOL_PROXY='1', DSCO_TOOL_ALLOWLIST='context_evict,context_compact,context_recall',
            DSCO_AUTO_COMPACT='0', DSCO_REQUEST_TARGET_BYTES='0', DSCO_TUI_ANIMATIONS='0',
            DSCO_CHRONICLE_DIR=str(root/'chronicle'), DSCO_CHRONICLE_MODE='full-local',
            DSCO_CONTEXT_DB=str(root/'context.db'), DSCO_SYSTEM_PROMPT='OWNED_MANUAL_SYSTEM', DSCO_HARD_TURN_CEILING='64')
        if write_denied: env['DSCO_ALLOW_WRITE']='0'
        if restart or batch: env['DSCO_RESUME_AFTER_CRASH']='1'
        command=[str(binary), '--profile','worker','--tui','-i','--provider','openai','-m','fixture-model',
            '--trust-tier','trusted','--approval-mode','never']
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH',40,120,1200,800))
        proc = subprocess.Popen(command, cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
            start_new_session=True)
        os.close(slave); transcript=bytearray(); state=0; compact_mtime=0; autosave=root/'.dsco/sessions/_autosave.json'
        before=None; after=None; context_before=None;context_after=None;display_start=0;compact_start=0
        try:
            until=time.monotonic()+60
            while time.monotonic()<until:
                if select.select([master],[],[],.05)[0]:
                    try: transcript.extend(os.read(master,262144))
                    except OSError: pass
                saved=None
                if autosave.exists():
                    try: saved=json.loads(autosave.read_text())
                    except ValueError: pass
                stored=json.dumps(saved) if saved else ''
                if state==0 and b'\x1b[?2004h' in transcript:
                    os.write(master,(FIRST+'\r').encode()); state=1
                elif state==1 and LONG[-100:] in stored:
                    os.write(master,(SECOND+'\r').encode()); state=2
                elif state==2 and 'OWNED_MANUAL_SHORT' in stored:
                    if batch: os.write(master,b'/quit\r');state=22
                    else: display_start=len(transcript);os.write(master,b'/context\r');state=21
                elif state==22 and proc.poll() is not None:
                    assert proc.returncode==0
                    seed=json.loads(autosave.read_text())
                    template=next(m for m in seed['messages'] if LONG in json.dumps(m))
                    generated=[]
                    for index in range(batch):
                        answer=f'OWNED_BATCH_{index:04d}_OWNED_MANUAL_BEGIN_'+ 'z'*7000 + '_OWNED_MANUAL_END'
                        seeded_answers.add(answer)
                        m=json.loads(json.dumps(template));m['content'][0]['text']=answer;generated.append(m)
                    seed['messages']=seed['messages'][:1]+generated+seed['messages'][-2:]
                    autosave.write_text(json.dumps(seed));autosave.chmod(0o600)
                    os.close(master);master,slave=pty.openpty()
                    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',40,120,1200,800))
                    display_start=len(transcript)
                    proc=subprocess.Popen(command,cwd=tmp,env=env,stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
                    os.close(slave);state=23
                elif state==23 and b'\x1b[?2004h' in transcript[display_start:]:
                    display_start=len(transcript);os.write(master,b'/context\r');state=21
                elif state==21:
                    display=re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]',b'',bytes(transcript[display_start:]))
                    found=re.search(rb'Context(?: estimate)?:\s*(\d+) /',display)
                    if not found: continue
                    context_before=int(found[1])
                    before=autosave.stat().st_size; compact_mtime=autosave.stat().st_mtime_ns
                    compact_start=len(transcript)
                    os.write(master,b'/compact\r'); state=3
                elif state==3 and saved and autosave.stat().st_mtime_ns != compact_mtime:
                    receipt=bytes(transcript[compact_start:])
                    if expected_noop:
                        if b'"changed":false' not in receipt and b'DSCO_ALLOW_WRITE' not in receipt: continue
                    elif 'ck:msg:' not in stored: continue
                    after=autosave.stat().st_size
                    if expected_noop: assert LONG[-100:] in stored
                    else: assert 'ck:msg:' in stored and LONG[-100:] not in stored
                    display_start=len(transcript);os.write(master,b'/context\r');state=31
                elif state==31:
                    display=re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]',b'',bytes(transcript[display_start:]))
                    found=re.search(rb'Context(?: estimate)?:\s*(\d+) /',display)
                    if not found: continue
                    context_after=int(found[1])
                    if not expected_noop: assert context_after < context_before, 'context display did not shrink'
                    if omit_usage: assert context_after>=512, 'missing usage erased schema/instruction overhead'
                    if restart: os.write(master,b'/quit\r');state=32
                    else: os.write(master,(THIRD+'\r').encode());state=4
                elif state==32 and proc.poll() is not None:
                    assert proc.returncode==0
                    os.close(master);master,slave=pty.openpty()
                    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',40,120,1200,800))
                    display_start=len(transcript)
                    proc=subprocess.Popen(command,cwd=tmp,env=env,stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
                    os.close(slave);state=33
                elif state==33 and b'\x1b[?2004h' in transcript[display_start:]:
                    os.write(master,(THIRD+'\r').encode());state=4
                elif state==4 and FINAL in stored:
                    os.write(master,b'/quit\r'); state=5
                if proc.poll() is not None and state not in (22,32): break
            assert not errors, errors
            assert proc.poll()==0 and state==5, (proc.poll(),state,len(requests),bytes(transcript[-800:]))
            if batch:
                # Archive depth may change; verify governed leaf recall and all
                # seeded bytes below rather than requiring extra provider calls.
                assert leaf_recalled, 'governed recall did not reach an archived leaf'
            else: assert len(requests)==(3 if expected_noop else 4), len(requests)
            archives_verified=0
            if batch:
                # Read only the owned SQLite store after both processes exit.
                # Independently verify the entire recursive index, not merely
                # the one leaf fetched through governed context_recall calls.
                blobs={}
                for db in root.rglob('*'):
                    if not db.is_file() or db.name.endswith(('-wal','-shm')): continue
                    with db.open('rb') as stream: signature=stream.read(16)
                    if signature!=b'SQLite format 3\x00': continue
                    connection=sqlite3.connect(f'file:{db}?mode=ro',uri=True)
                    try:
                        for cid,value in connection.execute("SELECT key,value FROM kv WHERE bucket='ctx_blob'"):
                            data=value.encode() if isinstance(value,str) else value
                            assert hashlib.sha256(data).hexdigest()==cid
                            blobs[cid]=data
                    except sqlite3.OperationalError: pass
                    finally: connection.close()
                visited=set();recovered=set()
                def walk(handle):
                    cid=handle.split(':',2)[2]
                    if cid in visited:return
                    visited.add(cid);assert cid in blobs, f'missing durable archive {handle}'
                    for message in json.loads(blobs[cid]):
                        for block in message['content']:
                            text=block.get('text') or ''
                            if text in seeded_answers:recovered.add(text)
                            for child in re.findall(r'key=(ck:msg:[A-Za-z0-9_-]+)',text):walk(child)
                walk(root_key)
                assert recovered==seeded_answers, (len(recovered),len(seeded_answers))
                archives_verified=len(recovered)
            if write_denied: assert b'DSCO_ALLOW_WRITE' in transcript, 'denial was not explained'
            result={'passed':True,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'expected_noop':expected_noop,'binary':str(binary),
                    'requests':len(requests),'automatic_compaction':False,
                    'write_denied':write_denied,
                    'autosave_bytes_before':before,'autosave_bytes_after':after,
                    'display_context_tokens_before':context_before,'display_context_tokens_after':context_after,
                    'request_bytes_before':None if batch else len(json.dumps(requests[1])),
                    'request_bytes_after':len(json.dumps(requests[2])),
                    'retrieval_verified':not expected_noop,'restart_verified':restart,
                    'provider_usage_omitted':omit_usage,'seeded_assistant_responses':batch,
                    'exact_archives_verified_after_restart':archives_verified,
                    'recursive_governed_recall_verified':bool(batch and leaf_recalled)}
            output.mkdir(parents=True,exist_ok=True)
            (output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
            (output/'transcript.ansi').write_bytes(transcript)
            print(json.dumps(result,indent=2))
        finally:
            output.mkdir(parents=True,exist_ok=True)
            (output/'transcript.ansi').write_bytes(transcript)
            if proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM)
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=3)
            os.close(master);server.shutdown();server.server_close();thread.join(timeout=2)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,default=Path('./dsco'))
    p.add_argument('--expected-noop',action='store_true');p.add_argument('--output',type=Path,required=True)
    p.add_argument('--write-denied',action='store_true')
    p.add_argument('--restart',action='store_true');p.add_argument('--omit-usage',action='store_true')
    p.add_argument('--batch',type=int,default=0)
    a=p.parse_args();run(a.binary.resolve(strict=True),a.expected_noop or a.write_denied,a.output,a.write_denied,a.restart,a.omit_usage,a.batch)
