import tempfile
import unittest
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

import nwp_pipeline


class NWPIngestTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.orig_db_path = nwp_pipeline.DB_PATH
        self.orig_ingest_dir = nwp_pipeline.INGEST_DIR
        self.orig_snapshot_dir = nwp_pipeline.SNAPSHOT_DIR

        base = Path(self.tmpdir.name)
        nwp_pipeline.DB_PATH = base / "nwp_forecasts.db"
        nwp_pipeline.INGEST_DIR = base / "ingest"
        nwp_pipeline.SNAPSHOT_DIR = nwp_pipeline.INGEST_DIR / "payload_snapshots"
        nwp_pipeline.init_db()

    def tearDown(self):
        nwp_pipeline.DB_PATH = self.orig_db_path
        nwp_pipeline.INGEST_DIR = self.orig_ingest_dir
        nwp_pipeline.SNAPSHOT_DIR = self.orig_snapshot_dir
        self.tmpdir.cleanup()

    def _nws_feature(self, observed_at, *, temp_c=20.0, dewpoint_c=10.0,
                     temp_unit="wmoUnit:degC", dewpoint_unit="wmoUnit:degC"):
        return {
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [-73.9654, 40.7829]},
            "properties": {
                "timestamp": observed_at.isoformat().replace("+00:00", "Z"),
                "temperature": {"value": temp_c, "unitCode": temp_unit},
                "dewpoint": {"value": dewpoint_c, "unitCode": dewpoint_unit},
                "windDirection": {"value": 180.0, "unitCode": "wmoUnit:degree_(angle)"},
                "windSpeed": {"value": 12.0, "unitCode": "wmoUnit:km_h-1"},
            },
        }

    def test_city_station_overrides_validate(self):
        self.assertEqual(nwp_pipeline._station_for_city("nyc", "observation"), "KNYC")
        self.assertEqual(nwp_pipeline._station_for_city("chi", "observation"), "KMDW")
        self.assertEqual(nwp_pipeline.validate_city_station_mappings(), [])

    def test_utc_normalization_and_stale_detection(self):
        naive = datetime(2026, 4, 10, 12, 0, 0)
        aware = nwp_pipeline._ensure_utc(naive)
        self.assertIsNotNone(aware.tzinfo)
        self.assertEqual(aware.tzinfo, timezone.utc)

        now = datetime.now(timezone.utc)
        self.assertFalse(nwp_pipeline.is_stale_observation(now - timedelta(hours=1)))
        self.assertTrue(nwp_pipeline.is_stale_observation(now - timedelta(hours=4)))
        self.assertTrue(nwp_pipeline.is_stale_observation(None))

    def test_store_forecasts_uses_idempotency_and_preview(self):
        init_time = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
        forecasts = [
            {
                "valid_utc": init_time,
                "fhr": 3,
                "t2m_f": 72.5,
                "td2m_f": 68.0,
                "wspd_kt": 12,
                "wdir": 180,
                "pmsl_mb": 1013.2,
                "cape": 25.0,
                "cin": -10.0,
                "pwat_mm": 21.0,
            }
        ]

        preview = nwp_pipeline.store_forecasts("nyc", "hrrr", 0, init_time, forecasts, dry_run=True)
        self.assertTrue(preview[0]["insert"])
        self.assertEqual(preview[0]["changes"]["t2m_f"]["after"], 72.5)

        nwp_pipeline.store_forecasts("nyc", "hrrr", 0, init_time, forecasts, dry_run=False)

        conn = __import__("sqlite3").connect(str(nwp_pipeline.DB_PATH))
        row = conn.execute(
            "SELECT city, model, init_cycle, fhr, idempotency_key FROM nwp_forecasts LIMIT 1"
        ).fetchone()
        conn.close()

        self.assertEqual(row[0], "nyc")
        self.assertEqual(row[1], "hrrr")
        self.assertTrue(row[4])

    def test_daily_missing_data_report_uses_stored_rows(self):
        init_time = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
        forecasts = [
            {
                "valid_utc": init_time,
                "fhr": 1,
                "t2m_f": 70.0,
                "td2m_f": 66.0,
                "wspd_kt": 8,
                "wdir": 90,
                "pmsl_mb": 1012.0,
                "cape": 0.0,
                "cin": 0.0,
                "pwat_mm": 10.0,
            }
        ]
        nwp_pipeline.store_forecasts("nyc", "hrrr", 0, init_time, forecasts, dry_run=False)

        report = nwp_pipeline.daily_missing_data_report(init_time)
        nyc_hrrr = next(item for item in report if item["city"] == "nyc" and item["model"] == "hrrr")
        self.assertEqual(nyc_hrrr["observed_rows"], 1)
        self.assertEqual(nyc_hrrr["missing_rows"], 23)

    def test_dry_run_diff_marks_updates(self):
        before = {"temp_f": 70.0, "station": "KNYC"}
        after = {"temp_f": 71.0, "station": "KNYC"}
        diff = nwp_pipeline.dry_run_forecast_diff(before, after)
        self.assertFalse(diff["insert"])
        self.assertIn("temp_f", diff["changes"])

    def test_station_metadata_and_freshness_helpers(self):
        metadata = nwp_pipeline.current_station_metadata("nyc", "observation")
        self.assertIsNotNone(metadata)
        self.assertEqual(metadata["station_icao"], "KNYC")

        stale = nwp_pipeline.record_feed_freshness(
            "nws-observations",
            datetime.now(timezone.utc) - timedelta(hours=5),
            city_key="nyc",
            sla_hours=3,
        )
        self.assertTrue(stale)

        nwp_pipeline.record_source_latency("nws", "observations", 250.0, city_key="nyc")
        latencies = nwp_pipeline.source_latency_distributions("nws")
        self.assertTrue(any(row["source"] == "observations" for row in latencies))

    def test_source_drift_records_missing_and_alias_fields(self):
        payload = [{"valid_utc": "2026-04-10T01:00:00Z", "fhr": 1, "tmp2m_f": 71.0}]

        report = nwp_pipeline.record_source_drift(
            "bufkit.parsed_forecast",
            payload,
            provider="bufkit",
            model="hrrr",
            city_key="nyc",
        )

        self.assertTrue(report["drift"])
        self.assertEqual(report["severity"], "critical")
        self.assertIn("t2m_f", report["missing_fields"])
        self.assertEqual(report["alias_hits"]["t2m_f"], ["tmp2m_f"])

        events = nwp_pipeline.source_drift_report("bufkit.parsed_forecast", limit=5)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["city"], "nyc")
        self.assertIn("t2m_f", events[0]["missing_fields"])

    def test_station_coverage_heatmap_counts_city_model_rows(self):
        start = datetime(2026, 4, 10, 0, tzinfo=timezone.utc)
        forecasts = []
        for hour in range(4):
            forecasts.append({
                "valid_utc": start + timedelta(hours=hour),
                "fhr": hour,
                "t2m_f": 70.0 + hour,
                "td2m_f": 61.0,
                "wspd_kt": 5,
                "wdir": 180,
            })
        nwp_pipeline.store_forecasts("nyc", "hrrr", 0, start, forecasts, dry_run=False)

        heatmap = nwp_pipeline.station_coverage_heatmap(
            start,
            start + timedelta(hours=4),
            city_keys=["nyc", "chi"],
            models=["hrrr"],
        )

        self.assertEqual(heatmap["expected_rows_per_cell"], 4)
        nyc = next(row for row in heatmap["rows"] if row["city"] == "nyc")
        chi = next(row for row in heatmap["rows"] if row["city"] == "chi")
        self.assertEqual(nyc["cells"]["hrrr"]["status"], "good")
        self.assertEqual(chi["cells"]["hrrr"]["status"], "missing")
        rendered = nwp_pipeline.render_station_coverage_heatmap(heatmap)
        self.assertIn("NYC", rendered.upper())
        self.assertIn("Legend", rendered)

    def test_checksum_manifest_policy_validation(self):
        payload = b"artifact-bytes"
        sha = nwp_pipeline._sha256_bytes(payload)
        manifest = {
            "providers": {"bufkit": {"required": True}},
            "artifacts": {"sample.buf": {"sha256": sha}},
        }

        ok = nwp_pipeline.validate_artifact_checksum("sample.buf", payload, manifest)
        self.assertTrue(ok["ok"])
        self.assertEqual(ok["provider"], "bufkit")

        mismatch = nwp_pipeline.validate_artifact_checksum("sample.buf", b"changed", manifest)
        self.assertFalse(mismatch["ok"])
        self.assertEqual(mismatch["status"], "mismatch")

        missing = nwp_pipeline.validate_artifact_checksum("missing.buf", payload, manifest)
        self.assertFalse(missing["ok"])
        self.assertEqual(missing["status"], "missing_manifest")

    def test_schema_fixtures_validate_required_paths(self):
        fixtures = nwp_pipeline.load_schema_fixtures()
        self.assertIn("nws.observations", fixtures)

        self.assertEqual(
            nwp_pipeline.validate_payload_against_fixture("nws.observations", {"features": []}, fixtures),
            [],
        )
        errors = nwp_pipeline.validate_payload_against_fixture("nws.observations", {}, fixtures)
        self.assertEqual(errors, ["features"])

        with self.assertRaises(nwp_pipeline.ExternalSchemaValidationError):
            nwp_pipeline.validate_payload_against_fixture("nws.observations", {}, fixtures, strict=True)

        with self.assertRaises(nwp_pipeline.ExternalSchemaValidationError):
            nwp_pipeline._validate_external_json("nws.observations", {}, ("features",), strict=True)

    def test_payload_snapshot_replay_and_retention_prune(self):
        payload = json.dumps({"features": []}).encode("utf-8")
        snapshot = nwp_pipeline.persist_payload_snapshot(
            "nws.observations",
            payload,
            city_key="nyc",
            payload_type="json",
            metadata={"url": "https://api.weather.gov/stations/KNYC/observations"},
        )
        rows = nwp_pipeline.payload_snapshot_rows("nws.observations")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["sha256"], snapshot["sha256"])

        replay = nwp_pipeline.replay_payload_snapshot(rows[0]["id"])
        self.assertEqual(replay["actions"][0]["type"], "validate_json")
        self.assertEqual(replay["actions"][0]["fixture_errors"], [])

        conn = __import__("sqlite3").connect(str(nwp_pipeline.DB_PATH))
        conn.execute("UPDATE payload_snapshots SET captured_at = ?", ("2000-01-01T00:00:00+00:00",))
        conn.commit()
        conn.close()

        dry = nwp_pipeline.prune_payload_snapshots(older_than_days=1, source="nws.observations", dry_run=True)
        self.assertEqual(dry["candidate_rows"], 1)
        self.assertTrue(Path(rows[0]["path"]).exists())

        applied = nwp_pipeline.prune_payload_snapshots(older_than_days=1, source="nws.observations", dry_run=False)
        self.assertEqual(applied["candidate_rows"], 1)
        self.assertFalse(Path(rows[0]["path"]).exists())

    def test_provider_health_score_uses_metrics_freshness_and_latency(self):
        nwp_pipeline.record_ingest_metric("obs", "nyc", "nws", ok=True)
        nwp_pipeline.record_ingest_metric("obs", "nyc", "nws", ok=False, reason="timeout")
        nwp_pipeline.record_feed_freshness(
            "nws-observations",
            datetime.now(timezone.utc) - timedelta(hours=5),
            city_key="nyc",
            sla_hours=3,
        )
        nwp_pipeline.record_source_latency("nws", "observations", 3000.0, city_key="nyc")

        health = nwp_pipeline.provider_health_score("nws", city_key="nyc")
        self.assertEqual(health["provider"], "nws")
        self.assertLess(health["score"], 100.0)
        self.assertIn(health["status"], {"degraded", "down"})
        self.assertIn("provider_failures", health["reasons"])
        self.assertIn("stale_feed", health["reasons"])

    def test_station_metadata_diff_export_and_import(self):
        exported = nwp_pipeline.export_station_metadata()
        self.assertGreaterEqual(len(exported["rows"]), len(nwp_pipeline.KALSHI_CITIES) * 3)
        self.assertTrue(nwp_pipeline.station_metadata_diff()["ok"])

        nyc_settlement = nwp_pipeline.current_station_metadata("nyc", "settlement")
        self.assertEqual(nyc_settlement["station_icao"], "KNYC")
        self.assertIn("product=CLI", nyc_settlement["source_url"])
        self.assertIn("issuedby=NYC", nyc_settlement["source_url"])

        edited = json.loads(json.dumps(exported))
        nyc_obs = next(
            row for row in edited["rows"]
            if row["city"] == "nyc" and row["station_role"] == "observation"
        )
        nyc_obs["station_icao"] = "KZZZ"
        preview = nwp_pipeline.import_station_metadata(edited, dry_run=True)
        self.assertTrue(any(item["action"] == "update" for item in preview["preview"]))
        self.assertTrue(nwp_pipeline.station_metadata_diff()["ok"])

        applied = nwp_pipeline.import_station_metadata(edited, dry_run=False)
        self.assertEqual(applied["errors"], [])
        diff = nwp_pipeline.station_metadata_diff()
        self.assertFalse(diff["ok"])
        mismatch = next(item for item in diff["mismatches"] if item["city"] == "nyc" and item["station_role"] == "observation")
        self.assertEqual(mismatch["actual_station"], "KZZZ")

    def test_live_station_metadata_validation_uses_injected_fetcher(self):
        def fake_fetcher(station):
            return {
                "geometry": {"type": "Point", "coordinates": [-73.9654, 40.7829]},
                "properties": {
                    "stationIdentifier": station,
                    "name": "Central Park",
                    "timeZone": "America/New_York",
                },
            }

        results = nwp_pipeline.validate_live_station_metadata(
            city_keys=["nyc"],
            roles=["settlement"],
            fetcher=fake_fetcher,
            max_distance_deg=0.1,
        )
        self.assertEqual(len(results), 1)
        self.assertTrue(results[0]["ok"])
        self.assertEqual(results[0]["station_icao"], "KNYC")

        conn = __import__("sqlite3").connect(str(nwp_pipeline.DB_PATH))
        row = conn.execute(
            "SELECT city, station_role, station_icao, ok FROM station_live_validation"
        ).fetchone()
        conn.close()
        self.assertEqual(row, ("nyc", "settlement", "KNYC", 1))

    def test_nws_observation_validation_and_store_rejects_bad_rows(self):
        observed_at = datetime(2026, 4, 10, 12, tzinfo=timezone.utc)
        valid_feature = self._nws_feature(observed_at)
        self.assertEqual(nwp_pipeline.validate_nws_observation_feature(valid_feature), [])

        bad_unit = self._nws_feature(observed_at, temp_unit="unit:degF")
        self.assertIn(
            "temperature:unexpected_unit:unit:degF",
            nwp_pipeline.validate_nws_observation_feature(bad_unit),
        )

        high_temp = self._nws_feature(observed_at, temp_c=100.0)
        self.assertTrue(
            any(err.startswith("temp_f:out_of_range") for err in nwp_pipeline.validate_nws_observation_feature(high_temp))
        )

        preview = nwp_pipeline.store_hourly_observations(
            "nyc",
            "KNYC",
            [
                {
                    "station": "KNYC",
                    "observed_at_utc": observed_at,
                    "temp_f": 68.0,
                    "dewpoint_f": 50.0,
                    "wind_dir": 180.0,
                    "wind_spd": 12.0,
                    "raw_payload": valid_feature,
                },
                {
                    "station": "KNYC",
                    "observed_at_utc": observed_at + timedelta(hours=1),
                    "temp_f": 180.0,
                    "dewpoint_f": 50.0,
                    "wind_dir": 180.0,
                    "wind_spd": 12.0,
                },
            ],
            dry_run=False,
        )
        self.assertEqual(len(preview), 2)
        self.assertTrue(preview[1]["rejected"])

        conn = __import__("sqlite3").connect(str(nwp_pipeline.DB_PATH))
        obs_count = conn.execute("SELECT COUNT(*) FROM hourly_observations").fetchone()[0]
        metric = conn.execute(
            "SELECT success_count, failure_count FROM ingest_metrics WHERE model = 'obs' AND city = 'nyc'"
        ).fetchone()
        conn.close()
        self.assertEqual(obs_count, 1)
        self.assertEqual(metric, (1, 1))

    def test_observation_gap_clusters_and_backfill_summary(self):
        start = datetime(2026, 4, 10, 0, tzinfo=timezone.utc)
        observations = []
        for hour in (0, 3):
            observations.append({
                "station": "KNYC",
                "observed_at_utc": start + timedelta(hours=hour),
                "temp_f": 60.0 + hour,
                "dewpoint_f": 50.0,
                "wind_dir": 180.0,
                "wind_spd": 10.0,
            })
        nwp_pipeline.store_hourly_observations("nyc", "KNYC", observations, dry_run=False)

        gaps = nwp_pipeline.observation_gap_clusters("nyc", start, start + timedelta(hours=5))
        self.assertEqual(gaps["expected_hours"], 5)
        self.assertEqual(gaps["observed_hours"], 2)
        self.assertEqual(gaps["missing_hours"], 3)
        self.assertEqual(gaps["gap_count"], 2)
        self.assertEqual(gaps["gaps"][0]["hours"], 2)
        self.assertEqual(gaps["gaps"][1]["hours"], 1)

        summary = nwp_pipeline.backfill_observation_dry_run_summary("nyc", start, start + timedelta(hours=5))
        self.assertEqual(summary["missing_hours"], 3)
        self.assertEqual(summary["backfill_ranges"][0]["hours"], 2)

    def test_observation_duplicate_detection_and_dedupe(self):
        observed_at = datetime(2026, 4, 10, 12, tzinfo=timezone.utc).isoformat()
        conn = __import__("sqlite3").connect(str(nwp_pipeline.DB_PATH))
        rows = [
            ("nyc", "KNYC", observed_at, 70.0, "dup-a"),
            ("nyc", "KNYC", observed_at, 71.0, "dup-b"),
            ("nyc", "KNYC", (datetime(2026, 4, 10, 13, tzinfo=timezone.utc)).isoformat(), 72.0, "unique"),
        ]
        conn.executemany(
            """
            INSERT INTO hourly_observations(city, station, observed_at_utc, temp_f, idempotency_key)
            VALUES (?, ?, ?, ?, ?)
            """,
            rows,
        )
        conn.commit()
        conn.close()

        dupes = nwp_pipeline.detect_duplicate_observations()
        self.assertEqual(len(dupes), 1)
        self.assertEqual(dupes[0]["count"], 2)

        removed = nwp_pipeline.dedupe_observations()
        self.assertEqual(removed, 1)
        self.assertEqual(nwp_pipeline.detect_duplicate_observations(), [])


if __name__ == "__main__":
    unittest.main()
