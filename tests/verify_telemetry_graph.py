"""Run after make dsco; pass its verbose build log to reuse platform link flags."""
import json,shlex,sqlite3,subprocess,sys,tempfile,os
from pathlib import Path
link=next(shlex.split(x) for x in Path(sys.argv[1]).read_text().splitlines() if ' -o dsco ' in x)
with tempfile.TemporaryDirectory() as d:
 i=link.index('-o');link[i+1]=d+'/fixture';link=[x for x in link if x!='build/obj/main.o'];link.insert(i,'tests/test_telemetry_chronicle.c')
 subprocess.run(link,check=True)
 proc=subprocess.Popen([d+'/fixture',d+'/chronicle'])
 import time
 deadline=time.monotonic()+85
 while proc.poll() is None:
  if time.monotonic()>deadline:
   proc.kill();proc.wait();raise RuntimeError('fixture deadline')
  print('fixture running',flush=True);time.sleep(5)
 assert proc.returncode==0
 expected_failures=4 if os.environ.get('DSCO_TEST_REAL_IDLE') else 3
 db=d+'/chronicle/indexes/chronicle.sqlite';graph=d+'/graph.sqlite'
 for expected in (True,False):
  r=subprocess.run(['./dsco','trace-kg','ingest',db,'--into',graph],capture_output=True,text=True,check=True)
  assert (json.loads(r.stdout)['assertions_added']>0)==expected
 r=subprocess.run(['./dsco','trace-kg','failures',graph,'--summary'],capture_output=True,text=True,check=True)
 rows=[json.loads(x) for x in r.stdout.splitlines()];assert sum(x['count'] for x in rows)==expected_failures,rows
 assert sum(len(x['evidence']) for x in rows)==expected_failures
 assert {x['timeout_origin'] for x in rows}=={'harness_idle','harness_wall'}
 assert any(x['requested_target']=='weather_fixture' for x in rows)
 c=sqlite3.connect(db);c.execute("update events set payload_json=json_set(payload_json,'$.timeout_origin','changed') where event_type='tool.call.completed'");c.commit()
 bad=subprocess.run(['./dsco','trace-kg','ingest',db,'--into',graph],capture_output=True,text=True);assert bad.returncode!=0;assert 'conflict' in bad.stderr
 print('PASS governed wall timeout -> Chronicle -> graph -> diagnostic; replay; metadata idle; conflict rejection')
