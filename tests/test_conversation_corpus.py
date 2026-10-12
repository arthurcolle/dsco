#!/usr/bin/env python3
"""Offline exhaustive compaction replay of local DSCO/Codex/Claude histories.

Never executes recorded tools or sends inference requests. Original stores are
read only. Raw test material lives in private scratch; the durable ledger stores
paths, hashes, counts, statuses and diagnostics. Foreign histories are adapted
to DSCO blocks, retaining unknown content verbatim as protected opaque blocks.
"""
import argparse
import collections
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
import threading
import time

STRINGS = ('type text tool_name tool_id tool_input image_media_type image_data '
           'image_url video_media_type video_data video_url doc_media_type '
           'doc_data doc_title cited_text').split()
RECEIPT = re.compile(r'^\[Archived (assistant response|completed tool exchange); key=(ck:\w+:[0-9a-f]{64});')


def encode(x):
    return json.dumps(x, ensure_ascii=False, separators=(',', ':'), sort_keys=True)


def block(kind='text', **kw):
    b = {k: None for k in STRINGS}
    b.update(type=kind, is_error=False, cite_start=0, cite_end=0)
    b.update(kw)
    return b


def opaque(x, kind='history_opaque'):
    return block(kind, text=encode(x))


def msg(role, content):
    return {'role': 'user' if role in ('user', 'system', 'developer') else 'assistant', 'content': content}


def native_message(m):
    if not isinstance(m, dict) or m.get('role') not in ('user', 'assistant') or not isinstance(m.get('content'), list):
        raise ValueError('invalid native message schema')
    out = []
    for b in m['content']:
        if not isinstance(b, dict):
            raise ValueError('invalid native content schema')
        c = block(**{k: v for k, v in b.items() if k in STRINGS + ['is_error', 'cite_start', 'cite_end']})
        if not c['type']:
            c['type'] = 'text'
        for k in STRINGS:
            if c[k] is not None and not isinstance(c[k], str):
                raise ValueError('non-string native field ' + k)
        out.append(c)
    return msg(m['role'], out)


def foreign_content(content):
    if isinstance(content, str):
        return [block(text=content)]
    if not isinstance(content, list):
        return [opaque(content)]
    out = []
    for c in content:
        if not isinstance(c, dict):
            out.append(opaque(c)); continue
        t = c.get('type')
        if t in ('text', 'input_text', 'output_text') and isinstance(c.get('text'), str):
            extra = {k: v for k, v in c.items() if k not in ('type', 'text')}
            out.append(block(text=c['text'], doc_data=encode(extra) if extra else None))
        elif t == 'tool_use':
            out.append(block('tool_use', tool_id=c.get('id'), tool_name=c.get('name'), tool_input=encode(c.get('input', {}))))
        elif t == 'tool_result':
            value = c.get('content', '')
            out.append(block('tool_result', tool_id=c.get('tool_use_id'), text=value if isinstance(value, str) else encode(value), is_error=bool(c.get('is_error', False))))
        else:
            out.append(opaque(c))
    return out


