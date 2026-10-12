#!/usr/bin/env python3
"""GET-only context evidence for all saved swimlanes. Never changes routing limits.

Reads reports/model-labs/capability-sources/latest.json (refreshed separately
by swimlane_sources.py --refresh); fetches exact OpenAI/Anthropic model docs;
writes one evidence row per lane. Absent provider context remains unknown, never
replaced by an old route cap. No inference or token purchases.
"""
import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
import re
import subprocess
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / 'reports/model-labs/capability-sources/latest.json'
DEFAULT_OUTPUT = ROOT / 'reports/model-labs/capability-sources/context-windows.json'
ANTHROPIC_DOC = 'https://platform.claude.com/docs/en/about-claude/models/overview.md'
OPENAI_DOC = 'https://developers.openai.com/api/docs/models/{model}.md'
VALID_ID = re.compile(r'[a-zA-Z0-9._-]{1,110}\Z')
NUMBER = re.compile(r'(\d[\d,]*)([kKmM])?\b')


def get(url):
    # OpenAI's docs CDN blocks Python urllib's fingerprint on this host; curl
    # is already the governed runtime HTTP client. No shell or credentials.
    if not url.startswith(('https://platform.claude.com/docs/en/about-claude/models/',
                           'https://developers.openai.com/api/docs/models/')):
        return None
    try:
        response = subprocess.run(['curl', '--fail', '--silent', '--show-error',
                                   '--location', '--proto', '=https', '--proto-redir', '=https',
                                   '--max-time', '8', '--max-filesize', str(512 * 1024),
                                   '-A', 'dsco-swimlane-context/1.0', url],
                                  capture_output=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if response.returncode or not response.stdout or len(response.stdout) > 512 * 1024:
        return None
    return response.stdout.decode('utf-8', errors='replace')


def number(text):
    match = NUMBER.search(text)
    if not match:
        return None
    value = int(match[1].replace(',', '')) * {'k': 1000, 'm': 1000000}.get((match[2] or '').lower(), 1)
    return value if value > 0 else None


def openai_spec(model, text):
    if f'Model ID: `{model}`' not in text:
        return None
    spec = {'context_tokens': None, 'max_input_tokens': None, 'max_output_tokens': None}
    for line in text.splitlines():
        if re.fullmatch(r'- \d[\d,]* ?(?:[kKmM])? context window', line):
            spec['context_tokens'] = number(line)
        elif re.fullmatch(r'- \d[\d,]* ?(?:[kKmM])? max output tokens', line):
            spec['max_output_tokens'] = number(line)
        elif line.startswith('- Maximum input tokens: '):
            spec['max_input_tokens'] = number(line.split(':', 1)[1])
    return spec if spec['context_tokens'] else None


def anthropic_specs(text):
    rows = {}
    for line in text.splitlines():
        if line.startswith('| Claude API ID ') or line.startswith('| [Context window]') or line.startswith('| Max output '):
            cells = [cell.strip() for cell in line.strip('|').split('|')]
            if line.startswith('| Claude API ID '): rows['id'] = cells
            elif line.startswith('| [Context window]'): rows['ctx'] = cells
            else: rows['out'] = cells
    result = {}
    ids, windows = rows.get('id', []), rows.get('ctx', [])
    for index, entry in enumerate(ids):
        match = re.fullmatch(r'`([^`]+)`', entry)
        if match and index < len(windows):
            ctx = number(windows[index])
            if ctx:
                outputs = rows.get('out', [])
                result[match[1]] = {'context_tokens': ctx, 'max_input_tokens': None,
                                    'max_output_tokens': number(outputs[index]) if index < len(outputs) else None}
    return result


def official_specs(routes, fetch=get):
    models = sorted({r['model'] for r in routes if r['provider'] in ('openai', 'openai-codex')
                     and r['model'].startswith(('gpt-', 'o1', 'o3', 'o4')) and VALID_ID.fullmatch(r['model'])})
    specs, states = {}, {'openai_docs_attempted': len(models), 'openai_docs_matched': 0,
                         'anthropic_docs': 'unavailable'}
    anthropic_text = fetch(ANTHROPIC_DOC)
    if anthropic_text:
        parsed = anthropic_specs(anthropic_text)
        specs.update({('anthropic', model): (spec, ANTHROPIC_DOC) for model, spec in parsed.items()})
        states['anthropic_docs'] = 'parsed' if parsed else 'unrecognized_format'
    def openai_one(model):
        url = OPENAI_DOC.format(model=urllib.parse.quote(model, safe=''))
        text = fetch(url)
        return model, openai_spec(model, text) if text else None, url
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for model, spec, url in pool.map(openai_one, models):
            if spec:
                for provider in ('openai', 'openai-codex'):
                    specs[(provider, model)] = spec, url
                states['openai_docs_matched'] += 1
    return specs, states


def build(overlay, official, states, observed):
    routes = overlay['routes']
    result = []
    for route in routes:
        provider, model = route['provider'], route['model']
        claimed = route.get('declared') or {}
        source = overlay['sources'].get(provider, {})
        spec = official.get((provider, model))
        if spec:
            value, url = spec
            context = value['context_tokens']
            kind = 'official_model_documentation'
            inputs, outputs = value['max_input_tokens'], value['max_output_tokens']
            fresh = observed
        elif source.get('status') in ('catalog_ok', 'local_account_catalog') and \
                isinstance(claimed.get('reported_context_tokens'), int) and \
                claimed['reported_context_tokens'] > 0:
            context = claimed['reported_context_tokens']
            kind = 'provider_catalog_reported' if source['status'] == 'catalog_ok' else 'local_account_catalog_reported'
            url = source.get('catalog_url') or source.get('source')
            inputs = None
            outputs = claimed.get('reported_output_tokens')
            fresh = source.get('observed_utc') or observed
        else:
            context, kind, url, inputs, outputs, fresh = None, 'unknown', None, None, None, None
        # Catalogs reuse 'context_window' for speech/audio frame budgets. Do
        # not advertise a sub-512 number as a language context in tokens.
        non_text_units = context if context is not None and context < 512 else None
        if non_text_units is not None:
            context, inputs, outputs, kind = None, None, None, 'non_text_context_units'
        result.append({'lane_id': route['lane_id'], 'provider': provider, 'model': model,
                       'published_context_tokens': context,
                       'provider_reported_non_text_units': non_text_units,
                       'max_input_tokens': inputs, 'max_output_tokens': outputs,
                       'evidence': kind, 'source': url, 'source_observed_utc': fresh,
                       'historical_route_context_tokens': route.get('historical_context_tokens')})
    assert len(result) == len(routes) == len({row['lane_id'] for row in result})
    summary = {'lanes': len(result), 'numeric_published_context': sum(r['published_context_tokens'] is not None for r in result),
               'unknown': sum(r['evidence'] == 'unknown' for r in result),
               'non_text_context_units': sum(r['evidence'] == 'non_text_context_units' for r in result),
               'by_evidence': {kind: sum(r['evidence'] == kind for r in result)
                               for kind in ('official_model_documentation', 'provider_catalog_reported',
                                            'local_account_catalog_reported', 'unknown', 'non_text_context_units')},
               'official_doc_fetch': states}
    return {'schema': 'dsco.swimlane_context_windows.v1', 'observed_utc': observed,
            'input_overlay_observed_utc': overlay['observed_utc'],
            'active_route_caps_changed': False, 'summary': summary, 'routes': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=DEFAULT_SOURCE)
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    overlay = json.loads(args.input.read_text())
    now = dt.datetime.now(dt.timezone.utc).isoformat()
    official, states = official_specs(overlay['routes'])
    report = build(overlay, official, states, now)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(report, indent=2, ensure_ascii=False) + '\n'
    temp = args.output.with_suffix(args.output.suffix + '.tmp')
    temp.write_text(payload)
    temp.replace(args.output)
    print(json.dumps({'path': str(args.output), 'summary': report['summary'],
                      'sha256': hashlib.sha256(payload.encode()).hexdigest()}, sort_keys=True))


if __name__ == '__main__':
    main()
