#!/usr/bin/env python3
"""Run a paced, resumable DSCO evaluation campaign with explicit resource limits.

The reference allowance reserves each attempt's full configured cost budget. It
is not a provider invoice or a guaranteed billing cap. Subscription receipts are
required by default; an uncertain attempt is never automatically replayed.
"""
import argparse
import fcntl
import json
import math
import signal
import sys
import time
from pathlib import Path

import harness_matrix as hm


def seal_inputs(manifest, source):
    files = {str(source): hm.sha256_file(source),
             str(Path(__file__).resolve()): hm.sha256_file(Path(__file__)),
             str(Path(hm.__file__).resolve()): hm.sha256_file(Path(hm.__file__))}
    for adapter in manifest['adapters']:
        binary = hm.resolve_binary(adapter['binary'], source.parent)
        files[str(binary)] = hm.sha256_file(binary)
    fixtures = {}
    for task in manifest['tasks']:
        if task.get('fixture'):
            fixture = (source.parent / task['fixture']).resolve()
            fixtures[str(fixture)] = hm.workspace_snapshot(fixture)['sha256']
        for grader in task.get('graders', []):
            for arg in grader.get('argv', []):
                path = Path(arg)
                if path.is_absolute() and path.is_file():
                    files[str(path)] = hm.sha256_file(path)
    return {'files': files, 'fixtures': fixtures}


def check_seal(seal):
    for path, digest in seal['files'].items():
        if not Path(path).is_file() or hm.sha256_file(Path(path)) != digest:
            raise hm.ManifestError('campaign input changed: ' + path)
    for path, digest in seal['fixtures'].items():
        if hm.workspace_snapshot(Path(path))['sha256'] != digest:
            raise hm.ManifestError('campaign fixture changed: ' + path)


def receipt_problem(result, adapter, subscription_only):
    receipts = result.get('native_receipts', [])
    if not receipts:
        return 'missing_native_receipts'
    for receipt in receipts:
        if (receipt.get('provider') != adapter['provider'] or
                receipt.get('actual_model') != adapter['model']):
            return 'provider_or_model_changed'
        if subscription_only and receipt.get('subscription_included') is not True:
            return 'non_subscription_receipt'
        value = receipt.get('budget_accounted_usd')
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
            return 'unknown_cost_accounting'
    return None


