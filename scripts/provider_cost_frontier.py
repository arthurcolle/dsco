#!/usr/bin/env python3
"""Offline scenario quote comparison. Never changes routing or invokes a model.

Input tokens mean UNCACHED input; cached tokens are additional input. Quote JSON
contains `quotes` with provider/model/quote_id/source/observed_at, USD-per-million
input_per_million/output_per_million/cached_per_million, request_fee,
context_length, supports_tools, and optional overrides. Overrides support
min_prompt_tokens and UTC weekday/HHMM windows, with inclusive start/exclusive end.
Empirical JSON accepts the existing dsco.empirical_frontier.v1 `rows` schema.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

RATE_FIELDS = ('input_per_million','output_per_million','cached_per_million','request_fee')
OR_FIELDS = {'prompt':'input_per_million','completion':'output_per_million',
             'input_cache_read':'cached_per_million','request':'request_fee'}
DAYS = ('monday','tuesday','wednesday','thursday','friday','saturday','sunday')


def nonnegative(value):
    if isinstance(value,bool): return None
    try: number=float(value)
    except (ValueError,TypeError,OverflowError): return None
    return number if math.isfinite(number) and number>=0 else None


def timestamp(value):
    parsed=datetime.fromisoformat(value.replace('Z','+00:00'))
    if parsed.tzinfo is None: raise ValueError('timestamps require a timezone')
    return parsed.astimezone(timezone.utc)


def openrouter_quotes(catalog, observed_at):
    result=[]
    for model in catalog.get('data',[]):
        if not isinstance(model,dict) or not model.get('id'): continue
        pricing=model.get('pricing') or {}
        top=model.get('top_provider') or {}
        contexts=[n for n in (nonnegative(model.get('context_length')),nonnegative(top.get('context_length'))) if n is not None]
        quote={'provider':'openrouter','model':model['id'],'quote_id':'openrouter:'+model['id'],
               'source':'https://openrouter.ai/api/v1/models','observed_at':observed_at,
               'context_length':min(contexts) if contexts else None,
               'max_output_tokens':top.get('max_completion_tokens'),
               'supports_tools':'tools' in (model.get('supported_parameters') or []),
               'billing_basis':'catalog_quote','request_fee':0.0,
               'quote_channel':'aggregator','overrides':[]}
        for raw,key in OR_FIELDS.items():
            if raw in pricing:
                value=nonnegative(pricing[raw])
                quote[key]=value*(1 if raw=='request' else 1e6) if value is not None else None
        for override in pricing.get('overrides',[]):
            transformed={k:v for k,v in override.items() if k not in OR_FIELDS}
            for raw,key in OR_FIELDS.items():
                if raw in override:
                    value=nonnegative(override[raw])
                    transformed[key]=value*(1 if raw=='request' else 1e6) if value is not None else None
            quote['overrides'].append(transformed)
        result.append(quote)
    return result


def _minutes(value):
    if type(value) is not int or value<0 or value>2359 or value%100>=60:
        raise ValueError('UTC times must be valid integer HHMM values')
    return (value//100)*60+value%100


def effective_rates(quote, prompt_tokens, at):
    rates={key:nonnegative(quote.get(key)) for key in RATE_FIELDS}
    applied=[]
    for i,override in enumerate(quote.get('overrides',[])):
        threshold=override.get('min_prompt_tokens')
        if threshold is not None:
            if type(threshold) is not int or threshold<0:raise ValueError('invalid context threshold')
            if prompt_tokens<threshold:continue
        days=override.get('utc_days')
        if days is not None:
            if not isinstance(days,list) or any(day not in DAYS for day in days):raise ValueError('invalid UTC weekdays')
            if DAYS[at.weekday()] not in days:continue
        if 'utc_start' in override or 'utc_end' in override:
            start,end=_minutes(override.get('utc_start',0)),_minutes(override.get('utc_end',0))
            minute=at.hour*60+at.minute
            active=(start<=minute<end) if start<end else (minute>=start or minute<end)
            if not active:continue
        selectors={'min_prompt_tokens','utc_days','utc_start','utc_end'}
        # Unknown conditions cannot safely be interpreted as universal prices.
        if set(override)-selectors-set(RATE_FIELDS):raise ValueError('unsupported pricing override fields')
        for key in RATE_FIELDS:
            if key in override:rates[key]=nonnegative(override[key])
        applied.append(i)
    return rates,applied


def scenario_cost(quote, *, input_tokens, output_tokens, cached_tokens=0, require_tools=False, at=None):
    if any(type(n) is not int or n<0 for n in (input_tokens,output_tokens,cached_tokens)):
        raise ValueError('token counts must be nonnegative integers')
    at=at or datetime.now(timezone.utc)
    if at.tzinfo is None:raise ValueError('scenario time requires a timezone')
    at=at.astimezone(timezone.utc)
    total=input_tokens+cached_tokens+output_tokens
    rates,applied=effective_rates(quote,input_tokens+cached_tokens,at)
    reasons=[]
    context=nonnegative(quote.get('context_length'))
    if context is None:reasons.append('unknown_context_limit')
    elif total>context:reasons.append('context_exceeded')
    if require_tools and quote.get('supports_tools') is not True:reasons.append('tool_support_not_confirmed')
    output_limit=nonnegative(quote.get('max_output_tokens'))
    if output_limit is not None and output_tokens>output_limit:reasons.append('output_limit_exceeded')
    required=[('input_per_million',input_tokens),('output_per_million',output_tokens),
              ('cached_per_million',cached_tokens)]
    for key,tokens in required:
        if tokens and rates[key] is None:reasons.append('unknown_'+key)
    if rates['request_fee'] is None:reasons.append('unknown_request_fee')
    if reasons:cost=None
    else:cost=rates['request_fee']+sum(tokens*rates[key]/1e6 for key,tokens in required if tokens)
    return {**{key:quote.get(key) for key in ('provider','model','quote_id','source','observed_at','quote_channel','billing_basis')},
        'scenario_at':at.isoformat(),'rates':rates,'applied_overrides':applied,
        'context_length':context,'supports_tools':quote.get('supports_tools'),
        'eligible':not reasons,'exclusions':reasons,'cost_per_call_usd':cost,
        'billed_free':cost==0 if cost is not None else False,
        'reference_inference_cost_usd':None,
        'reference_inference_note':'Billed/catalog quotes do not establish resource cost, including when the bill is zero.'}


def cache_break_even(direct,aggregator,*,total_input_tokens,output_tokens,at=None):
    """Solve direct cost == aggregator cost for cached fraction of fixed input.