def adapt(source, raw):
    diagnostics = collections.Counter()
    if source in ('dsco', 'dsco-chronicle'):
        d = json.loads(raw)
        if not isinstance(d, dict) or not isinstance(d.get('messages'), list):
            raise ValueError('not a native saved conversation')
        return [native_message(m) for m in d['messages']], diagnostics
    if source == 'dsco-worker-result':
        d=json.loads(raw)
        if not isinstance(d,dict) or 'output' not in d: raise ValueError('invalid worker result')
        metadata={k:v for k,v in d.items() if k!='output'}
        output=d['output'] if isinstance(d['output'],str) else encode(d['output'])
        diagnostics['synthetic_protected_tail_messages']=2
        return [msg('user',[opaque(metadata,'history_worker_metadata')]),msg('assistant',[block(text=output)]),msg('user',[block(text='CORPUS PROTECTED TAIL')]),msg('assistant',[block(text='CORPUS END')])],diagnostics
    messages = []
    for line in raw.splitlines():
        if not line.strip(): continue
        try:
            d = json.loads(line)
        except (ValueError, UnicodeError):
            diagnostics['malformed_lines'] += 1
            # A torn log record is retained exactly, rather than counted as read.
            messages.append(msg('user', [block('history_unparsed', text=line.decode('utf-8', errors='surrogateescape'))])); continue
        if source == 'claude':
            if d.get('type') in ('user', 'assistant') and isinstance(d.get('message'), dict):
                m = d['message']; messages.append(msg(m.get('role', d['type']), foreign_content(m.get('content', ''))))
            else:
                diagnostics['non_message_events'] += 1
            continue
        if d.get('type') == 'session_meta':
            instructions = d.get('payload', {}).get('base_instructions')
            if instructions:
                messages.append(msg('user', [opaque(instructions, 'history_instruction')]))
            continue
        if d.get('type') == 'compacted':
            # Preserve historical summary/boundary; replay append-only recorded
            # messages, not the provider's replacement of its active window.
            p = d.get('payload', {})
            messages.append(msg('user', [opaque(p, 'history_compaction')]))
            diagnostics['historical_compactions'] += 1
            continue
        if d.get('type') != 'response_item':
            diagnostics['non_message_events'] += 1; continue
        p = d.get('payload', {}); t = p.get('type')
        if t == 'message':
            messages.append(msg(p.get('role'), foreign_content(p.get('content', []))))
        elif t in ('function_call', 'custom_tool_call'):
            c = block('tool_use', tool_id=p.get('call_id'), tool_name=p.get('name'), tool_input=p.get('arguments', p.get('input', '{}')))
            if not isinstance(c['tool_input'], str): c['tool_input'] = encode(c['tool_input'])
            # Parallel call records form one assistant call message; split result
            # messages remain split, exercising the production span matcher.
            if messages and messages[-1]['role'] == 'assistant' and all(b['type'] == 'tool_use' for b in messages[-1]['content']):
                messages[-1]['content'].append(c)
            else:
                messages.append(msg('assistant', [c]))
        elif t in ('function_call_output', 'custom_tool_call_output'):
            v = p.get('output', '')
            messages.append(msg('user', [block('tool_result', tool_id=p.get('call_id'), text=v if isinstance(v, str) else encode(v))]))
        elif t == 'reasoning':
            messages.append(msg('assistant', [opaque(p, 'openai_codex_reasoning:corpus-replay')]))
        else:
            messages.append(msg('assistant', [opaque(p)])); diagnostics['opaque_response_items'] += 1
    return messages, diagnostics


def digest(x):
    return hashlib.sha256(encode(x).encode()).hexdigest()


