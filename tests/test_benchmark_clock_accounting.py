#!/usr/bin/env python3
"""Offline regression for sleep/clock gaps, scoring eligibility and safe manifests."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
import cli_compare as base
import cli_stress as stress


class ClockAccounting(unittest.TestCase):
    def test_clock_gaps(self):
        normal=base.elapsed_timing(100, 1000, 140, 1040.001)
        self.assertTrue(normal['timing_valid'])
        slept=base.elapsed_timing(100, 1000, 140, 1808)
        self.assertEqual(slept['wall_seconds'],40)
        self.assertEqual(slept['realtime_seconds'],808)
        self.assertEqual(slept['clock_gap_seconds'],768)
        self.assertFalse(slept['timing_valid'])
        self.assertFalse(base.elapsed_timing(100,1000,140,1030)['timing_valid'])
        self.assertFalse(base.elapsed_timing(100,1000,99,1000)['timing_valid'])
        self.assertFalse(base.timing_is_valid({'wall_seconds':40}))

    def test_execute_keeps_deadline_and_completion(self):
        process=mock.Mock(returncode=0)
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            with mock.patch.object(base.subprocess,'Popen',return_value=process), \
                 mock.patch.object(base,'wait_until_deadline') as wait, \
                 mock.patch.object(base.time,'perf_counter',side_effect=[100,140]), \
                 mock.patch.object(base.time,'time',side_effect=[1000,1808]):
                result=base.execute(['fixture'],p,{},123,p/'stdout',p/'stderr')
        wait.assert_called_once_with(process,123,100,1000)
        self.assertEqual(result['exit_code'],0)
        self.assertFalse(result['timeout'])
        self.assertFalse(result['timing_valid'])
        self.assertEqual(result['clock_gap_seconds'],768)

    def test_resume_crosses_deadline_without_waiting_out_active_time(self):
        process=mock.Mock(args=['fixture'])
        process.wait.side_effect=base.subprocess.TimeoutExpired(['fixture'],1)
        with mock.patch.object(base.time,'perf_counter',side_effect=[100,101]), \
             mock.patch.object(base.time,'time',side_effect=[1000,1808]):
            with self.assertRaises(base.subprocess.TimeoutExpired):
                base.wait_until_deadline(process,90,100,1000)
        process.wait.assert_called_once_with(timeout=1.0)

    def test_deadline_retries_recheck_clocks(self):
        process=mock.Mock(args=['fixture'])
        process.wait.side_effect=[base.subprocess.TimeoutExpired(['fixture'],1),None]
        with mock.patch.object(base.time,'perf_counter',side_effect=[100,101]), \
             mock.patch.object(base.time,'time',side_effect=[1000,1001]):
            base.wait_until_deadline(process,90,100,1000)
        self.assertEqual(process.wait.call_args_list,
                         [mock.call(timeout=1.0),mock.call(timeout=1.0)])

    def test_real_subprocess_input_survives_retry_and_deadline_still_kills(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            payload='x'*200000
            result=base.execute([sys.executable,'-c',
                'import sys,time; time.sleep(1.1); print(len(sys.stdin.read()))'],
                p,os.environ.copy(),5,p/'stdout',p/'stderr',payload)
            self.assertFalse(result['timeout'])
            self.assertEqual(result['exit_code'],0)
            self.assertEqual((p/'stdout').read_text().strip(),str(len(payload)))
            result=base.execute([sys.executable,'-c','import time; time.sleep(10)'],
                p,os.environ.copy(),0.1,p/'stdout',p/'stderr')
            self.assertTrue(result['timeout'])
            self.assertLess(result['wall_seconds'],3)

    def test_invalid_pairs_retained_but_not_scored(self):
        def row(variant,round_index,seconds,valid):
            return {'run_id':f'{variant}-{round_index}','variant':variant,'task':'fixture',
                    'round':round_index,'wall_seconds':seconds,'timing_valid':valid,
                    'correct':True,'passed':True,'timeout':False}
        rows=[row('dsco_codex',1,1,False),row('codex_matched',1,100,True),
              row('dsco_codex',2,20,True),row('codex_matched',2,100,True)]
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            summary=stress.summarize_results(p,rows,['dsco_codex','codex_matched'],180)
            pairs=json.loads((p/'paired-ratios.json').read_text())
            self.assertEqual(summary['dsco_codex']['passed'],2)
            self.assertEqual(summary['dsco_codex']['correct'],2)
            self.assertEqual(summary['dsco_codex']['successful_p50_seconds'],20)
            self.assertEqual(summary['dsco_codex']['target_met'],1)
            self.assertEqual(summary['dsco_codex']['target_comparisons'],1)
            self.assertEqual(summary['dsco_codex']['retained_pair_observations'],2)
            invalid=pairs[0]
            self.assertTrue(invalid['both_completed_and_correct'])
            self.assertFalse(invalid['target_met'])
            self.assertIsNone(invalid['observed_elapsed_ratio'])
            self.assertFalse(invalid['comparable'])
            all_invalid=stress.summarize_results(p,[rows[0]],['dsco_codex'],180)
            self.assertIsNone(all_invalid['dsco_codex']['successful_p50_seconds'])
            self.assertIsNone(all_invalid['dsco_codex']['p50_seconds'])

    def test_environment_whitelist(self):
        controls=base.environment_controls({'DSCO_CHATGPT_GLOBAL_GATE':'0',
            'DSCO_CHATGPT_GATE_MAX_WAIT_MS':'0','DSCO_CHATGPT_TRACE':'1',
            'DSCO_PROFILE':'worker','DSCO_CHEAP':'1',
            'OPENAI_API_KEY':'secret','CLAUDE_CODE_OAUTH_TOKEN':'secret','PATH':'secret'})
        self.assertEqual(set(controls),set(base.ENVIRONMENT_CONTROL_KEYS))
        self.assertNotIn('secret',json.dumps(controls))


if __name__=='__main__':
    unittest.main()