Both quotes must have known input/cache/output/request fees. Long-context tiers
are selected at fixed total input; no rate is inferred from a model-list API.
"""
    if any(type(n) is not int or n<0 for n in (total_input_tokens,output_tokens)):
        raise ValueError('token counts must be nonnegative integers')
    at=at or datetime.now(timezone.utc)
    if at.tzinfo is None:raise ValueError('scenario time requires a timezone')
    at=at.astimezone(timezone.utc)
    a,_=effective_rates(direct,total_input_tokens,at)
    b,_=effective_rates(aggregator,total_input_tokens,at)
    if any(a[k] is None or b[k] is None for k in RATE_FIELDS):
        return {'status':'unknown_rates','cached_fraction':None}
    intercept=(total_input_tokens*(a['input_per_million']-b['input_per_million'])+
        output_tokens*(a['output_per_million']-b['output_per_million']))/1e6+a['request_fee']-b['request_fee']
    slope=total_input_tokens*((a['cached_per_million']-a['input_per_million'])-
                              (b['cached_per_million']-b['input_per_million']))/1e6
    if abs(slope)<1e-15:
        return {'status':'equal_all_cache_fractions' if abs(intercept)<1e-15 else 'no_crossing',
                'cached_fraction':None,'cheaper':'equal' if abs(intercept)<1e-15 else ('direct' if intercept<0 else 'aggregator')}
    point=-intercept/slope
    return {'status':'crossing' if 0<=point<=1 else 'outside_feasible_cache_fraction',
            'cached_fraction':point if 0<=point<=1 else None,
            'direct_cheaper_condition':('cached_fraction > threshold' if slope<0 else 'cached_fraction < threshold'),
            'algebraic_threshold':point,'delta_at_uncached_usd':intercept,'delta_at_fully_cached_usd':intercept+slope}


def wilson(successes,attempts):
    z=1.959963984540054
    p=successes/attempts;den=1+z*z/attempts
    center=(p+z*z/(2*attempts))/den
    width=z*math.sqrt((p*(1-p)+z*z/(4*attempts))/attempts)/den
    return [max(0,center-width),min(1,center+width)]


def empirical_lanes(documents):
    grouped={}
    seen=set()
    for doc in documents:
        for row in doc.get('rows',[]):
            n,s=row.get('attempts'),row.get('successes')
            if type(n) is not int or type(s) is not int or n<=0 or not 0<=s<=n:continue
            workload=doc.get('workload','unspecified')
            identity=(workload,row.get('provider'),row.get('model'),row.get('effort'),row.get('lane_id'))
            if row.get('lane_id') and identity in seen:continue
            if row.get('lane_id'):seen.add(identity)
            key=(row.get('provider'),row.get('model'),row.get('effort'),workload)
            lane=grouped.setdefault(key,{'provider':key[0],'model':key[1],'effort':key[2],
                'workload':workload,'attempts':0,'successes':0,'latency_sum':0.,'latency_samples':0,'unpriced_attempts':0})
            lane['attempts']+=n;lane['successes']+=s
            latency=nonnegative(row.get('mean_latency_s'))
            if latency is not None:lane['latency_sum']+=latency*n;lane['latency_samples']+=n
            missing=row.get('unpriced_attempts',0)
            lane['unpriced_attempts']+=missing if type(missing) is int and 0<=missing<=n else n
    return list(grouped.values())


def pareto(rows,axes):
    # axes is (field, direction), direction=1 minimizes and -1 maximizes.
    eligible=[r for r in rows if all(nonnegative(r.get(key)) is not None for key,_ in axes)]
    result=[]
    for row in eligible:
        def dominates(other):
            return all(other[k]*d<=row[k]*d for k,d in axes) and any(other[k]*d<row[k]*d for k,d in axes)
        if not any(other is not row and dominates(other) for other in eligible):result.append(row)
    return result


def compare(quotes,scenario,empirical=()):
    rows=[]
    for quote in quotes:
        try:rows.append(scenario_cost(quote,**scenario))
        except (ValueError,TypeError) as exc:
            rows.append({**{k:quote.get(k) for k in ('provider','model','quote_id','source','observed_at','quote_channel','billing_basis')},
                         'eligible':False,'exclusions':[str(exc)],'cost_per_call_usd':None})
    suggestions=[]
    for lane in empirical_lanes(empirical):
        if lane['successes']==0:continue
        for quote in rows:
            if not quote['eligible'] or (quote['provider'],quote['model'])!=(lane['provider'],lane['model']):continue
            probability=lane['successes']/lane['attempts'];lo,hi=wilson(lane['successes'],lane['attempts'])
            cost=quote['cost_per_call_usd'];projection=cost/probability
            suggestions.append({**quote,**lane,'provisional':True,'observed_success_rate':probability,
                'success_probability_wilson_95':[lo,hi],
                'mean_latency_s':lane['latency_sum']/lane['latency_samples'] if lane['latency_samples'] else None,
                'projected_cost_per_verified_unit_usd':projection,
                'projection_interval_usd':[cost/hi,cost/lo if lo>0 else None],
                'projected_verified_units_per_dollar':1/projection if projection>0 else None})
    return {'schema':'dsco.scenario_cost_frontier.v1','auto_routing':False,
        'limitations':['Scenario input tokens are uncached; cached tokens are additional input.',
            'Cost per verified unit projects this per-call quote through the observed success fraction; workload and token usage must be comparable.',
            'Wilson intervals describe binomial sampling uncertainty only; pilots do not establish production reliability, quality, or sustained runtime.',
            'Free billed quotes provide no infinite-runtime or free-resource inference; zero-cost throughput projections are null.',
            'No provider inference calls, quota checks, or routing changes are performed.'],
        'quotes':rows,
        'catalog_cost_context_frontier':pareto(rows,[('cost_per_call_usd',1),('context_length',-1)]),
        'provisional_measured_suggestions':suggestions,
        'measured_pareto_frontier':[row for workload in sorted({s['workload'] for s in suggestions})
            for row in pareto([s for s in suggestions if s['workload']==workload],
                [('projected_cost_per_verified_unit_usd',1),('mean_latency_s',1),('observed_success_rate',-1)])]}


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--catalog',type=Path,required=True);p.add_argument('--quotes',type=Path)
    p.add_argument('--empirical',type=Path,action='append',default=[])
    p.add_argument('--input-tokens',type=int,required=True);p.add_argument('--output-tokens',type=int,required=True)
    p.add_argument('--cached-tokens',type=int,default=0);p.add_argument('--require-tools',action='store_true')
    p.add_argument('--at',type=timestamp,default=datetime.now(timezone.utc));p.add_argument('--output',type=Path)
    p.add_argument('--break-even-direct');p.add_argument('--break-even-aggregator')
    args=p.parse_args(argv)
    if min(args.input_tokens,args.output_tokens,args.cached_tokens)<0:p.error('token counts must be nonnegative')
    catalog=args.catalog.read_bytes()
    observed=datetime.fromtimestamp(args.catalog.stat().st_mtime,timezone.utc).isoformat()
    quotes=openrouter_quotes(json.loads(catalog),observed)
    if args.quotes:
        direct=json.loads(args.quotes.read_text()).get('quotes',[])
        for q in direct:
            if not all(q.get(k) for k in ('provider','model','quote_id','source','observed_at')):p.error('explicit quotes require identity/source/observed_at')
            timestamp(q['observed_at'])
            q.setdefault('quote_channel','explicit')
            q.setdefault('billing_basis','provided_quote')
            quotes.append(q)
    report=compare(quotes,dict(input_tokens=args.input_tokens,output_tokens=args.output_tokens,
        cached_tokens=args.cached_tokens,require_tools=args.require_tools,at=args.at),
        [json.loads(path.read_text()) for path in args.empirical])
    report['scenario']={'uncached_input_tokens':args.input_tokens,'output_tokens':args.output_tokens,
        'cached_input_tokens':args.cached_tokens,'require_tools':args.require_tools,'at':args.at.isoformat()}
    report['catalog_observed_at_basis']='snapshot_file_mtime'
    report['catalog_sha256']=hashlib.sha256(catalog).hexdigest()
    if args.break_even_direct or args.break_even_aggregator:
        by_id={q['quote_id']:q for q in quotes}
        if args.break_even_direct not in by_id or args.break_even_aggregator not in by_id:p.error('both break-even quote IDs must exist')
        report['cache_break_even']=cache_break_even(by_id[args.break_even_direct],by_id[args.break_even_aggregator],
            total_input_tokens=args.input_tokens+args.cached_tokens,output_tokens=args.output_tokens,at=args.at)
    encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
    if args.output:args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(encoded)
    else:print(encoded,end='')
    return 0


if __name__=='__main__':raise SystemExit(main())