class Worker:
    def __init__(self, driver, scratch):
        self.root = Path(tempfile.mkdtemp(prefix='worker-', dir=scratch))
        self.db = self.root / 'archive.db'
        env = {k: v for k, v in os.environ.items() if not k.startswith('DSCO_')}
        # One isolated principal processes thousands of independent fixtures.
        # Keep the production gate active; give this test principal a declared
        # corpus budget rather than exhausting an ordinary interactive budget.
        env.update(HOME=str(self.root), DSCO_CONTEXT_DB=str(self.db), DSCO_AGENT_BUDGET_GSU='1000000000')
        self.log = open(self.root / 'driver.log', 'w')
        self.proc = subprocess.Popen([str(driver), str(self.root)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True, env=env)
        self.conn = None

    def close(self):
        self.proc.stdin.close(); self.proc.wait(); self.log.close()
        if self.conn: self.conn.close()

    def expand(self, messages, known, seen):
        i = 0
        while i < len(messages):
            m = messages[i]
            if digest(m) in known:
                yield m; i += 1; continue
            b = m['content'][0] if len(m['content']) == 1 else {}
            r = RECEIPT.match(b.get('text') or '')
            if b.get('tool_name') == 'context_evict' and r:
                key = r.group(2); cid = key.rsplit(':', 1)[1]
                if cid in seen: raise AssertionError('archive index cycle')
                if not self.conn:
                    self.conn=sqlite3.connect(f'file:{self.db}?mode=ro', uri=True, check_same_thread=False)
                row = self.conn.execute("select value from kv where bucket='ctx_blob' and key=?", (cid,)).fetchone()
                if not row: raise AssertionError('new archive missing')
                value = row[0].encode() if isinstance(row[0], str) else row[0]
                if hashlib.sha256(value).hexdigest() != cid: raise AssertionError('archive hash mismatch')
                yield from self.expand(json.loads(value), known, seen | {cid})
                i += 1; continue
            if b.get('tool_name') == 'context_evict' and b.get('text') == 'Completed tool work is archived; consult the following retrieval receipt.':
                if i+1 >= len(messages) or not RECEIPT.match(messages[i+1]['content'][0].get('text') or ''):
                    raise AssertionError('tool receipt missing its archive')
                i += 1; continue
            yield m; i += 1

    def run(self, source, path):
        started = time.monotonic()
        result = {'source': source, 'path': str(path)}
        try:
            st = path.stat(); raw = path.read_bytes()
            result.update(source_bytes=len(raw), source_sha256=hashlib.sha256(raw).hexdigest())
            messages, diagnostics = adapt(source, raw)
            if source in ('codex','claude'):
                # C string fields cannot contain embedded NUL bytes. Preserve
                # the complete original block as an opaque JSON value whose
                # NULs are escaped, rather than silently truncating an import.
                for m in messages:
                    for i,b in enumerate(m['content']):
                        if any(isinstance(v,str) and '\x00' in v for v in b.values()):
                            m['content'][i]=opaque({'original_block':b},'history_embedded_nul')
                            diagnostics['nul_blocks_preserved']+=1
            result['adapter'] = dict(diagnostics)
            result['messages'] = len(messages)
            result['source_changed_during_read'] = (path.stat().st_mtime_ns, path.stat().st_size) != (st.st_mtime_ns, st.st_size)
            expected = [digest(m) for m in messages]
            known = set(expected)
            p = self.root/'input.json'
            # Native input uses the actual historical bytes: catch real loader
            # bugs, not just the adapter's explicitly filled nullable fields.
            if source in ('dsco','dsco-chronicle'): p.write_bytes(raw)
            else: p.write_text(encode({'messages': messages}))
            del raw, messages
            result['normalized_bytes'] = p.stat().st_size
            self.proc.stdin.write(str(p)+'\n'); self.proc.stdin.flush()
            line = self.proc.stdout.readline()
            if not line: raise RuntimeError('production driver exited')
            state = json.loads(line)
            result['driver'] = state
            if not all(state[k] for k in ('load', 'io', 'reload', 'stable')):
                raise AssertionError('production ' + next(k for k in ('load', 'io', 'reload', 'stable') if not state[k]))
            if not state['ok']:
                if 'unresolved tool protocol cannot be safely evicted' not in (state.get('receipt') or {}).get('detail',''):
                    raise AssertionError('unexpected production compaction refusal')
                result['retained_incomplete_protocol']=True
            before = json.loads((self.root/'before.json').read_bytes())
            actual = [digest(m) for m in before]
            if actual != expected:
                result['first_mismatch'] = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]), min(len(actual), len(expected)))
                raise AssertionError('native loader changed original message fields')
            protected=collections.Counter()
            for i,m in enumerate(before):
                receipt_message=all(b.get('tool_name')=='context_evict' for b in m['content']) and bool(m['content'])
                # Legacy tool receipts did not tag the user-side receipt with
                # tool_name. Require its valid key and paired archival marker.
                if i>0 and len(m['content'])==1 and RECEIPT.match(m['content'][0].get('text') or ''):
                    previous=before[i-1]
                    receipt_message |= previous['role']=='assistant' and len(previous['content'])==1 and previous['content'][0].get('tool_name')=='context_evict'
                results_only=bool(m['content']) and all(b['type']=='tool_result' for b in m['content'])
                if i==0 or i>=len(before)-2 or (m['role']=='user' and not results_only and not receipt_message):
                    protected[digest(m)]+=1
            del before, actual
            after_bytes = (self.root/'after.json').read_bytes()
            after = json.loads(after_bytes)
            inline=collections.Counter(digest(m) for m in after)
            if any(inline[k]<n for k,n in protected.items()): raise AssertionError('protected user/seed/tail message was removed from live history')
            result['protected_inline_messages']=sum(protected.values())
            reload = json.loads((self.root/'reloaded.json').read_bytes())
            if after != reload: raise AssertionError('save/reload changed message fields')
            if after != json.loads((self.root/'second.json').read_bytes()): raise AssertionError('second compaction mutated history')
            del reload
            reconstructed = [digest(m) for m in self.expand(after, known, set())]
            if reconstructed != expected: raise AssertionError('archive reconstruction changed message order or fields')
            # Exact reconstruction implies every user, seed, live call, media,
            # opaque reasoning and protected tail survives inline or in archive.
            result.update(status='pass', messages_after=len(after), before_bytes=(self.root/'before.json').stat().st_size, after_bytes=len(after_bytes), original_digest=digest(expected), reconstructed_digest=digest(reconstructed))
        except Exception as exc:
            result.update(status='fail', error=type(exc).__name__+': '+str(exc))
        result['seconds'] = round(time.monotonic()-started, 4)
        return result


