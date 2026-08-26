#!/usr/bin/env python3
"""Regression tests for the NumPy Chimera candidate router."""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

import numpy as np

try:
    from .ensemble import (
        BinaryCalibrator,
        RouterEnsemble,
        RouterTrainingData,
        _bootstrap_row_weights,
        fit_router_ensemble,
        prepare_catalog_quality_data,
    )
    from .evaluate import build_promotion_report, holdout_partition_integrity
    from .features import (
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        model_feature_names,
        task_feature_names,
    )
    from .model import CandidateRouter
    from .route import estimated_live_cost, route_request
    from .serve import RouterState
    from .train import load_dataset, main as train_main
except ImportError:  # Direct execution from this directory.
    from ensemble import (
        BinaryCalibrator,
        RouterEnsemble,
        RouterTrainingData,
        _bootstrap_row_weights,
        fit_router_ensemble,
        prepare_catalog_quality_data,
    )
    from evaluate import build_promotion_report, holdout_partition_integrity
    from features import (
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        model_feature_names,
        task_feature_names,
    )
    from model import CandidateRouter
    from route import estimated_live_cost, route_request
    from serve import RouterState
    from train import load_dataset, main as train_main


OUTCOMES = (
    "completion_success",
    "provider_failure",
    "tool_error",
    "cost_usd",
    "latency_s",
)


def _sigmoid(value: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-value))


def _zero_router(task_dim: int = 512, model_dim: int = 128) -> CandidateRouter:
    router = CandidateRouter(
        task_dim=task_dim,
        model_dim=model_dim,
        outcome_names=OUTCOMES,
        rank=4,
        seed=1,
        task_feature_names=(
            task_feature_names(task_dim)
            if task_dim >= 32
            else ["task_%d" % i for i in range(task_dim)]
        ),
        model_feature_names=(
            model_feature_names(model_dim)
            if model_dim >= 48
            else ["model_%d" % i for i in range(model_dim)]
        ),
        head_types=(
            "probability",
            "probability",
            "probability",
            "continuous",
            "continuous",
        ),
    )
    for name in router.parameter_names:
        getattr(router, name).fill(0.0)
    # A neutral but non-pathological default outcome.
    router.output_bias[:3] = np.asarray([1.4, -2.2, -2.2])
    router.outcome_mean[3:] = np.asarray([0.002, 0.5])
    router.training_metadata.update(
        {
            "task_feature_version": TASK_FEATURE_VERSION,
            "model_feature_version": MODEL_FEATURE_VERSION,
        }
    )
    return router


def _finite_router(task_dim: int = 2, model_dim: int = 3) -> CandidateRouter:
    """Return a hand-authored router whose complete export state is finite."""

    router = _zero_router(task_dim, model_dim)
    router.task_train_min.fill(-1.0)
    router.task_train_max.fill(1.0)
    router.model_train_min.fill(-1.0)
    router.model_train_max.fill(1.0)
    return router


def _small_ensemble_data(test_labels=None) -> RouterTrainingData:
    rng = np.random.default_rng(404)
    rows = 80
    task = rng.normal(size=(rows, 2))
    model = rng.normal(size=(rows, 3))
    logits = 1.3 * task[:, 0] * model[:, 0] - 0.4 * model[:, 1]
    labels = (_sigmoid(logits) > 0.5).astype(np.float64)[:, None]
    if test_labels is not None:
        labels[60:, 0] = np.asarray(test_labels, dtype=np.float64)
    return RouterTrainingData(
        task=task,
        model=model,
        labels=labels,
        weights=np.ones(rows, dtype=np.float64),
        train_indices=np.arange(0, 40, dtype=np.int64),
        validation_indices=np.arange(40, 60, dtype=np.int64),
        test_indices=np.arange(60, 80, dtype=np.int64),
        outcome_names=("completion_success",),
        head_types=("probability",),
        pair_indices=None,
        pair_preferences=None,
        pair_weights=None,
        pair_head=None,
        auxiliary={
            "metadata": {
                "task_feature_version": "test-task-v1",
                "model_feature_version": "test-model-v1",
            },
            "task_feature_names": ["task_0", "task_1"],
            "model_feature_names": ["model_0", "model_1", "model_2"],
            "group_ids": np.repeat(np.arange(40), 2),
        },
        split_name="temporal",
    )


