#!/usr/bin/env python3
"""Exercise the real launcher and real OS/network boundaries, without HTTP mocks."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/local_coder.py'


class LocalCoderLauncherTests(unittest.TestCase):
    def run_cli(self, *args, env=None, cwd=None):
        child = os.environ.copy()
        child.pop('DSCO_CODER_ENDPOINT', None)
        if env:
            child.update(env)
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              text=True, capture_output=True, env=child,
                              cwd=cwd, timeout=12)

    def test_help(self):
        result = self.run_cli('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--check', result.stdout)

    def test_prompt_required(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn('Supply a coding prompt', result.stderr)

    def test_non_loopback_rejected(self):
        result = self.run_cli('--check', '--endpoint', 'http://example.invalid/v1')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('Use a loopback HTTP endpoint', result.stderr)

    def test_endpoint_credentials_rejected_without_echo(self):
        result = self.run_cli('--check', '--endpoint',
                              'http://user:not-a-real-password@127.0.0.1:18080/v1')
        self.assertEqual(result.returncode, 2)
        self.assertNotIn('not-a-real-password', result.stderr)

    def test_endpoint_query_rejected(self):
        result = self.run_cli('--check', '--endpoint', 'http://127.0.0.1:18080/v1?x=1')
        self.assertEqual(result.returncode, 2)

    def test_environment_endpoint_also_validated(self):
        result = self.run_cli('--check', env={'DSCO_CODER_ENDPOINT': 'http://example.invalid/v1'})
        self.assertEqual(result.returncode, 2)

    def test_nonexistent_working_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli('--check', '--directory', str(Path(directory) / 'absent'))
        self.assertEqual(result.returncode, 2)
        self.assertIn('must be an existing directory', result.stderr)

    def test_real_connection_refusal(self):
        # Reserve a real local port without listening, so another service cannot race us.
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            base = f'http://127.0.0.1:{sock.getsockname()[1]}/v1'
            result = self.run_cli('--check', '--endpoint', base)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(json.loads(result.stderr)['ready'])
        self.assertEqual(result.stdout, '')

    @unittest.skipUnless(os.environ.get('DSCO_TEST_LOCAL_CODER') == '1',
                         'Opt in to the actual running model service')
    def test_live_service_from_another_directory(self):
        base = os.environ.get('DSCO_TEST_CODER_ENDPOINT', 'http://127.0.0.1:18080/v1')
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli('--check', '--endpoint', base, cwd=directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        status = json.loads(result.stdout)
        self.assertTrue(status['ready'])
        self.assertFalse(status['hosted_fallbacks'])
        self.assertEqual(status['provider'], 'mlx')
        self.assertIn('Qwen3-Coder-30B', status['model'])
        self.assertGreater(status['pinned_bytes'], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
