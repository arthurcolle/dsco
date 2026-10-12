#!/usr/bin/env python3
"""Offline CLI-boundary checks for scripts/test_jev.sh; no network or credentials."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/test_jev.sh"
FAKE = r'''#!/usr/bin/env python3
import json, os, sys
mode = os.environ.get("FIXTURE_MODE", "ok")
with open(os.environ["FIXTURE_CALLS"], "a") as f:
    f.write(json.dumps(sys.argv[1:]) + "\n")
if mode == "malformed":
    print("not json"); sys.exit(0)
if sys.argv[1] == "--jev-route":
    status = "model_locked" if mode == "fallback" else "selected"
    print(json.dumps({"selected_model":"fixture-model", "jev":{"status":status},
                      "completion_dispatched":False})); sys.exit(0)
assert sys.argv[1:3] == ["--tool-exec", "jev_decide"]
a = json.loads(sys.argv[3])
if mode == "denied":
    print(json.dumps({"ok":False,"result":{"ok":False,"receipt":{"status":"secrets_denied"}}}))
    sys.exit(1)
s = a["state"]
if a["kind"] == "bool":
    answer = {"decision":s != "2 + 2 = 5.", "p_yes":0.9 if s != "2 + 2 = 5." else 0.1}
else:
    choice = "strong" if s.startswith("Prove") else "abstain" if s.startswith("Do the") else "cheap"
    if mode == "wrong" and s.startswith("Extract"): choice = "strong"
    if mode == "unlisted": choice = "execute_shell"
    answer = {"choice":choice,"confidence":0.9}
answer.update(ok=True,kind=a["kind"],receipt={"status":"ok","input_tokens":100,"latency_ms":2})
if mode == "string": answer = json.dumps(answer)
print(json.dumps({"ok":True,"result":answer}))
'''


class JevScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jev-script-test-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.bin = self.base / "fake dsco"
        self.bin.write_text(FAKE)
        self.bin.chmod(0o700)
        self.calls = self.base / "calls.jsonl"
        self.out = self.base / "report with spaces"

    def run_script(self, mode="ok", args=()):
        env = dict(os.environ, DSCO_BIN=str(self.bin), FIXTURE_MODE=mode,
                   FIXTURE_CALLS=str(self.calls))
        result = subprocess.run(["bash", str(SCRIPT), "--out", str(self.out), *args],
                                env=env, cwd=self.base, capture_output=True, text=True, timeout=30)
        calls = [json.loads(x) for x in self.calls.read_text().splitlines()] if self.calls.exists() else []
        summary = json.loads((self.out / "summary.json").read_text()) if (self.out / "summary.json").exists() else None
        return result, summary, calls

    def test_suite_and_string_envelope(self):
        for mode in ("ok", "string"):
            with self.subTest(mode=mode):
                self.out = self.base / mode
                self.calls = self.base / (mode + ".calls")
                r, s, calls = self.run_script(mode)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual((s["passed"], s["attempted"], s["input_tokens"]), (6, 6, 600))
                self.assertEqual(len(calls), 6)
                self.assertFalse(s["completion_dispatched"])
                self.assertIsNone(s["cost_usd"])
                self.assertEqual(self.out.stat().st_mode & 0o777, 0o700)

    def test_wrong_answer_is_failure_not_transport_error(self):
        r, s, calls = self.run_script("wrong")
        self.assertEqual(r.returncode, 1)
        self.assertEqual((s["failed"], s["errors"], len(calls)), (1, 0, 6))

    def test_transport_and_malformed_stop_without_retry(self):
        for mode in ("denied", "malformed"):
            with self.subTest(mode=mode):
                self.out = self.base / mode
                self.calls = self.base / (mode + ".calls")
                r, s, calls = self.run_script(mode)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertEqual((s["errors"], len(calls)), (1, 1))

    def test_unlisted_choice_is_rejected(self):
        r, s, calls = self.run_script("unlisted")
        self.assertEqual(r.returncode, 2)
        self.assertEqual((s["errors"], len(calls)), (1, 3))

    def test_custom_text_is_data_and_unscored(self):
        question = 'Is "yes" correct?\nDon\'t run $(touch OWNED); `touch OWNED`.'
        r, s, calls = self.run_script(args=(question,))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(s["unscored"], 1)
        self.assertEqual(json.loads(calls[0][2])["state"], question)
        self.assertFalse((ROOT / "OWNED").exists())
        self.assertFalse((self.base / "OWNED").exists())

    def test_router_success_and_fallback(self):
        for mode, expected in (("ok", 0), ("fallback", 2)):
            with self.subTest(mode=mode):
                self.out = self.base / mode
                self.calls = self.base / (mode + ".calls")
                r, s, calls = self.run_script(mode, ("--route", "Explain a hash table."))
                self.assertEqual(r.returncode, expected, r.stderr)
                self.assertEqual(calls, [["--jev-route", "Explain a hash table."]])
                self.assertFalse(s["completion_dispatched"])

    def test_bad_args_and_existing_report_do_not_call_binary(self):
        r, _, calls = self.run_script(args=("--bogus",))
        self.assertEqual((r.returncode, calls), (2, []))
        self.out.mkdir()
        marker = self.out / "keep"
        marker.write_text("existing")
        r, _, calls = self.run_script()
        self.assertEqual((r.returncode, calls), (2, []))
        self.assertEqual(marker.read_text(), "existing")


if __name__ == "__main__":
    unittest.main()