class CandidateRouterModelTests(unittest.TestCase):
    def test_manual_gradient_matches_finite_difference(self) -> None:
        router = CandidateRouter(
            2,
            2,
            ("completion_success", "latency_s"),
            rank=2,
            seed=3,
            head_types=("probability", "positive"),
        )
        task = np.asarray([[0.2, -0.4], [1.0, 0.3], [-0.5, 0.8]])
        model = np.asarray([[0.1, 0.7], [-0.3, 0.2], [0.9, -0.1]])
        labels = np.asarray([[1.0, 2.0], [0.0, 0.4], [1.0, 1.2]])
        router.outcome_mean[1] = float(np.mean(np.log1p(labels[:, 1])))
        router.outcome_scale[1] = float(np.std(np.log1p(labels[:, 1])))
        _, gradient = router._point_loss_and_gradient(
            task, model, labels, np.ones(3), huber_delta=1.0
        )
        epsilon = 1.0e-6
        original = float(router.task_tower[0, 0])
        router.task_tower[0, 0] = original + epsilon
        plus, _ = router._point_loss_and_gradient(task, model, labels, np.ones(3), 1.0)
        router.task_tower[0, 0] = original - epsilon
        minus, _ = router._point_loss_and_gradient(task, model, labels, np.ones(3), 1.0)
        router.task_tower[0, 0] = original
        numerical = (plus - minus) / (2.0 * epsilon)
        self.assertAlmostEqual(float(gradient["task_tower"][0, 0]), numerical, places=7)

    def test_model_skip_gradient_matches_finite_difference(self) -> None:
        """Guard the direct candidate skip path independently of the towers."""

        router = CandidateRouter(
            2,
            2,
            ("completion_success",),
            rank=2,
            seed=17,
            head_types=("probability",),
        )
        task = np.asarray([[0.2, -0.4], [1.0, 0.3], [-0.5, 0.8]])
        model = np.asarray([[0.1, 0.7], [-0.3, 0.2], [0.9, -0.1]])
        labels = np.asarray([[1.0], [0.0], [1.0]])
        _, gradient = router._point_loss_and_gradient(
            task, model, labels, np.ones(3), huber_delta=1.0
        )
        epsilon = 1.0e-6
        original = float(router.model_skip[0, 0])
        router.model_skip[0, 0] = original + epsilon
        plus, _ = router._point_loss_and_gradient(task, model, labels, np.ones(3), 1.0)
        router.model_skip[0, 0] = original - epsilon
        minus, _ = router._point_loss_and_gradient(task, model, labels, np.ones(3), 1.0)
        router.model_skip[0, 0] = original
        numerical = (plus - minus) / (2.0 * epsilon)
        self.assertAlmostEqual(float(gradient["model_skip"][0, 0]), numerical, places=7)

    def test_fit_save_load_and_unseen_candidate(self) -> None:
        rng = np.random.default_rng(19)
        rows = 640
        task = rng.normal(size=(rows, 3))
        model = rng.normal(size=(rows, 4))
        interaction = 2.8 * task[:, 0] * model[:, 0] + 1.4 * task[:, 1] * model[:, 1]
        success = _sigmoid(interaction)
        provider_failure = _sigmoid(-2.0 - interaction)
        tool_error = _sigmoid(-2.5 + 0.5 * task[:, 2] - 0.3 * model[:, 1])
        cost = 0.002 + 0.001 * np.maximum(model[:, 2] + 2.0, 0.0)
        latency = 0.3 + 0.1 * np.maximum(model[:, 3] + 2.0, 0.0)
        labels = np.column_stack((success, provider_failure, tool_error, cost, latency))
        train = np.arange(0, 540, dtype=np.int64)
        validation = np.arange(540, rows, dtype=np.int64)

        router = CandidateRouter(
            3,
            4,
            OUTCOMES,
            rank=8,
            seed=13,
            head_types=(
                "probability",
                "probability",
                "probability",
                "continuous",
                "continuous",
            ),
        )
        result = router.fit(
            task,
            model,
            labels,
            train_indices=train,
            validation_indices=validation,
            epochs=140,
            batch_size=96,
            learning_rate=0.012,
            patience=30,
            seed=13,
        )
        self.assertGreater(result.best_epoch, 0)
        predicted = router.predict(task[validation], model[validation])[:, 0]
        correlation = float(np.corrcoef(predicted, labels[validation, 0])[0, 1])
        self.assertGreater(correlation, 0.80)

        with tempfile.TemporaryDirectory() as temporary:
            artifact = Path(temporary) / "router.npz"
            router.save(
                str(artifact),
                {
                    "test": True,
                    "task_feature_version": TASK_FEATURE_VERSION,
                    "model_feature_version": MODEL_FEATURE_VERSION,
                },
            )
            loaded = CandidateRouter.load(str(artifact))
            np.testing.assert_allclose(
                loaded.predict(task[validation], model[validation]),
                router.predict(task[validation], model[validation]),
                rtol=0.0,
                atol=0.0,
            )
            self.assertTrue(loaded.training_metadata["test"])

            request = {"task_features": [1.0, 0.0, 0.0]}
            catalog = [
                {
                    "id": "future/vendor-good",
                    "features": [1.2, 0.0, 0.0, 0.0],
                    "context_length": 4096,
                    "architecture": {
                        "input_modalities": ["text"],
                        "output_modalities": ["text"],
                    },
                },
                {
                    "id": "future/vendor-bad",
                    "features": [-1.2, 0.0, 0.0, 0.0],
                    "context_length": 4096,
                    "architecture": {
                        "input_modalities": ["text"],
                        "output_modalities": ["text"],
                    },
                },
            ]
            decision = route_request(
                loaded,
                request,
                catalog,
                policy={
                    "cost_weight": 0,
                    "latency_weight": 0,
                    "failure_weight": 0,
                    "preference_weight": 0,
                    "uncertainty_weight": 0,
                    "missing_metadata_penalty": 0,
                    "require_known_price": False,
                },
            )
            self.assertEqual(decision["selected_model"], "future/vendor-good")

    def test_seed_is_deterministic(self) -> None:
        rng = np.random.default_rng(4)
        task = rng.normal(size=(80, 2))
        model = rng.normal(size=(80, 2))
        labels = _sigmoid((task[:, :1] * model[:, :1]) * 2.0)
        routers = []
        for _ in range(2):
            router = CandidateRouter(
                2,
                2,
                ("completion_success",),
                rank=4,
                seed=5,
                head_types=("probability",),
            )
            router.fit(task, model, labels, epochs=8, batch_size=32, seed=5, patience=0)
            routers.append(router)
        np.testing.assert_array_equal(
            routers[0].predict(task, model), routers[1].predict(task, model)
        )

    def test_pair_only_head_learns_and_validates_preferences(self) -> None:
        rng = np.random.default_rng(91)
        pair_count = 50
        task = np.ones((pair_count * 2, 2), dtype=np.float64)
        stronger = rng.uniform(0.4, 1.5, size=pair_count)
        weaker = rng.uniform(-1.5, -0.4, size=pair_count)
        model = np.zeros((pair_count * 2, 2), dtype=np.float64)
        model[0::2, 0] = stronger
        model[1::2, 0] = weaker
        labels = np.full((pair_count * 2, 1), np.nan, dtype=np.float64)
        pairs = np.column_stack(
            (np.arange(0, pair_count * 2, 2), np.arange(1, pair_count * 2, 2))
        )
        preferences = np.ones(pair_count, dtype=np.float64)
        # A tie should be excluded rather than silently treated as "left".
        preferences[0] = 0.5
        train = np.arange(0, 80, dtype=np.int64)
        validation = np.arange(80, 100, dtype=np.int64)
        router = CandidateRouter(
            2,
            2,
            ("preference_score",),
            rank=3,
            seed=8,
            head_types=("continuous",),
        )
        router.fit(
            task,
            model,
            labels,
            train_indices=train,
            validation_indices=validation,
            pair_indices=pairs,
            pair_labels=preferences,
            pair_head="preference_score",
            pair_loss_weight=1.0,
            epochs=80,
            batch_size=40,
            learning_rate=0.02,
            patience=20,
            seed=8,
        )
        raw = router.raw_scores(task, model)[:, 0]
        validation_pairs = pairs[40:]
        accuracy = np.mean(raw[validation_pairs[:, 0]] > raw[validation_pairs[:, 1]])
        self.assertGreater(float(accuracy), 0.9)
        self.assertEqual(router.training_metadata["pair_rows"], 39)
        self.assertEqual(router.training_metadata["validation_pair_rows"], 10)


