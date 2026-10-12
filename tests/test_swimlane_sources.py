import datetime as dt
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import swimlane_sources as s


def saved(base, provider, models, status='catalog_ok'):
    base.mkdir(parents=True, exist_ok=True)
    (base / 'inventory.json').write_text(json.dumps([{
        'provider': provider, 'status': status, 'catalog_auth': 'credential',
        'catalog_url': 'https://example.invalid/models',
        'observed_at': dt.datetime.now(dt.timezone.utc).isoformat()}]))
    if status == 'catalog_ok':
        (base / (provider + '-models.json')).write_text(json.dumps({'data': models}))


def route(lane, provider='openrouter', model='vendor/model', **extra):
    return {'lane_id': lane, 'provider': provider, 'model': model,
            'context_tokens': 111111, **extra}


def test_exact_join_never_promotes_endpoint_or_old_context(tmp_path):
    base = tmp_path / 'catalog'
    saved(base, 'openrouter', [{'id': 'vendor/model', 'context_length': 400000,
                                'top_provider': {'max_completion_tokens': 30000},
                                'supported_parameters': ['tools']}])
    sources = s.catalogs(base, codex_cache=None)
    report = s.reconcile([route('a'), route('b', endpoint_tag='host/quant'),
                          route('c', model='unknown')], sources, 'now')
    assert report['summary']['lanes'] == 3
    a, b, c = report['routes']
    assert a['match'] == 'exact_provider_model'
    assert a['declared']['reported_context_tokens'] == 400000
    assert a['declared']['tools_advertised'] is True
    assert a['verified_context_tokens'] is None
    assert b['match'] == 'provider_model_not_endpoint_verified'
    assert c['match'] == 'model_not_in_successful_listing'
    assert c['historical_context_tokens'] == 111111 and c['declared'] is None
    assert report['summary']['unregistered_catalog_models'] == 0
    projection = report['sources']['openrouter']['model_declarations']
    assert report['sources']['openrouter']['model_declarations_sha256'] == hashlib.sha256(
        json.dumps(projection, sort_keys=True).encode()).hexdigest()


def test_failure_never_retires_and_bad_values_never_inflate(tmp_path):
    base = tmp_path / 'catalog'
    saved(base, 'openrouter', [], status='HTTP_401')
    report = s.reconcile([route('a')], s.catalogs(base, codex_cache=None), 'now')
    assert report['routes'][0]['match'] == 'feed_unavailable'
    assert report['routes'][0]['declared'] is None
    assert s.positive(True) is None and s.positive(99999999) is None
    assert s.capability({'context_window': '1000000', 'max_context_window': False})['reported_context_tokens'] is None
    saved(base, 'openrouter', [{'id': 'vendor/model', 'context_length': 200000}])
    old = s.catalogs(base, codex_cache=None, now=dt.datetime.now(dt.timezone.utc) + dt.timedelta(days=2))
    assert old['openrouter']['status'] == 'stale_catalog'
    assert s.reconcile([route('a')], old, 'later')['routes'][0]['match'] == 'feed_unavailable'


def test_native_cache_is_account_scoped_and_default_not_max(tmp_path):
    base = tmp_path / 'catalog'
    saved(base, 'openai-codex', [], status='HTTP_401')
    cache = tmp_path / 'codex.json'
    cache.write_text(json.dumps({'models': [{'slug': 'gpt-6-sol', 'context_window': 272000,
                                            'max_context_window': 872000,
                                            'supports_experimental_context': False}]}))
    now = dt.datetime.now(dt.timezone.utc)
    sources = s.catalogs(base, cache, now=now)
    route_a = s.reconcile([route('a', 'openai-codex', 'gpt-6-sol')], sources, 'now')['routes'][0]
    assert route_a['match'] == 'exact_provider_model'
    assert route_a['provider_status'] == 'local_account_catalog'
    assert route_a['declared']['reported_context_tokens'] == 272000
    assert route_a['declared']['reported_max_context_tokens'] == 872000
    assert route_a['declared']['supports_experimental_context'] is False
    assert route_a['verified_context_tokens'] is None
    new = s.reconcile([route('a', 'openai-codex', 'older-model')], sources, 'now')
    assert new['summary']['unregistered_catalog_models'] == 1
    assert new['unregistered_catalog_models'][0]['model'] == 'gpt-6-sol'
    assert 'lane_id' not in new['unregistered_catalog_models'][0]
    old = s.catalogs(base, cache, now=now + dt.timedelta(days=3))
    assert s.reconcile([route('a', 'openai-codex', 'gpt-6-sol')], old, 'now')['routes'][0]['match'] == 'feed_unavailable'


def test_query_is_bounded_and_does_not_refresh(tmp_path, monkeypatch, capsys):
    report = {'routes': [route('a', 'openai-codex', 'old')],
              'unregistered_catalog_models': [{'provider': 'openai-codex', 'model': 'gpt-6-sol'}],
              'sources': {'openai-codex': {'status': 'local_account_catalog', 'model_declarations': {'gpt-6-sol': {}}}}}
    path = tmp_path / 'latest.json'
    s.atomic_json(path, report)
    monkeypatch.setattr(sys, 'argv', ['swimlane_sources.py', '--provider', 'openai-codex',
                                      '--model', 'gpt-6-sol', '--output', str(path)])
    s.main()
    result = json.loads(capsys.readouterr().out)
    assert result['matching_lanes'] == 0
    assert result['unregistered_models'] == 1
    assert 'model_declarations' not in result['provider_source']


def test_conflicting_variants_and_duplicate_lanes_fail_closed(tmp_path):
    base = tmp_path / 'catalog'
    saved(base, 'openrouter', [{'id': 'vendor/model', 'context_length': 500000},
                                {'id': 'vendor/model', 'context_length': 1000000}])
    sources = s.catalogs(base, codex_cache=None)
    report = s.reconcile([route('a')], sources, 'now')
    assert report['routes'][0]['match'] == 'conflicting_catalog_variants'
    with pytest.raises(ValueError, match='Duplicate lane'):
        s.reconcile([route('a'), route('a')], sources, 'now')
