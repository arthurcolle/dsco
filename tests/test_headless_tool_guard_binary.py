#!/usr/bin/env python3
"""Fault-inject model responses into the real native binary; real tools/gates still execute."""
import argparse,json,os,pathlib,subprocess,tempfile,threading,http.server
p=argparse.ArgumentParser();p.add_argument('--binary',required=True);args=p.parse_args();binary=str(pathlib.Path(args.binary).resolve())

def text(value):return {'content':value}
def tool(name,arguments,ident='t1'):
 return {'tool_calls':[{'index':0,'id':ident,'type':'function','function':{'name':name,'arguments':json.dumps(arguments)}}]}

def scenario(name,replies,expected_exit,expected_calls,disabled=False,expected_choices=None,forced=True,overrides=None,expected_lines=None):
 with tempfile.TemporaryDirectory(prefix='dsco-headless-guard-') as tmp:
  root=pathlib.Path(tmp);(root/'read.txt').write_text('safe fixture');requests=[]
  class Handler(http.server.BaseHTTPRequestHandler):
   def do_POST(self):
    body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(body)
    delta=replies[min(len(requests)-1,len(replies)-1)]
    # Replace only a private fixture placeholder, not any model-provided path.
    delta=json.loads(json.dumps(delta).replace('CANARY_PATH',str(root/'must-not-exist')))
    reason='tool_calls' if delta.get('tool_calls') else 'stop'
    self.send_response(200);self.send_header('Content-Type','text/event-stream');self.send_header('Connection','close');self.end_headers()
    frames=[{'id':'fixture','model':'default_model','choices':[{'index':0,'delta':delta,'finish_reason':None}]},
            {'id':'fixture','model':'default_model','choices':[{'index':0,'delta':{},'finish_reason':reason}],
             'usage':{'prompt_tokens':10,'completion_tokens':10,'total_tokens':20}}]
    for frame in frames:self.wfile.write(('data: '+json.dumps(frame)+'\n\n').encode())
    self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush();self.close_connection=True
   def log_message(self,*a):pass
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  base=f'http://127.0.0.1:{server.server_port}/v1'
  env={'PATH':os.environ['PATH'],'HOME':tmp,'TMPDIR':tmp,'MLX_API_BASE':base,'MLX_BASE_URL':base,
       'DSCO_REQUIRE_TOOL_USE':'1' if forced else '0','DSCO_OR_DISABLE_TOOLS':'1' if disabled else '0',
       'DSCO_DISABLE_DEFAULT_FALLBACKS':'1','DSCO_AUTO_FALLBACK':'0','DSCO_AUTO_GOAL':'0',
       'DSCO_HARD_TURN_CEILING':'8','DSCO_MAX_TOKENS':'128','DSCO_BUDGET':'1',
       'DSCO_TOOL_ALLOWLIST':'read_file,bash','DSCO_ALLOW_CONTROL':'0','DSCO_ALLOW_SECRETS':'0',
       'DSCO_ALLOW_NET':'0','DSCO_PIXEL_TUI':'0','DSCO_MCP_HEADLESS':'0'}
  env.update(DSCO_PRICING_OFFLINE='1',DSCO_DISABLE_SHARED_HOME_OAUTH='1',
             DSCO_DISABLE_CODEX_OAUTH_DISCOVERY='1',DSCO_SECURE_STORE_NO_PROMPT='1',
             DSCO_NO_AUTO_SUPERVISE='1')
  if overrides:env.update(overrides)
  try:
   result=subprocess.run([binary,'--env-file','/dev/null','--profile','worker','--provider','mlx',
       '--model','mlx:default_model','--gov-model','standard','--trust-tier','trusted','--approval-mode','never',
       '--prompt','Use bash to print GUARD_OK. Do not claim completion without the real tool result.'],
       cwd=tmp,env=env,capture_output=True,text=True,timeout=30)
  finally:server.shutdown();server.server_close();thread.join(timeout=3)
  assert result.returncode==expected_exit,(name,result.returncode,result.stdout,result.stderr)
  assert len(requests) in (expected_calls if isinstance(expected_calls,tuple) else (expected_calls,)),(name,len(requests),result.stderr)
  assert not (root/'must-not-exist').exists(),'Raw tool-shaped text was executed'
  assert 'UNEXECUTED_CLAIM' not in result.stdout,'Ungrounded output leaked'
  if expected_lines is not None:
   assert len((root/'progress.txt').read_text().splitlines())==expected_lines,'Durable progress mismatch'
  if expected_exit==0:assert 'GUARD_OK' in result.stdout,(name,result.stdout,result.stderr)
  else:assert 'error:' in result.stderr,(name,result.stderr)
  choices=[r.get('tool_choice') for r in requests]
  if expected_choices is not None:assert choices==expected_choices,(name,choices)
  print(json.dumps({'case':name,'exit':result.returncode,'requests':len(requests),'choices':choices,'passed':True}),flush=True)

