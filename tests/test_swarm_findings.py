import json,sqlite3,subprocess,tempfile,unittest
from pathlib import Path
BIN=str(Path(__file__).resolve().parents[1]/'dsco')
class Findings(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.s=self.tmp.name+'/source';self.g=self.tmp.name+'/graph';self.c=sqlite3.connect(self.s)
  self.c.execute('create table events(event_id,session_id,trace_id,span_id,parent_span_id,event_type,payload_json,seq)')
 def tearDown(self):self.c.close();self.tmp.cleanup()
 def event(self,id,type,payload):
  self.c.execute('insert into events values(?,?,?,?,?,?,?,?)',(id,'session','trace','span',None,type,json.dumps(payload),int(id)));self.c.commit()
 def runcli(self,*args):return subprocess.run([BIN,'trace-kg',*args],capture_output=True,text=True)
 def ingest(self):return self.runcli('ingest',self.s,'--into',self.g)
 def test_outcome_mutation(self):
  self.event('1','tool.call.completed',{'tool_name':'bash','ok':False});self.assertEqual(self.ingest().returncode,0)
  self.c.execute('update events set payload_json=?',(json.dumps({'tool_name':'bash','ok':True}),));self.c.commit()
  self.assertNotEqual(self.ingest().returncode,0)
  with sqlite3.connect(self.g) as g:self.assertEqual(g.execute("select count(*) from kg_assertions where type='reported_success'").fetchone()[0],0)
 def test_reserved_type(self):
  self.event('1','reported_failure',{});self.assertEqual(self.ingest().returncode,0)
  self.assertEqual(self.runcli('failures',self.g).stdout,'');self.assertEqual(self.runcli('failures',self.g,'--summary').stdout,'')
 def test_ambiguous_target(self):
  self.event('1','tool.call.created',{'requested_tool_name':'a'});self.event('2','tool.call.created',{'requested_tool_name':'b'});self.event('3','tool.call.completed',{'ok':False})
  self.assertEqual(self.ingest().returncode,0)
  row=json.loads(self.runcli('failures',self.g,'--summary').stdout);self.assertEqual(row['requested_target'],'ambiguous');self.assertEqual(row['count'],1)
 def test_disjoint_old_projection(self):
  self.event('1','test',{});self.assertEqual(self.ingest().returncode,0)
  with sqlite3.connect(self.g) as g:
   g.execute('drop trigger kg_conflict');g.execute("update kg_assertions set record=json_set(record,'$.projection','telemetry.v2')")
  self.c.execute('delete from events');self.c.commit();self.event('2','test',{})
  self.assertNotEqual(self.ingest().returncode,0)
 def test_duplicate_completion_evidence(self):
  self.event('1','tool.call.completed',{'ok':False});self.assertEqual(self.ingest().returncode,0)
  with sqlite3.connect(self.g) as g:
   g.execute("insert into kg_assertions select id,'foreign-evidence',kind,type,from_id,to_id,label,record from kg_assertions where type='tool.call.completed'")
  row=json.loads(self.runcli('failures',self.g,'--summary').stdout);self.assertEqual(row['count'],1);self.assertEqual(len(set(row['evidence'])),1)
if __name__=='__main__':unittest.main()
