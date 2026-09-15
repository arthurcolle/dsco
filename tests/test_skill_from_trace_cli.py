#!/usr/bin/env python3
"""Offline tests for trace-bound candidate creation; no behavioral promotion."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

BINARY = Path(os.environ.get('DSCO_TEST_BINARY', './dsco')).resolve()
def make_frame(sequence=1, category='tool'):
    raw = json.dumps({'v':1,'type':'rl.trajectory.event','run_id':'fixture',
        'seq':sequence,'payload':{'category':category,'status':'failed',
        'payload':{'api_key':'SECRET_NOT_FOR_EXPORT','thinking':'PRIVATE_NOT_FOR_EXPORT'}}}).encode()
    return struct.pack('<II',len(raw),zlib.crc32(raw)) + raw

def reference(wal, frame):
    return 'chronicle:' + hashlib.sha256(wal).hexdigest() + ':' + hashlib.sha256(frame[8:]).hexdigest()

class FromTraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.wal = self.root/'journal.wal'
        self.episode = self.root/'episode.json'; self.out = self.root/'candidate'
        self.data = make_frame(); self.wal.write_bytes(self.data)
        self.spec = {'name':'observed-recovery','goal':'Recover from a failed tool call.',
                     'procedure':['Inspect failure before retrying.'],
                     'acceptance':['Requested artifact independently verified.'],
                     'evidence':[reference(self.data,self.data)]}
        self.env = dict(os.environ, HOME=str(self.root), DSCO_PRICING_OFFLINE='1')
    def invoke(self):
        self.episode.write_text(json.dumps(self.spec))
        return subprocess.run([str(BINARY),'learn','from-trace',str(self.wal),str(self.episode),str(self.out)],
            capture_output=True,text=True,timeout=10,env=self.env)
    def test_valid_reference_creates_unverified_candidate_without_payloads(self):
        r=self.invoke(); self.assertEqual(r.returncode,0,r.stderr)
        manifest=json.loads((self.out/'manifest.json').read_text())
        self.assertFalse(manifest['promotion_eligible']); self.assertEqual(manifest['acceptance_status'],'unverified')
        self.assertEqual((self.out/'episode.json').read_bytes(),self.episode.read_bytes())
        for p in self.out.iterdir():
            self.assertNotIn('SECRET_NOT_FOR_EXPORT',p.read_text()); self.assertNotIn('PRIVATE_NOT_FOR_EXPORT',p.read_text())
        r=subprocess.run([str(BINARY),'learn','verify',str(self.out)],capture_output=True,text=True,timeout=10,env=self.env)
        self.assertEqual(r.returncode,0,r.stderr)
    def test_rejects_forged_or_malformed_references(self):
        ref=self.spec['evidence'][0]
        for value in ('arbitrary claim',ref.replace(ref[10:74],'0'*64),ref[:75]+'0'*64,ref.upper(),ref+'x'):
            with self.subTest(value=value):
                self.spec['evidence']=[value]; self.assertNotEqual(self.invoke().returncode,0); self.assertFalse(self.out.exists())
    def test_rejects_non_tool_frame(self):
        self.data=make_frame(category='model'); self.wal.write_bytes(self.data)
        self.spec['evidence']=[reference(self.data,self.data)]
        self.assertNotEqual(self.invoke().returncode,0); self.assertFalse(self.out.exists())
    def test_rejects_corruption_and_partial_tail(self):
        for data in (self.data[:-1],self.data+b'x',self.data[:4]+b'xxxx'+self.data[8:]):
            with self.subTest(size=len(data)):
                self.wal.write_bytes(data); self.assertNotEqual(self.invoke().returncode,0); self.assertFalse(self.out.exists())
    def test_snapshot_change_invalidates_reference(self):
        self.wal.write_bytes(self.data+make_frame(2))
        self.assertNotEqual(self.invoke().returncode,0); self.assertFalse(self.out.exists())
    def test_all_references_must_resolve(self):
        self.spec['evidence'].append('chronicle:'+'0'*64+':'+'0'*64)
        self.assertNotEqual(self.invoke().returncode,0); self.assertFalse(self.out.exists())
    def test_existing_destination_untouched(self):
        self.out.mkdir(); sentinel=self.out/'sentinel'; sentinel.write_bytes(b'keep')
        self.assertNotEqual(self.invoke().returncode,0); self.assertEqual(sentinel.read_bytes(),b'keep')
        self.assertEqual(list(self.out.iterdir()),[sentinel])
    def test_source_symlink_rejected(self):
        target=self.root/'actual'; self.wal.rename(target); self.wal.symlink_to(target)
        self.assertNotEqual(self.invoke().returncode,0); self.assertFalse(self.out.exists())
if __name__=='__main__': unittest.main()
