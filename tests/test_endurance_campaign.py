#!/usr/bin/env python3
"""Deterministic campaign and process failure tests; never contacts a provider."""
import contextlib
import fcntl
import io
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
import endurance_campaign as ec
import harness_matrix as hm


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source, self.out = self.root / 'manifest.json', self.root / 'out'
        self.manifest = {'schema': hm.SCHEMA, 'name': 'fault-test',
            'defaults': {'timeout_seconds': 1, 'max_cost_usd': 0.25},
            'adapters': [{'id': 'a', 'kind': 'dsco_native', 'binary': sys.executable,
                'provider': 'openai-codex', 'model': 'test-model', 'effort': 'low',
                'env': {'DSCO_DISABLE_DEFAULT_FALLBACKS': '1', 'DSCO_AUTO_FALLBACK': '0',
                    'DSCO_DYNAMIC_FAILOVER': '0', 'DSCO_DISABLE_PROVIDER_FABRIC_AUTO': '1',
                    'DSCO_ENV_FILE': '/dev/null'}}],
            'tasks': [{'id': 'task', 'prompt': 'test', 'graders': [
                {'id': 'artifact', 'type': 'file_exists', 'path': 'done'}]}]}
        self.source.write_text(json.dumps(self.manifest))
        self.policy = dict(max_attempts=4, reference_allowance_usd=1.0,
            max_active_seconds=1000, interval_seconds=0, failure_limit=2,
            max_backoff_seconds=1, subscription_only=True)
        self.calls, self.good = 0, True
        self.receipts = [{'provider': 'openai-codex', 'actual_model': 'test-model',
                         'subscription_included': True, 'budget_accounted_usd': 0.001}]

    def fake_attempt(self, manifest, source, out, task, adapter, repeat, *rest):
        self.calls += 1
        result = {'attempt_id': hm.attempt_id(manifest['name'], task['id'], adapter['id'], repeat),
            'adapter_id': adapter['id'], 'task_id': task['id'], 'status': 'completed',
            'verified': self.good, 'effect_state': 'confirmed', 'native_receipts': self.receipts}
        hm.atomic_json(out / 'state' / (result['attempt_id'] + '.json'), result)
        return result

    def run_campaign(self, runner=None):
        with patch.object(hm, 'run_attempt', side_effect=runner or self.fake_attempt), contextlib.redirect_stdout(io.StringIO()):
            return ec.campaign(self.source, self.out, self.policy)

    def state(self):
        return json.loads((self.out / 'campaign.state.json').read_text())

    def test_complete_resume_does_not_replay_or_refund(self):
        self.assertEqual(self.run_campaign(), 0)
        self.assertEqual(self.run_campaign(), 0)
        self.assertEqual(self.calls, 4)
        self.assertEqual(self.state()['reserved_reference_usd'], 1)

    def test_allowance_reserved_before_each_call(self):
        self.policy['reference_allowance_usd'] = 0.49
        self.assertEqual(self.run_campaign(), 2)
        self.assertEqual(self.calls, 1)
        self.assertEqual(self.state()['reason'], 'reference_allowance_exhausted')

    def test_failure_circuit_breaker(self):
        self.good = False
        self.run_campaign()
        self.assertEqual(self.calls, 2)
        self.assertEqual(self.state()['reason'], 'consecutive_verification_failures')

    def test_missing_receipts_stop(self):
        self.receipts = []
        self.run_campaign()
        self.assertEqual(self.calls, 1)
        self.assertEqual(self.state()['reason'], 'missing_native_receipts')

    def test_subscription_fallback_stops(self):
        self.receipts[0]['subscription_included'] = False
        self.run_campaign()
        self.assertEqual(self.calls, 1)
        self.assertEqual(self.state()['reason'], 'non_subscription_receipt')

    def test_unknown_cost_is_not_zero(self):
        self.receipts[0]['budget_accounted_usd'] = None
        self.run_campaign()
        self.assertEqual(self.calls, 1)
        self.assertEqual(self.state()['reason'], 'unknown_cost_accounting')

    def test_route_change_stops(self):
        self.receipts[0]['actual_model'] = 'other'
        self.run_campaign()
        self.assertEqual(self.state()['reason'], 'provider_or_model_changed')

    def test_uncertain_crash_never_replayed(self):
        def crash(*args):
            self.calls += 1
            raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.run_campaign(crash)
        self.assertEqual(self.state()['reserved_reference_usd'], 0.25)
        self.run_campaign()
        self.assertEqual(self.calls, 1)
        self.assertEqual(self.state()['reason'], 'effect_unknown_requires_inspection')

    def test_committed_result_reconciles_after_parent_crash(self):
        def crash_after_commit(*args):
            self.fake_attempt(*args)
            raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.run_campaign(crash_after_commit)
        self.assertEqual(self.run_campaign(), 0)
        self.assertEqual(self.calls, 4)
        self.assertEqual(self.state()['reserved_reference_usd'], 1)

    def test_single_writer(self):
        self.out.mkdir()
        with (self.out / 'campaign.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(hm.ManifestError, 'another campaign'):
                self.run_campaign()
        self.assertEqual(self.calls, 0)

    def test_modified_manifest_rejected(self):
        self.run_campaign()
        self.source.write_text(self.source.read_text() + '\n')
        with self.assertRaisesRegex(hm.ManifestError, 'inputs changed'):
            self.run_campaign()
        self.assertEqual(self.calls, 4)

    def test_time_admission_does_not_launch(self):
        self.policy['max_active_seconds'] = 1
        self.run_campaign()
        self.assertEqual(self.calls, 0)
        self.assertEqual(self.state()['reason'], 'insufficient_time_for_next_attempt')

    def test_invalid_budget_override_cannot_disable_limits(self):
        self.manifest['adapters'][0]['budgets'] = {'max_cost_usd': -1}
        self.source.write_text(json.dumps(self.manifest))
        with self.assertRaises(hm.ManifestError):
            self.run_campaign()
        self.assertEqual(self.calls, 0)

    def test_nonfinite_allowance_rejected(self):
        with self.assertRaises(hm.ManifestError):
            hm.require_positive(float('nan'), 'budget', 1000)

    def test_summary_preserves_unknown_cost(self):
        self.receipts[0]['budget_accounted_usd'] = None
        self.run_campaign()
        summary = hm.summarize(self.out, self.manifest)['adapters']['a']
        self.assertIsNone(summary['budget_accounted_usd'])
        self.assertFalse(summary['cost_accounting_complete'])


class ProcessTests(unittest.TestCase):
    def run_process(self, code, stdin=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            return hm.execute([sys.executable, '-c', code], root, dict(os.environ),
                0.25, 1024, root/'stdout', root/'stderr', stdin)

    def test_wall_clock_rollback_cannot_extend_timeout(self):
        with patch.object(hm.time, 'time', side_effect=[10000, 0]):
            result = self.run_process('import time; time.sleep(20)')
        self.assertTrue(result['timed_out'])
        self.assertLess(result['wall_seconds'], 4)

    def test_unread_large_stdin_cannot_block_deadline(self):
        result = self.run_process('import time; time.sleep(20)', 'x' * 2_000_000)
        self.assertTrue(result['timed_out'])
        self.assertLess(result['wall_seconds'], 4)

    def test_fast_output_flood_is_failure(self):
        result = self.run_process('print("x" * 10000)')
        self.assertTrue(result['output_limit_exceeded'])

    def test_controller_interrupt_reaps_child(self):
        import subprocess
        children = []
        original = subprocess.Popen
        def remember(*args, **kwargs):
            child = original(*args, **kwargs)
            children.append(child)
            return child
        with patch.object(hm.subprocess, 'Popen', side_effect=remember), \
                patch.object(hm, 'process_group_rss_kib', side_effect=KeyboardInterrupt()):
            with self.assertRaises(KeyboardInterrupt):
                self.run_process('import time; time.sleep(20)')
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())

    def test_descendant_ignoring_term_is_killed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pidfile = root/'child.pid'
            child = 'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)'
            code = f'import subprocess,time,pathlib; p=subprocess.Popen([{sys.executable!r}, "-c", {child!r}]); pathlib.Path({str(pidfile)!r}).write_text(str(p.pid)); time.sleep(30)'
            hm.execute([sys.executable, '-c', code], root, dict(os.environ), .4, 1024, root/'out', root/'err')
            pid = int(pidfile.read_text())
            for _ in range(30):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return
                time.sleep(.05)
            self.fail('descendant survived timeout')


if __name__ == '__main__':
    unittest.main()
