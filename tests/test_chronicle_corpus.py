#!/usr/bin/env python3
"""Read-only Chronicle snapshot export and production corpus replay.

All sessions are inventoried. Recorded user messages, assistant output and tool
calls/results are replayed in Chronicle sequence order. Startup-only sessions
are counted separately. Missing/corrupt blobs are recorded as unavailable, never
silently promoted to a complete-history pass. Historical request snapshots are
counted; they are overlapping provider views, not additional conversations.
"""
import argparse
import base64
import collections
import hashlib
import itertools
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
from test_conversation_corpus import block, msg, encode, opaque


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--driver',type=Path,default=Path('build/conversation_corpus_driver'))
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    root=Path.home()/'.dsco/chronicle';db=root/'indexes/chronicle.sqlite'
    conn=sqlite3.connect(f'file:{db}?mode=ro',uri=True);conn.execute('BEGIN')
    summary={'database':str(db),'read_only':True,'sessions':conn.execute('select count(*) from sessions').fetchone()[0],'events':conn.execute('select count(*) from events').fetchone()[0],'request_snapshots':conn.execute("select count(*) from events where event_type='llm.request.created'").fetchone()[0]}
    blobs={r[0]:r[1:] for r in conn.execute('select sha256,local_path,codec,encryption,byte_len from blobs')}
    failures=[];stats=collections.Counter();started=time.monotonic()
    types=('user.message','llm.response.completed','tool.call.created','tool.call.completed')
    rows=conn.execute('select session_id,seq,event_id,event_type,span_id,payload_json from events where event_type in (?,?,?,?) order by session_id,seq,wall_time,event_id',types)
    with tempfile.TemporaryDirectory(prefix='dsco-chronicle-replay-') as tmp:
        scratch=Path(tmp);paths=[];session_ledger=[]
        def read_blob(key,session,event):
            if not key: return None
            try:
                path,codec,encryption,size=blobs[key]
                if codec!='raw' or encryption!='none':raise ValueError('unsupported blob encoding')
                raw=(root/path).read_bytes()
                if len(raw)!=size or hashlib.sha256(raw).hexdigest()!=key:raise ValueError('blob hash or length mismatch')
                stats['blob_reads_verified']+=1;stats['blob_bytes_verified']+=len(raw)
                try: return raw.decode('utf-8')
                except UnicodeDecodeError:
                    stats['binary_blobs_preserved']+=1
                    return {'encoding':'base64','sha256':key,'bytes':len(raw),'data':base64.b64encode(raw).decode('ascii')}
            except Exception as exc:
                failures.append({'session_id':session,'event_id':event,'blob_sha256':key,'error':type(exc).__name__+': '+str(exc)})
                return None
        for session,group in itertools.groupby(rows,key=lambda r:r[0]):
            history=[];pending={};events=0;initial_failures=len(failures)
            for _,seq,event,t,span,payload in group:
                events+=1
                if not payload:stats['metadata_only_events']+=1;continue
                d=json.loads(payload)
                if t=='user.message':
                    text=read_blob(d.get('blob_sha256'),session,event)
                    if text is not None:history.append(msg('user',[block(text=text) if isinstance(text,str) else opaque(text,'history_binary')]))
                    else:stats['events_without_recorded_content']+=1
                elif t=='llm.response.completed':
                    text=read_blob(d.get('output_blob_sha256'),session,event)
                    if text:history.append(msg('assistant',[block(text=text) if isinstance(text,str) else opaque(text,'history_binary')]))
                    else:stats['events_without_recorded_content']+=1
                elif t=='tool.call.created':
                    text=read_blob(d.get('args_blob_sha256'),session,event)
                    if text is not None:
                        if not isinstance(text,str):
                            history.append(msg('assistant',[opaque(text,'history_binary_tool_arguments')]));continue
                        cid=d.get('tool_id') or span or event
                        pending[span]=(cid,d.get('tool_name'))
                        c=block('tool_use',tool_id=cid,tool_name=d.get('tool_name'),tool_input=text)
                        if history and history[-1]['role']=='assistant' and all(b['type']=='tool_use' for b in history[-1]['content']):history[-1]['content'].append(c)
                        else:history.append(msg('assistant',[c]))
                    else:stats['events_without_recorded_content']+=1
                else:
                    text=read_blob(d.get('result_blob_sha256'),session,event)
                    if text is not None:
                        if not isinstance(text,str):
                            history.append(msg('user',[opaque(text,'history_binary_tool_result')]));continue
                        call=pending.pop(span,None)
                        if call:history.append(msg('user',[block('tool_result',tool_id=call[0],text=text,is_error=not d.get('ok',True))]))
                        else:history.append(msg('user',[opaque({'text':text,'event_id':event},'history_uncorrelated_tool_result')]));stats['uncorrelated_results']+=1
                    else:stats['events_without_recorded_content']+=1
            if history:
                p=scratch/(session+'.json');p.write_text(encode({'messages':history}))
                paths.append({'source':'dsco-chronicle','path':str(p)})
                session_ledger.append({'session_id':session,'events':events,'messages':len(history),'blob_errors':len(failures)-initial_failures})
            else:stats['sessions_without_replayable_content']+=1
            if len(session_ledger)%500==0 and history:print('EXPORTED',len(session_ledger),flush=True)
        conn.rollback();conn.close()
        summary.update(conversation_event_sessions=len(session_ledger)+stats['sessions_without_replayable_content'],replayable_sessions=len(paths),sessions_without_conversation_events=summary['sessions']-len(session_ledger)-stats['sessions_without_replayable_content'],stats=dict(stats),unavailable_blob_count=len(failures),export_seconds=round(time.monotonic()-started,2))
        (args.output/'chronicle-inventory.json').write_text(json.dumps(summary,indent=2)+'\n')
        (args.output/'unavailable-blobs.json').write_text(json.dumps(failures,indent=2)+'\n')
        (args.output/'chronicle-sessions.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in session_ledger))
        manifest=scratch/'manifest.json';manifest.write_text(json.dumps(paths))
        print('CHRONICLE INVENTORY',json.dumps(summary),flush=True)
        code=subprocess.call([sys.executable,str(Path(__file__).with_name('test_conversation_corpus.py')),'--driver',str(args.driver),'--output',str(args.output/'replay'),'--paths',str(manifest)])
    return code


if __name__=='__main__':raise SystemExit(main())
