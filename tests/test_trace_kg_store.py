import test_trace_kg as base
from test_trace_kg import BIN
import json
import sqlite3
import subprocess
import unittest

class Store(base.TraceKG):
    def command(self,*args):
        return subprocess.run([BIN,'trace-kg',*map(str,args)],capture_output=True,text=True)
    def ingest(self):
        return self.command('ingest',self.db,'--into',self.graph)
    def setUp(self):
        super().setUp()
        self.graph=self.db.parent/'graph.sqlite'
    def test_replay_append_and_queries(self):
        a=self.ingest(); self.assertEqual(a.returncode,0,a.stderr)
        self.assertGreater(json.loads(a.stdout)['assertions_added'],0)
        self.assertEqual(json.loads(self.ingest().stdout)['assertions_added'],0)
        f=self.command('failures',self.graph)
        self.assertEqual(f.returncode,0,f.stderr)
        rows=[json.loads(x) for x in f.stdout.splitlines()]
        self.assertTrue(any(x['type']=='reported_failure' for x in rows))
        self.assertFalse(any(x['type']=='reported_success' for x in rows))
        e=self.command('explain',self.graph,'1')
        self.assertEqual(e.stdout,f.stdout)
        self.assertEqual(self.command('explain',self.graph,"' OR 1=1 --").stdout,'')
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO events VALUES('new','s1','trace','newspan',NULL,'tool.call.completed',?,99)",(json.dumps({'tool_name':'weather','ok':False}),))
        self.assertGreater(json.loads(self.ingest().stdout)['assertions_added'],0)
        self.assertEqual(json.loads(self.ingest().stdout)['assertions_added'],0)
    def test_source_alias_rejected(self):
        before=self.db.read_bytes()
        self.assertNotEqual(self.command('ingest',self.db,'--into',self.db).returncode,0)
        self.assertEqual(before,self.db.read_bytes())
        alias=self.db.parent/'alias';alias.symlink_to(self.db)
        self.assertNotEqual(self.command('ingest',self.db,'--into',alias).returncode,0)
    def test_transaction_rolls_back(self):
        self.assertEqual(self.ingest().returncode,0)
        with sqlite3.connect(self.graph) as c:
            n=c.execute('select count(*) from kg_assertions').fetchone()[0]
            c.execute("CREATE TRIGGER fail_insert BEFORE INSERT ON kg_assertions WHEN NEW.label='new' BEGIN SELECT RAISE(ABORT,'injected failure'); END")
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO events VALUES('new','s1','trace','newspan',NULL,'test', '{}',99)")
        self.assertNotEqual(self.ingest().returncode,0)
        with sqlite3.connect(self.graph) as c:
            self.assertEqual(c.execute('select count(*) from kg_assertions').fetchone()[0],n)
            c.execute('DROP TRIGGER fail_insert')
        self.assertEqual(self.ingest().returncode,0)
    def test_missing_query_db_not_created(self):
        self.assertNotEqual(self.command('failures',self.graph).returncode,0)
        self.assertFalse(self.graph.exists())

if __name__=='__main__': unittest.main()
