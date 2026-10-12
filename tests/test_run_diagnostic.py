#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('run_diagnostic',ROOT/'scripts/run_diagnostic.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def event(kind,t,span='a',trace='run',**payload):
 return dict(event_type=kind,wall_time=t,span_id=span,trace_id=trace,session_id='session',payload=payload)
def sample():
 return [event('llm.request.created',10,byte_len=100),event('llm.response.completed',20,latency_ms=2000,usage=dict(input_tokens=10,cache_read_tokens=5,cache_write_tokens=0,output_tokens=3,reasoning_tokens=0)),event('tool.call.completed',21,span='tool',latency_ms=100,result_byte_len=42,secret='PRIVATE_CANARY_DO_NOT_EXPORT'),event('inference.cost',20,trace=None,estimated_inference_usd=.1,invoiced_usd=None),event('provider.queue.acquired',12,trace=None,queue_wait_ms=3000)]
class DiagnosticTests(unittest.TestCase):
 def test_metrics_and_privacy(self):
  d=m.analyze(sample(),'run');self.assertEqual(d['time']['wall_seconds'],10);self.assertEqual(d['time']['request_span_residual_seconds_sum'],8);self.assertEqual(d['context']['first_input_tokens'],15);self.assertIsNone(d['economics']['invoiced_usd']['total']);self.assertNotIn('PRIVATE_CANARY',json.dumps(d)+m.markdown(d));self.assertFalse(d['economics']['task_completion_verified'])
 def test_overlap_not_double_counted(self):
  rows=sample()+[event('llm.request.created',15,span='b',byte_len=100),event('llm.response.completed',25,span='b',latency_ms=1000)]
  d=m.analyze(rows,'run');self.assertTrue(d['time']['overlapping_requests']);self.assertEqual(d['time']['request_span_union_seconds'],15);self.assertEqual(d['time']['request_span_seconds_sum'],20)
 def test_missing_values_not_zero(self):
  rows=sample();rows[1]['payload']['usage'].pop('input_tokens');d=m.analyze(rows,'run');self.assertIsNone(d['context']['first_input_tokens']);self.assertIsNone(d['context']['usage']['input_tokens']['total'])
 def test_incomplete_pairs(self):
  d=m.analyze(sample()+[event('llm.request.created',30,span='b')],'run');self.assertEqual(d['coverage'],'partial');self.assertEqual(d['time']['request_count'],2)
 def test_duplicate_rejected(self):
  rows=sample();rows.append(rows[0]);self.assertRaises(ValueError,m.analyze,rows,'run')
 def test_bad_clock_and_nonfinite(self):
  rows=sample();rows[1]['wall_time']=1;self.assertRaises(ValueError,m.analyze,rows,'run');rows=sample();rows[1]['payload']['latency_ms']=float('nan');self.assertRaises(ValueError,m.analyze,rows,'run')
 def test_mixed_sessions_rejected(self):
  rows=sample();rows[1]['session_id']='other';self.assertRaises(ValueError,m.analyze,rows,'run')
 def test_foreign_trace_not_counted(self):
  d=m.analyze(sample()+[event('llm.response.completed',99,span='other',trace='other',latency_ms=999999)],'run');self.assertEqual(d['time']['completed_request_count'],1)
 def test_cli_and_partial_tail(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);source=root/'events.jsonl';source.write_text(''.join(json.dumps(x)+'\n' for x in sample()));before=source.read_bytes();out=root/'report';cmd=[sys.executable,str(ROOT/'scripts/run_diagnostic.py'),'--events',str(source),'--trace','run','--output',str(out),'--strict']
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(source.read_bytes(),before);self.assertTrue((out/'REPORT.md').exists());self.assertEqual((out/'REPORT.md').stat().st_mode&0o777,0o600)
   source.write_bytes(before+b'{"unfinished":');p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,3,p.stderr);self.assertEqual(json.loads((out/'diagnostic.json').read_text())['coverage'],'partial')
   source.write_text('not-json\n');p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,2);self.assertNotIn('Traceback',p.stderr)
if __name__=='__main__':unittest.main()
