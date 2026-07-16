#!/usr/bin/env python3
"""Run Matrix-served OpenAI-compatible models against tools.distributed.systems.
Secrets remain runner-side; models receive only public schemas and tool results.
"""
import argparse, concurrent.futures, json, os, pathlib, sys, urllib.parse, urllib.request
MODEL_URL="http://127.0.0.1:1234/v1/chat/completions"
TOOLS_URL="https://tools.distributed.systems"
def token():
 p=pathlib.Path.home()/'.config/dsco/tools_api_token'
 return os.getenv('TOOLS_API_TOKEN') or os.getenv('AUTH_TOKEN') or (p.read_text().strip() if p.exists() else '')
def http_json(url, method='GET', body=None, auth=False, timeout=120):
 data=json.dumps(body).encode() if body is not None else None; h={'Accept':'application/json'}
 if body is not None:h['Content-Type']='application/json'
 if auth:
  t=token()
  if not t: raise RuntimeError('TOOLS_API_TOKEN unavailable')
  h['Authorization']='Bearer '+t
 req=urllib.request.Request(url,data=data,headers=h,method=method)
 with urllib.request.urlopen(req,timeout=timeout) as r:return json.load(r)
def schema(t):
 # Native catalog entries use `inputs`; federated backend entries already
 # expose JSON Schema as `input_schema`.
 if isinstance(t.get('input_schema'),dict):
  return {'type':'function','function':{'name':t['name'],'description':t.get('description',''),'parameters':t['input_schema']}}
 props={}; required=[]
 for x in t.get('inputs',[]):
  typ=x.get('type','string'); typ={'float':'number','double':'number','int':'integer','bool':'boolean','dict':'object','list':'array'}.get(typ,typ)
  p={'type':typ};
  if x.get('description'):p['description']=x['description']
  if x.get('enum'):p['enum']=x['enum']
  if x.get('default') is not None:p['default']=x['default']
  props[x['name']]=p
  if x.get('required'):required.append(x['name'])
 parameters={'type':'object','properties':props}
 if required:parameters['required']=required
 return {'type':'function','function':{'name':t['name'],'description':t.get('description',''),'parameters':parameters}}
def catalog(limit):
 xs=http_json(f'{TOOLS_URL}/api/v1/tools?limit={limit}',auth=True)
 return [x for x in xs if x.get('status')=='active']
def execute(call):
 f=call['function']; name=f['name']; args=json.loads(f.get('arguments') or '{}')
 # Per-tool endpoint is the current canonical synchronous route.
 try:r=http_json(f'{TOOLS_URL}/api/v1/tools/{urllib.parse.quote(name,safe="")}/execute','POST',{'inputs':args},True)
 except Exception:
  r=http_json(f'{TOOLS_URL}/api/v1/tools/execute','POST',{'tool_name':name,'arguments':args},True)
 return call, r
def model_call(model,messages,tools,max_tokens):
 b={'model':model,'messages':messages,'tools':tools,'tool_choice':'auto','parallel_tool_calls':True,'temperature':0,'max_tokens':max_tokens}
 if model.startswith('qwen35-'):b['reasoning_effort']='none'
 return http_json(MODEL_URL,'POST',b,False,300)
def main():
 ap=argparse.ArgumentParser();ap.add_argument('prompt');ap.add_argument('--model',default='qwen35-2b-draft');ap.add_argument('--catalog-limit',type=int,default=100);ap.add_argument('--max-turns',type=int,default=8);ap.add_argument('--max-tokens',type=int,default=1024);ap.add_argument('--trace',action='store_true');a=ap.parse_args()
 ts=[schema(x) for x in catalog(a.catalog_limit)]; messages=[{'role':'user','content':a.prompt}]; trace=[]
 for turn in range(a.max_turns):
  x=model_call(a.model,messages,ts,a.max_tokens); c=x['choices'][0]; m=c['message']; calls=m.get('tool_calls') or []
  assistant={'role':'assistant','content':m.get('content') or ''}
  if calls:assistant['tool_calls']=calls
  messages.append(assistant)
  if not calls:
   out={'ok':True,'model':a.model,'turns':turn+1,'final':m.get('content') or '','tool_count':sum(len(z.get('calls',[])) for z in trace)}
   if a.trace:out['trace']=trace
   print(json.dumps(out,ensure_ascii=False,indent=2));return
  # Validate tool names against exactly the schemas shown to the model.
  allowed={z['function']['name'] for z in ts}
  if any(c['function']['name'] not in allowed for c in calls):raise RuntimeError('model requested unknown tool')
  with concurrent.futures.ThreadPoolExecutor(max_workers=min(16,len(calls))) as ex:
   results=list(ex.map(execute,calls))
  event={'turn':turn+1,'calls':[]}
  for call,res in results:
   event['calls'].append({'name':call['function']['name'],'arguments':json.loads(call['function'].get('arguments') or '{}'),'result':res})
   messages.append({'role':'tool','tool_call_id':call['id'],'name':call['function']['name'],'content':json.dumps(res,ensure_ascii=False)})
  trace.append(event)
 raise RuntimeError('max tool turns exceeded')
if __name__=='__main__':
 try:main()
 except Exception as e:print(json.dumps({'ok':False,'error':str(e)}),file=sys.stderr);sys.exit(1)
