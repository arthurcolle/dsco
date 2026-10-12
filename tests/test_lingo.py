#!/usr/bin/env python3
"""Lingo semantic and live governed-host conformance; no model or network needed."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
BINARY=str(Path(sys.argv[1] if len(sys.argv)>1 else ROOT/'dsco').resolve())
ENV={k:v for k,v in os.environ.items() if not k.startswith(('DSCO_ALLOW_', 'DSCO_GOV_', 'DSCO_TRUST_', 'DSCO_EXECUTION_'))}
ENV.update(DSCO_GOV_MODEL='standard',DSCO_NO_COLOR='1')
checks=[]
def invoke(source=None, path=None, args=None, env=None, mode=None, success=True):
    cmd=[BINARY]+(['--trust-tier',env['DSCO_TRUST_TIER']] if env and 'DSCO_TRUST_TIER' in env else [])+['lingo',mode or ('run' if path else 'eval'),str(path) if path else source]
    if args is not None: cmd.append(json.dumps(args))
    proc=subprocess.run(cmd,cwd=ROOT,env=ENV| (env or {}),capture_output=True,text=True,timeout=20)
    assert (proc.returncode==0)==success,(cmd,proc.returncode,proc.stdout,proc.stderr)
    return json.loads(proc.stdout)
def check(name,fn):
    fn();checks.append(name);print('PASS',name)
def error(source,contains,env=None):
    result=invoke(source=source,env=env,success=False)
    assert contains in json.dumps(result),result

def semantics():
    result=invoke(path='tests/lingo_semantics.lingo')['value']
    assert result['passed']==24,result
    checks.extend(result['tests'])
check('semantic suite',semantics)
check('routing example',lambda: (lambda v: v['baseline']==200 and v['scenario']['cost']==700 and v['repriced']==300 or (_ for _ in ()).throw(AssertionError(v)))(invoke(path='examples/lingo/routing.lingo')['value']))
check('nested diddles example',lambda: (lambda v: v=={'base':7,'hypothetical':8,'after':7} or (_ for _ in ()).throw(AssertionError(v)))(invoke(path='examples/lingo/diddles.lingo')['value']))
with tempfile.TemporaryDirectory(prefix='lingo-conformance-') as directory:
    p=Path(directory);(p/'input.txt').write_text('Lingo test observation\n')
    def live_io():
        r=invoke(path='examples/lingo/file-review.lingo',args={'input':str(p/'input.txt'),'output':str(p/'summary.json')})
        assert r['tool_calls']==2 and json.loads((p/'summary.json').read_text())==r['value']
    check('real read and write via DSCO',live_io)
    def denied_io():
        r=invoke(path='examples/lingo/file-review.lingo',args={'input':str(p/'input.txt'),'output':str(p/'denied.json')},env={'DSCO_ALLOW_WRITE':'0'},success=False)
        assert 'DSCO_ALLOW_WRITE' in r['error'] and not (p/'denied.json').exists(),r
    check('nested write respects capability denial',denied_io)
    def granted_script_keeps_tier():
        source='local l=require("lingo");local r=l.call("write_file",{path=args.path,content="denied"});assert(not r.ok and r.result:find("untrusted",1,true),r.result);return true'
        invoke(source=source,args={'path':str(p/'untrusted.txt')},env={'DSCO_TRUST_TIER':'untrusted','DSCO_ALLOW_RUN':'1'})
        assert not (p/'untrusted.txt').exists()
    check('exec grant admits script without elevating nested tool authority',granted_script_keeps_tier)

    (p/'syntax.lingo').write_text('error("must not execute")')
    check('check compiles without executing',lambda: invoke(path=p/'syntax.lingo',mode='check'))
    (p/'bad.lingo').write_text('local =')
    check('syntax failure reported',lambda: invoke(path=p/'bad.lingo',mode='check',success=False))
check('execution opt-out denies outer script',lambda:error('return 1','DSCO_ALLOW_RUN',{'DSCO_ALLOW_RUN':'0'}))
check('caller trust tier retained',lambda:error('return 1','untrusted',{'DSCO_TRUST_TIER':'untrusted'}))
check('instruction exhaustion is terminal',lambda:error('local l=require("lingo"); while true do l.try(function()while true do end end) end','budget exhausted'))
check('memory exhaustion is terminal',lambda:error('local l=require("lingo");l.try(function()return string.rep("x",64*1024*1024)end);return "escaped"','budget exhausted'))
check('binary chunks rejected',lambda:error('\x1bLJ\x02','wrong mode'))
check('recursive host invocation fails',lambda:invoke(source='local l=require("lingo");local r=l.call("lingo",{source="return 1"});assert(not r.ok and r.result:find("recursive",1,true));return true'))
check('bounded JSON expansion',lambda:error('local l=require("lingo");local x=string.rep("x",100000);return l.encode({x,x,x,x})','JSON size limit'))
# Same offline classifier probes as test-gate-claims; no secrets or URL are accessed.
check('same-session flow taint survives nested calls',lambda:invoke(source='''
local l=require("lingo")
local a=l.call("bash",{command='printf "%s\\n" "https://example.invalid/synthetic-untrusted"'})
assert(a.ok,a.result)
local b=l.call("bash",{command='printf "%s\\n" "~/.ssh/id_rsa synthetic-secret-marker"'})
assert(b.ok,b.result)
local c=l.call("bash",{command='printf gate-probe'})
assert(not c.ok and c.result:find("lethal-trifecta",1,true),c.result)
return true
'''))
def mcp_recovery():
    requests=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{
        'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'lingo-conformance','version':'1'}}}]
    for identifier,source in [(2,'local l=require("lingo");return l.decode("invalid")'),(3,'return 42')]:
        requests.append({'jsonrpc':'2.0','id':identifier,'method':'tools/call','params':{'name':'lingo','arguments':{'source':source}}})
    proc=subprocess.run([BINARY,'mcp','serve','--toolsets','all','--tier','trusted'],
        input='\n'.join(map(json.dumps,requests))+'\n',cwd=ROOT,env=ENV,capture_output=True,text=True,timeout=20)
    replies={x['id']:x for x in map(json.loads,proc.stdout.splitlines())}
    assert proc.returncode==0 and replies[2]['result']['isError'],(proc.stdout,proc.stderr)
    assert not replies[3]['result']['isError']
    assert json.loads(replies[3]['result']['content'][0]['text'])['value']==42
check('MCP host survives a callback error and runs the next script',mcp_recovery)
print(json.dumps({'passed':len(checks),'checks':checks},indent=2))
