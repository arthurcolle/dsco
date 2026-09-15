#!/usr/bin/env python3
"""Unroll every discovered model/endpoint/product without eligibility filtering.

Inventory is not launch authorization or inference readiness. Unknown capabilities,
prices and model access remain unknown. No credential values are read or emitted.
"""
import argparse,collections,datetime,hashlib,json
from pathlib import Path

IDENTITY=('provider','product','principal_profile','transport','billing','auth_mode','model','upstream','endpoint_tag','quantization','effort','endpoint','service_tier','catalog_variant')

def stable_id(row):
    data=json.dumps([row.get(k,'') for k in IDENTITY],ensure_ascii=False,separators=(',',':'))
    return 'lane-'+hashlib.sha256(data.encode()).hexdigest()[:24]

def items(doc):
    value=doc if isinstance(doc,list) else (doc.get('data') or doc.get('models') or [])
    return value if isinstance(value,list) else []

def unroll(base):
    catalog_dir=base/('provider-inventory-public' if (base/'provider-inventory-public/inventory.json').exists() else 'provider-inventory')
    inventory=json.loads((catalog_dir/'inventory.json').read_text())
    auth=json.loads((base/'provider-inventory/auth-lanes.json').read_text())['lanes']
    providers=json.loads((base/'upstream-catalog/providers.json').read_text())['data']
    upstream_map={r['name']:r['slug'] for r in providers}
    lanes={};sources={};unresolved=[];counts=collections.Counter()
    def source(path):
        path=path.resolve();key=str(path)
        if key not in sources:sources[key]={'path':key,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        return key
    def add(product,model,source_path,scope,*,upstream='',tag='',quantization='',endpoint_meta=None,efforts=None):
        model_id=model.get('id',model.get('slug',model.get('name')))
        if not isinstance(model_id,str) or not model_id:return
        meta=endpoint_meta or model
        tiers=['default']+[t['id'] for t in model.get('service_tiers',[]) if isinstance(t,dict) and t.get('id') and t['id']!='default']
        for effort,tier in ((e,t) for e in efforts or ['auto'] for t in tiers):
            row={k:product.get(k,'') for k in IDENTITY}
            row.update(model=model_id,upstream=upstream,endpoint_tag=tag,quantization=quantization or '',effort=effort,service_tier=tier,
                native_effort_supported=effort in ('auto','none','minimal','low','medium','high','xhigh','max'),
                auth_ready=product.get('ready'),auth_readiness_basis='local_auth_only',
                inference_verified=False,model_access_verified=False,catalog_scope=scope,
                source=source(source_path),source_model_metadata=model,endpoint_metadata=endpoint_meta,
                pricing_raw=meta.get('pricing'),context_tokens=meta.get('context_length',meta.get('context_window')),
                max_output_tokens=meta.get('max_completion_tokens',meta.get('max_output_tokens')),
                supported_parameters=meta.get('supported_parameters'),
                pin_precision=('endpoint_catalog_identity' if tag else 'upstream_catalog_identity' if upstream else 'provider_model'),
                runtime_pin={'provider':product['provider'],'model':model_id,'effort':effort})
            if upstream:row['runtime_pin']['upstream']=upstream
            if tag:row['runtime_pin']['endpoint_tag']=tag
            if quantization:row['runtime_pin']['quantization']=quantization
            row['lane_id']=stable_id(row)
            if row['lane_id'] in lanes:
                original=row['lane_id'];variant=1
                while row['lane_id'] in lanes:
                    row['catalog_variant']=str(variant);variant+=1;row['lane_id']=stable_id(row)
                row['same_runtime_identity_as']=original
                row['pin_precision']='shared_runtime_identity_requires_variant_resolution'
            lanes[row['lane_id']]=row;counts[scope]+=1
    byprovider=collections.defaultdict(list)
    for a in auth:byprovider[a['provider']].append(a)
    for p in inventory:
        name=p['provider'];products=byprovider.get(name) or [dict(provider=name,product=name,principal_profile='default',transport=p['transport'],billing='unknown',auth_mode='unknown',endpoint=p.get('base_url') or '',ready=False)]
        catalog=catalog_dir/f'{name}-models.json'
        for product in products:
            models=items(json.loads(catalog.read_text())) if catalog.exists() else []
            scope='first_party_catalog'
            # Account-scoped catalogs must not be replaced with OpenAI API IDs.
            account_catalog=base/'oauth-catalogs/codex.json'
            if name=='openai-codex':
                models=items(json.loads(account_catalog.read_text())) if account_catalog.exists() else [];catalog_for_product=account_catalog;scope='account_catalog'
            else:catalog_for_product=catalog
            if product.get('billing')=='included' and models and name!='openai-codex':scope='provider_catalog_subscription_access_unverified'
            if not models:
                unresolved.append({'provider':name,'product':product.get('product'),'principal_profile':product.get('principal_profile'),'reason':'no discovered model catalog','auth_ready':product.get('ready'),'discovery_status':p['status']})
                fallback=product.get('model') or p.get('default_model')
                if fallback:add(product,{'id':fallback},base/'provider-inventory/auth-lanes.json','configured_model_only')
                continue
            for model in models:
                if not isinstance(model,dict):continue
                efforts=model.get('supported_reasoning_levels')
                if isinstance(efforts,list):efforts=[x.get('effort') if isinstance(x,dict) else x for x in efforts];efforts=[x for x in efforts if isinstance(x,str) and x]
                else:efforts=None
                add(product,model,catalog_for_product,scope,efforts=efforts)
                if name=='huggingface':
                    for host in model.get('providers',[]):
                        add(product,model,catalog_for_product,'huggingface_upstream',upstream=host['provider'],endpoint_meta=host)
    # External products can exist without a native provider profile (e.g. grok CLI).
    profiled={p['provider'] for p in inventory}
    for name,products in byprovider.items():
        if name in profiled:continue
        for product in products:
            external=base/'oauth-catalogs/grok.json' if name=='xai-grok' else None
            if external and external.exists():
                for m in items(json.loads(external.read_text())):
                    efforts=[e.get('value',e.get('id')) for e in m.get('reasoning_efforts',[]) if isinstance(e,dict)]
                    add(product,m,external,'external_account_catalog',efforts=efforts)
            else:
                if product.get('model'):add(product,{'id':product['model']},base/'provider-inventory/auth-lanes.json','external_configured_model')
                unresolved.append({'provider':name,'product':product.get('product'),'reason':'external executor model catalog not discovered','auth_ready':product.get('ready')})
    orproducts=byprovider.get('openrouter',[])
    endpoint_count=0
    for path in sorted((base/'upstream-catalog/endpoints').glob('*.json')):
        data=json.loads(path.read_text()).get('data',{})
        for endpoint in data.get('endpoints',[]):
            endpoint_count+=1
            host=upstream_map.get(endpoint.get('provider_name'))
            if not host:host=endpoint.get('tag','').split('/')[0]
            for product in orproducts:
                add(product,{'id':data['id'],'architecture':data.get('architecture')},path,'openrouter_endpoint',
                    upstream=host,tag=endpoint.get('tag',''),quantization=endpoint.get('quantization',''),endpoint_meta=endpoint)
    rows=sorted(lanes.values(),key=lambda r:tuple(str(r.get(k,'')) for k in IDENTITY))
    return {'schema':'dsco.full_lane_inventory.v1','generated_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'identity_dimensions':list(IDENTITY),'lane_count':len(rows),'counts_by_scope':dict(counts),
        'native_profiles':len(inventory),'provider_inventory':inventory,'upstream_providers':providers,'openrouter_provider_slugs':len(providers),'openrouter_endpoint_records':endpoint_count,
        'unresolved_catalogs':unresolved,'source_manifest':list(sources.values()),'lanes':rows,
        'limitations':['No eligibility, price, modality, readiness or quality filter removes a discovered lane.',
            'API and subscription products are distinct; auth readiness does not prove model access.',
            'Only explicitly enumerated reasoning levels expand; unknown effort support remains auto.',
            'Endpoint identity is retained even when runtime supports only provider-level pinning.',
            'Inventory has no concurrency cap; launching lanes requires separate budgets and validation.',
            'Unavailable catalogs remain explicit gaps; configured fallback models are labeled, never claimed exhaustive.']}

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--discovery',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args(argv)
    result=unroll(a.discovery);a.output.mkdir(parents=True,exist_ok=True)
    (a.output/'inventory.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    with (a.output/'lanes.jsonl').open('w') as out:
        for row in result['lanes']:out.write(json.dumps(row,allow_nan=False)+'\n')
    shards=a.output/'shards';shards.mkdir(exist_ok=True);written=set()
    for i in range(0,len(result['lanes']),64):
        written.add(f'lanes-{i//64:04d}.json')
        (shards/f'lanes-{i//64:04d}.json').write_text(json.dumps({'schema':'dsco.lane_inventory_shard.v1','inventory_only':True,'lanes':result['lanes'][i:i+64]},indent=2)+'\n')
    for old in shards.glob('lanes-????.json'):
        if old.name not in written:old.unlink()
    print(json.dumps({k:v for k,v in result.items() if k not in ('lanes','source_manifest','unresolved_catalogs','provider_inventory','upstream_providers')}))

if __name__=='__main__':main()
