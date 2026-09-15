#!/usr/bin/env python3
import copy,json,sys,tempfile,unittest
from datetime import datetime,timezone,timedelta
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import compile_frontier_policy as f

class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.now=datetime(2026,9,4,0,59,30,tzinfo=timezone.utc)
        self.row=dict(provider='direct',model='m',effort='low',attempts=3,successes=3,unpriced_attempts=0,
            strict_validation=True,observed_at=(self.now-timedelta(seconds=5)).isoformat(),sources=[])
        for i in range(3):
            path=self.root/str(i);(path/'workers/t').mkdir(parents=True)
            r=dict(provider='direct',model='m',attempt_token='t',expected={'sorted':[1,2],'checksum':3},
                elapsed_s=1,started_at=self.now.timestamp()-6,status='verified',exit_code=0,incomplete_cost_receipt=False)
            (path/'result.json').write_text(json.dumps(r));(path/'workers/t/stdout.log').write_text(json.dumps(r['expected']))
            self.row['sources'].append(str(path/'result.json'))
        self.doc={'schema':'dsco.route_evidence.v1','workload':'integer-json-v1','rows':[self.row]}
        self.quote=dict(provider='direct',model='m',source='https://example.test/pricing',observed_at=(self.now-timedelta(seconds=10)).isoformat(),
            input_per_million=.2,cached_per_million=.01,output_per_million=.6,request_fee=0,
            supports_tools=True,context_length=10000,max_output_tokens=1000)
    def compile(self,**kw):
        return f.compile_policy(self.doc,[self.quote],root=self.root,now=self.now,**kw)
    def test_compiles_exact_identity_and_manifest(self):
        policy,audit=self.compile();self.assertEqual(policy['frontier_policy']['lanes'][0]['verified'],3)
        self.assertEqual(len(audit['receipt_manifest']),3);self.assertEqual(policy['max_agents'],1)
    def test_embedded_json_and_boolean_not_verified(self):
        p=self.root/'0/workers/t/stdout.log'
        for text in ['echo \'{"sorted":[1,2],"checksum":3}\'','{"sorted":[true,2],"checksum":3}']:
            p.write_text(text)
            with self.assertRaises(ValueError):self.compile()
    def test_forged_success_and_unknown_count_rejected(self):
        self.row['successes']=2
        with self.assertRaises(ValueError):self.compile()
        self.row['successes']=3;self.row['unpriced_attempts']=1
        with self.assertRaises(ValueError):self.compile()
    def test_duplicate_receipt_or_lane_rejected(self):
        self.row['sources'][1]=self.row['sources'][0]
        with self.assertRaises(ValueError):self.compile()
    def test_stale_or_future_evidence_rejected(self):
        for delta in (-4000,20):
            self.row['observed_at']=(self.now+timedelta(seconds=delta)).isoformat()
            with self.assertRaises(ValueError):self.compile()
    def test_receipt_timestamp_checked_independently(self):
        p=self.root/'0/result.json';r=json.loads(p.read_text());r['started_at']=0;p.write_text(json.dumps(r))
        with self.assertRaises(ValueError):self.compile()
    def test_missing_caps_or_used_price_not_invented(self):
        for field in ['max_output_tokens','context_length','input_per_million','request_fee']:
            q=copy.deepcopy(self.quote);self.quote[field]=None
            with self.assertRaises(ValueError):self.compile()
            self.quote=q
    def test_timeband_expiry_precedes_rate_change(self):
        self.quote['overrides']=[{'utc_days':['friday'],'utc_start':100,'utc_end':400,'input_per_million':.4}]
        p,_=self.compile();self.assertEqual(p['frontier_policy']['expires_at'],int(self.now.replace(hour=1,minute=0,second=0).timestamp()))
    def test_wrong_workload_and_ambiguous_quotes(self):
        self.doc['workload']='coding'
        with self.assertRaises(ValueError):self.compile()
        self.doc['workload']='integer-json-v1'
        with self.assertRaises(ValueError):f.compile_policy(self.doc,[self.quote,self.quote],root=self.root,now=self.now)

if __name__=='__main__':unittest.main()
