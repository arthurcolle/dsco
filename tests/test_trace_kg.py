"""Black-box trace KG contract tests; accepts DSCO_TRACE_KG_BIN override."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest

BIN = os.environ.get('DSCO_TRACE_KG_BIN', str(Path(__file__).resolve().parents[1] / 'dsco'))

class TraceKG(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / 'trace.sqlite'
        with sqlite3.connect(self.db) as c:
            c.execute('CREATE TABLE events(event_id TEXT, session_id TEXT, trace_id TEXT, span_id TEXT, parent_span_id TEXT, event_type TEXT, payload_json TEXT, seq INTEGER)')
            for i, (sess, event, payload) in enumerate([
                ('s1', 'tool.call.created', {'tool_name': 'weather', 'arguments': 'SECRET_NOT_EXPORTED'}),
                ('s1', 'tool.call.completed', {'tool_name': 'weather', 'ok': False}),
                ('s1', 'tool.call.completed', {'tool_name': 'weather', 'ok': True}),
                ('s1', 'tool.call.completed', {'tool_name': 'weather', 'ok': 'true'}),
                ('s2', 'tool.call.completed', {'tool_name': 'weather', 'ok': True}),
            ]):
                c.execute('INSERT INTO events VALUES(?,?,?,?,?,?,?,?)', (str(i), sess, 'trace', 'span', 'parent', event, json.dumps(payload), i))
    def tearDown(self):
        self.tmp.cleanup()
    def run_export(self, *args):
        return subprocess.run([BIN, 'trace-kg', 'export', str(self.db), *args], capture_output=True, text=True)
    def test_deterministic_closed_graph_and_privacy(self):
        before = self.db.read_bytes()
        a, b = self.run_export(), self.run_export()
        self.assertEqual(a.returncode, 0, a.stderr)
        self.assertEqual(a.stdout, b.stdout)
        self.assertEqual(before, self.db.read_bytes())
        self.assertNotIn('SECRET_NOT_EXPORTED', a.stdout)
        rows = [json.loads(line) for line in a.stdout.splitlines()]
        nodes = {x['id'] for x in rows if x['kind'] == 'node'}
        for row in rows:
            self.assertEqual(row['epistemic_status'], 'observed_telemetry')
            self.assertIn(row['evidence'], nodes)
            if row['kind'] == 'edge':
                self.assertIn(row['from'], nodes)
                self.assertIn(row['to'], nodes)
        self.assertEqual(sum(x['kind']=='node' and x['type']=='reported_success' for x in rows), 2)
        self.assertEqual(sum(x['kind']=='node' and x['type']=='reported_failure' for x in rows), 1)
        spans = {x['id'] for x in rows if x['kind']=='node' and x['type']=='span'}
        self.assertEqual(len(spans), 4)  # session-scoped span and parent
    def test_filter(self):
        a = self.run_export('s2')
        self.assertEqual(a.returncode, 0, a.stderr)
        rows = [json.loads(x) for x in a.stdout.splitlines()]
        self.assertEqual(sum(x['type']=='tool.call.completed' for x in rows), 1)
        self.assertEqual(self.run_export("s1' OR 1=1 --").stdout, '')
    def test_missing_db_not_created(self):
        self.db.unlink()
        self.assertNotEqual(self.run_export().returncode, 0)
        self.assertFalse(self.db.exists())
    def test_malformed_payload_is_not_success(self):
        with sqlite3.connect(self.db) as c:
            c.execute("UPDATE events SET payload_json='{broken'")
        a = self.run_export()
        self.assertEqual(a.returncode, 0, a.stderr)
        self.assertNotIn('reported_success', a.stdout)
    def test_invalid_schema(self):
        with sqlite3.connect(self.db) as c:
            c.execute('DROP TABLE events')
        self.assertNotEqual(self.run_export().returncode, 0)

if __name__ == '__main__':
    unittest.main()
