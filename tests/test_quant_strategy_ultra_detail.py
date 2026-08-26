import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from quant_spec_reports import build_quant_spec_bundle, sample_speculative_cases
from quant_strategy_sim import run_strategy_simulation, write_strategy_simulation_outputs


AS_OF = "2026-04-10T12:00:00Z"
ROOT = Path(__file__).resolve().parents[1]


def _load_ultra_module():
    spec = importlib.util.spec_from_file_location(
        "generate_quant_strategy_ultra_detail",
        ROOT / "scripts" / "generate_quant_strategy_ultra_detail.py",
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class QuantStrategyUltraDetailTests(unittest.TestCase):
    def test_ultra_detail_report_and_plots(self):
        module = _load_ultra_module()
        cases = sample_speculative_cases()
        bundle = build_quant_spec_bundle(cases, as_of=AS_OF)
        result = run_strategy_simulation(
            cases,
            as_of=AS_OF,
            paths_per_horizon=300,
            chunk_size=100,
            seed=19,
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            write_strategy_simulation_outputs(result, tmpdir)
            manifest = module.write_ultra_detail(result, out_dir=tmpdir, bundle=bundle)

            self.assertIn("ultra_detail_report.md", manifest["created"])
            self.assertIn("ultra_detail/executive_decision_board.png", manifest["created"])
            self.assertIn("ultra_detail/strategy_metric_lattice.png", manifest["created"])
            self.assertIn("ultra_detail/utility_score_heatmap.png", manifest["created"])
            self.assertIn("ultra_detail/decision_contact_sheet.png", manifest["created"])
            self.assertIn("ultra_detail/ultra_detail_contact_sheet.png", manifest["created"])
            self.assertIn("ultra_detail/strategy_profile_edge_static.png", manifest["created"])
            self.assertIn("ultra_detail/all_strategy_horizon_rows.csv", manifest["created"])
            self.assertIn("ultra_detail/horizon_playbook.csv", manifest["created"])
            self.assertIn("ultra_detail/weather_monitor_list.csv", manifest["created"])

            report = (Path(tmpdir) / "ultra_detail_report.md").read_text(encoding="utf-8")
            self.assertIn("Decision Playbook", report)
            self.assertIn("Weather Monitor List", report)
            self.assertIn("All Strategy-Horizon Rows", report)
            self.assertIn("Largest Weather Exposures", report)

            with Image.open(Path(tmpdir) / "ultra_detail" / "strategy_metric_lattice.png") as image:
                self.assertGreater(image.size[0], 1500)
                self.assertGreater(image.size[1], 1800)


if __name__ == "__main__":
    unittest.main()
