#!/usr/bin/env python3
"""Black-box boundary probes against the installed command; no response mocks."""
import hashlib,json,os,pathlib,subprocess,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path.home()/'.local/bin/dsco-local-coder'
OUT=ROOT/'reports/local-coder-deep/cli-boundaries.json'
LIVE='http://127.0.0.1:18080/v1'
CASES=[]
def case(name,args,exit_code,env=None):CASES.append((name,args,exit_code,env or {}))
case('missing_prompt',[],2)
case('empty_prompt',[''],2)
for name,url in [
 ('external_http','http://example.invalid/v1'),('external_https','https://example.invalid/v1'),
 ('ftp','ftp://127.0.0.1/v1'),('credentials','http://user:fake-test-password@127.0.0.1:18080/v1'),
 ('username_only','http://user@127.0.0.1:18080/v1'),('query',LIVE+'?x=1'),('fragment',LIVE+'#x'),
 ('wrong_path','http://127.0.0.1:18080/v2'),('empty_host','http://:18080/v1'),
 ('malformed_ipv6','http://[::1/v1'),('nonnumeric_port','http://127.0.0.1:abc/v1'),
 ('port_too_large','http://127.0.0.1:65536/v1'),('negative_port','http://127.0.0.1:-1/v1'),
 ('zero_port','http://127.0.0.1:0/v1'),('newline',LIVE+'\n'),('tab','http://127.0.0.1\t:18080/v1'),
 ('lookalike_host','http://127.0.0.1.evil.invalid/v1'),('numeric_alias','http://2130706433/v1'),
 ('short_ipv4','http://127.1/v1'),('ipv6_zone','http://[::1%lo0]:18080/v1')]:
 case(name,['--check','--endpoint',url],2)
case('trailing_slash',['--check','--endpoint',LIVE+'/'],0)
case('uppercase_hostname',['--check','--endpoint','http://LOCALHOST:18080/v1'],0)
case('cli_overrides_env',['--check','--endpoint',LIVE],0,{'DSCO_CODER_ENDPOINT':'http://127.0.0.1:1/v1'})
case('valid_env_endpoint',['--check'],0,{'DSCO_CODER_ENDPOINT':LIVE})
case('invalid_env_endpoint',['--check'],2,{'DSCO_CODER_ENDPOINT':'http://example.invalid/v1'})
case('empty_env_endpoint',['--check'],2,{'DSCO_CODER_ENDPOINT':''})
case('missing_binary',['--dsco-bin','/nonexistent/dsco-deep-test','No tools needed.'],2)
case('invalid_trust',['No tools needed.'],2,{'DSCO_TRUST_TIER':'not-a-tier'})
case('invalid_approval',['No tools needed.'],2,{'DSCO_APPROVAL_MODE':'not-a-mode'})
case('check_is_service_only',['--check'],0,{'DSCO_TRUST_TIER':'not-a-tier'})
results=[]
with tempfile.TemporaryDirectory(prefix='local-coder-cli-') as tmp:
 nonexec=pathlib.Path(tmp)/'not-executable';nonexec.write_text('not executable');nonexec.chmod(0o644)
 case('nonexecutable_binary',['--dsco-bin',str(nonexec),'No tools needed.'],2)
 case('directory_as_binary',['--dsco-bin',tmp,'No tools needed.'],2)
 case('nonexistent_directory',['--check','--directory',tmp+'/absent'],2)
 for name,args,expected,overrides in CASES:
  env=os.environ.copy();env.pop('DSCO_CODER_ENDPOINT',None);env.update(overrides)
  start=time.monotonic()
  try:
   r=subprocess.run([str(BIN),*args],capture_output=True,text=True,env=env,timeout=12)
   passed=r.returncode==expected and 'Traceback (most recent call last)' not in r.stderr
   if name=='credentials':passed=passed and 'fake-test-password' not in r.stderr
   result={'case':name,'passed':passed,'expected_exit':expected,'actual_exit':r.returncode,'seconds':time.monotonic()-start,'stderr':r.stderr[:1400],'stdout':r.stdout[:1000]}
  except subprocess.TimeoutExpired:result={'case':name,'passed':False,'timeout':True,'seconds':time.monotonic()-start}
  results.append(result);print(json.dumps({'case':name,'passed':result['passed'],'actual_exit':result.get('actual_exit')}),flush=True)
summary={'cases':len(results),'passed':sum(x['passed'] for x in results),'failed':sum(not x['passed'] for x in results),'installed_sha256':hashlib.sha256(BIN.read_bytes()).hexdigest(),'results':results,'scope':'Installed launcher, real argument parsing and live service checks. No model inference requested by these cases; invalid binary/policy cases must fail before exec. No source modifications.'}
OUT.write_text(json.dumps(summary,indent=2)+'\n');assert json.loads(OUT.read_text())==summary
print('SUMMARY',summary['cases'],summary['passed'],summary['failed'])
