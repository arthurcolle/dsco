#!/usr/bin/env python3
"""Bounded native provider qualification from a credential-free candidate manifest."""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.request
import urllib.error


def utc(value):
    return datetime.datetime.fromtimestamp(value, datetime.timezone.utc).isoformat().replace('+00:00', 'Z')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidates', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--planner-repo', type=Path, default=Path('/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction'))
    parser.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1] / 'dsco')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    payload = json.loads(args.candidates.read_text())
    candidates = payload if isinstance(payload, list) else payload.get('candidates', payload.get('lanes', payload.get('rows')))
    if not isinstance(candidates, list) or not 1 <= len(candidates) <= 40:
        raise SystemExit('Expected 1..40 provider/model candidates')
    allowed_env = {'DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY', 'DSCO_CLAUDE_CODE_OAUTH_TOKEN', 'CLAUDE_CODE_OAUTH_TOKEN'}
    for candidate in candidates:
        if not isinstance(candidate, dict) or not candidate.get('provider') or not candidate.get('model'):
            raise SystemExit('Every candidate needs provider and model')
        upstream = candidate.get('upstream')
        if upstream is not None and (candidate['provider'] != 'openrouter' or not isinstance(upstream, str) or not upstream.strip() or ',' in upstream or len(upstream) > 160):
            raise SystemExit('upstream must be one OpenRouter provider slug')
        for key, value in candidate.get('env', {}).items():
            if key not in allowed_env or value not in ('', '0', '1'):
                raise SystemExit('Candidate env only permits credential-free auth selection flags')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    started = time.time()
    sys.path.insert(0, str(args.planner_repo))
    from cognitive_agent.executor import execute_one
    from cognitive_agent.planning import PlannerStore
    manifest = {'schema': 'dsco.qualification_run.v1', 'workload': 'integer-json-v1', 'started_at': utc(started),
                'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'candidates': candidates, 'max_concurrent': 4, 'timeout_s': 30, 'lease_s': 45,
                'per_attempt_budget_usd': .05, 'reserved_ceiling_usd': len(candidates) * .05,
                'strict_validation': True, 'candidate_source': str(args.candidates.resolve())}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    receipts = []

    def run(index, candidate):
        folder = out / f'lane-{index:02d}'
        folder.mkdir()
        numbers = [index % 11 + 2, 6, 1, index % 7 + 3]
        expected = {'sorted': sorted(numbers), 'checksum': sum(numbers)}
        with PlannerStore(folder / 'planner.sqlite') as store:
            store.add_goal('qualification', f'For the integer list {numbers}, return exactly one standalone JSON object with sorted (ascending list) and checksum (sum of the four original integer values). No prose, code fences, commands, or outer array.', acceptance=['standalone JSON object with exact values'], max_attempts=1)

            def verify(goal, output):
                try:
                    actual = json.loads(output.strip())
                except ValueError:
                    return None
                if actual != expected:
                    return None
                return {goal['acceptance'][0]: 'Whole-response JSON parse and exact object equality passed'}

            receipt = execute_one(store, binary=str(binary), provider=candidate['provider'], model=candidate['model'],
                                  effort=candidate.get('effort', 'low'), worker=f'provider-qualification-{index}',
                                  artifacts=folder / 'workers', verify=verify, timeout=30, budget=.05, cwd=str(root),
                                  env={**os.environ, **candidate.get('env', {}),
                                       **({'DSCO_OR_PROVIDER_ONLY': candidate['upstream'], 'DSCO_OR_ALLOW_FALLBACKS': '0'} if candidate.get('upstream') else {})})
        receipt['upstream'] = candidate.get('upstream')
        receipt['quantization'] = candidate.get('quantization')
        receipt['catalog_source'] = candidate.get('source')
        receipt['upstream_evidence'] = 'request_pin_only' if candidate.get('upstream') else 'native_provider'
        if candidate.get('upstream') and os.environ.get('OPENROUTER_API_KEY'):
            request_ids = [c.get('request_id') for c in receipt.get('cost_records', []) if str(c.get('request_id', '')).startswith('gen-')]
            for request_id in dict.fromkeys(request_ids):
                request = urllib.request.Request('https://openrouter.ai/api/v1/generation?id=' + request_id,
                    headers={'Authorization': 'Bearer ' + os.environ['OPENROUTER_API_KEY']})
                try:
                    with urllib.request.urlopen(request, timeout=5) as response:
                        metadata = json.load(response)
                    (folder / 'generation-metadata.json').write_text(json.dumps(metadata, indent=2))
                    reported = metadata.get('data', {}).get('provider_name')
                    if reported:
                        receipt['reported_upstream'] = reported
                        receipt['upstream_evidence'] = 'openrouter_generation_metadata'
                except Exception as exc:
                    receipt['generation_metadata_error'] = {'type': type(exc).__name__, 'http_status': getattr(exc, 'code', None)}

        receipt['lane_index'] = index
        receipt['expected'] = expected
        (folder / 'result.json').write_text(json.dumps(receipt, indent=2))
        print(json.dumps({'lane': index, 'provider': candidate['provider'], 'model': candidate['model'],
                          'status': receipt['status'], 'elapsed_s': receipt['elapsed_s'],
                          'known_accounted_usd': receipt['known_accounted_usd'],
                          'incomplete_cost_receipt': receipt['incomplete_cost_receipt']}), flush=True)
        return receipt

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run, i, candidate) for i, candidate in enumerate(candidates)]
        for future in concurrent.futures.as_completed(futures):
            receipts.append(future.result())
            (out / 'receipts.json').write_text(json.dumps(receipts, indent=2))
    receipts.sort(key=lambda r: r['lane_index'])
    rows = []
    for receipt in receipts:
        index = receipt['lane_index']
        candidate = candidates[index]
        rows.append({'provider': candidate['provider'], 'upstream': candidate.get('upstream'), 'model': candidate['model'], 'effort': candidate.get('effort', 'low'),
                     'attempts': 1, 'successes': int(receipt['status'] == 'verified'),
                     'mean_latency_s': receipt['elapsed_s'], 'unpriced_attempts': int(bool(receipt['incomplete_cost_receipt'])),
                     'strict_validation': True, 'observed_at': utc(receipt['started_at'] + receipt['elapsed_s']),
                     'upstream_evidence': receipt.get('upstream_evidence'), 'reported_upstream': receipt.get('reported_upstream'),
                     'quantization': candidate.get('quantization'), 'catalog_source': candidate.get('source'),
                     'sources': [str((out / f'lane-{index:02d}' / 'result.json').relative_to(root))]})
    evidence = {'schema': 'dsco.route_evidence.v1', 'workload': 'integer-json-v1', 'generated_at': utc(time.time()), 'rows': rows}
    (out / 'route-evidence.json').write_text(json.dumps(evidence, indent=2))
    (out / 'receipts.json').write_text(json.dumps(receipts, indent=2))
    costs = [cost for receipt in receipts for cost in receipt.get('cost_records', [])]
    (out / 'cost-records.json').write_text(json.dumps(costs, indent=2))
    pids = {receipt['pid'] for receipt in receipts if receipt.get('pid')}
    survivors = []
    for line in subprocess.check_output(['ps', '-axo', 'pid=,ppid=,pgid=,comm='], text=True).splitlines():
        parts = line.strip().split(None, 3)
        if len(parts) == 4 and (int(parts[0]) in pids or int(parts[2]) in pids):
            survivors.append(line.strip())
    summary = {'duration_s': time.time() - started, 'attempts': len(receipts),
               'successes': sum(receipt['status'] == 'verified' for receipt in receipts),
               'known_accounted_usd': sum(receipt['known_accounted_usd'] or 0 for receipt in receipts),
               'incomplete_receipts': sum(bool(receipt['incomplete_cost_receipt']) for receipt in receipts),
               'reserved_ceiling_usd': len(candidates) * .05, 'surviving_owned_pids_or_groups': survivors, 'rows': rows}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary), flush=True)
    return int(bool(survivors))


if __name__ == '__main__':
    raise SystemExit(main())
