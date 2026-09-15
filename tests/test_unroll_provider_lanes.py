#!/usr/bin/env python3
import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from unroll_provider_lanes import unroll

class UnrollTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.base=Path(self.tmp.name)
        def write(name,data):
            p=self.base/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(data))
        self.write=write
        providers=['openai','openai-codex','openrouter','huggingface']
        write('provider-inventory/inventory.json',[{'provider':x,'transport':'native','status':'catalog_ok'} for x in providers])
        write('provider-inventory/auth-lanes.json',{'lanes':[dict(provider=x,product=x,principal_profile='default',transport='native',billing='included' if x=='openai-codex' else 'metered',auth_mode=x,endpoint='https://example.test/'+x,ready=True) for x in providers]})
        write('provider-inventory/openai-models.json',{'data':[{'id':'shared'},{'id':'audio-only','architecture':{'output_modalities':['audio']}}]})
        write('oauth-catalogs/codex.json',{'models':[{'slug':'shared','visibility':'hidden','supported_reasoning_levels':[{'effort':'low'},{'effort':'ultra'}],'service_tiers':[{'id':'priority'}]}]})
        write('provider-inventory/openrouter-models.json',{'data':[{'id':'author/m'},{'id':'no-endpoint'}]})
        write('provider-inventory/huggingface-models.json',{'data':[{'id':'author/m','providers':[{'provider':'host-a','pricing':None},{'provider':'host-b','pricing':{'input':0}}]}]})
        write('upstream-catalog/providers.json',{'data':[{'name':'Host','slug':'host'}]})
        write('upstream-catalog/endpoints/m.json',{'data':{'id':'author/m','endpoints':[{'provider_name':'Host','tag':'host/fp8','quantization':'fp8','pricing':{'prompt':'.1'}},{'provider_name':'Host','tag':'host/fp8','quantization':'fp8','pricing':{'prompt':'.2'}}]}})
    def test_full_cartesian_account_identity_and_no_modality_filter(self):
        d=unroll(self.base);self.assertEqual(d['lane_count'],13)
        api=[r for r in d['lanes'] if r['provider']=='openai'];codex=[r for r in d['lanes'] if r['provider']=='openai-codex']
        self.assertEqual({r['model'] for r in api},{'shared','audio-only'});self.assertEqual(len(codex),4)
        self.assertTrue(all(r['model']=='shared' for r in codex));self.assertTrue(any(not r['native_effort_supported'] for r in codex))
        self.assertEqual(len({r['lane_id'] for r in d['lanes']}),13)
    def test_conflicting_same_tag_records_are_not_discarded(self):
        d=unroll(self.base);rows=[r for r in d['lanes'] if r['catalog_scope']=='openrouter_endpoint']
        self.assertEqual(len(rows),2);self.assertEqual({r['pricing_raw']['prompt'] for r in rows},{'.1','.2'})
        self.assertTrue(any(r.get('same_runtime_identity_as') for r in rows))
    def test_hf_hosts_unknown_and_zero_prices_retained(self):
        rows=[r for r in unroll(self.base)['lanes'] if r['catalog_scope']=='huggingface_upstream']
        self.assertEqual(len(rows),2);self.assertEqual(rows[0]['pricing_raw'],None);self.assertEqual(rows[1]['pricing_raw']['input'],0)
    def test_missing_codex_catalog_never_borrows_api_models(self):
        (self.base/'oauth-catalogs/codex.json').unlink();d=unroll(self.base)
        self.assertFalse(any(r['provider']=='openai-codex' for r in d['lanes']))
        self.assertTrue(any(r['provider']=='openai-codex' for r in d['unresolved_catalogs']))
    def test_deterministic_ids_and_complete_sources(self):
        a=unroll(self.base);b=unroll(self.base)
        self.assertEqual([r['lane_id'] for r in a['lanes']],[r['lane_id'] for r in b['lanes']])
        sources={r['path'] for r in a['source_manifest']};self.assertTrue(all(r['source'] in sources for r in a['lanes']))

if __name__=='__main__':unittest.main()
