#!/usr/bin/env python3
"""Exercise the shipped MCP health dispatch with subprocess/network grants off."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'dsco'
requests=[
 {'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'health-regression','version':'1'}}},
 {'jsonrpc':'2.0','method':'notifications/initialized'},
 {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'swarm','arguments':{'action':'health','stall_threshold_seconds':60,'worker_limit':20}}}]
env=os.environ.copy()
env.update(DSCO_PRICING_OFFLINE='1',DSCO_ALLOW_RUN='0',DSCO_ALLOW_NET='0',DSCO_ALLOW_CONTROL='0',DSCO_GOV_MODEL='standard')
env.pop('DSCO_GOV_BYPASS',None)
with tempfile.TemporaryDirectory(prefix='dsco-health-mcp-') as directory:
 result=subprocess.run([str(binary),'mcp','serve','--toolsets','all','--tier','trusted'],
  input=''.join(json.dumps(r)+'\n' for r in requests),capture_output=True,text=True,env=env,cwd=directory,timeout=30)
 messages=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
 reply=next((m for m in messages if m.get('id')==2),None)
 assert reply and 'result' in reply, (reply,result.stderr[-2000:])
 assert not reply['result'].get('isError'),reply
 text=''.join(c.get('text','') for c in reply['result']['content'])
 health=json.loads(text)
 assert health['schema']=='dsco.swarm_health.v1',health
 assert health['counts']['active']==0 and health['workers']==[]
 assert health['costs']['includes_subscriptions'] is True
 assert health['last_observed_output_seconds_ago'] is None
 assert health['automatic_kill'] is False
 print('PASS: actual MCP swarm health works with run/net/control disabled; valid empty-runtime snapshot')
