#!/usr/bin/env python3
"""Compile an opt-in policy from fresh quotes and independently checked receipts.

The initial validator supports integer-json-v1 only. It never treats a model's
self-reported success or an embedded JSON fragment as verified work.
"""
import argparse
from datetime import datetime, timezone, timedelta
import hashlib
import json
from pathlib import Path
import math
from provider_cost_frontier import timestamp, openrouter_quotes, effective_rates


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def validate_evidence(document, root, now, ttl):
    if document.get('schema') != 'dsco.route_evidence.v1' or document.get('workload') != 'integer-json-v1':
        raise ValueError('unsupported evidence schema/workload')
    rows=[]; seen=set(); manifest=[]
    for row in document['rows']:
        key=tuple(row.get(k,'') for k in ('provider','model','effort','upstream','quantization','auth_class'))
        if not all(isinstance(k,str) and k for k in key[:3]) or not all(isinstance(k,str) for k in key[3:]) or key in seen:
            raise ValueError('missing or duplicate route identity')
        seen.add(key)
        age=(now-timestamp(row['observed_at'])).total_seconds()
        if age<0 or age>ttl: raise ValueError('stale or future evidence')
        if row.get('strict_validation') is not True: raise ValueError('strict validation required')
        sources=row.get('sources',[])
        if len(sources)!=row.get('attempts') or len(set(sources))!=len(sources) or not sources:
            raise ValueError('each attempt requires a unique receipt')
        passed=0;unknown=0;latencies=[];last=0
        for source in sources:
            p=(root/source).resolve();raw=p.read_bytes();r=json.loads(raw)
            if tuple(r.get(k) for k in ('provider','model'))!=key[:2]: raise ValueError('receipt identity mismatch')
            for field in ('upstream','quantization','auth_class'):
                if row.get(field,'') != r.get(field,''):raise ValueError('receipt endpoint identity mismatch')
            if not finite(r.get('elapsed_s')) or not finite(r.get('started_at')): raise ValueError('invalid attempt timing')
            finished=r['started_at']+r['elapsed_s'];last=max(last,finished)
            if not 0 <= now.timestamp()-finished <= ttl: raise ValueError('stale or future receipt')
            folder=p.parent/'workers'/r['attempt_token']
            output=(folder/'stdout.log').read_bytes()
            expected=r.get('expected')
            if not isinstance(expected,dict) or set(expected)!={'sorted','checksum'} or not isinstance(expected['sorted'],list) or not expected['sorted'] or any(type(n) is not int for n in expected['sorted']) or type(expected['checksum']) is not int or expected['sorted']!=sorted(expected['sorted']) or sum(expected['sorted'])!=expected['checksum']:
                raise ValueError('invalid independent expected integer result')
            try: actual=json.loads(output)
            except (ValueError,UnicodeError): actual=None
            valid=(type(actual) is dict and set(actual)=={'sorted','checksum'} and type(actual.get('checksum')) is int and isinstance(actual.get('sorted'),list) and all(type(n) is int for n in actual['sorted']) and actual==expected and r.get('exit_code')==0)
            if (r.get('status')=='verified')!=valid: raise ValueError('receipt verification disagrees with independent check')
            passed+=int(valid);unknown+=int(bool(r.get('incomplete_cost_receipt',True)))
            latencies.append(r['elapsed_s'])
            manifest.append({'path':str(p),'sha256':hashlib.sha256(raw).hexdigest(),'stdout_sha256':hashlib.sha256(output).hexdigest()})
        if passed!=row.get('successes') or unknown!=row.get('unpriced_attempts'): raise ValueError('aggregate contradicts receipts')
        rows.append({**row,'attempts':len(sources),'successes':passed,'unpriced_attempts':unknown,'mean_latency_s':sum(latencies)/len(latencies),'observed_at':datetime.fromtimestamp(last,timezone.utc).isoformat()})
    return rows,manifest


def quote_expiry(quote, total_input, now, ttl):
    """A flattened quote expires before any UTC/context-selected rate change."""
    end=now+timedelta(seconds=ttl)
    initial,_=effective_rates(quote,total_input,now)
    probe=now.replace(second=0,microsecond=0)+timedelta(minutes=1)
    while probe<end:
        if effective_rates(quote,total_input,probe)[0]!=initial: return probe
        probe+=timedelta(minutes=1)
    return end


