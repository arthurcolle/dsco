"""Offline pricing algebra and empirical-frontier regression tests."""
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SPEC=importlib.util.spec_from_file_location('frontier',Path(__file__).resolve().parents[1]/'scripts/provider_cost_frontier.py')
f=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(f)
AT=datetime(2026,9,4,2,tzinfo=timezone.utc)


def quote(**changes):
    return dict(provider='direct',model='model',quote_id='direct:model',source='https://example.test/prices',
        observed_at=AT.isoformat(),input_per_million=2.,output_per_million=6.,cached_per_million=.5,
        request_fee=.01,context_length=100000,supports_tools=True,**changes)


class CostFrontierTests(unittest.TestCase):
    def cost(self,q,**kwargs):
        return f.scenario_cost(q,input_tokens=1000,output_tokens=100,cached_tokens=200,at=AT,**kwargs)

    def test_token_rates_and_fixed_request_fee(self):
        self.assertAlmostEqual(self.cost(quote())['cost_per_call_usd'],.0127)

    def test_unknown_negative_cache_is_never_free(self):
        for bad in (-1,None,'NaN'):
            q=quote();q['cached_per_million']=bad
            self.assertIsNone(self.cost(q)['cost_per_call_usd'])
        q=quote();del q['cached_per_million']
        self.assertIsNone(self.cost(q)['cost_per_call_usd'])
        self.assertIsNotNone(f.scenario_cost(q,input_tokens=100,output_tokens=10,at=AT)['cost_per_call_usd'])

    def test_context_and_tools_are_filters(self):
        q=quote();q['context_length']=1299
        self.assertIn('context_exceeded',self.cost(q)['exclusions'])
        q=quote();q['supports_tools']=None
        self.assertFalse(self.cost(q,require_tools=True)['eligible'])

    def test_long_context_override_uses_total_prompt(self):
        q=quote();q['overrides']=[{'min_prompt_tokens':1200,'input_per_million':4,'output_per_million':12}]
        self.assertEqual(self.cost(q)['applied_overrides'],[0])
        self.assertAlmostEqual(self.cost(q)['cost_per_call_usd'],.0153)

    def test_time_pricing_weekdays_boundaries(self):
        q=quote();q['overrides']=[{'utc_days':['monday','tuesday','wednesday','thursday','friday'],
            'utc_start':100,'utc_end':400,'input_per_million':4}]
        self.assertEqual(self.cost(q)['rates']['input_per_million'],4)
        q4=f.scenario_cost(q,input_tokens=1,output_tokens=1,at=AT.replace(hour=4))
        self.assertEqual(q4['rates']['input_per_million'],2)
        weekend=f.scenario_cost(q,input_tokens=1,output_tokens=1,at=AT.replace(day=5))
        self.assertEqual(weekend['rates']['input_per_million'],2)

    def test_openrouter_conversion_preserves_negative_and_request_fee(self):
        rows=f.openrouter_quotes({'data':[{'id':'vendor/m','context_length':10000,
            'supported_parameters':['tools'],'pricing':{'prompt':'-1','completion':'0.000006','request':'0.02'}}]},AT.isoformat())
        self.assertIsNone(rows[0]['input_per_million'])
        self.assertEqual(rows[0]['output_per_million'],6)
        self.assertEqual(rows[0]['request_fee'],.02)
        self.assertNotIn('cached_per_million',rows[0])

    def test_break_even_matches_direct_cost_algebra(self):
        direct=quote();direct.update(input_per_million=.22,cached_per_million=.007,output_per_million=.66,request_fee=0)
        router=quote();router.update(input_per_million=.0861,cached_per_million=.01722,output_per_million=.1722,request_fee=0)
        b=f.cache_break_even(direct,router,total_input_tokens=10000,output_tokens=100,at=AT)
        self.assertEqual(b['status'],'crossing')
        fraction=b['cached_fraction'];self.assertTrue(0<fraction<1)
        for q in (direct,router):
            q['linear_cost']=(10000*((1-fraction)*q['input_per_million']+fraction*q['cached_per_million'])+100*q['output_per_million'])/1e6
        self.assertAlmostEqual(direct['linear_cost'],router['linear_cost'])
        router['cached_per_million']=None
        self.assertEqual(f.cache_break_even(direct,router,total_input_tokens=10000,output_tokens=100)['status'],'unknown_rates')

    def test_only_measured_successes_generate_suggestions(self):
        q=quote();scenario=dict(input_tokens=100,output_tokens=10,at=AT)
        self.assertEqual(f.compare([q],scenario)['provisional_measured_suggestions'],[])
        doc={'rows':[{'provider':'direct','model':'model','effort':'low','attempts':2,'successes':1,'mean_latency_s':3}]}
        result=f.compare([q],scenario,[doc]);s=result['provisional_measured_suggestions'][0]
        self.assertAlmostEqual(s['projected_cost_per_verified_unit_usd'],s['cost_per_call_usd']*2)
        self.assertLess(s['success_probability_wilson_95'][0],.5)
        self.assertGreater(s['success_probability_wilson_95'][1],.5)
        doc['rows'][0]['successes']=0
        self.assertEqual(f.compare([q],scenario,[doc])['provisional_measured_suggestions'],[])

    def test_free_bill_never_claims_infinite_useful_runtime(self):
        q=quote();q.update(input_per_million=0,output_per_million=0,cached_per_million=0,request_fee=0)
        doc={'rows':[{'provider':'direct','model':'model','attempts':1,'successes':1,'mean_latency_s':1}]}
        row=f.compare([q],dict(input_tokens=100,output_tokens=100,at=AT),[doc])['provisional_measured_suggestions'][0]
        self.assertEqual(row['cost_per_call_usd'],0)
        self.assertIsNone(row['reference_inference_cost_usd'])
        self.assertIsNone(row['projected_verified_units_per_dollar'])

    def test_pareto_keeps_speed_cost_tradeoff(self):
        rows=[{'cost':1,'time':4},{'cost':2,'time':2},{'cost':3,'time':5}]
        self.assertEqual(f.pareto(rows,[('cost',1),('time',1)]),rows[:2])

    def test_snapshot_duplicates_and_distinct_workloads_not_pooled(self):
        row={'provider':'direct','model':'model','effort':'low','lane_id':'one',
             'attempts':1,'successes':1,'mean_latency_s':1}
        doc={'workload':'A','rows':[row]}
        grouped=f.empirical_lanes([doc,doc,{'workload':'B','rows':[row]}])
        self.assertEqual(len(grouped),2)
        self.assertTrue(all(g['attempts']==1 for g in grouped))

    def test_top_provider_limits_are_honored(self):
        rows=f.openrouter_quotes({'data':[{'id':'vendor/m','context_length':10000,
            'top_provider':{'context_length':1000,'max_completion_tokens':50},
            'pricing':{'prompt':'0','completion':'0'}}]},AT.isoformat())
        self.assertEqual(rows[0]['context_length'],1000)
        result=f.scenario_cost(rows[0],input_tokens=100,output_tokens=51,at=AT)
        self.assertIn('output_limit_exceeded',result['exclusions'])

    def test_cli_json_works_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'catalog.json';path.write_text(json.dumps({'data':[]}))
            p=subprocess.run([sys.executable,str(Path(f.__file__)), '--catalog',str(path),
                '--input-tokens','100','--output-tokens','20','--at',AT.isoformat()],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            self.assertFalse(json.loads(p.stdout)['auto_routing'])


if __name__=='__main__':unittest.main()