class RouterEnsembleTests(unittest.TestCase):
    def test_binary_calibration_is_monotone_and_improves_validation_brier(self) -> None:
        labels = np.asarray([0.0] * 30 + [1.0] * 30)[:, None]
        # Correctly ordered but severely overconfident toward the positive class.
        predictions = np.linspace(0.60, 0.98, 60)[:, None]
        calibrator = BinaryCalibrator.fit(
            predictions,
            labels,
            ("probability",),
            ("completion_success",),
            validation_indices_hash="validation-only-test",
        )
        calibrated = calibrator.transform(predictions)
        before = float(np.mean((predictions - labels) ** 2))
        after = float(np.mean((calibrated - labels) ** 2))
        self.assertTrue(calibrator.active[0])
        self.assertGreaterEqual(calibrator.slopes[0], 0.0)
        self.assertLess(after, before)
        self.assertEqual(
            calibrator.metadata["validation_indices_sha256"],
            "validation-only-test",
        )

    def test_distribution_save_load_determinism_and_single_model_compatibility(
        self,
    ) -> None:
        first = _finite_router()
        second = _finite_router()
        second.output_bias[0] += 0.8
        ensemble = RouterEnsemble(
            [first, second],
            BinaryCalibrator.identity(first.head_types),
            {"artifact_role": "test_ensemble"},
        )
        task = np.asarray([[0.2, 0.1], [-0.4, 0.7]])
        model = np.asarray([[0.3, -0.1, 0.6], [-0.5, 0.2, 0.0]])
        distribution = ensemble.predict_distribution(task, model)
        self.assertTrue(
            all(np.all(np.isfinite(value)) for value in distribution.values())
        )
        self.assertTrue(np.any(distribution["std"][:, 0] > 0.0))
        calibrated_members = ensemble.calibrator.transform_members(
            ensemble.predict_members(task, model)
        )
        np.testing.assert_array_equal(
            distribution["mean"], np.mean(calibrated_members, axis=0)
        )
        np.testing.assert_array_equal(
            distribution["calibrated_member_std"],
            np.std(calibrated_members, axis=0),
        )
        self.assertTrue(
            np.all(distribution["std"] >= distribution["calibrated_member_std"])
        )
        self.assertTrue(np.all(distribution["std"] >= distribution["uncalibrated_std"]))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first_path = root / "ensemble-a.npz"
            second_path = root / "ensemble-b.npz"
            ensemble.save(str(first_path))
            ensemble.save(str(second_path))
            self.assertEqual(first_path.read_bytes(), second_path.read_bytes())
            loaded = RouterEnsemble.load(str(first_path))
            for key, value in distribution.items():
                np.testing.assert_array_equal(
                    loaded.predict_distribution(task, model)[key], value
                )

            legacy_path = root / "legacy.npz"
            first.save(str(legacy_path))
            legacy = RouterEnsemble.load(str(legacy_path))
            self.assertEqual(len(legacy.members), 1)
            self.assertTrue(legacy.training_metadata["single_model_compatibility"])
            np.testing.assert_array_equal(
                legacy.predict(task, model), first.predict(task, model)
            )

    def test_nonfinite_member_is_rejected(self) -> None:
        router = _finite_router()
        router.model_skip[0, 0] = np.nan
        with self.assertRaisesRegex(ValueError, "non-finite member"):
            RouterEnsemble([router])

        invalid_bounds = _finite_router()
        invalid_bounds.task_train_min[0] = np.inf
        invalid_bounds.task_train_max[0] = np.inf
        with self.assertRaisesRegex(ValueError, "invalid member"):
            RouterEnsemble([invalid_bounds])

        invalid_scale = _finite_router()
        invalid_scale.model_scale[0] = 0.0
        with self.assertRaisesRegex(ValueError, "nonpositive member"):
            RouterEnsemble([invalid_scale])

    def test_malformed_or_nonmonotone_calibrator_is_rejected(self) -> None:
        router = _finite_router()
        malformed = BinaryCalibrator(
            slopes=np.ones(5),
            intercepts=np.zeros(4),
            active=np.zeros(5, dtype=bool),
            head_types=router.head_types,
            metadata={},
        )
        with self.assertRaisesRegex(ValueError, "calibrator contract"):
            RouterEnsemble([router], malformed)
        nonmonotone = BinaryCalibrator.identity(router.head_types)
        nonmonotone.slopes[0] = -1.0
        with self.assertRaisesRegex(ValueError, "nonnegative"):
            RouterEnsemble([router], nonmonotone)

    def test_constant_calibration_cannot_erase_bootstrap_uncertainty(self) -> None:
        first = _finite_router()
        second = _finite_router()
        second.output_bias[0] += 1.0
        calibrator = BinaryCalibrator.identity(first.head_types)
        calibrator.slopes[0] = 0.0
        calibrator.intercepts[0] = -2.0
        calibrator.active[0] = True
        ensemble = RouterEnsemble([first, second], calibrator)
        distribution = ensemble.predict_distribution(
            np.asarray([[0.0, 0.0]]), np.asarray([[0.0, 0.0, 0.0]])
        )
        self.assertEqual(float(distribution["calibrated_member_std"][0, 0]), 0.0)
        self.assertGreater(float(distribution["uncalibrated_std"][0, 0]), 0.0)
        self.assertEqual(
            float(distribution["std"][0, 0]),
            float(distribution["uncalibrated_std"][0, 0]),
        )

    def test_quality_artifact_suppresses_benchmark_columns_at_inference(self) -> None:
        router = _finite_router()
        router.model_skip[1, 0] = 2.0
        ensemble = RouterEnsemble(
            [router],
            metadata={
                "artifact_role": "catalog_quality_predictor",
                "benchmark_feature_columns_zeroed": [1],
            },
        )
        task = np.asarray([[0.0, 0.0]])
        baseline_model = np.asarray([[0.1, 0.0, -0.2]])
        leaked_model = np.asarray([[0.1, 9.0, -0.2]])
        np.testing.assert_array_equal(
            ensemble.predict(task, baseline_model),
            ensemble.predict(task, leaked_model),
        )
        np.testing.assert_array_equal(
            ensemble.raw_scores(task, baseline_model),
            ensemble.raw_scores(task, leaked_model),
        )
        np.testing.assert_array_equal(
            ensemble.support_distance(task, baseline_model),
            ensemble.support_distance(task, leaked_model),
        )

    def test_bootstrap_preserves_validation_weights_and_validation_loss(self) -> None:
        base = np.linspace(1.0, 2.0, 8)
        train = np.arange(0, 4, dtype=np.int64)
        groups = np.asarray([0, 0, 1, 1, 2, 2, 3, 3])
        selected, weights, multipliers = _bootstrap_row_weights(train, groups, base, 9)
        self.assertGreater(selected.size, 0)
        np.testing.assert_array_equal(weights[4:], base[4:])
        self.assertTrue(np.all(multipliers[selected] > 0.0))

        ensemble = fit_router_ensemble(
            _small_ensemble_data(),
            members=1,
            rank=3,
            epochs=3,
            batch_size=16,
            patience=0,
            pair_loss_weight=0.0,
            seed=31,
        )
        metadata = ensemble.members[0].training_metadata
        self.assertEqual(metadata["validation_rows"], 20)
        self.assertTrue(np.isfinite(metadata["best_validation_loss"]))
        self.assertGreater(metadata["best_validation_loss"], 0.0)

    def test_test_labels_do_not_affect_fit_or_validation_calibration(self) -> None:
        configurations = []
        for test_labels in (np.zeros(20), np.ones(20)):
            ensemble = fit_router_ensemble(
                _small_ensemble_data(test_labels),
                members=1,
                rank=3,
                epochs=4,
                batch_size=16,
                patience=0,
                pair_loss_weight=0.0,
                seed=41,
            )
            configurations.append(ensemble)
        np.testing.assert_array_equal(
            configurations[0].calibrator.slopes,
            configurations[1].calibrator.slopes,
        )
        np.testing.assert_array_equal(
            configurations[0].calibrator.intercepts,
            configurations[1].calibrator.intercepts,
        )
        self.assertEqual(
            configurations[0].calibrator.metadata["validation_indices_sha256"],
            configurations[1].calibrator.metadata["validation_indices_sha256"],
        )
        for name in configurations[0].members[0].parameter_names:
            np.testing.assert_array_equal(
                getattr(configurations[0].members[0], name),
                getattr(configurations[1].members[0], name),
            )
        with tempfile.TemporaryDirectory() as temporary:
            first_path = Path(temporary) / "first.npz"
            second_path = Path(temporary) / "second.npz"
            configurations[0].save(str(first_path))
            configurations[1].save(str(second_path))
            self.assertEqual(first_path.read_bytes(), second_path.read_bytes())

    def test_promotion_gate_is_fail_closed_without_holdouts_or_pairs(self) -> None:
        data = _small_ensemble_data()
        ensemble = fit_router_ensemble(
            data,
            members=1,
            rank=2,
            epochs=2,
            batch_size=16,
            patience=0,
            pair_loss_weight=0.0,
            seed=55,
        )
        report = build_promotion_report(ensemble, data)
        self.assertEqual(report["promotion"]["decision"], "hold")
        failed = {
            gate["name"] for gate in report["promotion"]["gates"] if not gate["passed"]
        }
        self.assertIn("minimum_common_test_pairs", failed)
        self.assertIn("strict_cold_start_holdouts_available", failed)
        self.assertIn("promotion_provenance_verified", failed)
        self.assertIsNone(report["provenance"]["test_labels_used_for_calibration"])

    def test_holdout_integrity_requires_complete_valid_fold_partition(self) -> None:
        data = _small_ensemble_data()
        data = replace(
            data,
            split_name="model-holdout",
            train_indices=np.arange(0, 40, dtype=np.int64),
            validation_indices=np.arange(40, 60, dtype=np.int64),
            test_indices=np.arange(60, 80, dtype=np.int64),
        )
        ids = np.asarray(["model-%d" % (index // 20) for index in range(80)])
        folds = np.asarray([0] * 40 + [8] * 20 + [9] * 20)
        data.auxiliary["model_holdout_ids"] = ids
        data.auxiliary["model_holdout_fold"] = folds
        integrity = holdout_partition_integrity(data, "model")
        self.assertTrue(integrity["passed"])

        incomplete_data = replace(data, test_indices=np.arange(60, 79, dtype=np.int64))
        incomplete = holdout_partition_integrity(incomplete_data, "model")
        self.assertFalse(incomplete["passed"])
        self.assertFalse(incomplete["rows_complete"])

        data.auxiliary["model_holdout_fold"] = folds.copy()
        data.auxiliary["model_holdout_fold"][0] = 10
        invalid_fold = holdout_partition_integrity(data, "model")
        self.assertFalse(invalid_fold["passed"])
        self.assertFalse(invalid_fold["fold_domain_0_through_9"])

    def test_catalog_quality_lane_removes_benchmark_features_and_rejects_raw_scale(
        self,
    ) -> None:
        feature_names = np.asarray(
            [
                "numeric:price",
                "benchmark_intelligence_index",
                "numeric:context",
                "artificial_analysis_coding_index",
                "numeric:tools",
                "has_benchmark_agentic_index",
            ]
        )
        quality_rows = 30
        quality_features = np.ones((quality_rows, feature_names.size), dtype=np.float32)
        quality_labels = np.tile(
            np.asarray([[0.75, 0.6, 0.55]], dtype=np.float32), (quality_rows, 1)
        )
        quality_folds = np.asarray([0] * 20 + [8] * 5 + [9] * 5, dtype=np.uint8)

        def write_dataset(path: Path, labels: np.ndarray) -> None:
            np.savez_compressed(
                path,
                task_features=np.zeros((12, 4), dtype=np.float32),
                model_features=np.zeros((12, feature_names.size), dtype=np.float32),
                labels=np.zeros((12, 5), dtype=np.float32),
                task_feature_names=np.asarray(
                    ["task_%d" % index for index in range(4)]
                ),
                model_feature_names=feature_names,
                catalog_quality_model_ids=np.asarray(
                    ["vendor/model-%d" % index for index in range(quality_rows)]
                ),
                catalog_quality_features=quality_features,
                catalog_quality_labels=labels,
                catalog_quality_label_mask=np.ones_like(labels, dtype=np.uint8),
                catalog_quality_label_names=np.asarray(
                    [
                        "general_intelligence_quality",
                        "coding_quality",
                        "agentic_quality",
                    ]
                ),
                catalog_quality_label_provenance=np.full(
                    labels.shape, "openrouter.aa.test", dtype="U32"
                ),
                catalog_quality_model_holdout_fold=quality_folds,
                catalog_quality_provider_holdout_fold=quality_folds,
                catalog_quality_family_holdout_fold=quality_folds,
            )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            normalized_path = root / "normalized.npz"
            write_dataset(normalized_path, quality_labels)
            quality = prepare_catalog_quality_data(
                str(normalized_path), split_axis="family"
            )
            self.assertIsNotNone(quality)
            assert quality is not None
            self.assertEqual(
                quality.auxiliary["quality_leakage_columns_zeroed"], [1, 3, 5]
            )
            np.testing.assert_array_equal(quality.model[:, [1, 3, 5]], 0.0)
            self.assertEqual(
                quality.auxiliary["quality_leakage_nonzero_before_zeroing"],
                quality_rows * 3,
            )

            raw_scale_path = root / "raw-scale.npz"
            raw_scale = quality_labels.copy()
            raw_scale[0, 0] = 75.0
            write_dataset(raw_scale_path, raw_scale)
            with self.assertRaisesRegex(ValueError, "normalized to \\[0, 1\\]"):
                prepare_catalog_quality_data(str(raw_scale_path), split_axis="family")


class RoutePolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.router = _zero_router()
        self.catalog = [
            {
                "id": "vendor/eligible",
                "context_length": 131072,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "supported_parameters": ["tools", "response_format"],
                "pricing": {"prompt": "0.000001", "completion": "0.000002"},
            },
            {
                "id": "vendor/no-tools",
                "context_length": 131072,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "supported_parameters": ["temperature"],
                "pricing": {"prompt": "0", "completion": "0"},
            },
            {
                "id": "vendor/small-context",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "supported_parameters": ["tools"],
                "pricing": {"prompt": "0", "completion": "0"},
            },
            {
                "id": "vendor/image-output",
                "context_length": 131072,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["image"],
                },
                "supported_parameters": ["tools"],
                "pricing": {"prompt": "0", "completion": "0"},
            },
            {
                "id": "vendor/cheap:batch",
                "context_length": 131072,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "supported_parameters": ["tools"],
                "pricing": {"prompt": "0", "completion": "0"},
            },
        ]

    def test_hard_filters_precede_utility(self) -> None:
        decision = route_request(
            self.router,
            {"prompt": "use a tool", "estimated_prompt_tokens": 1000},
            self.catalog,
            policy={"min_context": 100000, "required_parameters": ["tools"]},
            now_epoch=1_700_000_000,
        )
        self.assertEqual(decision["selected_model"], "vendor/eligible")
        rejected = {
            item["model_id"]: item["rejection_reasons"]
            for item in decision["candidates"]
            if not item["feasible"]
        }
        self.assertIn("missing_parameter:tools", rejected["vendor/no-tools"])
        self.assertIn("context_too_small", rejected["vendor/small-context"])
        self.assertIn("missing_output_modality:text", rejected["vendor/image-output"])
        self.assertIn("batch_variant_not_allowed", rejected["vendor/cheap:batch"])

    def test_explicit_override_is_authoritative_and_auditable(self) -> None:
        decision = route_request(
            self.router,
            {"prompt": "use a tool"},
            self.catalog,
            policy={"min_context": 100000, "required_parameters": ["tools"]},
            explicit_model="openrouter/vendor/no-tools",
            now_epoch=1_700_000_000,
        )
        self.assertEqual(decision["selected_model"], "vendor/no-tools")
        self.assertTrue(decision["explicit_override"])
        self.assertIn("missing_parameter:tools", decision["override_violations"])

    def test_override_missing_from_catalog_is_still_honored(self) -> None:
        decision = route_request(
            self.router,
            {"prompt": "hello"},
            self.catalog,
            explicit_model="future/model-2039",
        )
        self.assertEqual(decision["selected_model"], "future/model-2039")
        self.assertFalse(decision["catalog_match"])
        self.assertEqual(decision["override_violations"], ["model_not_in_catalog"])

    def test_current_catalog_cost_calculation(self) -> None:
        cost = estimated_live_cost(
            self.catalog[0], prompt_tokens=1000, completion_tokens=500
        )
        self.assertAlmostEqual(cost or 0.0, 0.002)

    def test_missing_token_counts_still_choose_cheaper_live_model(self) -> None:
        catalog = [
            {
                "id": "vendor/expensive",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "pricing": {"prompt": "0.0001", "completion": "0.0002"},
            },
            {
                "id": "vendor/cheap",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "pricing": {"prompt": "0.000001", "completion": "0.000002"},
            },
        ]
        decision = route_request(
            self.router, {"prompt": "Summarize this short text."}, catalog
        )
        self.assertEqual(decision["selected_model"], "vendor/cheap")
        self.assertGreater(decision["token_estimates"]["prompt_tokens"], 0)
        self.assertEqual(decision["token_estimates"]["completion_tokens"], 512)

    def test_live_task_benchmark_prevents_tiny_completion_model_from_looking_best(
        self,
    ) -> None:
        catalog = [
            {
                "id": "vendor/tiny-free",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "pricing": {"prompt": "0", "completion": "0"},
                "benchmarks": {
                    "artificial_analysis": {
                        "intelligence_index": 8,
                        "coding_index": 5,
                        "agentic_index": 2,
                    }
                },
            },
            {
                "id": "vendor/capable-cheap",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "pricing": {"prompt": "0.000001", "completion": "0.000002"},
                "benchmarks": {
                    "artificial_analysis": {
                        "intelligence_index": 70,
                        "coding_index": 85,
                        "agentic_index": 75,
                    }
                },
            },
        ]
        decision = route_request(
            self.router,
            {"prompt": "Implement and test a Python parser."},
            catalog,
        )
        self.assertEqual(decision["selected_model"], "vendor/capable-cheap")
        self.assertGreater(
            decision["candidates"][0]["benchmark_quality"],
            decision["candidates"][1]["benchmark_quality"],
        )

    def test_structured_output_capability_is_not_widened(self) -> None:
        catalog = [
            {
                "id": "vendor/response-format-only",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "supported_parameters": ["response_format"],
                "pricing": {"prompt": "0", "completion": "0"},
            },
            {
                "id": "vendor/strict-structured",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "supported_parameters": ["structured_outputs"],
                "pricing": {"prompt": "0.000001", "completion": "0.000001"},
            },
        ]
        decision = route_request(
            self.router,
            {
                "prompt": "return strict JSON",
                "required_parameters": ["structured_outputs"],
            },
            catalog,
        )
        self.assertEqual(decision["selected_model"], "vendor/strict-structured")

    def test_feature_contract_mismatch_fails_before_scoring(self) -> None:
        self.router.training_metadata["model_feature_version"] = (
            "future-incompatible-v99"
        )
        with self.assertRaisesRegex(ValueError, "feature contract mismatch"):
            route_request(self.router, {"prompt": "hello"}, self.catalog)


class DatasetContractTests(unittest.TestCase):
    def test_exact_npz_contract_trains_and_exports(self) -> None:
        rng = np.random.default_rng(23)
        rows = 48
        task = rng.normal(size=(rows, 512)).astype(np.float32)
        model = rng.normal(size=(rows, 128)).astype(np.float32)
        labels = np.column_stack(
            (
                rng.integers(0, 2, size=rows),
                rng.integers(0, 2, size=rows),
                rng.integers(0, 2, size=rows),
                rng.uniform(0.0, 0.02, size=rows),
                rng.uniform(0.1, 2.0, size=rows),
            )
        ).astype(np.float32)
        mask = np.ones_like(labels, dtype=np.uint8)
        mask[0, 4] = 0
        pairs = np.asarray(
            [[group * 4, group * 4 + 1] for group in range(12)], dtype=np.int64
        )
        preferences = (labels[pairs[:, 0], 0] >= labels[pairs[:, 1], 0]).astype(
            np.float32
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dataset = root / "router_dataset.npz"
            artifact = root / "router_model.npz"
            np.savez_compressed(
                dataset,
                task_features=task,
                model_features=model,
                labels=labels,
                label_mask=mask,
                model_ids=np.asarray(
                    ["vendor/model-%d" % (i % 4) for i in range(rows)]
                ),
                prompt_hashes=np.asarray(["%064x" % (i // 4) for i in range(rows)]),
                instance_ids=np.asarray(["instance-%d" % i for i in range(rows)]),
                group_ids=np.repeat(np.arange(12), 4),
                temporal_split=np.asarray([0] * 32 + [1] * 8 + [2] * 8, dtype=np.uint8),
                pair_indices=pairs,
                pair_deltas=labels[pairs[:, 0]] - labels[pairs[:, 1]],
                pair_preference=preferences,
            )
            sidecar = dataset.with_suffix(".json")
            sidecar.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "task_feature_version": TASK_FEATURE_VERSION,
                        "model_feature_version": MODEL_FEATURE_VERSION,
                        "task_feature_names": task_feature_names(),
                        "model_feature_names": model_feature_names(),
                        "label_names": list(OUTCOMES),
                        "catalog_snapshot_sha256": "0" * 64,
                    }
                ),
                encoding="utf-8",
            )
            loaded_task, loaded_model, loaded_labels, _, auxiliary = load_dataset(
                str(dataset)
            )
            self.assertEqual(loaded_task.shape, (rows, 512))
            self.assertEqual(loaded_model.shape, (rows, 128))
            self.assertTrue(np.isnan(loaded_labels[0, 4]))
            self.assertEqual(auxiliary["label_names"], list(OUTCOMES))

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = train_main(
                    [
                        str(dataset),
                        "--output",
                        str(artifact),
                        "--rank",
                        "4",
                        "--epochs",
                        "3",
                        "--batch-size",
                        "24",
                        "--patience",
                        "0",
                        "--quiet",
                    ]
                )
            self.assertEqual(status, 0)
            exported = CandidateRouter.load(str(artifact))
            self.assertIn("preference_score", exported.outcome_names)
            self.assertEqual(exported.task_dim, 512)
            self.assertEqual(exported.model_dim, 128)
            self.assertEqual(exported.head_types[3:5], ("positive", "positive"))
            positive_predictions = exported.predict(task[:4], model[:4])[:, 3:5]
            self.assertTrue(np.all(positive_predictions >= 0.0))
            summary = json.loads(output.getvalue())
            self.assertEqual(summary["format"], "chimera-router-numpy-v1")
            self.assertEqual(summary["split_kind"], "temporal")
            self.assertEqual(summary["train_rows"], 32)
            self.assertEqual(summary["validation_rows"], 8)
            self.assertEqual(summary["test_rows"], 8)


class RouterServiceTests(unittest.TestCase):
    def test_stable_service_contract_and_explicit_model(self) -> None:
        catalog = [
            {
                "id": "vendor/weak",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "pricing": {"prompt": "0", "completion": "0"},
                "benchmarks": {"artificial_analysis": {"intelligence_index": 5}},
            },
            {
                "id": "vendor/strong",
                "context_length": 4096,
                "architecture": {
                    "input_modalities": ["text"],
                    "output_modalities": ["text"],
                },
                "pricing": {"prompt": "0.000001", "completion": "0.000001"},
                "benchmarks": {"artificial_analysis": {"intelligence_index": 80}},
            },
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "router.npz"
            catalog_path = root / "catalog.json"
            _zero_router().save(
                str(artifact),
                {
                    "task_feature_version": TASK_FEATURE_VERSION,
                    "model_feature_version": MODEL_FEATURE_VERSION,
                },
            )
            catalog_path.write_text(json.dumps({"data": catalog}), encoding="utf-8")
            state = RouterState(artifact, catalog_path)
            self.assertEqual(state.health()["catalog_models"], 2)
            automatic = state.route(
                {
                    "model": "chimera/auto",
                    "messages": [{"role": "user", "content": "hello"}],
                }
            )
            self.assertEqual(automatic["selected_model"], "vendor/strong")
            self.assertEqual(
                automatic["openrouter_request_patch"]["models"][0], "vendor/strong"
            )
            explicit = state.route(
                {
                    "model": "vendor/weak",
                    "messages": [{"role": "user", "content": "hello"}],
                }
            )
            self.assertEqual(explicit["selected_model"], "vendor/weak")
            self.assertTrue(explicit["explicit_override"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
