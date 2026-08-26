#!/usr/bin/env python3
"""Focused regression tests for the Chimera Router v2 dataset contract."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

import numpy as np

try:
    from .build_dataset import (
        CatalogIndex,
        OutcomeRow,
        arrays_from_rows,
        attach_feedback_snapshot_arrays,
        build_metadata,
        collect_feedback_rows,
        prompt_digest,
        save_npz_deterministic,
    )
    from .feedback import FeedbackStore
    from .features import (
        DEFAULT_TASK_DIM,
        MODEL_BENCHMARK_DERIVED_INDICES,
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        feature_contract,
        model_family_id,
        model_features,
    )
except ImportError:
    from build_dataset import (  # type: ignore
        CatalogIndex,
        OutcomeRow,
        arrays_from_rows,
        attach_feedback_snapshot_arrays,
        build_metadata,
        collect_feedback_rows,
        prompt_digest,
        save_npz_deterministic,
    )
    from feedback import FeedbackStore  # type: ignore
    from features import (  # type: ignore
        DEFAULT_TASK_DIM,
        MODEL_BENCHMARK_DERIVED_INDICES,
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        feature_contract,
        model_family_id,
        model_features,
    )


def _catalog_payload() -> bytes:
    models = [
        {
            "id": "anthropic/claude-opus-4.6",
            "name": "Claude Opus 4.6",
            "description": "Agentic reasoning and coding model with tool calling.",
            "context_length": 200000,
            "architecture": {
                "input_modalities": ["text", "image"],
                "output_modalities": ["text"],
                "tokenizer": "Claude",
            },
            "pricing": {"prompt": "0.000005", "completion": "0.000025"},
            "supported_parameters": ["tools", "reasoning", "structured_outputs"],
            "benchmarks": {
                "artificial_analysis": {
                    "intelligence_index": 71.0,
                    "coding_index": 76.5,
                    "agentic_index": 68.0,
                }
            },
        },
        {
            "id": "anthropic/claude-opus-4.8:batch",
            "name": "Claude Opus 4.8 Batch",
            "description": "Long-context reasoning model for document analysis.",
            "architecture": {
                "input_modalities": ["text"],
                "output_modalities": ["text"],
            },
            "pricing": {"prompt": "0", "completion": "0"},
            "benchmarks": {"artificial_analysis": {"coding_index": 77.0}},
        },
        {
            "id": "other/vision-x",
            "name": "Vision X",
            "description": "Fast multilingual image and video understanding.",
            "architecture": {
                "input_modalities": ["text", "image", "video"],
                "output_modalities": ["text"],
            },
            "pricing": {},
        },
    ]
    return json.dumps({"data": models}, sort_keys=True, separators=(",", ":")).encode()


class SemanticFeatureTests(unittest.TestCase):
    def test_description_changes_semantic_hash_and_quality_suppresses_targets(
        self,
    ) -> None:
        descriptor = json.loads(_catalog_payload())["data"][0]
        full = model_features(descriptor)
        clean = model_features(descriptor, include_benchmark_priors=False)
        changed = dict(descriptor)
        changed["description"] = "Creative multilingual visual storytelling model."
        changed_clean = model_features(changed, include_benchmark_priors=False)

        self.assertEqual(MODEL_FEATURE_VERSION, "chimera-model-semantic-v2")
        self.assertEqual(full.shape, (128,))
        self.assertTrue(np.all(clean[list(MODEL_BENCHMARK_DERIVED_INDICES)] == 0.0))
        self.assertTrue(np.any(full[list(MODEL_BENCHMARK_DERIVED_INDICES)] != 0.0))
        self.assertFalse(np.array_equal(clean, changed_clean))
        self.assertNotIn(
            "model.identity.v1", feature_contract()["model_hash_namespaces"]
        )
        self.assertFalse(feature_contract()["full_model_id_feature"])

    def test_family_groups_vendor_variants(self) -> None:
        self.assertEqual(
            model_family_id("anthropic/claude-opus-4.6"),
            model_family_id("openrouter/anthropic/claude-opus-4.8:batch"),
        )
        self.assertEqual(model_family_id("openai/gpt-5.6-sol-pro"), "openai/gpt-5-6")


class DatasetV2ContractTests(unittest.TestCase):
    def _rows(self, catalog: CatalogIndex, prompt: str) -> list:
        model_ids = ("anthropic/claude-opus-4.6", "other/vision-x")
        rows = []
        for index, model_id in enumerate(model_ids):
            resolved, descriptor, matched = catalog.resolve(model_id)
            rows.append(
                OutcomeRow(
                    prompt_hash=prompt_digest(prompt),
                    row_id=hashlib.sha256(("row-%d" % index).encode()).hexdigest(),
                    instance_id="instance-%d" % index,
                    prompt_event_id=index + 1,
                    event_epoch=float(100 + index),
                    model_id=resolved,
                    observed_model_id=model_id,
                    provider_id=resolved.split("/", 1)[0],
                    catalog_match=matched,
                    prompt=prompt,
                    descriptor=descriptor,
                    labels=np.asarray(
                        [1.0, 0.0, 0.0, 0.01, 1.0 + index], dtype=np.float32
                    ),
                    label_mask=np.ones(5, dtype=np.uint8),
                )
            )
        return rows

    def test_separate_quality_lane_holdouts_and_deterministic_archive(self) -> None:
        payload = _catalog_payload()
        catalog = CatalogIndex(payload)
        prompt = "UNIQUE_RAW_PROMPT_MUST_NOT_APPEAR_68af934c"
        rows = self._rows(catalog, prompt)
        arrays = arrays_from_rows(rows, catalog, 512, 128, 0)

        self.assertEqual(arrays["labels"].shape, (2, 5))
        self.assertEqual(arrays["catalog_quality_features"].shape, (3, 128))
        self.assertEqual(arrays["catalog_quality_labels"].shape, (3, 3))
        self.assertEqual(arrays["catalog_quality_label_mask"].shape, (3, 3))
        self.assertEqual(
            arrays["catalog_quality_label_names"].tolist(),
            [
                "general_intelligence_quality",
                "coding_quality",
                "agentic_quality",
            ],
        )
        self.assertTrue(
            np.all(
                arrays["catalog_quality_features"][
                    :, list(MODEL_BENCHMARK_DERIVED_INDICES)
                ]
                == 0.0
            )
        )
        self.assertEqual(
            arrays["catalog_quality_label_mask"].sum(axis=0).tolist(), [1, 2, 1]
        )
        observed = arrays["catalog_quality_label_mask"] != 0
        self.assertTrue(
            np.all(arrays["catalog_quality_label_provenance"][observed] != "")
        )
        self.assertTrue(
            np.all(arrays["catalog_quality_label_provenance"][~observed] == "")
        )

        for stem in ("model", "provider", "family"):
            self.assertIn(stem + "_holdout_ids", arrays)
            self.assertIn(stem + "_holdout_fold", arrays)
            self.assertIn("catalog_quality_" + stem + "_holdout_ids", arrays)
            self.assertIn("catalog_quality_" + stem + "_holdout_fold", arrays)
        # The two Anthropic variants share a family but remain distinct models.
        quality_ids = arrays["catalog_quality_model_ids"].tolist()
        first = quality_ids.index("anthropic/claude-opus-4.6")
        second = quality_ids.index("anthropic/claude-opus-4.8:batch")
        self.assertAlmostEqual(
            float(arrays["catalog_quality_labels"][first, 0]), 0.71, places=6
        )
        self.assertEqual(
            arrays["catalog_quality_family_ids"][first],
            arrays["catalog_quality_family_ids"][second],
        )
        self.assertNotEqual(
            arrays["catalog_quality_model_holdout_ids"][first],
            arrays["catalog_quality_model_holdout_ids"][second],
        )

        with tempfile.TemporaryDirectory() as directory:
            first_path = Path(directory) / "first.npz"
            second_path = Path(directory) / "second.npz"
            save_npz_deterministic(first_path, arrays)
            save_npz_deterministic(second_path, arrays)
            first_bytes = first_path.read_bytes()
            self.assertEqual(first_bytes, second_path.read_bytes())
            self.assertNotIn(prompt.encode(), first_bytes)

            artifact_hash = hashlib.sha256(first_bytes).hexdigest()
            metadata = build_metadata(
                arrays,
                catalog,
                {"source_event_max_id": 2},
                512,
                128,
                0,
                artifact_hash,
                Path("baseline.db"),
                Path("catalog.json"),
            )
            encoded_metadata = json.dumps(metadata, sort_keys=True).encode()
            self.assertNotIn(prompt.encode(), encoded_metadata)
            self.assertFalse(
                metadata["catalog_quality_contract"]["anti_leak"][
                    "benchmark_priors_in_features"
                ]
            )
            self.assertFalse(
                metadata["label_contract"]["catalog_quality_labels_included"]
            )

    def test_feedback_v3_revision_snapshot_masks_and_redaction(self) -> None:
        payload = _catalog_payload()
        catalog = CatalogIndex(payload)
        task_vector = np.linspace(-0.75, 0.75, DEFAULT_TASK_DIM, dtype=np.float32)
        secret_prompt = "FEEDBACK_RAW_PROMPT_MUST_NOT_EMIT_4d14838d"
        secret_request_id = "FEEDBACK_REQUEST_ID_MUST_NOT_EMIT_19af39c1"
        secret_label_source = "FEEDBACK_LABEL_SOURCE_MUST_NOT_EMIT_f3c91cc8"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            feedback_path = root / "feedback.db"
            store = FeedbackStore(feedback_path)
            store.register_catalog(
                snapshot_id=catalog.snapshot_sha256,
                content_sha256=catalog.snapshot_sha256,
                model_count=len(catalog.models),
                source_url="fixture-catalog.json",
                captured_at="2026-08-14T00:00:00Z",
            )
            decision = {
                "selected_model": "future-provider/future-model-2035",
                "candidates": [
                    {
                        "model_id": "future-provider/future-model-2035",
                        "feasible": True,
                        "utility": 0.5,
                    },
                    {
                        "model_id": "anthropic/claude-opus-4.6",
                        "feasible": True,
                        "utility": 0.4,
                    },
                ],
                "fallback_models": ["anthropic/claude-opus-4.6"],
            }
            store.record_decision(
                {"prompt": secret_prompt},
                decision,
                checkpoint_id="fixture-router",
                catalog_snapshot_id=catalog.snapshot_sha256,
                request_id=secret_request_id,
                task_features=task_vector,
                task_feature_version=TASK_FEATURE_VERSION,
            )
            revision_one = store.record_outcome(
                {
                    "request_id": secret_request_id,
                    "completed_at": "2026-08-14T00:02:00Z",
                    "actual_model": "future-provider/future-model-2035",
                    "http_success": False,
                    "task_success": True,
                    "provider_failure": None,
                    "tool_valid": False,
                    "cost_usd": 0.0,
                    "e2e_ms": 250.0,
                    "label_source": secret_label_source,
                    "label_confidence": 0.75,
                }
            )["revision_id"]
            revision_two = store.record_outcome(
                {
                    "request_id": secret_request_id,
                    # Deliberately earlier: immutable revision order, not this
                    # caller-supplied timestamp, defines the as-of snapshot.
                    "completed_at": "2026-08-14T00:01:00Z",
                    "actual_model": "future-provider/future-model-2035",
                    "http_success": True,
                    "provider_failure": False,
                    "tool_valid": True,
                    "cost_usd": 0.125,
                    "e2e_ms": 1000.0,
                    "label_source": secret_label_source,
                    "label_confidence": 0.8,
                }
            )["revision_id"]

            first_rows, first_counters, first_snapshot = collect_feedback_rows(
                feedback_path,
                catalog,
                max_revision_id=revision_one,
            )
            self.assertEqual(len(first_rows), 1)
            first_row = first_rows[0]
            self.assertEqual(first_row.source_kind, "feedback_v3")
            self.assertFalse(first_row.catalog_match)
            self.assertEqual(first_row.model_id, "future-provider/future-model-2035")
            np.testing.assert_array_equal(
                first_row.label_mask,
                np.asarray([1, 0, 1, 1, 1], dtype=np.uint8),
            )
            np.testing.assert_allclose(
                first_row.labels,
                np.asarray([0.0, 0.0, 1.0, 0.0, 0.25], dtype=np.float32),
            )
            np.testing.assert_array_equal(
                first_row.precomputed_task_features, task_vector
            )
            self.assertEqual(
                first_counters["feedback_task_success_observations_unmapped"], 1
            )
            self.assertEqual(first_snapshot["included_revision_id"], revision_one)
            self.assertEqual(
                first_snapshot["outcome_source_table"],
                "route_outcome_revisions",
            )

            second_rows, _, second_snapshot = collect_feedback_rows(
                feedback_path,
                catalog,
                max_revision_id=revision_two,
            )
            self.assertEqual(len(second_rows), 1)
            np.testing.assert_array_equal(
                second_rows[0].label_mask,
                np.ones(5, dtype=np.uint8),
            )
            np.testing.assert_allclose(
                second_rows[0].labels,
                np.asarray([1.0, 0.0, 0.0, 0.125, 1.0], dtype=np.float32),
            )
            self.assertNotEqual(
                first_snapshot["logical_snapshot_sha256"],
                second_snapshot["logical_snapshot_sha256"],
            )
            with self.assertRaisesRegex(ValueError, "exceeds observed maximum"):
                collect_feedback_rows(
                    feedback_path,
                    catalog,
                    max_revision_id=revision_two + 1,
                )

            base_rows = self._rows(catalog, "baseline fixture prompt")
            combined = base_rows + first_rows
            combined.sort(
                key=lambda row: (
                    row.prompt_hash,
                    row.model_id,
                    row.event_epoch,
                    row.row_id,
                )
            )
            arrays = arrays_from_rows(combined, catalog, 512, 128, 0)
            attach_feedback_snapshot_arrays(arrays, first_snapshot)
            feedback_index = arrays["row_sources"].tolist().index("feedback_v3")
            self.assertEqual(
                arrays["row_label_provenance"][feedback_index, 0],
                "feedback.route_outcome_revisions.http_success",
            )
            self.assertAlmostEqual(
                float(arrays["row_label_confidence"][feedback_index]), 0.75
            )
            self.assertEqual(
                int(arrays["feedback_source_max_revision_id"][0]), revision_one
            )

            artifact_one = root / "with-feedback-one.npz"
            artifact_two = root / "with-feedback-two.npz"
            save_npz_deterministic(artifact_one, arrays)
            save_npz_deterministic(artifact_two, arrays)
            self.assertEqual(artifact_one.read_bytes(), artifact_two.read_bytes())
            metadata = build_metadata(
                arrays,
                catalog,
                {"source_event_max_id": 2, **first_counters},
                512,
                128,
                0,
                hashlib.sha256(artifact_one.read_bytes()).hexdigest(),
                Path("baseline.db"),
                Path("catalog.json"),
                first_snapshot,
            )
            emitted = (
                artifact_one.read_bytes()
                + json.dumps(metadata, sort_keys=True).encode()
            )
            for secret in (secret_prompt, secret_request_id, secret_label_source):
                self.assertNotIn(secret.encode(), emitted)

            # A post-cutoff correction cannot alter a revision-one replay.
            store.record_outcome(
                {
                    "request_id": secret_request_id,
                    "http_success": True,
                    "cost_usd": 999.0,
                    "label_source": "later-correction",
                    "label_confidence": 1.0,
                }
            )
            replay_rows, _, replay_snapshot = collect_feedback_rows(
                feedback_path,
                catalog,
                max_revision_id=revision_one,
                expected_snapshot_sha256=first_snapshot["logical_snapshot_sha256"],
            )
            np.testing.assert_array_equal(replay_rows[0].labels, first_row.labels)
            self.assertEqual(replay_snapshot, first_snapshot)

            connection = sqlite3.connect(str(feedback_path))
            connection.execute(
                "UPDATE route_request_features SET feature_sha256 = ?",
                ("0" * 64,),
            )
            connection.commit()
            connection.close()
            with self.assertRaisesRegex(ValueError, "feature SHA-256 mismatch"):
                collect_feedback_rows(
                    feedback_path,
                    catalog,
                    max_revision_id=revision_one,
                )

            valid_feature_hash = hashlib.sha256(
                task_vector.astype("<f4", copy=False).tobytes(order="C")
            ).hexdigest()
            connection = sqlite3.connect(str(feedback_path))
            connection.execute(
                "UPDATE route_request_features "
                "SET feature_sha256 = ?, task_feature_version = ?",
                (valid_feature_hash, "chimera-task-wrong-v0"),
            )
            connection.commit()
            connection.close()
            with self.assertRaisesRegex(ValueError, "feature version mismatch"):
                collect_feedback_rows(
                    feedback_path,
                    catalog,
                    max_revision_id=revision_one,
                )

            connection = sqlite3.connect(str(feedback_path))
            connection.execute(
                "UPDATE route_request_features "
                "SET task_feature_version = ?, task_dim = ?",
                (TASK_FEATURE_VERSION, DEFAULT_TASK_DIM - 1),
            )
            connection.commit()
            connection.close()
            with self.assertRaisesRegex(ValueError, "feature dimension mismatch"):
                collect_feedback_rows(
                    feedback_path,
                    catalog,
                    max_revision_id=revision_one,
                )

            connection = sqlite3.connect(str(feedback_path))
            connection.execute(
                "UPDATE route_request_features SET task_dim = ?",
                (DEFAULT_TASK_DIM,),
            )
            connection.execute("INSERT INTO router_schema(version) VALUES (4)")
            connection.commit()
            connection.close()
            with self.assertRaisesRegex(ValueError, "feedback schema mismatch"):
                collect_feedback_rows(
                    feedback_path,
                    catalog,
                    max_revision_id=revision_one,
                )

    def test_empty_feedback_snapshot_has_zero_high_water_and_provenance(self) -> None:
        catalog = CatalogIndex(_catalog_payload())
        with tempfile.TemporaryDirectory() as directory:
            feedback_path = Path(directory) / "empty-feedback.db"
            FeedbackStore(feedback_path)
            feedback_rows, counters, snapshot = collect_feedback_rows(
                feedback_path,
                catalog,
                max_revision_id=0,
            )
            self.assertEqual(feedback_rows, [])
            self.assertEqual(counters["feedback_rows_imported"], 0)
            self.assertEqual(snapshot["included_revision_id"], 0)

            baseline_rows = self._rows(catalog, "empty feedback fixture")
            arrays = arrays_from_rows(baseline_rows, catalog, 512, 128, 0)
            attach_feedback_snapshot_arrays(arrays, snapshot)
            self.assertEqual(arrays["row_sources"].tolist(), ["baseline", "baseline"])
            self.assertEqual(int(arrays["feedback_source_max_revision_id"][0]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
