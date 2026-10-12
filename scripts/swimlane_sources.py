#!/usr/bin/env python3
"""Read-only provider/model capability evidence for every saved DSCO swimlane.

No inference, routing, entitlement, pricing-unit conversion, or context-limit promotion.
Use --refresh to GET configured provider catalogs; failed feeds remain unknown.
"""
import argparse
import collections
import datetime as dt
import hashlib
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ROUTES = ROOT / 'reports/model-labs/current/routes.jsonl'
DEFAULT_OUT = ROOT / 'reports/model-labs/capability-sources/latest.json'
CODEX_CACHE = Path.home() / '.dsco/codex_models.json'
MAX_AGE_SECONDS = 86400


def read(path):
    return json.loads(Path(path).read_text())


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def positive(value):
    # Never interpret booleans, strings, fractional prices or absurd sentinels as limits.
    return value if type(value) is int and 0 < value <= 16000000 else None


def items(doc):
    value = doc if isinstance(doc, list) else (doc.get('data') or doc.get('models') or [])
    return value if isinstance(value, list) else []


def capability(model):
    # Names are vendor-specific. Report the observed field; don't equate input-only
    # limits, aggregate context, and the larger experimental ceiling.
    for key in ('context_window', 'context_length', 'max_input_tokens'):
        context = positive(model.get(key))
        if context is not None:
            break
    else:
        context, key = None, None
    for output_key in ('max_output_tokens', 'max_completion_tokens', 'max_tokens'):
        output = positive(model.get(output_key))
        if output is not None:
            break
    else:
        output, output_key = None, None
    parameters = model.get('supported_parameters')
    tools = ('tools' in parameters) if isinstance(parameters, list) and all(isinstance(p, str) for p in parameters) else None
    return {'reported_context_tokens': context, 'context_field': key,
            'reported_max_context_tokens': positive(model.get('max_context_window')),
            'reported_output_tokens': output, 'output_field': output_key,
            'supports_experimental_context': model.get('supports_experimental_context') if type(model.get('supports_experimental_context')) is bool else None,
            'tools_advertised': tools}


def catalogs(inventory_dir, codex_cache=CODEX_CACHE, now=None, max_age=MAX_AGE_SECONDS):
    """Only successful, readable feeds. Preserve failure and conflicting-ID states."""
    inventory_dir = Path(inventory_dir)
    now = now or dt.datetime.now(dt.timezone.utc)
    result = {}
    for row in read(inventory_dir / 'inventory.json'):
        provider = row['provider']
        state = {'status': row.get('status', 'unknown'), 'scope': row.get('catalog_auth'),
                 'catalog_url': row.get('catalog_url'), 'models': {}, 'sha256': None,
                 'observed_utc': row.get('observed_at'), 'source': None}
        path = inventory_dir / (provider + '-models.json')
        if state['status'] == 'catalog_ok':
            if path.is_file():
                try:
                    state['models'] = index_models(items(read(path)))
                    state['sha256'], state['source'] = digest(path), str(path)
                    stamp = dt.datetime.fromisoformat(row['observed_at'].replace('Z', '+00:00'))
                    age = (now - stamp).total_seconds()
                    if stamp.tzinfo is None or not 0 <= age <= max_age:
                        state['status'] = 'stale_catalog'
                except (KeyError, TypeError, ValueError, OSError, json.JSONDecodeError):
                    state['status'] = 'invalid_catalog_file'
            else:
                state['status'] = 'missing_catalog_file'
        result[provider] = state
    # Codex OAuth is not an API-key /models endpoint. Native DSCO refreshes this
    # account-scoped catalog separately; a public HTTP 401 must not hide it.
    path = Path(codex_cache) if codex_cache else None
    if path and path.is_file():
        observed = dt.datetime.fromtimestamp(path.stat().st_mtime, dt.timezone.utc)
        state = result.setdefault('openai-codex', {'status': 'no_catalog', 'models': {}})
        if 0 <= (now - observed).total_seconds() <= max_age:
            try:
                state.update(status='local_account_catalog', scope='local_native_cache',
                             models=index_models(items(read(path))), source=str(path),
                             sha256=digest(path), observed_utc=observed.isoformat())
            except (ValueError, OSError, json.JSONDecodeError):
                state.update(status='invalid_native_cache', models={})
        elif state['status'] != 'catalog_ok':
            state.update(status='stale_native_cache', models={})
    return result


def index_models(models):
    found = {}
    for model in models:
        if not isinstance(model, dict):
            continue
        name = model.get('id') or model.get('slug') or model.get('name')
        if not isinstance(name, str) or not name or len(name) > 150:
            continue
        cap = capability(model)
        if name in found and found[name] != cap:
            found[name] = None  # Conflicting variants must not silently win.
        elif name not in found:
            found[name] = cap
    return found


