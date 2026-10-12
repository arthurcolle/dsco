#!/usr/bin/env python3
"""Qualify actual native scheduling with owned local SSE; no paid inference."""
import argparse,concurrent.futures,hashlib,http.server,json,os,pathlib,subprocess,tempfile,threading,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--binary',type=pathlib.Path,required=True);ap.add_argument('--output',type=pathlib.Path,required=True);ap.add_argument('--requests',default='8',help='comma-separated request-wave sizes (default: 8)');ap.add_argument('--capacities',default='1,2,4',help='comma-separated lane capacities (default: 1,2,4)');ap.add_argument('--service-delay-ms',type=int,default=300,help='owned fixture service time (default: 300)');a=ap.parse_args();binary=a.binary.resolve();a.output.mkdir(parents=True,exist_ok=True);rows=[]
 def positive_csv(value,label,maximum):
  try:values=sorted(set(int(item.strip()) for item in value.split(',') if item.strip()))
  except ValueError as error:ap.error(f'{label} must contain integers: {error}')
  if not values or values[0]<1 or values[-1]>maximum:ap.error(f'{label} values must be in 1..{maximum}')
  return values
 request_counts=positive_csv(a.requests,'--requests',512);capacities=positive_csv(a.capacities,'--capacities',512)
 if not 10<=a.service_delay_ms<=5000:ap.error('--service-delay-ms must be in 10..5000')
 with tempfile.TemporaryDirectory(prefix='dsco-scheduler-wire-') as tmp:
  root=pathlib.Path(tmp);(root/'.dsco').mkdir();(root/'.dsco/codex_models.json').write_text(json.dumps({'models':[{'slug':'gpt-6-astra','supported_in_api':True,'context_window':272000,'default_reasoning_level':'low','priority':1}]}))
  state={'active':0,'max':0,'requests':0};lock=threading.Lock()
  class Handler(http.server.BaseHTTPRequestHandler):
   def log_message(self,*a):pass
   def do_POST(self):
    req=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
    with lock:state['active']+=1;state['max']=max(state['max'],state['active']);state['requests']+=1
    try:
     time.sleep(a.service_delay_ms/1000)
     events=[{'type':'response.created','response':{'id':'owned-response'}},{'type':'response.output_text.delta','delta':'SCHEDULER_OK'},{'type':'response.completed','response':{'status':'completed','usage':{'input_tokens':10,'output_tokens':5,'input_tokens_details':{'cached_tokens':0}}}}]
     data=''.join('data: '+json.dumps(e)+'\n\n' for e in events).encode();self.send_response(200);self.send_header('Content-Type','text/event-stream');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);self.wfile.flush()
    finally:
     with lock:state['active']-=1
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;t=threading.Thread(target=server.serve_forever,daemon=True);t.start();pid_lock=threading.Lock();live_pids=set()
  try:
   for request_count in request_counts:
    for capacity in capacities:
     state.update(active=0,max=0,requests=0);started=time.monotonic()
     def run(i):
      e=dict(PATH=os.environ['PATH'],HOME=tmp,TMPDIR=tmp,DSCO_ENV_FILE='/dev/null',DSCO_CHATGPT_OAUTH_TOKEN='owned-fixture-only',DSCO_CHATGPT_ACCOUNT_ID='owned-fixture-account',DSCO_CHATGPT_BASE_URL=f'http://127.0.0.1:{server.server_port}/responses',OPENAI_API_KEY='owned-fixture-only',DSCO_DISABLE_SHARED_HOME_OAUTH='1',DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',DSCO_TOOLMGMT='0',DSCO_DISABLE_DEFAULT_FALLBACKS='1',DSCO_AUTO_FALLBACK='0',DSCO_DYNAMIC_FAILOVER='0',DSCO_AUTO_GOAL='0',DSCO_REQUIRE_TOOL_USE='0',DSCO_PIXEL_TUI='0',DSCO_MCP_HEADLESS='0',DSCO_NO_AUTO_SUPERVISE='1',DSCO_NO_SUPERVISE='1',DSCO_CHATGPT_GLOBAL_GATE='1',DSCO_CHATGPT_LANE_CONCURRENCY=str(capacity),DSCO_CHATGPT_GATE_MAX_WAIT_MS='10000',DSCO_CHATGPT_MIN_INTERVAL_MS='0',DSCO_CHATGPT_SWIMLANE=f'owned-capacity-{capacity}-requests-{request_count}',DSCO_ADMISSION_SESSION=f'owned-session-{i}',DSCO_WORKER='1',DSCO_CHRONICLE_DIR=str(root/f'chronicle-{request_count}-{capacity}-{i}'),DSCO_RUNS_DIR=str(root/f'runs-{request_count}-{capacity}-{i}'),DSCO_BUDGET='0.05',DSCO_MAX_TOKENS='32',DSCO_HARD_TURN_CEILING='2')
      p=subprocess.Popen([str(binary),'--env-file','/dev/null','--profile','worker','--provider','openai-codex','--model','gpt-6-astra','--prompt','Reply with SCHEDULER_OK only.'],cwd=root,env=e,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
      with pid_lock:live_pids.add(p.pid)
      try:
       stdout,stderr=p.communicate(timeout=20)
      except subprocess.TimeoutExpired:
       p.kill();stdout,stderr=p.communicate();raise
      finally:
       with pid_lock:live_pids.discard(p.pid)
      assert p.returncode==0 and 'SCHEDULER_OK' in stdout,(i,p.returncode,stdout,stderr)
      events=[]
      for f in (root/f'chronicle-{request_count}-{capacity}-{i}'/'events').rglob('*.jsonl'):
       events.extend(json.loads(line) for line in f.read_text().splitlines())
      queue=[x['payload'] for x in events if x['event_type']=='provider.queue.acquired'];assert len(queue)==1,(i,queue)
      assert queue[0]['capacity']==capacity;return queue[0]['queue_wait_ms']
     peak_processes=0;peak_rss_kib=0
     with concurrent.futures.ThreadPoolExecutor(max_workers=request_count) as pool:
      futures=[pool.submit(run,i) for i in range(request_count)]
      while not all(future.done() for future in futures):
       with pid_lock:pids=sorted(live_pids)
       peak_processes=max(peak_processes,len(pids))
       if pids:
        sample=subprocess.run(['ps','-o','rss=','-p',','.join(str(pid) for pid in pids)],capture_output=True,text=True,check=False)
        if sample.returncode==0:
         peak_rss_kib=max(peak_rss_kib,sum(int(value) for value in sample.stdout.split()))
       time.sleep(.01)
      waits=[future.result() for future in futures]
     expected=min(capacity,request_count)
     assert state['requests']==request_count and 1<=state['max']<=expected,state
     # A capacity below the wave size must fill.  When capacity equals the
     # wave, short owned requests can finish before hundreds of independent
     # processes have launched; that observed ramp is a benchmark result, not
     # a gate failure.
     if capacity<request_count:assert state['max']==capacity,state
     rows.append({'capacity':capacity,'requests':request_count,'max_simultaneous_transports':state['max'],'capacity_utilization':round(state['max']/expected,4),'wall_seconds':round(time.monotonic()-started,3),'queue_wait_ms':waits,'peak_live_processes':peak_processes,'peak_aggregate_rss_bytes':peak_rss_kib*1024,'chronicle_queue_events_without_stream_db':True})
  finally:server.shutdown();server.server_close();t.join(timeout=3)
 result={'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'passed':True,'cases':rows};out=a.output/'binary-scheduling.json';out.write_text(json.dumps(result,indent=2)+'\n');assert json.loads(out.read_text())==result;print(json.dumps(result,indent=2))
if __name__=='__main__':main()
