#!/usr/bin/env python3
"""Real CLI + Unix IPC + durable replay + gate failure checks, no inference."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lingo_stream', ROOT / 'scripts/lingo_stream.py')
stream = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stream)
BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'dsco').resolve()
ENV = {k: v for k, v in os.environ.items() if not k.startswith(('DSCO_ALLOW_', 'DSCO_GOV_', 'DSCO_EVENT_STREAM', 'DSCO_GOAL', 'DSCO_ACTIVE_GOAL'))}
ENV.update(DSCO_GOV_MODEL='standard', DSCO_AUTO_GOAL='0', DSCO_NO_COLOR='1')

with tempfile.TemporaryDirectory(prefix='lingo-ipc-tests-') as temp:
    root = Path(temp)
    source = '''
local l=require("lingo")
local w=l.world{id="ipc-proof"}
w:define("Input", {x=l.stored("integer"), doubled=l.derived("integer",function(self)return self.x*2 end)})
w:new("Input","one",{x=2})
local one=w:ref("one")
assert(one.doubled==4)
assert(one.doubled==4)
w:scenario("possible", function() w:override(one,"x",9); assert(one.doubled==18) end)
w:set(one,"x",3)
assert(one.doubled==6)
for i=1,4600 do assert(one.x==3) end
local denied=l.call("write_file",{path=args.path,content="must not happen"})
assert(not denied.ok)
return {answer=one.doubled,denied=denied.result}
'''
    result, events, metrics = stream.capture(BINARY, ['eval', source, json.dumps({'path': str(root/'denied.txt')})], root/'run', ENV | {'DSCO_ALLOW_WRITE': '0'}, quiet=True)
    assert metrics['exit_code'] == 0 and result['value']['answer'] == 6, result
    assert not (root/'denied.txt').exists()
    records = [e['record'] for e in events]
    semantic = [e for e in records if e['source']=='lingo']
    assert len(semantic) > 4600 and len({e['payload']['seq'] for e in semantic if e['payload'].get('world_id')=='ipc-proof'}) > 4600
    kinds = {e['event'] for e in semantic}
    for name in ('world_created','stored_read','calculated','dependency_added','dependency_removed','invalidated','cache_hit'):
        assert name in kinds, (name, kinds)
    assert any(e['event']=='governance.stage' and e['payload']['stage']=='capability_floor' and e['payload']['enforced'] for e in records)
    assert any(e['event']=='tool.returned' and e['payload']['tool']=='write_file' and 'DSCO_ALLOW_WRITE' in e['payload']['detail'] for e in records)
    assert records[-1]['event']=='stream.closed'
    print('PASS native Lingo events beyond inspector capacity; exact IPC/outbox equality; governed write denial')
    cursor = events[len(events)//2]['seq']
    replay, replayed, replay_metrics = stream.capture(BINARY, ['replay', result['stream']['db_path'], '--after', str(cursor)], root/'replay', ENV, quiet=True)
    assert replay_metrics['exit_code']==0 and replayed == [e for e in events if e['seq']>cursor]
    print('PASS native replay from acknowledged cursor returns identical remaining frames')
    failed_env = ENV | {'DSCO_EVENT_STREAM_DB': str(root/'missing.sqlite')}
    code = f'require("lingo").call("write_file",{{path={json.dumps(str(root/"blocked.txt"))},content="no"}})'
    denied = subprocess.run([str(BINARY),'lingo','eval',code],env=failed_env,capture_output=True,text=True,timeout=15)
    assert denied.returncode != 0 and not (root/'blocked.txt').exists(), denied.stdout
    assert 'event_capture_failed' in denied.stdout, denied.stdout
    print('PASS unavailable recorder fails closed before script/tool execution')
    oversized='''local l=require("lingo");local w=l.world{id="oversized"};
    w:define("X",{x=l.stored("string")});
    l.try(function()w:new("X","x",{x=string.rep("x",300000)})end);
    return "escaped"'''
    failed, _, failed_metrics = stream.capture(BINARY, ['eval', oversized], root/'encoding-failure', ENV,
        quiet=True, expect_capture_error=True)
    assert failed_metrics['exit_code'] != 0 and 'event capture failed' in failed['error'], failed
    print('PASS semantic event encoding failure poisons the recorder and cannot be caught to continue')
    print(json.dumps({'native_events':len(events),'emission_to_consumer_ms':metrics['emission_to_consumer_ms']}))
