#!/usr/bin/env python3
"""Additional checks not shown to the local coding agent; preserve its first-run results."""
import csv,hashlib,importlib.util,io,json,pathlib,random,re
ROOT=pathlib.Path(__file__).resolve().parents[1]
summary_path=sorted((ROOT/'reports/local-coder-deep').glob('coding-*/summary.json'))[-1]
summary=json.loads(summary_path.read_text());cases={x['case']:x for x in summary['results']}
def load(name,file):
 p=pathlib.Path(cases[name]['workspace'])/file;spec=importlib.util.spec_from_file_location('heldout_'+name,p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m,p
parser,p1=load('csv_parser','parser.py');events,p2=load('jsonl_errors','events.py')
hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [p1,p2]};results=[]
def record(name,fn):
 try:fn();d={'case':name,'passed':True}
 except Exception as e:d={'case':name,'passed':False,'error':type(e).__name__+': '+str(e)[:600]}
 results.append(d)
def csv_check(name):
 f=io.StringIO(newline='');w=csv.writer(f);w.writerow(['name','quantity']);w.writerow([name,7])
 actual=parser.parse_inventory(f.getvalue());expected=[{'name':name,'quantity':7}]
 assert actual==expected,repr({'actual':actual,'expected':expected})
for name in ['comma,part','a "quoted" part','line\nbreak','carriage\rreturn','東京','']:
 record('csv_value_'+repr(name),lambda name=name:csv_check(name))
def bad_quantity(value):
 try:parser.parse_inventory('name,quantity\nx,'+value+'\n')
 except ValueError:return
 raise AssertionError('Existing invalid-quantity error behavior was silently replaced with row dropping')
for value in ['not-an-int','']:record('csv_invalid_quantity_'+repr(value),lambda value=value:bad_quantity(value))
rng=random.Random(477)
for i in range(100):
 rows=[{'id':j,'city':rng.choice(['東京','Paris','São Paulo']),'nested':{'ok':bool(j%2),'values':[None,j,-j]},'text':'quote " and newline\n'} for j in range(rng.randint(1,12))]
 text='\n'.join(('  \n' if rng.randrange(2) else '')+json.dumps(x,ensure_ascii=False) for x in rows)
 if i%2:text+='\n'
 def check(text=text,rows=rows):assert events.load_events(text)==rows
 record('jsonl_generated_'+str(i),check)
for i in range(1,21):
 text='{}\n'*(i-1)+'not JSON'
 def check(text=text,line=i):
  try:events.load_events(text)
  except ValueError as e:
   assert re.search(r'(?i)line\s+'+str(line)+r'\b',str(e)),str(e);return
  raise AssertionError('Malformed JSON accepted')
 record('jsonl_error_line_'+str(i),check)
assert all(hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()==h for p,h in hashes.items())
out={'cases':len(results),'passed':sum(x['passed'] for x in results),'failed':sum(not x['passed'] for x in results),'unchanged_model_patch_hashes':hashes,'results':results,'scope':'Held-out checks on real model-written functions. No additional model attempts or patch changes. CSV invalid-quantity probes check behavior present before the requested fix.'}
p=ROOT/'reports/local-coder-deep/heldout.json';p.write_text(json.dumps(out,indent=2)+'\n');assert json.loads(p.read_text())==out
print(json.dumps({k:v for k,v in out.items() if k!='results'},indent=2))
for d in results:
 if not d['passed']:print(json.dumps(d))
