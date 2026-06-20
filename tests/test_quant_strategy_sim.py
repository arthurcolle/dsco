import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from quant_spec_reports import sample_speculative_cases
from quant_strategy_sim import (
    HORIZONS_MINUTES,
    STRATEGIES,
    WEATHER_FACTORS,
    run_strategy_simulation,
    write_strategy_simulation_outputs,
)


AS_OF = "2026-04-10T12:00:00Z"
ROOT = Path(__file__).resolve().parents[1]


def _load_sim_plot_module():
    spec = importlib.util.spec_from_file_location(
        "generate_quant_strategy_sim",
        ROOT / "scripts" / "generate_quant_strategy_sim.py",
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class QuantStrategySimulationTests(unittest.TestCase):
    def test_strategy_weather_overlay_simulation_outputs(self):
        result = run_strategy_simulation(
            sample_speculative_cases(),
            as_of=AS_OF,
            paths_per_horizon=800,
            chunk_size=200,
            seed=7,
        )

        self.assertEqual(len(result["summary_rows"]), len(HORIZONS_MINUTES) * len(STRATEGIES))
        self.assertEqual(
            result["assumptions"]["strategy_evaluations"],
            800 * len(HORIZONS_MINUTES) * len(STRATEGIES),
        )
        self.assertEqual({row["strategy"] for row in result["summary_rows"]}, set(STRATEGIES))
        self.assertEqual({row["factor"] for row in result["weather_overlay_rows"]}, set(WEATHER_FACTORS))
        self.assertEqual(
            len(result["weather_exposure_rows"]),
            len(HORIZONS_MINUTES) * len(STRATEGIES) * len(WEATHER_FACTORS),
        )
        self.assertTrue(any(row["avg_active_positions"] > 0 for row in result["summary_rows"]))

        with tempfile.TemporaryDirectory() as tmpdir:
            manifest = write_strategy_simulation_outputs(result, tmpdir)
            self.assertIn("strategy_simulation_report.md", manifest["created"])
            self.assertIn("strategy_horizon_summary.csv", manifest["created"])
            report = (Path(tmpdir) / "strategy_simulation_report.md").read_text(encoding="utf-8")
            self.assertIn("not live market data", report)
            self.assertIn("Strategy evaluations", report)

    def test_strategy_simulation_plot_generation(self):
        module = _load_sim_plot_module()
        result = run_strategy_simulation(
            sample_speculative_cases(),
            as_of=AS_OF,
            paths_per_horizon=600,
            chunk_size=150,
            seed=11,
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            write_strategy_simulation_outputs(result, tmpdir)
            manifest = module.write_strategy_simulation_plots(result, tmpdir)
            self.assertIn("strategy_simulation_plot_report.md", manifest["created"])
            self.assertIn("plots/strategy_mean_heatmap.png", manifest["created"])
            self.assertIn("plots/index.html", manifest["created"])

            image_path = Path(tmpdir) / "plots" / "strategy_mean_heatmap.png"
            with Image.open(image_path) as image:
                self.assertGreater(image.size[0], 1000)
                self.assertGreater(image.size[1], 600)

            report = (Path(tmpdir) / "strategy_simulation_plot_report.md").read_text(encoding="utf-8")
            self.assertIn("Strategy evaluations", report)
            self.assertIn("Strongest weather beta", report)


if __name__ == "__main__":
    unittest.main()
