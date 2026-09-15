#!/usr/bin/env python3
"""Offline MCP regressions. Run with a freshly built dsco path as argv[1]."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 else './dsco').resolve())


class ReviewRegressions(unittest.TestCase):
    def test_core_python_and_verified_value(self):
        with tempfile.TemporaryDirectory(prefix='dsco-review-regression-') as tmp:
            root = Path(tmp)
            run = root / 'runs' / 'fixture'
            run.mkdir(parents=True)
            with (run / 'journal.wal').open('wb') as wal:
                for passed, price, cost in [(True, 10, 2), (False, 1000, 3)]:
                    frame = json.dumps({
                        'schema': 'value.receipt.v1',
                        'verification': {'passed': passed},
                        'economics': {'price_usd': price, 'compute_cost_usd': cost,
                                      'human_minutes': 30},
                        'autonomous': False, 'reuse': {},
                        'risk': {'incident': False},
                        'performance': {'recovery_ms': 0.000},
                    }, separators=(',', ':')).encode()
                    # Summary reads Chronicle's length/checksum frame header.
                    wal.write(struct.pack('<II', len(frame), 0) + frame)
            env = {'PATH': os.environ['PATH'], 'HOME': tmp,
                   'DSCO_RUNS_DIR': str(root / 'runs'),
                   'DSCO_ENV_FILE': '/dev/null', 'DSCO_NO_KEYCHAIN': '1',
                   'DSCO_MCP_AUTOCONNECT': '0', 'DSCO_GOV_MODEL': 'standard',
                   'DSCO_NO_WORKSPACE_BOOTSTRAP': '1'}
            requests = [
                {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list'},
                {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                 'params': {'name': 'value_ledger',
                            'arguments': {'action': 'summary', 'run_id': 'fixture'}}},
            ]
            proc = subprocess.run([BINARY, 'mcp', 'serve', '--tier', 'trusted'],
                                  input=''.join(json.dumps(r) + '\n' for r in requests),
                                  text=True, capture_output=True, cwd=tmp, env=env, timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            replies = {r['id']: r for line in proc.stdout.splitlines()
                       if line.startswith('{') for r in [json.loads(line)] if 'id' in r}
            self.assertIn('dsco-python-3x', {t['name'] for t in replies[1]['result']['tools']})
            self.assertFalse(replies[2]['result'].get('isError', False), replies[2])
            summary = json.loads(replies[2]['result']['content'][0]['text'])
            self.assertEqual(summary['receipts'], 2)
            self.assertEqual(summary['verified'], 1)
            self.assertEqual(summary['verified_work_value_usd'], 10)
            self.assertEqual(summary['compute_cost_usd'], 5)
            self.assertEqual(summary['gross_compute_margin'], 0.5)
            self.assertEqual(summary['human_leverage_usd_per_hr'], 10)


if __name__ == '__main__':
    unittest.main()
