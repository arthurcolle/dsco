import math
import sqlite3
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

import calibration


class CalibrationFoundationTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

        self._orig_calib_dir = calibration.CALIB_DIR
        self._orig_calib_db = calibration.CALIB_DB

        calibration.CALIB_DIR = Path(self._tmp.name) / "calibration"
        calibration.CALIB_DB = calibration.CALIB_DIR / "calibration.db"
        calibration._ensure_db()

        self.addCleanup(self._restore_globals)

    def _restore_globals(self):
        calibration.CALIB_DIR = self._orig_calib_dir
        calibration.CALIB_DB = self._orig_calib_db

    def _seed_emos_verification(self, city="nyc", start=None, days=35):
        start = start or datetime(2026, 3, 1, tzinfo=timezone.utc)
        for idx in range(days):
            day = start + timedelta(days=idx)
            obs = 65.0 + (idx % 7) * 1.3 + math.sin(idx / 3.0)
            calibration.record_verification(
                city,
                day,
                {
                    "hrrr": {"high_f": obs + 1.2, "low_f": obs - 9.0, "cycle": 12, "lead_hours": 12.0},
                    "gfs": {"high_f": obs - 1.4, "low_f": obs - 11.0, "cycle": 0, "lead_hours": 24.0},
                    "nam": {"high_f": obs + 0.4, "low_f": obs - 10.0, "cycle": 6, "lead_hours": 18.0},
                },
                obs_high=obs,
                obs_low=obs - 10.0,
            )

    def test_station_profile_overrides_and_mapping_validation(self):
        mismatches = calibration.validate_station_mappings()
        self.assertEqual(mismatches, [])

        nyc = calibration.get_station_profile("nyc", datetime(2026, 4, 10, tzinfo=timezone.utc))
        self.assertEqual(nyc["settlement_icao"], "KNYC")
        self.assertEqual(nyc["observation_icao"], "KNYC")
        self.assertEqual(nyc["observation_source"], "Central Park")
        self.assertEqual(nyc["fallback_order"][0], "station_obs")

        chi = calibration.get_station_profile("chi", datetime(2026, 4, 10, tzinfo=timezone.utc))
        self.assertEqual(chi["settlement_icao"], "KMDW")
        self.assertEqual(chi["observation_icao"], "KMDW")

    def test_calibrate_city_persists_last_good_and_reuses_it(self):
        target = datetime(2026, 4, 10, 12, tzinfo=timezone.utc)
        result = calibration.calibrate_city(
            "nyc",
            {"hrrr": 72.0, "gfs": 74.0, "nam": 73.0},
            target,
        )

        self.assertIn("why", result)
        self.assertIn("confidence", result)
        self.assertEqual(result["station_profile"]["observation_source"], "Central Park")
        self.assertEqual(result["parameter_set"], "day")

        fallback = calibration.calibrate_city("nyc", {}, target)
        self.assertEqual(fallback["source"], "last_good_calibration")
        self.assertIn("last_good_calibration", fallback["fallback_reason_codes"])
        self.assertIn("why", fallback)

    def test_schema_checksum_report_and_validation_helpers(self):
        checksum_1 = calibration.compute_payload_checksum({"a": 1, "b": 2})
        checksum_2 = calibration.compute_payload_checksum({"b": 2, "a": 1})
        self.assertEqual(checksum_1, checksum_2)

        schema_errors = calibration.validate_external_schema({"station": "KNYC"}, ["station", "time"])
        self.assertEqual(schema_errors, ["missing_required_field:time"])

        report = calibration.build_calibration_report_stub("nyc", datetime(2026, 4, 10, tzinfo=timezone.utc))
        self.assertEqual(report["metrics"]["crps"], None)
        self.assertEqual(report["target_date"], "2026-04-10")

        validation_errors = calibration.validate_calibration_inputs({"hrrr": None, "gfs": 71.0})
        self.assertTrue(any("missing mandatory" in err for err in validation_errors))

    def test_missing_data_report_and_dedupe_helpers(self):
        conn = sqlite3.connect(str(calibration.CALIB_DB))
        conn.execute("""
            INSERT INTO verification
            (city, date, model, cycle, fcst_high_f, fcst_low_f,
             obs_high_f, obs_low_f, bias_high, bias_low, abs_err_high, abs_err_low, lead_hours)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, ("nyc", "2026-04-10", "hrrr", 0, 71.0, None, None, None, None, None, None, None, 12.0))
        conn.commit()
        conn.close()

        report = calibration.build_missing_data_daily_report("2026-04-10", "nyc")
        self.assertEqual(report["date"], "2026-04-10")
        self.assertEqual(report["cities"][0]["missing_obs"], 1)

        duplicates = calibration.detect_duplicate_verification_rows("nyc", "2026-04-10")
        self.assertEqual(duplicates, [])
        self.assertEqual(calibration.dedupe_verification_rows("nyc"), 0)

    def test_drift_alert_and_circuit_breaker(self):
        alert = calibration.calibration_drift_alert("crps", 13.0, 10.0, threshold_pct=20.0)
        self.assertTrue(alert["alert"])
        self.assertEqual(alert["reason"], "metric_drift")

        breaker = calibration.IngestionCircuitBreaker(failure_threshold=2, reset_after_seconds=3600)
        self.assertTrue(breaker.allow())
        self.assertTrue(breaker.record_failure())
        self.assertFalse(breaker.record_failure())
        self.assertFalse(breaker.allow())
        breaker.record_success()
        self.assertTrue(breaker.allow())

    def test_persist_raw_payload_snapshot(self):
        snapshot = calibration.persist_raw_payload_snapshot(
            "aviationweather",
            {"station": "KNYC", "temp_f": 71.2},
            city_key="nyc",
        )
        self.assertIn("checksum", snapshot)
        self.assertEqual(snapshot["city"], "nyc")

        conn = sqlite3.connect(str(calibration.CALIB_DB))
        row = conn.execute(
            "SELECT payload_json FROM calibration_runs WHERE city = ? ORDER BY id DESC LIMIT 1",
            ("nyc",),
        ).fetchone()
        conn.close()
        self.assertIsNotNone(row)

    def test_rolling_and_season_matched_emos_training_entrypoints(self):
        self._seed_emos_verification()
        target = datetime(2026, 4, 10, tzinfo=timezone.utc)

        rolling = calibration.train_emos_rolling(
            "nyc",
            target_date=target,
            window_days=90,
            deploy=True,
        )
        self.assertTrue(rolling["deployed"])
        self.assertEqual(rolling["training_mode"], "rolling")
        self.assertGreaterEqual(rolling["n_train"], 30)
        self.assertTrue(rolling["stability"]["ok"])

        season = calibration.train_emos_season_matched(
            "nyc",
            target_date=target,
            lookback_years=1,
            deploy=False,
        )
        self.assertFalse(season["deployed"])
        self.assertEqual(season["training_mode"], "season_matched")
        self.assertEqual(season["season"], calibration._season(target))
        self.assertGreaterEqual(season["n_train"], 30)

        conn = sqlite3.connect(str(calibration.CALIB_DB))
        params = conn.execute(
            "SELECT n_train, crps_train FROM emos_params WHERE city = 'nyc' AND param = 'high'"
        ).fetchone()
        run_count = conn.execute("SELECT COUNT(*) FROM emos_training_runs").fetchone()[0]
        conn.close()
        self.assertIsNotNone(params)
        self.assertGreaterEqual(params[0], 30)
        self.assertGreater(run_count, 0)

    def test_shrinkage_tuning_stability_gate_and_pit_histogram(self):
        self._seed_emos_verification()
        target = datetime(2026, 4, 10, tzinfo=timezone.utc)
        calibration.train_emos_rolling("nyc", target_date=target, window_days=90, deploy=True)

        shrink = calibration.tune_pooled_station_shrinkage(
            "nyc",
            target_date=target,
            window_days=90,
            alphas=[0.0, 0.5, 1.0],
            deploy=False,
        )
        self.assertTrue(shrink["ok"])
        self.assertIn(shrink["best_alpha"], {0.0, 0.5, 1.0})
        self.assertEqual(len(shrink["grid"]), 3)
        self.assertIn("recommended_params", shrink)

        bad_gate = calibration.coefficient_stability_gate(
            {"a": 100.0, "b": 3.0, "c": 60.0, "d": 12.0, "n_train": 40, "crps_train": 5.0},
            {"a": 0.0, "b": 1.0, "c": 4.0, "d": 0.5, "n_train": 40, "crps_train": 4.0},
        )
        self.assertFalse(bad_gate["ok"])
        self.assertIn("slope_out_of_bounds", bad_gate["reasons"])

        pit = calibration.build_pit_histogram(
            "nyc",
            param="high",
            start_date=datetime(2026, 3, 1, tzinfo=timezone.utc),
            end_date=target + timedelta(days=1),
            bins=5,
        )
        self.assertTrue(pit["has_params"])
        self.assertEqual(pit["n"], 35)
        self.assertEqual(len(pit["bins"]), 5)
        self.assertEqual(sum(item["count"] for item in pit["bins"]), 35)


if __name__ == "__main__":
    unittest.main()