bad=text('UNEXECUTED_CLAIM\n<function=bash>\n<parameter=command>touch CANARY_PATH</parameter>\n</function>\n</tool_call>')
scenario('plain_noop_rejected',[text('UNEXECUTED_CLAIM')],1,3)
scenario('malformed_markup_rejected',[bad],1,3)
scenario('repair_then_real_tool',[bad,tool('bash',{'command':'printf GUARD_OK'}),text('GUARD_OK')],0,3,
         expected_choices=['required','required','auto'])
scenario('malformed_after_valid_read',[tool('read_file',{'path':'read.txt'}),bad,tool('bash',{'command':'printf GUARD_OK'},'t2'),text('GUARD_OK')],0,4,
         expected_choices=['required','auto','required','auto'])
scenario('explicit_tool_disable_wins',[text('UNEXECUTED_CLAIM')],1,1,disabled=True)
scenario('disabled_unforced_ignoring_provider',[tool('bash',{'command':'touch CANARY_PATH'})],1,1,disabled=True,forced=False)
scenario('disabled_unforced_text',[text('GUARD_OK')],0,1,disabled=True,forced=False)
mixed={**tool('bash',{'command':'printf GUARD_OK'}),'content':'UNEXECUTED_CLAIM'}
scenario('success_prose_not_released_before_dispatch',[mixed,text('GUARD_OK')],0,2)
scenario('denied_dispatch_is_terminal',[mixed],1,1,overrides={'DSCO_ALLOW_RUN':'0'})
scenario('unknown_tool_cannot_satisfy_execution',[tool('nonexistent_tool',{}),text('UNEXECUTED_CLAIM')],1,(1,2))
invalid=tool('bash',{});invalid['tool_calls'][0]['function']['arguments']='{bad'
scenario('invalid_arguments_cannot_satisfy_execution',[invalid,text('UNEXECUTED_CLAIM')],1,(1,2))
mixed_bad={**tool('read_file',{'path':'read.txt'}),**bad}
scenario('mixed_malformed_batch_rejected',[mixed_bad],1,3)
scenario('repeated_failures_after_success',
         [tool('read_file',{'path':'read.txt'}), tool('read_file',{'path':'missing.txt'})],
         1,4,overrides={'DSCO_HARD_TURN_CEILING':'100'})
scenario('identical_success_is_not_progress',[tool('read_file',{'path':'read.txt'})],
         # The first uncached read may differ from subsequent cached results.
         1,(8,9),overrides={'DSCO_HARD_TURN_CEILING':'100'})
scenario('explicit_zero_ceiling_allows_useful_work',
         [tool('bash',{'command':"printf 'x\\n' >> progress.txt; printf GUARD_OK"},f'progress-{i}') for i in range(40)] + [text('GUARD_OK')],
         0,41,overrides={'DSCO_HARD_TURN_CEILING':'0'},expected_lines=40)
recovery=[tool('bash',{'command':'printf GUARD_OK'})]
for i in range(4):
 recovery.extend([tool('read_file',{'path':'missing.txt'},f'fail-{i}-a'),
                  tool('read_file',{'path':'also-missing.txt'},f'fail-{i}-b'),
                  tool('bash',{'command':"printf 'x\\n' >> progress.txt; printf GUARD_OK"},f'recover-{i}')])
scenario('two_failures_then_durable_progress_can_continue',recovery+[text('GUARD_OK')],
         0,14,overrides={'DSCO_HARD_TURN_CEILING':'0'},expected_lines=4)
print('PASS: 16 real-binary fault-injection scenarios')
