#!/usr/bin/env python3
"""Keep arbitrary provider trials distinct from matched-model speed claims."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
import cli_stress as stress


class ModelConfiguration(unittest.TestCase):
    def test_both_clients_receive_requested_model_and_effort(self):
        for dsco, cli, models, expected in [
            ('dsco_codex', 'codex_matched', {'codex_model': 'selected-openai-model'}, 'selected-openai-model'),
            ('dsco_anthropic', 'claude_matched', {'claude_model': 'selected-anthropic-model'}, 'selected-anthropic-model'),
        ]:
            configs = []
            for variant in (dsco, cli):
                argv = ['binary', 'exec', 'prompt'] if cli == variant and cli.startswith('codex') else ['binary', '-p', 'prompt']
                env = {}
                config = stress.configure_variant(variant, argv, env, 'low', models)
                self.assertIn(expected, argv)
                self.assertEqual(config['model'], expected)
                self.assertEqual(config['effort'], 'low')
                if variant == dsco:
                    self.assertEqual(env['DSCO_DISABLE_DEFAULT_FALLBACKS'], '1')
                    self.assertEqual(env['DSCO_AUTO_FALLBACK'], '0')
                configs.append(config)
            self.assertEqual(configs[0], configs[1])

    def test_dsco_override_keeps_reference_and_medium_variant_independent(self):
        models = {'codex_model': 'reference-model', 'dsco_provider': 'another-provider',
                  'dsco_model': 'arbitrary/model-name'}
        argv, env = ['binary', '-p', 'prompt'], {}
        dsco = stress.configure_variant('dsco_medium', argv, env, 'high', models)
        self.assertEqual((dsco['provider'], dsco['model'], dsco['effort']),
                         ('another-provider', 'arbitrary/model-name', 'medium'))
        self.assertEqual(argv.count('--effort'), 1)
        ref = stress.configure_variant('codex_matched', ['binary', 'exec', 'prompt'], {}, 'high', models)
        self.assertEqual((ref['model'], ref['effort']), ('reference-model', 'high'))

    def test_different_models_cannot_claim_matched_configuration_win(self):
        def row(variant, seconds, model):
            return {'run_id': variant, 'variant': variant, 'task': 'fixture', 'round': 1,
                    'wall_seconds': seconds, 'timing_valid': True, 'correct': True,
                    'passed': True, 'timeout': False,
                    'requested_configuration': {'provider': 'openai-codex', 'model': model, 'effort': 'high'}}
        rows = [row('dsco_codex', 10, 'fast-model'), row('codex_matched', 100, 'reference-model')]
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            result = stress.summarize_results(out, rows, ['dsco_codex', 'codex_matched'], 180)
            pair = json.loads((out / 'paired-ratios.json').read_text())[0]
            self.assertTrue(pair['target_met'])
            self.assertFalse(pair['harness_target_met'])
            self.assertEqual(result['dsco_codex']['matched_configuration_target_met'], 0)
            rows[1]['requested_configuration']['model'] = 'fast-model'
            result = stress.summarize_results(out, rows, ['dsco_codex', 'codex_matched'], 180)
            self.assertEqual(result['dsco_codex']['matched_configuration_target_met'], 1)

    def test_partial_provider_override_fails_before_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'must-not-be-created'
            for flag in ('--dsco-provider', '--dsco-model'):
                run = subprocess.run([sys.executable, str(Path(stress.__file__)), '--output', str(out),
                                      flag, 'incomplete'], capture_output=True, text=True)
                self.assertEqual(run.returncode, 2)
                self.assertIn('must be supplied together', run.stderr)
                self.assertFalse(out.exists())

    def test_invalid_clock_stops_new_task_scheduling(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'trial'
            row = {'run_id': 'invalid', 'variant': 'dsco', 'task': 'aggregate_records', 'round': 1,
                   'wall_seconds': 10, 'timing_valid': False, 'correct': True, 'passed': True, 'timeout': False}
            with mock.patch.object(sys, 'argv', ['cli_stress.py', '--output', str(out), '--variants', 'dsco',
                                                '--tasks', 'aggregate_records', 'intervals', '--rounds', '2']), \
                 mock.patch.object(stress, 'one_run', return_value=row) as run, \
                 mock.patch('builtins.print'):
                stress.main()
            self.assertEqual(run.call_count, 1)
            self.assertEqual(len(json.loads((out / 'results.json').read_text())), 1)
            self.assertTrue((out / 'interrupted.json').exists())


if __name__ == '__main__':
    unittest.main()
