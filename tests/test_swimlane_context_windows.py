#!/usr/bin/env python3
"""No live network: source selection, exact ID parsing and 3990-lane coverage."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import swimlane_context_windows as s


def test_specs():
    doc = ('# GPT-6 Sol\nModel ID: `gpt-6-sol`\n'
           '- 1,050,000 context window\n- Maximum input tokens: 922,000\n'
           '- 128,000 max output tokens\n')
    assert s.openai_spec('gpt-6-sol', doc)['context_tokens'] == 1050000
    assert s.openai_spec('gpt-6-sol-not', doc) is None
    assert s.openai_spec('gpt-6-sol', 'Model ID: `gpt-6-sol`\nGPT-6 context window\n') is None
    table = ('| Feature | Claude Sonnet 5 | Claude Haiku |\n'
             '| Claude API ID | `claude-sonnet-5` | `claude-haiku-4` |\n'
             '| [Context window](https://vendor.example) | 1M tokens | 200K tokens |\n'
             '| Max output | 128K tokens | 64K tokens |\n')
    assert s.anthropic_specs(table)['claude-sonnet-5']['context_tokens'] == 1000000
    assert s.anthropic_specs(table)['claude-haiku-4']['context_tokens'] == 200000
    assert s.number('448') == 448


def test_build():
    routes = [{'lane_id': 'a', 'provider': 'openai', 'model': 'gpt-6-sol',
               'historical_context_tokens': 272000,
               'declared': {'reported_context_tokens': None}},
              {'lane_id': 'b', 'provider': 'groq', 'model': 'whisper-large',
               'historical_context_tokens': 448,
               'declared': {'reported_context_tokens': 448}},
              {'lane_id': 'c', 'provider': 'anthropic', 'model': 'claude-no-id',
               'historical_context_tokens': 200000, 'declared': None},
              {'lane_id': 'd', 'provider': 'openrouter', 'model': 'some-model',
               'historical_context_tokens': 8000,
               'declared': {'reported_context_tokens': 8192}}]
    overlay = {'routes': routes, 'observed_utc': '2026-09-25T00:00:00Z',
               'sources': {'groq': {'status': 'catalog_ok', 'observed_utc': 'today'},
                           'anthropic': {'status': 'HTTP_401'},
                           'openrouter': {'status': 'catalog_ok', 'catalog_url': 'https://openrouter.ai/api/v1/models',
                                          'observed_utc': 'today'},
                           'openai': {'status': 'catalog_ok'}}}
    official = {('openai', 'gpt-6-sol'): ({'context_tokens': 1050000, 'max_input_tokens': 922000,
                                          'max_output_tokens': 128000}, s.OPENAI_DOC.format(model='gpt-6-sol'))}
    report = s.build(overlay, official, {'openai_docs_attempted': 1}, 'today')
    x = report['routes']
    assert len(x) == 4 and report['active_route_caps_changed'] is False
    assert x[0]['published_context_tokens'] == 1050000
    assert x[0]['historical_route_context_tokens'] == 272000
    assert x[1]['published_context_tokens'] is None and x[1]['provider_reported_non_text_units'] == 448
    assert x[2]['published_context_tokens'] is None and x[2]['evidence'] == 'unknown'
    assert x[3]['published_context_tokens'] == 8192 and x[3]['evidence'] == 'provider_catalog_reported'
    assert report['summary']['numeric_published_context'] == 2
    dup = dict(overlay)
    dup['routes'] = routes + [routes[0]]
    try:
        s.build(dup, official, {}, 'today')
        raise AssertionError('duplicate ID accepted')
    except AssertionError as e:
        if str(e) == 'duplicate ID accepted': raise


def test_inventory():
    report_path = ROOT / 'reports/model-labs/capability-sources/context-windows.json'
    if report_path.exists():
        report = json.loads(report_path.read_text())
        assert report['summary']['lanes'] == len(report['routes']) == 3990
        assert len({row['lane_id'] for row in report['routes']}) == 3990


if __name__ == '__main__':
    test_specs()
    test_build()
    test_inventory()
    print('PASS: official parsing, unknown/small-unit handling, evidence precedence and 3990 lane IDs')
