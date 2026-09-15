#!/usr/bin/env python3
"""No-model Chronicle trace adapter tests against the real binary."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

BINARY=Path(os.environ.get('DSCO_TEST_BINARY','./dsco')).resolve()
# CLI startup may refresh pricing in a detached process; tests are offline.
os.environ['DSCO_PRICING_OFFLINE']='1'
def frame(seq=1, status='failed', category='tool'):
    obj={'v':1,'type':'rl.trajectory.event','seq':seq,'run_id':'private-run-secret',
         'payload':{'category':category,'status':status,'payload':{'token':'SECRET_SENTINEL','thinking':'PRIVATE_REASONING'}}}
    raw=json.dumps(obj).encode()
    return struct.pack('<II',len(raw),zlib.crc32(raw))+raw
class TraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.path=Path(self.tmp.name)/'journal.wal'
    def invoke(self, data):
        self.path.write_bytes(data)
        return subprocess.run([str(BINARY),'learn','trace',str(self.path)],capture_output=True,text=True,timeout=10)
    def test_selects_tool_evidence_omits_all_private_text(self):
        data=frame()+frame(2,'ok')+frame(3,'ok','model')
        r=self.invoke(data); self.assertEqual(r.returncode,0,r.stderr)
        out=json.loads(r.stdout)
        self.assertEqual(out['source_sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(out['journal_frames'],3); self.assertEqual(out['tool_events'],2)
        self.assertEqual([x['status'] for x in out['events']],['failed','ok'])
        self.assertEqual(out['events'][1]['offset_bytes'],len(frame()))
        self.assertEqual(out['events'][0]['frame_sha256'],hashlib.sha256(frame()[8:]).hexdigest())
        self.assertFalse(out['extraction_ready']); self.assertFalse(out['promotion_eligible'])
        for secret in ('SECRET_SENTINEL','PRIVATE_REASONING','private-run-secret'):
            self.assertNotIn(secret,r.stdout+r.stderr)
    def test_rejects_corrupt_truncated_and_invalid_json(self):
        raw=b'{no json}'; invalid=struct.pack('<II',len(raw),zlib.crc32(raw))+raw
        for data in (b'',b'1234',frame()[:-1],frame()+b'x',frame()[:4]+b'xxxx'+frame()[8:],invalid,struct.pack('<II',17000000,0)):
            with self.subTest(size=len(data)):
                r=self.invoke(data); self.assertNotEqual(r.returncode,0); self.assertEqual(r.stdout,'')
    def test_unknown_status_is_not_echoed(self):
        r=self.invoke(frame(status='SECRET_SENTINEL')); self.assertEqual(r.returncode,0)
        self.assertEqual(json.loads(r.stdout)['events'][0]['status'],'unknown'); self.assertNotIn('SECRET_SENTINEL',r.stdout)
    def test_selection_cap_is_explicit(self):
        r=self.invoke(b''.join(frame(i) for i in range(257))); self.assertEqual(r.returncode,0)
        out=json.loads(r.stdout); self.assertEqual(len(out['events']),256); self.assertTrue(out['selection_truncated'])
    def test_symlink_is_rejected(self):
        target=self.path.with_name('real'); target.write_bytes(frame()); self.path.symlink_to(target)
        r=subprocess.run([str(BINARY),'learn','trace',str(self.path)],capture_output=True,text=True,timeout=10)
        self.assertNotEqual(r.returncode,0)
if __name__=='__main__': unittest.main()