def compile_policy(evidence, quotes, *, root, now, ttl=3600, prompt_tokens=8000,
                   output_tokens=100, cache_fraction=0., max_latency=10.):
    if type(ttl) is not int or not 0<ttl<=86400: raise ValueError('TTL must be 1..86400 seconds')
    if type(prompt_tokens) is not int or type(output_tokens) is not int or min(prompt_tokens,output_tokens)<1:
        raise ValueError('positive integer token bounds required')
    if not finite(cache_fraction) or cache_fraction>1 or not finite(max_latency) or max_latency<=0:
        raise ValueError('invalid cache fraction or latency')
    rows,manifest=validate_evidence(evidence,root,now,ttl)
    by_route={}
    for q in quotes:
        key=(q.get('provider'),q.get('model'),q.get('upstream',''),q.get('quantization',''),q.get('auth_class',''))
        if key in by_route: raise ValueError('ambiguous duplicate quote route')
        by_route[key]=q
    lanes=[];excluded=[];expiry=now+timedelta(seconds=ttl)
    for row in rows:
        key=(row['provider'],row['model'],row.get('upstream',''),row.get('quantization',''),row.get('auth_class',''));q=by_route.get(key)
        reason=None
        if not q: reason='no exact route quote'
        elif not q.get('source') or not q.get('observed_at'): reason='missing quote provenance'
        else:
            age=(now-timestamp(q['observed_at'])).total_seconds()
            if not 0<=age<ttl: reason='stale or future quote'
        if reason:
            excluded.append({'provider':key[0],'model':key[1],'reason':reason});continue
        if not finite(q.get('context_length')) or not finite(q.get('max_output_tokens')) or q['context_length']<=0 or q['max_output_tokens']<=0:
            excluded.append({'provider':key[0],'model':key[1],'reason':'unknown context or output limit'});continue
        rates,_=effective_rates(q,prompt_tokens,now)
        cached=int(prompt_tokens*cache_fraction)
        used=['input_per_million','output_per_million','request_fee']+(['cached_per_million'] if cached else [])
        if any(not finite(rates.get(k)) for k in used):
            excluded.append({'provider':key[0],'model':key[1],'reason':'unknown used rate'});continue
        expiry=min(expiry,quote_expiry(q,prompt_tokens,now,ttl),timestamp(q['observed_at'])+timedelta(seconds=ttl),timestamp(row['observed_at'])+timedelta(seconds=ttl))
        # Zero has no implication of unlimited inference; native ranking retains
        # cost and latency directly and never computes inverse cost for zero.
        lanes.append({'provider':key[0],'model':key[1],'effort':row['effort'],
            'upstream':row.get('upstream',''),'quantization':row.get('quantization',''),'auth_class':row.get('auth_class') or 'api',
            'source':q['source'],'quote_observed_at':int(timestamp(q['observed_at']).timestamp()),
            'evidence_observed_at':int(timestamp(row['observed_at']).timestamp()),
            'route_specific':True,'strict_validation':True,'unpriced_attempts':row['unpriced_attempts'],
            'tools':q.get('supports_tools') is True,'context_tokens':q.get('context_length'),
            'max_output_tokens':q.get('max_output_tokens'),
            **rates,'cached_tokens':cached,'attempts':row['attempts'],'verified':row['successes'],
            'mean_latency_seconds':row['mean_latency_s']})
    if not lanes: raise ValueError('no routes have fresh usable quotes and evidence')
    policy={'schema':'dsco.cost_frontier_policy.v1','workload':'integer-json-v1','validator':'standalone-json-v1',
        'expires_at':int(expiry.timestamp()),'max_quote_age_seconds':ttl,'max_evidence_age_seconds':ttl,
        'min_attempts':3,'minimum_success_lower_bound':.4,'max_latency_seconds':max_latency,
        'prompt_tokens':prompt_tokens,'output_tokens':output_tokens,'require_tools':False,'lanes':lanes}
    return {'action':'provider_fabric','selection':'cost_frontier','workload':policy['workload'],
        'validator':policy['validator'],'frontier_policy':policy,'max_agents':1,'replicas':1,
        'race':False,'mode':'spawn','include_metered':True}, {'generated_at':now.isoformat(),
        'receipt_manifest':manifest,'excluded':excluded,'cache_fraction_is_scenario_hint':cache_fraction,
        'limitations':['Three attempts are a small qualification, not production reliability.',
            'Caller must keep tasks within the named workload and declared token bounds.',
            'Cache hints are not proof of cache hits; use zero without measured reuse.',
            'No task is verified merely because the selected worker exits successfully.']}


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True);p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--quotes',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--ttl',type=int,default=3600);p.add_argument('--prompt-tokens',type=int,default=8000)
    p.add_argument('--output-tokens',type=int,default=100);p.add_argument('--cache-fraction',type=float,default=0.)
    p.add_argument('--max-latency',type=float,default=10.)
    a=p.parse_args(argv);now=datetime.now(timezone.utc)
    catalog=json.loads(a.catalog.read_text())
    quotes=openrouter_quotes(catalog,datetime.fromtimestamp(a.catalog.stat().st_mtime,timezone.utc).isoformat())
    # Historical scenario reports assumed absent request fees were zero. An
    # execution policy requires an explicit quote; missing fees remain unknown.
    raw_by_id={m['id']:m for m in catalog['data']}
    for q in quotes:
        if 'request' not in raw_by_id[q['model']].get('pricing',{}):q['request_fee']=None
    quotes+=json.loads(a.quotes.read_text())['quotes']
    policy,audit=compile_policy(json.loads(a.evidence.read_text()),quotes,root=a.root,now=now,ttl=a.ttl,
        prompt_tokens=a.prompt_tokens,output_tokens=a.output_tokens,cache_fraction=a.cache_fraction,max_latency=a.max_latency)
    audit['inputs']=[{'path':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in (a.evidence,a.catalog,a.quotes)]
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(policy,indent=2,allow_nan=False)+'\n')
    a.output.with_suffix('.audit.json').write_text(json.dumps(audit,indent=2,allow_nan=False)+'\n')
    print(json.dumps({'policy':str(a.output),'lanes':len(policy['frontier_policy']['lanes']),'excluded':audit['excluded']}))


if __name__=='__main__':main()
