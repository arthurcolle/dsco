#!/usr/bin/env python3
"""Focused tests for the redacted Chimera feedback loop."""

from __future__ import annotations

import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from .feedback import FEEDBACK_SCHEMA_VERSION, FeedbackStore, request_digest
from .features import (
    DEFAULT_MODEL_DIM,
    DEFAULT_TASK_DIM,
    MODEL_FEATURE_VERSION,
    TASK_FEATURE_VERSION,
)
from .model import CandidateRouter
from .serve import RouterState


class FeedbackStoreTests(unittest.TestCase):
    @staticmethod
    def _record_minimal_decision(store: FeedbackStore, request_id: str) -> str:
        return store.record_decision(
            {"messages": [{"role": "user", "content": "redacted"}]},
            {
                "selected_model": "vendor/model",
                "candidates": [],
                "fallback_models": [],
            },
            checkpoint_id="router",
            catalog_snapshot_id=None,
            request_id=request_id,
        )

    @staticmethod
    def _create_legacy_database(path: Path, versions: tuple[int, ...]) -> None:
        connection = sqlite3.connect(str(path))
        connection.executescript(
            """
            PRAGMA foreign_keys = ON;
            CREATE TABLE router_schema(version INTEGER PRIMARY KEY);
            CREATE TABLE route_decisions(
                request_id TEXT PRIMARY KEY,
                decided_at TEXT NOT NULL,
                chosen_model TEXT NOT NULL
            );
            CREATE TABLE route_outcomes(
                request_id TEXT PRIMARY KEY,
                completed_at TEXT NOT NULL,
                intended_model TEXT NOT NULL,
                actual_model TEXT,
                actual_provider TEXT,
                generation_id TEXT,
                http_success INTEGER,
                task_success INTEGER,
                provider_failure INTEGER,
                tool_valid INTEGER,
                schema_valid INTEGER,
                refusal INTEGER,
                user_retry INTEGER,
                accepted_or_used INTEGER,
                prompt_tokens INTEGER,
                completion_tokens INTEGER,
                reasoning_tokens INTEGER,
                cache_read_tokens INTEGER,
                cache_write_tokens INTEGER,
                cost_usd REAL,
                ttft_ms REAL,
                e2e_ms REAL,
                finish_reason TEXT,
                label_source TEXT NOT NULL,
                label_confidence REAL NOT NULL,
                censored INTEGER NOT NULL,
                FOREIGN KEY(request_id) REFERENCES route_decisions(request_id)
            );
            INSERT INTO route_decisions(request_id, decided_at, chosen_model)
            VALUES ('legacy-request', '2026-08-01T00:00:00.000Z', 'vendor/legacy');
            INSERT INTO route_outcomes(
                request_id, completed_at, intended_model, actual_model,
                task_success, cost_usd, label_source, label_confidence, censored
            ) VALUES (
                'legacy-request', '2026-08-01T00:00:01.000Z', 'vendor/legacy',
                'vendor/legacy', 1, 0.004, 'legacy_verifier', 0.9, 0
            );
            """
        )
        connection.executemany(
            "INSERT INTO router_schema(version) VALUES (?)",
            ((version,) for version in versions),
        )
        connection.commit()
        connection.close()

    def test_router_state_records_features_and_accepts_outcome(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "router.npz"
            catalog_path = root / "catalog.json"
            database = root / "feedback.db"
            router = CandidateRouter(
                DEFAULT_TASK_DIM,
                DEFAULT_MODEL_DIM,
                ("completion_success",),
                rank=2,
                head_types=("probability",),
            )
            router.save(
                str(artifact),
                {
                    "task_feature_version": TASK_FEATURE_VERSION,
                    "model_feature_version": MODEL_FEATURE_VERSION,
                },
            )
            catalog_path.write_text(
                json.dumps(
                    {
                        "data": [
                            {
                                "id": "vendor/model",
                                "context_length": 8192,
                                "architecture": {
                                    "input_modalities": ["text"],
                                    "output_modalities": ["text"],
                                },
                                "pricing": {
                                    "prompt": "0.000001",
                                    "completion": "0.000001",
                                },
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            state = RouterState(artifact, catalog_path, feedback_db=database)
            decision = state.route(
                {
                    "request": {"prompt": "A prompt kept out of SQLite"},
                    "policy": {"missing_metadata_penalty": 0},
                }
            )
            self.assertEqual(decision["selected_model"], "vendor/model")
            self.assertIn("request_id", decision)
            connection = sqlite3.connect(str(database))
            feature = connection.execute(
                "SELECT task_feature_version, task_dim, length(task_features) "
                "FROM route_request_features"
            ).fetchone()
            self.assertEqual(
                feature,
                (TASK_FEATURE_VERSION, DEFAULT_TASK_DIM, DEFAULT_TASK_DIM * 4),
            )
            connection.close()
            recorded = state.outcome(
                {
                    "request_id": decision["request_id"],
                    "task_success": True,
                    "label_source": "unit_verifier",
                    "label_confidence": 1.0,
                }
            )
            self.assertTrue(recorded["recorded"])

    def test_records_redacted_exposure_and_delayed_outcome(self) -> None:
        request = {
            "messages": [
                {"role": "user", "content": "secret prompt must never be stored"}
            ],
            "estimated_prompt_tokens": 12,
        }
        decision = {
            "schema_version": 1,
            "selected_model": "vendor/good",
            "fallback_models": ["vendor/backup"],
            "explicit_override": False,
            "policy": {"max_cost_usd": 0.01},
            "token_estimates": {"prompt_tokens": 12, "completion_tokens": 20},
            "candidates": [
                {
                    "model_id": "vendor/good",
                    "feasible": True,
                    "utility": 0.8,
                    "predictions": {"completion_success": 0.9},
                    "failure_probability": 0.02,
                    "expected_latency_s": 0.4,
                    "expected_cost_usd": 0.001,
                    "rejection_reasons": [],
                },
                {
                    "model_id": "other/rejected",
                    "feasible": False,
                    "utility": 0.1,
                    "predictions": {"completion_success": 0.4},
                    "failure_probability": 0.2,
                    "expected_latency_s": 1.2,
                    "expected_cost_usd": 0.02,
                    "rejection_reasons": ["cost_above_limit"],
                },
            ],
        }
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "feedback.db"
            store = FeedbackStore(database)
            store.register_catalog(
                snapshot_id="catalog-hash",
                content_sha256="catalog-hash",
                model_count=2,
                source_url="fixture.json",
            )
            request_id = store.record_decision(
                request,
                decision,
                checkpoint_id="checkpoint-hash",
                catalog_snapshot_id="catalog-hash",
                policy_id="test-policy",
                logging_propensity=1.0,
                task_features=[1.0, -0.25, 0.5],
                task_feature_version="chimera-task-test-v1",
            )
            result = store.record_outcome(
                {
                    "request_id": request_id,
                    "actual_model": "vendor/good",
                    "actual_provider": "provider-a",
                    "http_success": True,
                    "task_success": True,
                    "schema_valid": True,
                    "cost_usd": 0.0008,
                    "e2e_ms": 440,
                    "label_source": "deterministic_verifier",
                    "label_confidence": 1.0,
                }
            )
            self.assertTrue(result["recorded"])

            connection = sqlite3.connect(str(database))
            route_row = connection.execute(
                "SELECT request_hash, chosen_model, policy_id FROM route_decisions"
            ).fetchone()
            self.assertEqual(
                route_row,
                (request_digest(request), "vendor/good", "test-policy"),
            )
            exposures = connection.execute(
                "SELECT model_id, chosen, predicted_latency_ms "
                "FROM candidate_exposures ORDER BY rank"
            ).fetchall()
            self.assertEqual(exposures[0], ("vendor/good", 1, 400.0))
            self.assertEqual(exposures[1][0:2], ("other/rejected", 0))
            feature = connection.execute(
                "SELECT task_feature_version, task_dim, encoding, length(task_features) "
                "FROM route_request_features"
            ).fetchone()
            self.assertEqual(feature, ("chimera-task-test-v1", 3, "float32-le", 12))
            outcome = connection.execute(
                "SELECT task_success, cost_usd, label_source FROM route_outcomes"
            ).fetchone()
            self.assertEqual(outcome, (1, 0.0008, "deterministic_verifier"))

            # Query every text-valued user table column: prompt material must
            # not be present anywhere in the logical store.
            text_values = []
            tables = (
                "catalog_snapshots",
                "route_decisions",
                "route_request_features",
                "candidate_exposures",
                "route_outcomes",
                "route_outcome_revisions",
                "pairwise_comparisons",
            )
            for table in tables:
                columns = connection.execute("PRAGMA table_info(%s)" % table).fetchall()
                text_columns = [row[1] for row in columns if "TEXT" in row[2].upper()]
                for column in text_columns:
                    text_values.extend(
                        str(row[0])
                        for row in connection.execute(
                            'SELECT "%s" FROM "%s" WHERE "%s" IS NOT NULL'
                            % (column, table, column)
                        ).fetchall()
                    )
            connection.close()
            self.assertNotIn("secret prompt", "\n".join(text_values))

    def test_rejects_outcome_for_unknown_request(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            store = FeedbackStore(Path(temporary) / "feedback.db")
            with self.assertRaises(KeyError):
                store.record_outcome(
                    {
                        "request_id": "missing",
                        "label_source": "human",
                        "label_confidence": 0.8,
                    }
                )

    def test_migrates_v1_and_v2_outcomes_once_without_rewriting_projection(
        self,
    ) -> None:
        for versions in ((1,), (1, 2)):
            with (
                self.subTest(versions=versions),
                tempfile.TemporaryDirectory() as temporary,
            ):
                database = Path(temporary) / "legacy.db"
                self._create_legacy_database(database, versions)
                FeedbackStore(database)
                # Migration is idempotent when the service reopens the DB.
                FeedbackStore(database)

                connection = sqlite3.connect(str(database))
                installed = connection.execute(
                    "SELECT version FROM router_schema ORDER BY version"
                ).fetchall()
                projection = connection.execute(
                    "SELECT completed_at, task_success, cost_usd, label_source "
                    "FROM route_outcomes"
                ).fetchone()
                revisions = connection.execute(
                    "SELECT revision_id, request_id, revision_number, recorded_at, "
                    "supersedes_revision_id, task_success, cost_usd, label_source "
                    "FROM route_outcome_revisions"
                ).fetchall()
                connection.close()

                self.assertEqual(installed, [(1,), (2,), (3,)])
                self.assertEqual(
                    projection,
                    (
                        "2026-08-01T00:00:01.000Z",
                        1,
                        0.004,
                        "legacy_verifier",
                    ),
                )
                self.assertEqual(
                    revisions,
                    [
                        (
                            1,
                            "legacy-request",
                            1,
                            "2026-08-01T00:00:01.000Z",
                            None,
                            1,
                            0.004,
                            "legacy_verifier",
                        )
                    ],
                )

    def test_outcome_corrections_are_append_only_and_return_revision_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "feedback.db"
            store = FeedbackStore(database)
            request_id = self._record_minimal_decision(store, "request-a")
            first = store.record_outcome(
                {
                    "request_id": request_id,
                    "task_success": False,
                    "label_source": "automatic",
                    "label_confidence": 0.6,
                }
            )
            second = store.record_outcome(
                {
                    "request_id": request_id,
                    "task_success": True,
                    "label_source": "human_correction",
                    "label_confidence": 0.9,
                }
            )
            third = store.record_outcome(
                {
                    "request_id": request_id,
                    "task_success": False,
                    "label_source": "adjudication",
                    "label_confidence": 1.0,
                }
            )

            self.assertEqual(
                [first["revision_id"], second["revision_id"], third["revision_id"]],
                [1, 2, 3],
            )
            self.assertEqual(
                [
                    first["supersedes_revision_id"],
                    second["supersedes_revision_id"],
                    third["supersedes_revision_id"],
                ],
                [None, 1, 2],
            )
            connection = sqlite3.connect(str(database))
            revisions = connection.execute(
                "SELECT revision_number, task_success, label_source "
                "FROM route_outcome_revisions ORDER BY revision_id"
            ).fetchall()
            projection = connection.execute(
                "SELECT task_success, label_source FROM route_outcomes"
            ).fetchone()
            self.assertEqual(
                revisions,
                [
                    (1, 0, "automatic"),
                    (2, 1, "human_correction"),
                    (3, 0, "adjudication"),
                ],
            )
            self.assertEqual(projection, (0, "adjudication"))
            with self.assertRaises(sqlite3.IntegrityError):
                connection.execute(
                    "UPDATE route_outcome_revisions SET task_success = 1 "
                    "WHERE revision_id = 1"
                )
            with self.assertRaises(sqlite3.IntegrityError):
                connection.execute(
                    "DELETE FROM route_outcome_revisions WHERE revision_id = 1"
                )
            connection.close()

    def test_latest_outcomes_are_reproducible_at_revision_high_water(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            store = FeedbackStore(Path(temporary) / "feedback.db")
            self._record_minimal_decision(store, "request-a")
            self._record_minimal_decision(store, "request-b")
            a1 = store.record_outcome(
                {
                    "request_id": "request-a",
                    "task_success": False,
                    "label_source": "initial",
                    "label_confidence": 0.5,
                }
            )
            b1 = store.record_outcome(
                {
                    "request_id": "request-b",
                    "task_success": True,
                    "label_source": "initial",
                    "label_confidence": 0.5,
                }
            )
            high_water = store.outcome_revision_high_water()
            a2 = store.record_outcome(
                {
                    "request_id": "request-a",
                    "task_success": True,
                    "label_source": "correction",
                    "label_confidence": 1.0,
                }
            )
            store.record_outcome(
                {
                    "request_id": "request-b",
                    "task_success": False,
                    "label_source": "correction",
                    "label_confidence": 1.0,
                }
            )

            self.assertEqual(high_water, b1["revision_id"])
            old_snapshot = store.latest_outcomes_as_of(high_water)
            after_a_correction = store.latest_outcomes_as_of(a2["revision_id"])
            current = store.latest_outcomes_as_of()
            self.assertEqual(old_snapshot["revision_high_water"], high_water)
            self.assertEqual(
                [
                    (row["request_id"], row["task_success"])
                    for row in old_snapshot["outcomes"]
                ],
                [("request-a", 0), ("request-b", 1)],
            )
            self.assertEqual(
                [
                    (row["request_id"], row["task_success"])
                    for row in after_a_correction["outcomes"]
                ],
                [("request-a", 1), ("request-b", 1)],
            )
            self.assertEqual(
                [
                    (row["request_id"], row["task_success"])
                    for row in current["outcomes"]
                ],
                [("request-a", 1), ("request-b", 0)],
            )
            # Replaying an old high-water remains byte-for-byte equivalent at
            # the logical row level after later corrections arrive.
            self.assertEqual(store.latest_outcomes_as_of(high_water), old_snapshot)
            self.assertEqual(a1["revision_id"], 1)

    def test_fails_closed_without_mutating_a_newer_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "future.db"
            connection = sqlite3.connect(str(database))
            connection.execute(
                "CREATE TABLE router_schema(version INTEGER PRIMARY KEY)"
            )
            connection.execute("INSERT INTO router_schema(version) VALUES (4)")
            connection.commit()
            connection.close()

            with self.assertRaisesRegex(ValueError, "newer than this binary"):
                FeedbackStore(database)
            connection = sqlite3.connect(str(database))
            tables = connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"
            ).fetchall()
            versions = connection.execute(
                "SELECT version FROM router_schema"
            ).fetchall()
            connection.close()
            self.assertEqual(tables, [("router_schema",)])
            self.assertEqual(versions, [(FEEDBACK_SCHEMA_VERSION + 1,)])

    def test_propensity_and_boolean_validation(self) -> None:
        decision = {
            "selected_model": "vendor/model",
            "candidates": [],
            "fallback_models": [],
        }
        with tempfile.TemporaryDirectory() as temporary:
            store = FeedbackStore(Path(temporary) / "feedback.db")
            with self.assertRaises(ValueError):
                store.record_decision(
                    {"messages": []},
                    decision,
                    checkpoint_id="router",
                    catalog_snapshot_id=None,
                    logging_propensity=0.0,
                )


if __name__ == "__main__":
    unittest.main()