def campaign(source, out, policy):
    manifest = hm.load_manifest(source)
    matrix = [(task, adapter) for task in manifest['tasks']
              for adapter in manifest['adapters'] if adapter.get('enabled', True)
              and (not task.get('adapters') or adapter['id'] in task['adapters'])]
    if not matrix or any(a['kind'] != 'dsco_native' or not t.get('graders') for t, a in matrix):
        raise hm.ManifestError('campaign requires native DSCO adapters and independent graders')
    if manifest.get('fleet_mesh'):
        raise hm.ManifestError('endurance campaigns currently require local adapters')
    if policy['subscription_only']:
        for _, adapter in matrix:
            flags = adapter.get('env', {})
            required = {'DSCO_DISABLE_DEFAULT_FALLBACKS': '1', 'DSCO_AUTO_FALLBACK': '0',
                        'DSCO_DYNAMIC_FAILOVER': '0', 'DSCO_DISABLE_PROVIDER_FABRIC_AUTO': '1',
                        'DSCO_ENV_FILE': '/dev/null'}
            if any(flags.get(k) != v for k, v in required.items()):
                raise hm.ManifestError('subscription campaign requires explicit fallback disabling and isolated env file')
    out.mkdir(parents=True, exist_ok=True)
    with (out / 'campaign.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise hm.ManifestError('another campaign process owns this output directory') from exc
        plan_path, state_path = out / 'campaign.plan.json', out / 'campaign.state.json'
        plan = {'policy': policy, 'inputs': seal_inputs(manifest, source)}
        if plan_path.exists():
            if json.loads(plan_path.read_text()) != plan:
                raise hm.ManifestError('campaign configuration or inputs changed; use a new output directory')
            if not state_path.exists():
                raise hm.ManifestError('campaign state missing; refusing to reset resource accounting')
        else:
            hm.atomic_json(state_path, {'attempts': [], 'reserved_reference_usd': 0.0,
                'consecutive_failures': 0, 'active': None, 'active_seconds': 0.0,
                'next_due_unix': time.time(), 'status': 'running'})
            hm.atomic_json(plan_path, plan)
            hm.atomic_json(out / 'manifest.json', json.loads(source.read_text()))
        state = json.loads(state_path.read_text())
        base_elapsed, started = state['active_seconds'], time.monotonic()

        def save():
            state['active_seconds'] = base_elapsed + time.monotonic() - started
            state['heartbeat_unix'] = time.time()
            hm.atomic_json(state_path, state)

        def stop(reason):
            state.update(status='stopped', reason=reason)
            save()

        def finish(result, adapter):
            problem = receipt_problem(result, adapter, policy['subscription_only'])
            if result.get('effect_state') == 'unknown' or result.get('status') == 'effect_unknown':
                problem = 'effect_unknown_requires_inspection'
            verified = result.get('verified') is True
            state['attempts'].append({'attempt_id': result['attempt_id'],
                                      'verified': verified, 'receipt_problem': problem})
            state['active'] = None
            state['consecutive_failures'] = 0 if verified else state['consecutive_failures'] + 1
            if problem:
                stop(problem)
            elif state['consecutive_failures'] >= policy['failure_limit']:
                stop('consecutive_verification_failures')
            else:
                delay = policy['interval_seconds'] * 2 ** min(state['consecutive_failures'], 10)
                state['next_due_unix'] = time.time() + min(delay, policy['max_backoff_seconds'])
                save()
            print(json.dumps({'attempt_id': result['attempt_id'], 'verified': verified,
                              'receipt_problem': problem, 'reserved_reference_usd': state['reserved_reference_usd']}), flush=True)

        try:
            if state['active']:
                pending = out / 'state' / (state['active'] + '.json')
                result = json.loads(pending.read_text()) if pending.exists() else {}
                if result.get('status') == 'completed':
                    finish(result, matrix[len(state['attempts']) % len(matrix)][1])
                else:
                    stop('effect_unknown_requires_inspection')
            # Resume never reopens a terminal campaign or drops its reservations.
            delay = min(max(0, state['next_due_unix'] - time.time()), policy['max_backoff_seconds'])
            next_due = time.monotonic() + delay
            while state['status'] == 'running':
                count = len(state['attempts'])
                if count >= policy['max_attempts']:
                    state.update(status='completed', reason='attempt_limit_reached')
                    break
                if base_elapsed + time.monotonic() - started >= policy['max_active_seconds']:
                    stop('active_time_limit_reached')
                    break
                if time.monotonic() < next_due:
                    save()
                    time.sleep(min(10, next_due - time.monotonic(), max(0.01,
                        policy['max_active_seconds'] - state['active_seconds'])))
                    continue
                check_seal(plan['inputs'])
                task, adapter = matrix[count % len(matrix)]
                repeat = count // len(matrix) + 1
                budget = hm.merged_budgets(manifest['defaults'], adapter, task)
                remaining = policy['max_active_seconds'] - (base_elapsed + time.monotonic() - started)
                # Reserve enough time for the entire attempt and its graders.
                timeout = budget['timeout_seconds'] + 5 + sum(
                    float(g.get('timeout_seconds', budget['grader_timeout_seconds'])) + 5
                    for g in task['graders'] if g['type'] == 'command')
                if timeout > remaining:
                    stop('insufficient_time_for_next_attempt')
                    break
                reserved = state['reserved_reference_usd'] + budget['max_cost_usd']
                if reserved > policy['reference_allowance_usd'] + 1e-9:
                    stop('reference_allowance_exhausted')
                    break
                state['reserved_reference_usd'] = round(reserved, 8)
                state['active'] = hm.attempt_id(manifest['name'], task['id'], adapter['id'], repeat)
                save()  # Reserve before invoking a model; crashes cannot refund this.
                result = hm.run_attempt(manifest, source, out, task, adapter, repeat, False, None)
                finish(result, adapter)
                hm.summarize(out, manifest)
                next_due = time.monotonic() + max(0, state['next_due_unix'] - time.time())
        except hm.ManifestError as exc:
            stop('configuration_error: ' + str(exc))
            raise
        finally:
            save()
        hm.summarize(out, manifest)
        print(json.dumps(state, indent=2))
        return 0 if state['status'] == 'completed' and all(a['verified'] for a in state['attempts']) else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-attempts', type=int, required=True)
    parser.add_argument('--reference-allowance-usd', type=float, required=True)
    parser.add_argument('--max-active-seconds', type=float, required=True)
    parser.add_argument('--interval-seconds', type=float, default=300)
    parser.add_argument('--failure-limit', type=int, default=2)
    parser.add_argument('--max-backoff-seconds', type=float, default=3600)
    parser.add_argument('--allow-metered', action='store_true')
    args = parser.parse_args()
    policy = {k: v for k, v in vars(args).items() if k not in ('manifest', 'output', 'allow_metered')}
    policy['subscription_only'] = not args.allow_metered
    if any(not math.isfinite(v) or v <= 0 for k, v in policy.items()
           if k != 'subscription_only' and k != 'interval_seconds') or not math.isfinite(args.interval_seconds) or args.interval_seconds < 0:
        parser.error('limits must be positive finite numbers; interval may be zero')
    if args.max_backoff_seconds < args.interval_seconds:
        parser.error('max-backoff-seconds must be at least interval-seconds')
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        return campaign(args.manifest.expanduser().resolve(), args.output.expanduser().resolve(), policy)
    except (hm.ManifestError, OSError, ValueError) as exc:
        print(json.dumps({'error': type(exc).__name__, 'detail': str(exc)}), file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('Campaign interrupted; durable active attempt requires inspection before any replay.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
