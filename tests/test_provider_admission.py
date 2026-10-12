#!/usr/bin/env python3
"""Owned cross-process admission: fairness, crash cleanup, migration, fail-closed state."""
import argparse,fcntl,json,os,pathlib,signal,subprocess,tempfile,time,uuid
from test_subscription_gate_wait import HELPER
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--output',type=pathlib.Path,required=True);a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='dsco-admission-') as tmp:
  root=pathlib.Path(tmp);src=root/'probe.c';src.write_text(HELPER);binary=root/'probe'
  subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-g','-Iinclude',str(src),'src/subscription_gate.c','src/provider_admission.c','-o',str(binary)],cwd=ROOT,check=True)
  env=dict(os.environ,HOME=tmp,DSCO_CHATGPT_GLOBAL_GATE='1',DSCO_CHATGPT_GATE_MAX_WAIT_MS='1000',DSCO_CHATGPT_LANE_CONCURRENCY='1',DSCO_CHATGPT_MIN_INTERVAL_MS='0');children=[];results={}
  def base(scope):
   h=1469598103934665603
   for b in scope.encode():h=((h^b)*1099511628211)&((1<<64)-1)
   return root/'.dsco'/f'openai-codex-gate-{h:016x}'
  def spawn(mode,scope,session='A',worker=False,**extra):
   e=dict(env,DSCO_ADMISSION_SESSION=session,DSCO_WORKER='1' if worker else '0',DSCO_SWARM_DEPTH='0');e.update(extra)
   p=subprocess.Popen([str(binary),mode,scope],env=e,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);children.append(p);return p
  def collect(p):
   out,err=p.communicate(timeout=4);assert p.returncode==0,(out,err);r=json.loads(out.strip());assert r['fd_before']==r['fd_after'],r;return r
  def holder(scope):
   p=spawn('holder',scope);assert json.loads(p.stdout.readline())['ready'];return p
  def release(p):p.stdin.write('g\n');p.stdin.flush();return collect(p)
  def queued(scope,n):
   end=time.monotonic()+2
   while time.monotonic()<end:
    if len(list(pathlib.Path(str(base(scope))+'.waiters').glob('t-*')))>=n:return
    time.sleep(.005)
   raise AssertionError(('waiters absent',scope,n))
  try:
   scope=uuid.uuid4().hex;h=holder(scope)
   b1=spawn('holdbrief',scope,'B',True);queued(scope,1);b2=spawn('holdbrief',scope,'B',True);queued(scope,2);c=spawn('holdbrief',scope,'C',True);queued(scope,3);release(h)
   ordered=sorted([(name,collect(p)) for name,p in [('B1',b1),('B2',b2),('C',c)]],key=lambda x:x[1]['acquired_ms'])
   assert [n for n,r in ordered]==['B1','C','B2'],ordered;results['session_round_robin']=[n for n,r in ordered]
   scope=uuid.uuid4().hex;h=holder(scope);bg=spawn('holdbrief',scope,'BG',True);queued(scope,1);fg=[]
   for i in range(5):fg.append((f'F{i}',spawn('holdbrief',scope,f'F{i}')));queued(scope,i+2)
   release(h);ordered=sorted([('BG',collect(bg))]+[(n,collect(p)) for n,p in fg],key=lambda x:x[1]['acquired_ms']);names=[n for n,r in ordered]
   assert names[0]!='BG' and names.index('BG')<=3,names;results['foreground_with_background_progress']=names
   scope=uuid.uuid4().hex;h=holder(scope);dead=spawn('probe',scope,'dead');queued(scope,1);dead.kill();dead.communicate(timeout=3);release(h);r=collect(spawn('probe',scope,'survivor'));assert r['ok'];assert not list(pathlib.Path(str(base(scope))+'.waiters').glob('t-*'));results['crashed_waiter_reaped']=True
   scope=uuid.uuid4().hex;h=holder(scope);h.kill();h.communicate(timeout=3);assert collect(spawn('probe',scope))['ok'];results['crashed_permit_released']=True
   account='owned-legacy-'+uuid.uuid4().hex;path=base(account);path.parent.mkdir(exist_ok=True);fd=os.open(path,os.O_RDWR|os.O_CREAT,0o600);fcntl.flock(fd,fcntl.LOCK_EX)
   r=collect(spawn('probe',account+'|model'));assert not r['ok'] and 950<=r['waited_ms']<1300,r
   r=collect(spawn('cancel',account+'|model'));assert not r['ok'] and r['waited_ms']<350,r
   fcntl.flock(fd,fcntl.LOCK_UN);os.close(fd);h=holder(account+'|model');fd=os.open(path,os.O_RDWR)
   try:fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);raise AssertionError('old client overlapped new client')
   except BlockingIOError:pass
   os.close(fd);release(h);results['legacy_both_directions_and_cancellation']=True
   scope=uuid.uuid4().hex;assert collect(spawn('probe',scope))['ok'];state=pathlib.Path(str(base(scope))+'.waiters/state');state.write_bytes(b'corrupt')
   r=collect(spawn('probe',scope));assert not r['ok'] and not r['held_at_return'],r;results['corrupt_state_fails_closed']=True
   scope=uuid.uuid4().hex;assert collect(spawn('probe',scope))['ok'];base(scope).write_text('broken cooldown');assert not collect(spawn('probe',scope))['ok'];results['corrupt_cooldown_fails_closed']=True
   scope=uuid.uuid4().hex;assert collect(spawn('probe',scope))['ok'];fd=os.open(base(scope),os.O_RDWR);fcntl.flock(fd,fcntl.LOCK_EX)
   r=collect(spawn('cancel',scope));assert not r['ok'] and r['waited_ms']<350,r;os.close(fd);results['busy_metadata_is_cancellable']=True
  finally:
   for p in children:
    if p.poll() is None:p.kill();p.communicate(timeout=3)
  results['passed']=True;p=a.output/'admission.json';p.write_text(json.dumps(results,indent=2)+'\n');assert json.loads(p.read_text())==results;print(json.dumps(results,indent=2))
if __name__=='__main__':main()