def reconcile(routes, sources, observed_utc):
    rows = []
    for route in routes:
        name, model = route['provider'], route['model']
        source = sources.get(name, {})
        status = source.get('status', 'no_provider_catalog')
        exact = source.get('models', {}).get(model) if status in ('catalog_ok', 'local_account_catalog') else None
        if status not in ('catalog_ok', 'local_account_catalog'):
            match = 'feed_unavailable'
        elif model not in source['models']:
            match = 'model_not_in_successful_listing'
        elif exact is None:
            match = 'conflicting_catalog_variants'
        else:
            match = 'exact_provider_model'
        endpoint = bool(route.get('endpoint_tag') or route.get('hosting_provider') or route.get('quantization'))
        # A provider-model listing is NOT proof of a host/quantization/endpoint cap.
        scoped = 'provider_model_not_endpoint_verified' if endpoint and match == 'exact_provider_model' else match
        rows.append({'lane_id': route['lane_id'], 'provider': name, 'model': model,
                     'endpoint_tag': route.get('endpoint_tag'), 'match': scoped,
                     'provider_status': status, 'scope': source.get('scope'),
                     'source_sha256': source.get('sha256') if exact is not None else None,
                     'source_observed_utc': source.get('observed_utc'),
                     'declared': exact, 'verified_context_tokens': None,
                     'historical_context_tokens': route.get('context_tokens')})
    if len({r['lane_id'] for r in rows}) != len(rows):
        raise ValueError('Duplicate lane IDs: refusing incomplete reconciliation')
    # Persist the bounded, structured projection so the evidence is inspectable
    # even after the temporary GET files are removed. Raw vendor prose is omitted.
    projections = {k: dict(sorted(v.get('models', {}).items())) for k, v in sorted(sources.items())}
    saved_pairs = {(r['provider'], r['model']) for r in rows}
    # New catalog IDs are candidates, not fabricated executable product lanes.
    unmatched = [{'provider': provider, 'model': model, 'declared': cap,
                  'source_sha256': sources[provider].get('sha256'),
                  'source_observed_utc': sources[provider].get('observed_utc')}
                 for provider, models in projections.items()
                 if sources[provider]['status'] in ('catalog_ok', 'local_account_catalog')
                 for model, cap in models.items()
                 if (provider, model) not in saved_pairs and cap is not None]
    return {'schema': 'dsco.swimlane_sources.v1', 'observed_utc': observed_utc,
            'summary': {'lanes': len(rows), 'providers': len({r['provider'] for r in rows}),
                        'unregistered_catalog_models': len(unmatched),
                        'match': dict(collections.Counter(r['match'] for r in rows)),
                        'provider_status': dict(collections.Counter(k + ':' + v['status'] for k, v in sources.items()))},
            'unregistered_catalog_models': unmatched,
            'sources': {k: {**{f: v.get(f) for f in ('status', 'scope', 'catalog_url', 'sha256', 'observed_utc')},
                            'model_declarations_sha256': hashlib.sha256(json.dumps(projections[k], sort_keys=True).encode()).hexdigest(),
                            'model_declarations': projections[k]}
                        for k, v in sorted(sources.items())},
            'routes': rows,
            'limitations': ['Catalog limits are declarations, not verified context or account entitlement.',
                            'Endpoint-specific limits remain unknown without endpoint evidence.',
                            'Missing/failed feeds are unknown, never retirement or zero context.',
                            'No observed capability is automatically installed in model routing.']}


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix='.swimlane-', delete=False) as handle:
        temp = Path(handle.name)
        try:
            json.dump(value, handle, indent=2)
            handle.write('\n')
            handle.flush()
            import os
            os.fsync(handle.fileno())
            temp.replace(path)
        finally:
            temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh', action='store_true', help='GET current provider catalogs (no inference)')
    parser.add_argument('--inventory', type=Path, help='Saved inventory directory, for offline replay/tests')
    parser.add_argument('--routes', type=Path, default=ROUTES)
    parser.add_argument('--codex-cache', type=Path, default=CODEX_CACHE)
    parser.add_argument('--output', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--provider', help='Inspect saved evidence without a network request')
    parser.add_argument('--model', help='Exact model ID (requires --provider)')
    parser.add_argument('--lane-id', help='Inspect one saved swimlane ID')
    parser.add_argument('--limit', type=int, default=5, help='Maximum matching routes printed (1..50)')
    args = parser.parse_args()
    if args.provider or args.lane_id:
        if args.refresh or args.inventory or not 1 <= args.limit <= 50 or (args.model and not args.provider):
            parser.error('Queries use --output only and --limit must be 1..50')
        report = read(args.output)
        selected = [r for r in report['routes'] if (not args.provider or r['provider'] == args.provider)
                    and (not args.model or r['model'] == args.model)
                    and (not args.lane_id or r['lane_id'] == args.lane_id)]
        candidates = [r for r in report['unregistered_catalog_models']
                      if (not args.provider or r['provider'] == args.provider)
                      and (not args.model or r['model'] == args.model)] if not args.lane_id else []
        provider_source = report['sources'].get(args.provider, {}) if args.provider else {}
        print(json.dumps({'matching_lanes': len(selected), 'routes': selected[:args.limit],
                          'unregistered_models': len(candidates), 'candidates': candidates[:args.limit],
                          'provider_source': {k: v for k, v in provider_source.items() if k != 'model_declarations'}}))
        return
    if args.model or not 1 <= args.limit <= 50 or args.refresh == bool(args.inventory):
        parser.error('Choose --refresh, --inventory, or a --provider/--lane-id query')
    def build(base):
        now = dt.datetime.now(dt.timezone.utc)
        sources = catalogs(base, args.codex_cache, now=now)
        with args.routes.open() as stream:
            routes = [json.loads(line) for line in stream]
        report = reconcile(routes, sources, now.isoformat())
        atomic_json(args.output, report)
        saved = read(args.output)
        if saved['summary']['lanes'] != len(routes):
            raise RuntimeError('Written overlay lost lanes')
        print(json.dumps({'output': str(args.output), 'summary': saved['summary']}))
    if args.refresh:
        from model_lab_watch import refresh
        with tempfile.TemporaryDirectory(prefix='dsco-swimlane-sources-') as tmp:
            refresh(Path(tmp))
            build(Path(tmp))
    else:
        build(args.inventory)


if __name__ == '__main__':
    main()
