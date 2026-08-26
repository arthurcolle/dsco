import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from quant_spec_reports import (
    build_quant_spec_bundle,
    sample_speculative_cases,
    write_quant_spec_reports,
)


AS_OF = "2026-04-10T12:00:00Z"
ROOT = Path(__file__).resolve().parents[1]


def _load_plot_module():
    spec = importlib.util.spec_from_file_location(
        "generate_quant_spec_plots",
        ROOT / "scripts" / "generate_quant_spec_plots.py",
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class QuantSpecPlotTests(unittest.TestCase):
    def test_advanced_plot_report_generation(self):
        module = _load_plot_module()
        bundle = build_quant_spec_bundle(sample_speculative_cases(), as_of=AS_OF)
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir)
            write_quant_spec_reports(bundle, report_dir)
            manifest = module.write_advanced_plots(bundle, report_dir)

            self.assertIn("advanced_plot_report.md", manifest["created"])
            self.assertIn("plots/advanced_dashboard.png", manifest["created"])
            self.assertIn("plots/index.html", manifest["created"])

            image_path = report_dir / "plots" / "advanced_dashboard.png"
            with Image.open(image_path) as image:
                self.assertGreater(image.size[0], 1200)
                self.assertGreater(image.size[1], 800)

            report = (report_dir / "advanced_plot_report.md").read_text(encoding="utf-8")
            self.assertIn("Monte Carlo mean PnL", report)
            index = (report_dir / "index.md").read_text(encoding="utf-8")
            self.assertIn("Advanced plot report", index)


if __name__ == "__main__":
    unittest.main()
