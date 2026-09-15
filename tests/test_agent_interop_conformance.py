#!/usr/bin/env python3
"""Deterministic failure-classification checks for interop receipts."""

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.agent_interop_conformance import invocation_failure  # noqa: E402


class InteropConformanceTests(unittest.TestCase):
    def test_oauth_expiry_is_an_external_authentication_blocker(self):
        output = (
            '{"error":"authentication_failed",'
            '"result":"Failed to authenticate: OAuth session expired and could not be refreshed"}'
        )
        self.assertEqual(
            invocation_failure(output, "", {}),
            ("authentication", True, "OAuth session expired and could not be refreshed"),
        )

    def test_rate_limit_is_external(self):
        self.assertEqual(
            invocation_failure("provider rate limit reached", "", {}),
            ("rate_limit", True, "provider rate limit reached"),
        )

    def test_timeout_takes_precedence(self):
        self.assertEqual(
            invocation_failure("authentication_failed", "", {"timed_out": True}),
            ("timeout", False, "harness invocation timed out"),
        )

    def test_jsonl_result_becomes_diagnostic(self):
        output = '\n'.join([
            '{"type":"system","subtype":"init"}',
            '{"type":"result","result":"specific child failure"}',
        ])
        self.assertEqual(
            invocation_failure(output, "", {}),
            ("child_exit", False, "specific child failure"),
        )


if __name__ == "__main__":
    unittest.main()