def inventory(home):
    roots = [('dsco',home/'.dsco/sessions','*.json'),('dsco',home/'.dsco/archive/sessions','*.json'),('codex',home/'.codex/sessions','*.jsonl'),('codex',home/'.codex/archived_sessions','*.jsonl'),('claude',home/'.claude/projects','*.jsonl')]
    files=[]; seen=set()
    for source, root, pattern in roots:
        if root.exists():
            for p in sorted(root.rglob(pattern)):
                if p.is_file() and str(p.resolve()) not in seen:
                    seen.add(str(p.resolve()));files.append(('dsco-worker-result' if source=='dsco' and '.RESULT.' in p.name else source,p))
    return roots,files


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--driver',type=Path,default=Path('build/conversation_corpus_driver'))
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--workers',type=int,default=4)
    ap.add_argument('--limit',type=int,default=0,help='0 means all inventoried histories')
    ap.add_argument('--source',choices=['dsco','dsco-worker-result','dsco-chronicle','codex','claude'])
    ap.add_argument('--paths',type=Path,help='JSON list of explicit source/path pairs for reproducing failures')
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=True);os.chmod(args.output,0o700)
    roots,files=inventory(Path.home())
    if args.paths: files=[(x['source'],Path(x['path'])) for x in json.loads(args.paths.read_text())]
    if args.source: files=[x for x in files if x[0]==args.source]
    if args.limit: files=files[:args.limit]
    summary={'scope':'all discovered files' if not args.limit and not args.paths and not args.source else 'selected files','roots':[{'source':s,'path':str(p),'pattern':g,'exists':p.exists()} for s,p,g in roots], 'inventoried':len(files),'sources':dict(collections.Counter(s for s,p in files)),'source_bytes':sum(p.stat().st_size for s,p in files),'driver_sha256':hashlib.sha256(args.driver.read_bytes()).hexdigest()}
    (args.output/'inventory.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('INVENTORY',json.dumps(summary['sources']),summary['source_bytes'],flush=True)
    local=threading.local();workers=[];lock=threading.Lock()
    with tempfile.TemporaryDirectory(prefix='dsco-history-replay-') as scratch:
        os.chmod(scratch,0o700)
        def task(item):
            if not hasattr(local,'worker'):
                local.worker=Worker(args.driver.resolve(),scratch)
                with lock:workers.append(local.worker)
            return local.worker.run(*item)
        counts=collections.Counter(); failures=[]; changed=0;totals=collections.Counter(); by_source=collections.defaultdict(collections.Counter);last=time.monotonic();started=last
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool, open(args.output/'results.jsonl','w') as ledger:
            # Publish each completed fixture immediately; a large conversation
            # must not hide thousands of already completed sibling fixtures.
            jobs=[pool.submit(task,item) for item in files]
            for finished in concurrent.futures.as_completed(jobs):
                r=finished.result()
                ledger.write(json.dumps(r,ensure_ascii=True)+'\n'); ledger.flush()
                counts[r['status']]+=1;by_source[r['source']][r['status']]+=1
                if r['status']=='fail':failures.append(r)
                else:
                    changed+=bool(r['driver']['receipt'].get('changed'))
                    for k in ('messages','messages_after','before_bytes','after_bytes'):totals[k]+=r.get(k,0)
                if time.monotonic()-last>15:
                    print('PROGRESS',sum(counts.values()),dict(counts),'changed',changed,flush=True);last=time.monotonic()
        for w in workers:w.close()
        summary.update(status='pass' if not failures else 'fail',counts=dict(counts),by_source={k:dict(v) for k,v in by_source.items()},changed=changed,totals=dict(totals),seconds=round(time.monotonic()-started,2),failure_count=len(failures),checks=['native load equals independent normalization','governed production compaction','exact ordered reconstruction through SHA256 verified SQLite archive graph','protected user, seed and recent tail stay inline','all-field save/reload equality','second compaction is a no-op'],limitations=['Foreign histories test adapted message contents, not replay of recorded actions or provider behavior.','Non-message telemetry events excluded; historical compaction boundaries retained.','Worker output fixtures add two protected tail markers to test old output archival.','Preexisting receipt messages are preserved as original content; old archive availability is not newly established.','Files appended during snapshot are flagged; snapshot bytes are tested.'])
        (args.output/'failures.json').write_text(json.dumps(failures,indent=2)+'\n')
        (args.output/'validation.json').write_text(json.dumps(summary,indent=2)+'\n')
        print('COMPLETE',json.dumps({k:summary[k] for k in ('status','counts','by_source','changed','totals','seconds')}),flush=True)
    return 0 if not failures else 1


if __name__=='__main__': raise SystemExit(main())
