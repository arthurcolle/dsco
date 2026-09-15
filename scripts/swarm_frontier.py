#!/usr/bin/env python3
"""Summarize matched live swarm trials; never equate missing cost with zero."""
import argparse
import json
from pathlib import Path
import re

ANSI = re.compile(r'\x1b\[[0-9;?]*[ -/]*[@-~]')


def verified_output(output):
    clean = ANSI.sub('', output)
    lines = []
    for line in clean.splitlines():
        line = re.sub(r'^\s*[│┃]\s*\d+\s*[│┃]\s?', '', line)
        lines.append(line)
    text = '\n'.join(lines)
    decoder = json.JSONDecoder()
    for match in re.finditer(r'\{', text):
        try:
            value, _ = decoder.raw_decode(text[match.start():])
        except ValueError:
            continue
        if isinstance(value, dict) and value.get('ready') == ['B', 'A'] and value.get('applied') == ['s1', 's2']:
            return True
    return False


def nondominated(rows):
    eligible = [r for r in rows if r['successes'] and r['known_cost_usd'] is not None]
    return [r['lane_id'] for r in eligible if not any(
        s is not r and s['cost_per_verified_usd'] <= r['cost_per_verified_usd']
        and s['mean_latency_s'] <= r['mean_latency_s']
        and s['success_rate'] >= r['success_rate']
        and (s['cost_per_verified_usd'] < r['cost_per_verified_usd']
             or s['mean_latency_s'] < r['mean_latency_s']
             or s['success_rate'] > r['success_rate']) for s in eligible)]


def summarize(directory):
    directory = Path(directory)
    groups = {}
    for path in sorted(directory.glob('execution/batch-*.json')):
        data = json.loads(path.read_text())
        for lane, worker in zip(data['lanes'], data['result'].get('results', [])):
            key = lane['lane_id']
            r = groups.setdefault(key, dict(lane, attempts=0, successes=0, costs=[], latencies=[], failures=[]))
            r['attempts'] += 1
            r['costs'].append(worker.get('budget_accounted_usd'))
            r['latencies'].append(worker.get('elapsed_sec', 0))
            success = worker.get('status') == 'done' and verified_output(worker.get('output', ''))
            r['successes'] += int(success)
            if not success:
                r['failures'].append({'status': worker.get('status'), 'exit_code': worker.get('exit_code'),
                    'output_tail': ANSI.sub('', worker.get('output', ''))[-700:]})
    results = []
    for row in groups.values():
        row['known_cost_usd'] = sum(x for x in row['costs'] if x is not None) if any(x is not None for x in row['costs']) else None
        row['unpriced_attempts'] = sum(x is None for x in row['costs'])
        row['success_rate'] = row['successes'] / row['attempts']
        row['mean_latency_s'] = sum(row['latencies']) / row['attempts']
        row['cost_per_verified_usd'] = row['known_cost_usd'] / row['successes'] if row['successes'] and row['known_cost_usd'] is not None else None
        if row['unpriced_attempts']:
            row['cost_per_verified_usd'] = None
        results.append(row)
    frontier_rows = [r for r in results if r['cost_per_verified_usd'] is not None]
    return {'schema': 'dsco.empirical_frontier.v1', 'workload': 'steering-ready-frontier',
            'limits': 'Pilot workload only, not general coding quality. Costs retain their recorded reference/reported basis; direct-provider prices may differ. Unpriced and interrupted requests are not zero. No multi-hour reliability inference from short trials.',
            'rows': results, 'pareto_lane_ids': nondominated(frontier_rows)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = summarize(args.directory)
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + '\n')
    print(text)
