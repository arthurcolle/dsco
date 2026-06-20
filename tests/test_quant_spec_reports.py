import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from quant_spec_reports import (
    build_edge_screener_report,
    build_quant_spec_bundle,
    cases_to_jsonable,
    load_cases,
    sample_speculative_cases,
    write_quant_spec_reports,
)


AS_OF = "2026-04-10T12:00:00Z"


class QuantSpecReportTests(unittest.TestCase):
    def test_edge_screener_ranks_trade_candidates(self):
        cases = sample_speculative_cases()
        report = build_edge_screener_report(cases, as_of=AS_OF)

        self.assertEqual(report["summary"]["case_count"], len(cases))
        self.assertGreaterEqual(report["summary"]["trade_candidates"], 3)
        self.assertEqual(report["rows"][0]["decision"], "TRADE_CANDIDATE")
        self.assertGreater(report["rows"][0]["net_edge"], 0.09)
        self.assertEqual(report["rows"][0]["spread_regime"], "normal")
        self.assertEqual(report["assumptions"]["data_status"], "synthetic_sample")

    def test_bundle_contains_scenario_tail_liquidity_and_decay_reports(self):
        bundle = build_quant_spec_bundle(sample_speculative_cases(), as_of=AS_OF)
        reports = bundle["reports"]

        self.assertEqual(bundle["case_count"], 7)
        self.assertIn("scenario_ladder", reports)
        self.assertIn("tail_risk", reports)
        self.assertIn("liquidity_execution", reports)
        self.assertIn("signal_decay", reports)
        self.assertIn("monte_carlo", reports)
        self.assertIn("correlation_allocation", reports)
        self.assertIn("market_disagreement", reports)
        self.assertIn("weather_sensitivity", reports)
        self.assertLess(reports["tail_risk"]["summary"]["worst_pnl"], 0)
        self.assertGreater(reports["liquidity_execution"]["summary"]["avg_fill_probability"], 0)
        self.assertGreaterEqual(reports["signal_decay"]["summary"]["wait_for_confirmation"], 1)
        self.assertGreater(reports["monte_carlo"]["summary"]["iterations"], 1000)
        self.assertGreater(reports["monte_carlo"]["summary"]["loss_probability"], 0)
        self.assertEqual(
            sum(item["count"] for item in reports["monte_carlo"]["histogram"]),
            reports["monte_carlo"]["summary"]["iterations"],
        )
        self.assertGreater(reports["correlation_allocation"]["summary"]["effective_bets"], 1)
        self.assertGreaterEqual(reports["market_disagreement"]["summary"]["dislocation_count"], 1)
        self.assertEqual(reports["weather_sensitivity"]["summary"]["highest_pin_market"], "KXHIGHNY-26APR11-T75")

    def test_write_reports_and_load_case_input(self):
        cases = sample_speculative_cases()
        with tempfile.TemporaryDirectory() as tmpdir:
            input_path = Path(tmpdir) / "cases.json"
            input_path.write_text(json.dumps({"cases": cases_to_jsonable(cases)}), encoding="utf-8")
            loaded = load_cases(input_path)
            self.assertEqual(len(loaded), len(cases))

            bundle = build_quant_spec_bundle(loaded, as_of=AS_OF)
            manifest = write_quant_spec_reports(bundle, Path(tmpdir) / "reports")

            self.assertIn("index.md", manifest["created"])
            self.assertIn("edge_screener.csv", manifest["created"])
            self.assertIn("monte_carlo.md", manifest["created"])
            self.assertIn("correlation_allocation.csv", manifest["created"])
            self.assertIn("bundle.json", manifest["created"])
            self.assertIn("cases.json", manifest["created"])
            index = (Path(tmpdir) / "reports" / "index.md").read_text(encoding="utf-8")
            self.assertIn("not live market data", index)
            self.assertIn("Monte Carlo PnL", index)
            csv_text = (Path(tmpdir) / "reports" / "edge_screener.csv").read_text(encoding="utf-8")
            self.assertIn("KXHIGHCHI-26APR11-T68", csv_text)


if __name__ == "__main__":
    unittest.main()
