#!/usr/bin/env python3
"""Evaluate Chimera Router v2 and emit a fail-closed promotion report.

The report is deterministic and machine-readable.  Calibration parameters are
already fit on validation rows by :mod:`ensemble`; this evaluator never refits
them on test.  Model/provider/family diagnostics train fresh ensembles whose
test IDs are excluded from training, rather than relabeling an in-sample score
as a cold-start metric.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import numpy as np

try:
    from .ensemble import (
        BinaryCalibrator,
        QUALITY_TASK_FEATURE_VERSION,
        RouterEnsemble,
        RouterTrainingData,
        fit_router_ensemble,
        prepare_catalog_quality_data,
        prepare_router_training_data,
    )
    from .route import RoutingPolicy
except ImportError:  # Direct execution.
    from ensemble import (
        BinaryCalibrator,
        QUALITY_TASK_FEATURE_VERSION,
        RouterEnsemble,
        RouterTrainingData,
        fit_router_ensemble,
        prepare_catalog_quality_data,
        prepare_router_training_data,
    )
    from route import RoutingPolicy


PROMOTION_REPORT_VERSION = 2


def _json_safe(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {str(key): _json_safe(child) for key, child in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(child) for child in value]
    if isinstance(value, np.ndarray):
        return _json_safe(value.tolist())
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def _write_json_atomic(path: Path, payload: Mapping[str, Any]) -> None:
    encoded = (json.dumps(_json_safe(payload), indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, path)
    finally:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass


def _ece(probability: np.ndarray, labels: np.ndarray, bins: int = 10) -> float:
    probability = np.asarray(probability, dtype=np.float64)
    labels = np.asarray(labels, dtype=np.float64)
    result = 0.0
    for index in range(bins):
        lower = index / float(bins)
        upper = (index + 1) / float(bins)
        mask = (probability >= lower) & (
            probability <= upper if index == bins - 1 else probability < upper
        )
        if np.any(mask):
            result += float(np.mean(mask)) * abs(
                float(np.mean(probability[mask])) - float(np.mean(labels[mask]))
            )
    return result


def _binary_metrics(probability: np.ndarray, labels: np.ndarray) -> Dict[str, float]:
    p = np.clip(np.asarray(probability, dtype=np.float64), 1.0e-9, 1.0 - 1.0e-9)
    y = np.asarray(labels, dtype=np.float64)
    return {
        "count": int(y.size),
        "brier": float(np.mean((p - y) ** 2)),
        "log_loss": float(np.mean(-(y * np.log(p) + (1.0 - y) * np.log1p(-p)))),
        "ece_10": _ece(p, y),
    }


def outcome_metrics(
    ensemble: RouterEnsemble, data: RouterTrainingData, indices: np.ndarray
) -> Dict[str, Any]:
    selected = np.asarray(indices, dtype=np.int64).reshape(-1)
    if selected.size == 0:
        return {"rows": 0, "available": False, "reason": "empty_split"}
    distribution = ensemble.predict_distribution(
        data.task[selected], data.model[selected]
    )
    calibrated = distribution["mean"]
    uncalibrated = distribution["uncalibrated_mean"]
    labels = data.labels[selected]
    result: Dict[str, Any] = {
        "rows": int(selected.size),
        "available": True,
        "finite": bool(
            np.all(np.isfinite(calibrated)) and np.all(np.isfinite(distribution["std"]))
        ),
        "mean_epistemic_std": float(np.mean(distribution["std"])),
        "heads": {},
    }
    aggregate_before = 0.0
    aggregate_after = 0.0
    aggregate_count = 0
    for head, (name, head_type) in enumerate(zip(data.outcome_names, data.head_types)):
        observed = np.isfinite(labels[:, head])
        if not np.any(observed):
            continue
        actual = labels[observed, head]
        if head_type == "probability":
            binary = (actual == 0.0) | (actual == 1.0)
            if not np.any(binary):
                continue
            before = _binary_metrics(
                uncalibrated[observed, head][binary], actual[binary]
            )
            after = _binary_metrics(calibrated[observed, head][binary], actual[binary])
            result["heads"][name] = {
                "type": "binary",
                "uncalibrated": before,
                "calibrated": after,
                "brier_improvement": before["brier"] - after["brier"],
            }
            aggregate_before += before["brier"] * before["count"]
            aggregate_after += after["brier"] * after["count"]
            aggregate_count += before["count"]
        else:
            residual = calibrated[observed, head] - actual
            result["heads"][name] = {
                "type": head_type,
                "count": int(actual.size),
                "mae": float(np.mean(np.abs(residual))),
                "rmse": float(np.sqrt(np.mean(residual * residual))),
            }
    if aggregate_count:
        result["binary_aggregate"] = {
            "count": aggregate_count,
            "brier_uncalibrated": aggregate_before / aggregate_count,
            "brier_calibrated": aggregate_after / aggregate_count,
            "brier_improvement": (aggregate_before - aggregate_after) / aggregate_count,
        }
    return result


def _feature_index(names: Sequence[str], exact: str) -> Optional[int]:
    try:
        return list(names).index(exact)
    except ValueError:
        return None


def current_heuristic_scores(
    data: RouterTrainingData,
) -> Tuple[np.ndarray, Dict[str, Any]]:
    """Replay feature-observable current utility terms and hard eligibility."""

    model_names = list(data.auxiliary["model_feature_names"])
    task_names = list(data.auxiliary["task_feature_names"])
    indices = {
        "intelligence": _feature_index(
            model_names, "numeric:benchmark_intelligence_div100"
        ),
        "coding": _feature_index(model_names, "numeric:benchmark_coding_div100"),
        "agentic": _feature_index(model_names, "numeric:benchmark_agentic_div100"),
        "prompt_price": _feature_index(
            model_names, "numeric:prompt_usd_per_million_log1p"
        ),
        "completion_price": _feature_index(
            model_names, "numeric:completion_usd_per_million_log1p"
        ),
        "context": _feature_index(model_names, "numeric:context_length_log2_div24"),
        "tools": _feature_index(model_names, "bool:supports_tools"),
        "code_task": _feature_index(task_names, "bool:code_task"),
        "action_task": _feature_index(task_names, "bool:action_request"),
        "utf8_bytes": _feature_index(task_names, "numeric:utf8_byte_count_log1p_div10"),
        "has_prompt_price": _feature_index(model_names, "bool:has_prompt_price"),
        "has_completion_price": _feature_index(
            model_names, "bool:has_completion_price"
        ),
        "has_benchmark": _feature_index(model_names, "bool:has_benchmark"),
        "input_text": _feature_index(model_names, "bool:input_text"),
        "input_image": _feature_index(model_names, "bool:input_image"),
        "output_text": _feature_index(model_names, "bool:output_text"),
        "batch_variant": _feature_index(model_names, "bool:batch_variant"),
        "image_task": _feature_index(task_names, "bool:image_task"),
    }
    required = (indices["intelligence"], indices["coding"], indices["agentic"])
    if all(value is None for value in required):
        return np.full(data.task.shape[0], np.nan), {
            "available": False,
            "reason": "benchmark_descriptor_columns_missing",
        }
    intelligence = (
        data.model[:, indices["intelligence"]]
        if indices["intelligence"] is not None
        else np.zeros(data.model.shape[0])
    )
    coding = (
        data.model[:, indices["coding"]]
        if indices["coding"] is not None
        else intelligence
    )
    agentic = (
        data.model[:, indices["agentic"]]
        if indices["agentic"] is not None
        else intelligence
    )
    code_task = (
        data.task[:, indices["code_task"]] >= 0.5
        if indices["code_task"] is not None
        else np.zeros(data.task.shape[0], dtype=bool)
    )
    action_task = (
        data.task[:, indices["action_task"]] >= 0.5
        if indices["action_task"] is not None
        else np.zeros(data.task.shape[0], dtype=bool)
    )
    quality = intelligence.copy()
    quality = np.where(
        code_task & action_task,
        0.20 * intelligence + 0.45 * coding + 0.35 * agentic,
        quality,
    )
    quality = np.where(
        code_task & ~action_task, 0.25 * intelligence + 0.75 * coding, quality
    )
    quality = np.where(
        ~code_task & action_task, 0.35 * intelligence + 0.65 * agentic, quality
    )
    prompt_price = (
        data.model[:, indices["prompt_price"]]
        if indices["prompt_price"] is not None
        else np.zeros(data.model.shape[0])
    )
    completion_price = (
        data.model[:, indices["completion_price"]]
        if indices["completion_price"] is not None
        else np.zeros(data.model.shape[0])
    )
    byte_count = (
        np.expm1(np.clip(data.task[:, indices["utf8_bytes"]] * 10.0, 0.0, 30.0))
        if indices["utf8_bytes"] is not None
        else np.full(data.task.shape[0], 4.0)
    )
    prompt_tokens = np.maximum(np.ceil(byte_count / 4.0), 1.0)
    prompt_per_million = np.expm1(np.maximum(prompt_price, 0.0))
    completion_per_million = np.expm1(np.maximum(completion_price, 0.0))
    has_prompt_price = (
        data.model[:, indices["has_prompt_price"]] >= 0.5
        if indices["has_prompt_price"] is not None
        else np.ones(data.model.shape[0], dtype=bool)
    )
    has_completion_price = (
        data.model[:, indices["has_completion_price"]] >= 0.5
        if indices["has_completion_price"] is not None
        else np.ones(data.model.shape[0], dtype=bool)
    )
    known_price = has_prompt_price & has_completion_price
    live_cost = (
        prompt_tokens * prompt_per_million + 512.0 * completion_per_million
    ) / 1_000_000.0
    has_benchmark = (
        data.model[:, indices["has_benchmark"]] >= 0.5
        if indices["has_benchmark"] is not None
        else np.ones(data.model.shape[0], dtype=bool)
    )
    policy = RoutingPolicy()
    # Historical descriptor rows do not encode live latency. Its missingness is
    # therefore a constant and cannot affect within-pair ordering.
    missing_count = (~known_price).astype(np.float64) + (~has_benchmark).astype(
        np.float64
    )
    score = (
        policy.benchmark_weight * np.where(has_benchmark, quality, 0.0)
        - policy.cost_weight * np.where(known_price, live_cost, 0.0)
        - policy.missing_metadata_penalty * missing_count
    )
    input_text = (
        data.model[:, indices["input_text"]] >= 0.5
        if indices["input_text"] is not None
        else np.ones(data.model.shape[0], dtype=bool)
    )
    output_text = (
        data.model[:, indices["output_text"]] >= 0.5
        if indices["output_text"] is not None
        else np.ones(data.model.shape[0], dtype=bool)
    )
    input_image = (
        data.model[:, indices["input_image"]] >= 0.5
        if indices["input_image"] is not None
        else np.zeros(data.model.shape[0], dtype=bool)
    )
    image_task = (
        data.task[:, indices["image_task"]] >= 0.5
        if indices["image_task"] is not None
        else np.zeros(data.task.shape[0], dtype=bool)
    )
    batch_variant = (
        data.model[:, indices["batch_variant"]] >= 0.5
        if indices["batch_variant"] is not None
        else np.zeros(data.model.shape[0], dtype=bool)
    )
    eligible = (
        known_price
        & input_text
        & output_text
        & (~image_task | input_image)
        & ~batch_variant
    )
    # A finite floor permits an eligible candidate to beat an ineligible one;
    # equal ineligible pairs remain ties and are excluded from common coverage.
    score = np.where(eligible, score, -1.0e12)
    return score, {
        "available": True,
        "baseline_name": "current_utility_term_proxy",
        "definition": "feature-observable current RoutingPolicy terms plus known-price/text/modality/batch eligibility",
        "feature_indices": indices,
        "policy_weights": {
            "benchmark_weight": policy.benchmark_weight,
            "cost_weight": policy.cost_weight,
            "missing_metadata_penalty": policy.missing_metadata_penalty,
        },
        "prompt_tokens": "ceil(recovered_utf8_bytes/4)",
        "completion_tokens": 512,
        "scope": "proxy: operational heads, expiry/region/ZDR/provider constraints, and request-only parameters are unavailable in the dataset",
        "eligible_rows": int(np.sum(eligible)),
    }


def validation_model_prior_scores(
    data: RouterTrainingData,
    pairs: np.ndarray,
    preferences: np.ndarray,
    reference_mask: np.ndarray,
) -> Tuple[np.ndarray, Dict[str, Any]]:
    """Fit a smoothed model-ID win-rate prior on validation pairs only."""

    raw_model_ids = data.auxiliary.get("model_ids")
    if raw_model_ids is None:
        return np.full(data.task.shape[0], np.nan), {
            "available": False,
            "reason": "model_ids_missing",
        }
    model_ids = np.asarray(raw_model_ids).reshape(-1).astype(str)
    if model_ids.size != data.task.shape[0]:
        return np.full(data.task.shape[0], np.nan), {
            "available": False,
            "reason": "model_ids_length_mismatch",
        }
    informative = reference_mask & np.isfinite(preferences) & (preferences != 0.5)
    wins: Dict[str, float] = {}
    appearances: Dict[str, int] = {}
    for pair_index in np.flatnonzero(informative):
        left, right = pairs[pair_index]
        left_id, right_id = model_ids[left], model_ids[right]
        left_won = preferences[pair_index] > 0.5
        wins[left_id] = wins.get(left_id, 0.0) + float(left_won)
        wins[right_id] = wins.get(right_id, 0.0) + float(not left_won)
        appearances[left_id] = appearances.get(left_id, 0) + 1
        appearances[right_id] = appearances.get(right_id, 0) + 1
    # A symmetric beta prior makes unseen/cold-start IDs an explicit tie.
    priors = {
        model_id: (wins.get(model_id, 0.0) + 1.0) / (count + 2.0)
        for model_id, count in appearances.items()
    }
    scores = np.asarray([priors.get(model_id, 0.5) for model_id in model_ids])
    best_model = (
        min(priors, key=lambda model_id: (-priors[model_id], model_id))
        if priors
        else None
    )
    return scores, {
        "available": bool(priors),
        "source": "validation_pairs_only",
        "models_observed": len(priors),
        "validation_pairs": int(np.sum(informative)),
        "best_single_model": best_model,
        "unseen_model_prior": 0.5,
    }


def pair_policy_metrics(
    ensemble: RouterEnsemble,
    data: RouterTrainingData,
    evaluation_indices: np.ndarray,
    majority_reference_indices: np.ndarray,
) -> Dict[str, Any]:
    if data.pair_indices is None or data.pair_preferences is None:
        return {"available": False, "pair_count": 0, "reason": "dataset_has_no_pairs"}
    pairs = np.asarray(data.pair_indices, dtype=np.int64)
    preferences = np.asarray(data.pair_preferences, dtype=np.float64)
    membership = np.zeros(data.task.shape[0], dtype=bool)
    membership[np.asarray(evaluation_indices, dtype=np.int64)] = True
    reference = np.zeros(data.task.shape[0], dtype=bool)
    reference[np.asarray(majority_reference_indices, dtype=np.int64)] = True
    test_mask = membership[pairs[:, 0]] & membership[pairs[:, 1]]
    reference_mask = reference[pairs[:, 0]] & reference[pairs[:, 1]]
    informative_reference = (
        reference_mask & np.isfinite(preferences) & (preferences != 0.5)
    )
    if np.any(informative_reference):
        majority_left = bool(np.mean(preferences[informative_reference] > 0.5) >= 0.5)
    else:
        majority_left = True

    predictions = ensemble.predict(data.task, data.model)
    if "preference_score" in data.outcome_names:
        learned_scores = predictions[:, data.outcome_names.index("preference_score")]
    else:
        success_index = (
            data.outcome_names.index("completion_success")
            if "completion_success" in data.outcome_names
            else 0
        )
        learned_scores = predictions[:, success_index]
    heuristic, heuristic_metadata = current_heuristic_scores(data)
    model_prior, model_prior_metadata = validation_model_prior_scores(
        data, pairs, preferences, reference_mask
    )
    cost_index = (
        data.outcome_names.index("cost_usd") if "cost_usd" in data.outcome_names else -1
    )
    if cost_index < 0:
        return {"available": False, "pair_count": 0, "reason": "cost_head_missing"}
    cost = data.labels[:, cost_index]
    left, right = pairs[:, 0], pairs[:, 1]
    common = (
        test_mask
        & np.isfinite(preferences)
        & (preferences != 0.5)
        & np.isfinite(learned_scores[left])
        & np.isfinite(learned_scores[right])
        & np.isfinite(cost[left])
        & np.isfinite(cost[right])
        & (np.abs(cost[left] - cost[right]) > 1.0e-12)
        & np.isfinite(heuristic[left])
        & np.isfinite(heuristic[right])
        & (np.abs(heuristic[left] - heuristic[right]) > 1.0e-12)
        & np.isfinite(model_prior[left])
        & np.isfinite(model_prior[right])
    )
    if not np.any(common):
        return {
            "available": False,
            "pair_count": 0,
            "reason": "no_common_pairs_across_all_baselines",
            "current_utility_term_proxy": heuristic_metadata,
            "validation_model_prior": model_prior_metadata,
        }
    expected_left = preferences[common] > 0.5
    learned_left = learned_scores[left[common]] > learned_scores[right[common]]
    cheapest_left = cost[left[common]] < cost[right[common]]
    heuristic_left = heuristic[left[common]] > heuristic[right[common]]
    majority_prediction = np.full(expected_left.shape, majority_left, dtype=bool)
    model_prior_left = model_prior[left[common]] > model_prior[right[common]]
    model_prior_tie = model_prior[left[common]] == model_prior[right[common]]
    model_prior_left = np.where(model_prior_tie, majority_prediction, model_prior_left)
    accuracies = {
        "learned": float(np.mean(learned_left == expected_left)),
        "cheapest_observed": float(np.mean(cheapest_left == expected_left)),
        "validation_majority": float(np.mean(majority_prediction == expected_left)),
        "current_utility_term_proxy": float(np.mean(heuristic_left == expected_left)),
        "validation_model_prior": float(np.mean(model_prior_left == expected_left)),
    }
    baseline_best = max(
        accuracies["cheapest_observed"],
        accuracies["validation_majority"],
        accuracies["current_utility_term_proxy"],
        accuracies["validation_model_prior"],
    )
    return {
        "available": True,
        "pair_count": int(np.sum(common)),
        "coverage_of_split_pairs": float(np.sum(common) / max(np.sum(test_mask), 1)),
        "accuracies": accuracies,
        "best_baseline_accuracy": baseline_best,
        "learned_margin_over_best_baseline": accuracies["learned"] - baseline_best,
        "majority_direction_learned_from_validation": "left"
        if majority_left
        else "right",
        "current_utility_term_proxy": heuristic_metadata,
        "validation_model_prior": model_prior_metadata,
        "cheapest_definition": "lower observed cost_usd where both outcomes are present",
    }


def holdout_partition_integrity(data: RouterTrainingData, axis: str) -> Dict[str, Any]:
    """Prove a supplied entity fold is group-constant and split-disjoint."""

    raw_ids = data.auxiliary.get(axis + "_holdout_ids")
    raw_folds = data.auxiliary.get(axis + "_holdout_fold")
    if raw_ids is None or raw_folds is None:
        return {
            "passed": False,
            "axis": axis,
            "reason": "supplied_holdout_ids_or_folds_missing",
        }
    ids = np.asarray(raw_ids).reshape(-1).astype(str)
    folds = np.asarray(raw_folds, dtype=np.int64).reshape(-1)
    rows = data.task.shape[0]
    if ids.size != rows or folds.size != rows:
        return {
            "passed": False,
            "axis": axis,
            "reason": "holdout_array_length_mismatch",
        }
    train_ids = set(ids[data.train_indices].tolist())
    validation_ids = set(ids[data.validation_indices].tolist())
    test_ids = set(ids[data.test_indices].tolist())
    ids_disjoint = not (
        train_ids.intersection(validation_ids)
        or train_ids.intersection(test_ids)
        or validation_ids.intersection(test_ids)
    )
    fold_constant = True
    for entity_id in np.unique(ids):
        if np.unique(folds[ids == entity_id]).size != 1:
            fold_constant = False
            break
    split_fold_consistent = bool(
        np.all(folds[data.train_indices] <= 7)
        and np.all(folds[data.validation_indices] == 8)
        and np.all(folds[data.test_indices] == 9)
    )
    row_indices = np.concatenate(
        (data.train_indices, data.validation_indices, data.test_indices)
    )
    rows_disjoint = np.unique(row_indices).size == row_indices.size
    rows_complete = bool(
        row_indices.size == rows
        and np.unique(row_indices).size == rows
        and np.all((row_indices >= 0) & (row_indices < rows))
    )
    fold_domain_valid = bool(np.all((folds >= 0) & (folds <= 9)))
    partitions_nonempty = bool(train_ids and validation_ids and test_ids)
    passed = bool(
        ids_disjoint
        and fold_constant
        and split_fold_consistent
        and rows_disjoint
        and rows_complete
        and fold_domain_valid
        and partitions_nonempty
    )
    return {
        "passed": passed,
        "axis": axis,
        "ids_disjoint": ids_disjoint,
        "fold_constant_per_id": fold_constant,
        "split_fold_consistent": split_fold_consistent,
        "rows_disjoint": rows_disjoint,
        "rows_complete": rows_complete,
        "fold_domain_0_through_9": fold_domain_valid,
        "partitions_nonempty": partitions_nonempty,
        "unique_ids": {
            "train": len(train_ids),
            "validation": len(validation_ids),
            "test": len(test_ids),
        },
    }


def temporal_partition_integrity(data: RouterTrainingData) -> Dict[str, Any]:
    """Verify a complete, group-disjoint, first-observation temporal split."""

    raw_split = data.auxiliary.get("temporal_split")
    raw_groups = data.auxiliary.get("group_ids")
    raw_epochs = data.auxiliary.get("event_epochs")
    if raw_split is None or raw_groups is None or raw_epochs is None:
        return {
            "passed": False,
            "reason": "temporal_split_group_ids_or_event_epochs_missing",
        }
    split = np.asarray(raw_split, dtype=np.int64).reshape(-1)
    groups = np.asarray(raw_groups).reshape(-1).astype(str)
    epochs = np.asarray(raw_epochs, dtype=np.float64).reshape(-1)
    rows = data.task.shape[0]
    if split.size != rows or groups.size != rows or epochs.size != rows:
        return {"passed": False, "reason": "temporal_array_length_mismatch"}
    split_domain_exact = set(np.unique(split).tolist()) == {0, 1, 2}
    finite_epochs = bool(np.all(np.isfinite(epochs)))
    expected = (
        np.flatnonzero(split == 0),
        np.flatnonzero(split == 1),
        np.flatnonzero(split == 2),
    )
    split_indices_match = all(
        np.array_equal(np.sort(actual), expected_partition)
        for actual, expected_partition in zip(
            (data.train_indices, data.validation_indices, data.test_indices), expected
        )
    )
    partitions_nonempty = all(partition.size > 0 for partition in expected)
    group_split_constant = True
    group_first_epochs: Dict[int, List[float]] = {0: [], 1: [], 2: []}
    if split_domain_exact and finite_epochs:
        for group_id in np.unique(groups):
            group_mask = groups == group_id
            group_splits = np.unique(split[group_mask])
            if group_splits.size != 1:
                group_split_constant = False
                break
            group_first_epochs[int(group_splits[0])].append(
                float(np.min(epochs[group_mask]))
            )
    else:
        group_split_constant = False
    ordered_first_observations = bool(
        group_split_constant
        and all(group_first_epochs[value] for value in (0, 1, 2))
        and max(group_first_epochs[0]) <= min(group_first_epochs[1])
        and max(group_first_epochs[1]) <= min(group_first_epochs[2])
    )
    passed = bool(
        split_domain_exact
        and finite_epochs
        and split_indices_match
        and partitions_nonempty
        and group_split_constant
        and ordered_first_observations
    )
    return {
        "passed": passed,
        "split_domain_exact_0_1_2": split_domain_exact,
        "finite_event_epochs": finite_epochs,
        "split_indices_match": split_indices_match,
        "partitions_nonempty": partitions_nonempty,
        "group_split_constant": group_split_constant,
        "first_observation_epochs_ordered": ordered_first_observations,
        "rows": {
            "train": int(expected[0].size),
            "validation": int(expected[1].size),
            "test": int(expected[2].size),
        },
    }


def evaluate_split(
    ensemble: RouterEnsemble, data: RouterTrainingData
) -> Dict[str, Any]:
    result = {
        "split_name": data.split_name,
        "train_rows": int(data.train_indices.size),
        "validation": outcome_metrics(ensemble, data, data.validation_indices),
        "test": outcome_metrics(ensemble, data, data.test_indices),
        "policy": pair_policy_metrics(
            ensemble, data, data.test_indices, data.validation_indices
        ),
    }
    if data.split_name in ("model-holdout", "provider-holdout", "family-holdout"):
        axis = data.split_name.split("-", 1)[0]
        result["partition_integrity"] = holdout_partition_integrity(data, axis)
    return result


def quality_metrics(
    ensemble: RouterEnsemble, data: RouterTrainingData
) -> Dict[str, Any]:
    if data.test_indices.size == 0:
        return {"available": False, "reason": "quality_test_split_empty"}
    prediction = ensemble.predict(
        data.task[data.test_indices], data.model[data.test_indices]
    )
    labels = data.labels[data.test_indices]
    validation_labels = data.labels[data.validation_indices]
    expected_suppression_value = data.auxiliary.get("quality_leakage_columns_zeroed")
    actual_suppression_value = ensemble.training_metadata.get(
        "benchmark_feature_columns_zeroed"
    )
    expected_suppression = list(
        expected_suppression_value if expected_suppression_value is not None else []
    )
    actual_suppression = list(
        actual_suppression_value if actual_suppression_value is not None else []
    )
    artifact_metadata = ensemble.training_metadata
    dataset_metadata = data.auxiliary.get("metadata") or {}
    expected_split_hashes = {
        "train": _indices_sha256(data.train_indices),
        "validation": _indices_sha256(data.validation_indices),
        "test": _indices_sha256(data.test_indices),
    }
    artifact_split_hashes = {
        "train": artifact_metadata.get("train_indices_sha256"),
        "validation": artifact_metadata.get("validation_indices_sha256"),
        "test": artifact_metadata.get("test_indices_sha256"),
    }
    actual_dataset_sha256 = _file_sha256(data.auxiliary.get("dataset_path"))
    family_integrity = holdout_partition_integrity(data, "family")
    contract_checks = {
        "artifact_role_quality": artifact_metadata.get("artifact_role")
        == "catalog_quality_predictor",
        "weak_label_source_explicit": artifact_metadata.get("label_source")
        == "OpenRouter Artificial Analysis weak supervision",
        "suppressed_columns_nonempty_and_exact": bool(expected_suppression)
        and actual_suppression == expected_suppression,
        "label_provenance_present": bool(
            artifact_metadata.get("label_provenance_counts")
        ),
        "quality_task_contract": artifact_metadata.get("task_feature_version")
        == QUALITY_TASK_FEATURE_VERSION,
        "feature_and_outcome_contracts_match": tuple(ensemble.task_feature_names)
        == tuple(data.auxiliary["task_feature_names"])
        and tuple(ensemble.model_feature_names)
        == tuple(data.auxiliary["model_feature_names"])
        and tuple(ensemble.outcome_names) == tuple(data.outcome_names)
        and tuple(ensemble.head_types) == tuple(data.head_types),
        "model_feature_version_matches": bool(
            artifact_metadata.get("model_feature_version")
            and artifact_metadata.get("model_feature_version")
            == dataset_metadata.get("model_feature_version")
        ),
        "dataset_sha256_matches": bool(
            actual_dataset_sha256
            and actual_dataset_sha256 == dataset_metadata.get("artifact_sha256")
            and actual_dataset_sha256
            == artifact_metadata.get("dataset_artifact_sha256")
        ),
        "catalog_snapshot_matches": bool(
            dataset_metadata.get("catalog_snapshot_sha256")
            and dataset_metadata.get("catalog_snapshot_sha256")
            == artifact_metadata.get("catalog_snapshot_sha256")
        ),
        "family_split_and_hashes_match": artifact_metadata.get("split_name")
        == "family-holdout"
        and expected_split_hashes == artifact_split_hashes
        and bool(family_integrity.get("passed")),
        "soft_quality_calibration_disabled": artifact_metadata.get(
            "training_config", {}
        ).get("calibrate_binary")
        is False
        and ensemble.calibrator.metadata.get("source")
        == "identity_nonbinary_quality_labels"
        and not np.any(ensemble.calibrator.active)
        and np.array_equal(
            ensemble.calibrator.slopes,
            np.ones(len(ensemble.outcome_names), dtype=np.float64),
        )
        and np.array_equal(
            ensemble.calibrator.intercepts,
            np.zeros(len(ensemble.outcome_names), dtype=np.float64),
        ),
    }
    heads: Dict[str, Any] = {}
    weighted_improvement = 0.0
    weighted_count = 0
    for head, name in enumerate(data.outcome_names):
        observed = np.isfinite(labels[:, head])
        validation_observed = np.isfinite(validation_labels[:, head])
        if not np.any(observed) or not np.any(validation_observed):
            continue
        actual = labels[observed, head]
        residual = prediction[observed, head] - actual
        baseline_value = float(np.mean(validation_labels[validation_observed, head]))
        baseline_residual = baseline_value - actual
        learned_rmse = float(np.sqrt(np.mean(residual * residual)))
        baseline_rmse = float(np.sqrt(np.mean(baseline_residual * baseline_residual)))
        heads[name] = {
            "count": int(actual.size),
            "learned_rmse": learned_rmse,
            "validation_mean_baseline_rmse": baseline_rmse,
            "rmse_improvement": baseline_rmse - learned_rmse,
            "mae": float(np.mean(np.abs(residual))),
        }
        weighted_improvement += (baseline_rmse - learned_rmse) * actual.size
        weighted_count += actual.size
    return {
        "available": bool(heads),
        "split_name": data.split_name,
        "test_rows": int(data.test_indices.size),
        "heads": heads,
        "weighted_rmse_improvement": weighted_improvement / weighted_count
        if weighted_count
        else None,
        "label_source": ensemble.training_metadata.get("label_source"),
        "benchmark_feature_columns_zeroed": ensemble.training_metadata.get(
            "benchmark_feature_columns_zeroed"
        ),
        "contract": {
            "passed": all(contract_checks.values()),
            "checks": contract_checks,
            "expected_benchmark_feature_columns_zeroed": expected_suppression,
            "artifact_benchmark_feature_columns_zeroed": actual_suppression,
            "actual_dataset_sha256": actual_dataset_sha256,
            "expected_split_hashes": expected_split_hashes,
            "artifact_split_hashes": artifact_split_hashes,
            "family_partition_integrity": family_integrity,
        },
    }


def _gate(name: str, passed: bool, actual: Any, requirement: str) -> Dict[str, Any]:
    return {
        "name": name,
        "passed": bool(passed),
        "actual": _json_safe(actual),
        "requirement": requirement,
    }


def _indices_sha256(indices: np.ndarray) -> str:
    canonical = np.sort(np.asarray(indices, dtype="<i8").reshape(-1))
    return hashlib.sha256(canonical.tobytes()).hexdigest()


def _file_sha256(path: Optional[str]) -> Optional[str]:
    if not path:
        return None
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as handle:
            for block in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(block)
    except OSError:
        return None
    return digest.hexdigest()


def promotion_provenance(
    ensemble: RouterEnsemble, data: RouterTrainingData
) -> Dict[str, Any]:
    """Verify the artifact, dataset, feature, split, and calibrator contracts."""

    artifact = ensemble.training_metadata
    dataset = data.auxiliary.get("metadata") or {}
    actual_dataset_sha256 = _file_sha256(data.auxiliary.get("dataset_path"))
    temporal_integrity = temporal_partition_integrity(data)
    expected_validation_hash = _indices_sha256(data.validation_indices)
    validation_member_predictions = ensemble.predict_members(
        data.task[data.validation_indices], data.model[data.validation_indices]
    )
    recomputed_calibrator = BinaryCalibrator.fit(
        np.mean(validation_member_predictions, axis=0),
        data.labels[data.validation_indices],
        data.head_types,
        data.outcome_names,
        validation_indices_hash=expected_validation_hash,
    )
    calibrator_recomputed_exactly = bool(
        np.array_equal(recomputed_calibrator.slopes, ensemble.calibrator.slopes)
        and np.array_equal(
            recomputed_calibrator.intercepts, ensemble.calibrator.intercepts
        )
        and np.array_equal(recomputed_calibrator.active, ensemble.calibrator.active)
    )
    expected_split_hashes = {
        "train": _indices_sha256(data.train_indices),
        "validation": expected_validation_hash,
        "test": _indices_sha256(data.test_indices),
    }
    stored_split_hashes = {
        "train": artifact.get("train_indices_sha256"),
        "validation": artifact.get("validation_indices_sha256"),
        "test": artifact.get("test_indices_sha256"),
    }
    feature_versions_match = bool(
        artifact.get("task_feature_version")
        and artifact.get("task_feature_version") == dataset.get("task_feature_version")
        and artifact.get("model_feature_version")
        and artifact.get("model_feature_version")
        == dataset.get("model_feature_version")
    )
    checks = {
        "artifact_role_operational": artifact.get("artifact_role")
        == "operational_router",
        "artifact_and_data_are_temporal": artifact.get("split_name") == "temporal"
        and data.split_name == "temporal",
        "temporal_partition_integrity": bool(temporal_integrity.get("passed")),
        "dataset_sha256_matches_sidecar_and_artifact": bool(
            actual_dataset_sha256
            and actual_dataset_sha256 == dataset.get("artifact_sha256")
            and actual_dataset_sha256 == artifact.get("dataset_artifact_sha256")
        ),
        "catalog_snapshot_matches": bool(
            dataset.get("catalog_snapshot_sha256")
            and dataset.get("catalog_snapshot_sha256")
            == artifact.get("catalog_snapshot_sha256")
        ),
        "feature_versions_match": feature_versions_match,
        "feature_names_match": tuple(ensemble.task_feature_names)
        == tuple(data.auxiliary["task_feature_names"])
        and tuple(ensemble.model_feature_names)
        == tuple(data.auxiliary["model_feature_names"]),
        "outcome_contract_matches": tuple(ensemble.outcome_names)
        == tuple(data.outcome_names)
        and tuple(ensemble.head_types) == tuple(data.head_types),
        "split_hashes_match": expected_split_hashes == stored_split_hashes,
        "calibrator_is_validation_only": ensemble.calibrator.metadata.get("source")
        == "validation_only",
        "calibrator_validation_hash_matches": ensemble.calibrator.metadata.get(
            "validation_indices_sha256"
        )
        == expected_validation_hash,
        "calibrator_recomputed_from_validation_exactly": calibrator_recomputed_exactly,
    }
    calibration_partition_verified = bool(
        checks["artifact_and_data_are_temporal"]
        and checks["temporal_partition_integrity"]
        and checks["dataset_sha256_matches_sidecar_and_artifact"]
        and checks["feature_versions_match"]
        and checks["feature_names_match"]
        and checks["outcome_contract_matches"]
        and checks["split_hashes_match"]
        and checks["calibrator_is_validation_only"]
        and checks["calibrator_validation_hash_matches"]
        and checks["calibrator_recomputed_from_validation_exactly"]
    )
    return {
        "passed": all(checks.values()),
        "checks": checks,
        "actual_dataset_sha256": actual_dataset_sha256,
        "sidecar_dataset_sha256": dataset.get("artifact_sha256"),
        "artifact_dataset_sha256": artifact.get("dataset_artifact_sha256"),
        "expected_split_hashes": expected_split_hashes,
        "artifact_split_hashes": stored_split_hashes,
        "calibration_partition_verified": calibration_partition_verified,
        "temporal_partition_integrity": temporal_integrity,
        "test_labels_used_for_calibration": (
            False if calibration_partition_verified else None
        ),
    }


def build_promotion_report(
    primary_ensemble: RouterEnsemble,
    primary_data: RouterTrainingData,
    *,
    strict_holdouts: Optional[Mapping[str, Mapping[str, Any]]] = None,
    quality_evaluation: Optional[Mapping[str, Any]] = None,
    quality_lane_present: bool = False,
    minimum_pairs: int = 100,
    minimum_strict_pairs: int = 20,
    minimum_policy_margin: float = 0.02,
    minimum_calibration_improvement: float = 1.0e-4,
    test_calibration_tolerance: float = 0.002,
    minimum_quality_improvement: float = 0.001,
    minimum_quality_rows_per_head: int = 10,
) -> Dict[str, Any]:
    primary = evaluate_split(primary_ensemble, primary_data)
    validation_binary = primary["validation"].get("binary_aggregate") or {}
    test_binary = primary["test"].get("binary_aggregate") or {}
    policy = primary["policy"]
    strict = dict(strict_holdouts or {})
    provenance = promotion_provenance(primary_ensemble, primary_data)
    gates: List[Dict[str, Any]] = []
    gates.append(
        _gate(
            "promotion_provenance_verified",
            bool(provenance["passed"]),
            provenance["checks"],
            "artifact/dataset hashes, contracts, temporal split, and validation-only calibration all match",
        )
    )
    pair_count = int(policy.get("pair_count") or 0)
    gates.append(
        _gate(
            "minimum_common_test_pairs",
            pair_count >= minimum_pairs,
            pair_count,
            ">= %d" % minimum_pairs,
        )
    )
    for axis in ("model", "provider", "family"):
        diagnostic = strict.get(axis, {})
        integrity = diagnostic.get("partition_integrity") or {}
        gates.append(
            _gate(
                "strict_%s_partition_integrity" % axis,
                bool(integrity.get("passed")),
                integrity,
                "supplied %s IDs are split-disjoint with one constant fold per ID"
                % axis,
            )
        )
        axis_policy = diagnostic.get("policy") or {}
        axis_pairs = int(axis_policy.get("pair_count") or 0)
        axis_margin = axis_policy.get("learned_margin_over_best_baseline")
        gates.append(
            _gate(
                "strict_%s_policy_materially_beats_baselines" % axis,
                bool(axis_policy.get("available"))
                and axis_pairs >= minimum_strict_pairs
                and axis_margin is not None
                and float(axis_margin) >= minimum_policy_margin,
                {
                    "available": axis_policy.get("available"),
                    "pairs": axis_pairs,
                    "margin": axis_margin,
                },
                ">= %d common pairs and margin >= %.6f"
                % (
                    minimum_strict_pairs,
                    minimum_policy_margin,
                ),
            )
        )
    margin = policy.get("learned_margin_over_best_baseline")
    gates.append(
        _gate(
            "learned_policy_materially_beats_baselines",
            margin is not None and float(margin) >= minimum_policy_margin,
            margin,
            ">= %.6f over cheapest, validation-majority, validation-model-prior, and current-utility proxy"
            % minimum_policy_margin,
        )
    )
    validation_improvement = validation_binary.get("brier_improvement")
    gates.append(
        _gate(
            "validation_only_calibration_improves_brier",
            validation_improvement is not None
            and float(validation_improvement) >= minimum_calibration_improvement,
            validation_improvement,
            ">= %.6f" % minimum_calibration_improvement,
        )
    )
    test_before = test_binary.get("brier_uncalibrated")
    test_after = test_binary.get("brier_calibrated")
    test_delta = (
        None
        if test_before is None or test_after is None
        else float(test_after) - float(test_before)
    )
    gates.append(
        _gate(
            "test_calibration_non_regression",
            test_delta is not None and test_delta <= test_calibration_tolerance,
            test_delta,
            "calibrated minus uncalibrated Brier <= %.6f" % test_calibration_tolerance,
        )
    )
    active_head_regression: Dict[str, Any] = {}
    for head_index, name in enumerate(primary_ensemble.outcome_names):
        if not primary_ensemble.calibrator.active[head_index]:
            continue
        head_metrics = primary["test"].get("heads", {}).get(name) or {}
        improvement = head_metrics.get("brier_improvement")
        active_head_regression[name] = {
            "count": (head_metrics.get("calibrated") or {}).get("count"),
            "brier_improvement": improvement,
            "passed": improvement is not None
            and float(improvement) >= -test_calibration_tolerance,
        }
    gates.append(
        _gate(
            "each_active_binary_head_calibration_non_regression",
            bool(active_head_regression)
            and all(value["passed"] for value in active_head_regression.values()),
            active_head_regression,
            "every active calibrated head has test Brier improvement >= -%.6f"
            % test_calibration_tolerance,
        )
    )
    gates.append(
        _gate(
            "finite_primary_predictions",
            bool(primary["validation"].get("finite"))
            and bool(primary["test"].get("finite")),
            {
                "validation": primary["validation"].get("finite"),
                "test": primary["test"].get("finite"),
            },
            "all validation and test predictions finite",
        )
    )
    strict_available = all(
        axis in strict
        and int(strict[axis].get("test", {}).get("rows") or 0) > 0
        and bool(strict[axis].get("test", {}).get("finite"))
        for axis in ("model", "provider", "family")
    )
    gates.append(
        _gate(
            "strict_cold_start_holdouts_available",
            strict_available,
            {
                axis: int(strict.get(axis, {}).get("test", {}).get("rows") or 0)
                for axis in ("model", "provider", "family")
            },
            "non-empty finite model/provider/family holdout test metrics",
        )
    )
    if quality_lane_present:
        quality_contract = (
            quality_evaluation.get("contract") if quality_evaluation else None
        ) or {}
        gates.append(
            _gate(
                "catalog_quality_artifact_contract_verified",
                bool(quality_contract.get("passed")),
                quality_contract,
                "separate quality artifact has explicit weak-label provenance and exact nonempty benchmark suppression",
            )
        )
        expected_quality_heads = {
            "general_intelligence_quality",
            "coding_quality",
            "agentic_quality",
        }
        quality_heads = (
            quality_evaluation.get("heads", {}) if quality_evaluation else {}
        )
        per_head_quality = {
            name: {
                "count": int(quality_heads.get(name, {}).get("count") or 0),
                "rmse_improvement": quality_heads.get(name, {}).get("rmse_improvement"),
            }
            for name in sorted(expected_quality_heads)
        }
        gates.append(
            _gate(
                "each_catalog_quality_head_beats_validation_mean",
                set(quality_heads) == expected_quality_heads
                and all(
                    value["count"] >= minimum_quality_rows_per_head
                    and value["rmse_improvement"] is not None
                    and float(value["rmse_improvement"]) >= 0.0
                    for value in per_head_quality.values()
                ),
                per_head_quality,
                "exactly general/coding/agentic, each with >= %d test labels and nonnegative RMSE improvement"
                % minimum_quality_rows_per_head,
            )
        )
        quality_improvement = (
            quality_evaluation.get("weighted_rmse_improvement")
            if quality_evaluation
            else None
        )
        gates.append(
            _gate(
                "catalog_quality_predictor_beats_validation_mean",
                quality_improvement is not None
                and float(quality_improvement) >= minimum_quality_improvement,
                quality_improvement,
                ">= %.6f weighted RMSE improvement without benchmark leakage"
                % minimum_quality_improvement,
            )
        )

    report: Dict[str, Any] = {
        "schema_version": PROMOTION_REPORT_VERSION,
        "artifact_role": "chimera_router_promotion_report",
        "provenance": {
            "dataset_artifact_sha256": primary_ensemble.training_metadata.get(
                "dataset_artifact_sha256"
            ),
            "catalog_snapshot_sha256": primary_ensemble.training_metadata.get(
                "catalog_snapshot_sha256"
            ),
            "ensemble_members": len(primary_ensemble.members),
            "calibration_source": primary_ensemble.calibrator.metadata.get("source"),
            "calibration_validation_indices_sha256": primary_ensemble.calibrator.metadata.get(
                "validation_indices_sha256"
            ),
            "verification": provenance,
            "test_labels_used_for_calibration": provenance[
                "test_labels_used_for_calibration"
            ],
        },
        "thresholds": {
            "minimum_pairs": minimum_pairs,
            "minimum_strict_pairs": minimum_strict_pairs,
            "minimum_policy_margin": minimum_policy_margin,
            "minimum_calibration_improvement": minimum_calibration_improvement,
            "test_calibration_tolerance": test_calibration_tolerance,
            "minimum_quality_improvement": minimum_quality_improvement,
            "minimum_quality_rows_per_head": minimum_quality_rows_per_head,
        },
        "temporal": primary,
        "strict_holdouts": strict,
        "catalog_quality": quality_evaluation,
        "promotion": {
            "passed": all(gate["passed"] for gate in gates),
            "decision": "promote" if all(gate["passed"] for gate in gates) else "hold",
            "gates": gates,
        },
    }
    canonical = json.dumps(
        _json_safe(report), sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    report["report_sha256"] = hashlib.sha256(canonical).hexdigest()
    return report


def _diagnostic_config(
    primary: RouterEnsemble, members: int, epochs: Optional[int]
) -> Dict[str, Any]:
    stored = dict(primary.training_metadata.get("training_config") or {})
    return {
        "members": members,
        "rank": int(stored.get("rank", primary.members[0].rank)),
        "epochs": int(epochs if epochs is not None else stored.get("epochs", 200)),
        "batch_size": int(stored.get("batch_size", 512)),
        "learning_rate": float(stored.get("learning_rate", 0.005)),
        "weight_decay": float(stored.get("weight_decay", 1.0e-5)),
        "patience": int(stored.get("patience", 30)),
        "pair_loss_weight": float(stored.get("pair_loss_weight", 0.25)),
        "seed": int(stored.get("seed", 7)),
        "verbose": False,
    }


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="v2 dataset NPZ")
    parser.add_argument("--metadata", help="optional dataset sidecar")
    parser.add_argument(
        "--ensemble", required=True, help="operational v2 or v1 router artifact"
    )
    parser.add_argument("--quality-ensemble", help="catalog quality ensemble")
    parser.add_argument("--promotion-report", required=True)
    parser.add_argument("--diagnostic-members", type=int, default=3)
    parser.add_argument("--diagnostic-epochs", type=int)
    parser.add_argument("--skip-strict-retrain", action="store_true")
    parser.add_argument("--minimum-pairs", type=int, default=100)
    parser.add_argument("--minimum-strict-pairs", type=int, default=20)
    parser.add_argument("--minimum-policy-margin", type=float, default=0.02)
    parser.add_argument("--minimum-calibration-improvement", type=float, default=1.0e-4)
    parser.add_argument("--test-calibration-tolerance", type=float, default=0.002)
    parser.add_argument("--minimum-quality-improvement", type=float, default=0.001)
    parser.add_argument("--minimum-quality-rows-per-head", type=int, default=10)
    parser.add_argument("--allow-failed-promotion", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    primary = RouterEnsemble.load(args.ensemble)
    primary_data = prepare_router_training_data(
        args.dataset,
        args.metadata,
        split_axis="temporal",
        seed=int(primary.training_metadata.get("training_config", {}).get("seed", 7)),
        pair_loss_weight=float(
            primary.training_metadata.get("training_config", {}).get(
                "pair_loss_weight", 0.25
            )
        ),
    )
    strict: Dict[str, Any] = {}
    if not args.skip_strict_retrain:
        config = _diagnostic_config(
            primary, max(args.diagnostic_members, 1), args.diagnostic_epochs
        )
        for offset, axis in enumerate(("model", "provider", "family")):
            try:
                diagnostic_data = prepare_router_training_data(
                    args.dataset,
                    args.metadata,
                    split_axis=axis,
                    seed=config["seed"] + 100000 * (offset + 1),
                    pair_loss_weight=config["pair_loss_weight"],
                )
                axis_config = dict(config)
                axis_config["seed"] += 100000 * (offset + 1)
                diagnostic = fit_router_ensemble(diagnostic_data, **axis_config)
                strict[axis] = evaluate_split(diagnostic, diagnostic_data)
            except ValueError as error:
                strict[axis] = {
                    "split_name": axis + "-holdout",
                    "available": False,
                    "error": str(error),
                    "test": {"rows": 0, "finite": False},
                }

    quality_data = prepare_catalog_quality_data(
        args.dataset, args.metadata, split_axis="family"
    )
    quality_lane_present = quality_data is not None
    quality_evaluation: Optional[Mapping[str, Any]] = None
    if quality_data is not None and args.quality_ensemble:
        quality_model = RouterEnsemble.load(args.quality_ensemble)
        quality_evaluation = quality_metrics(quality_model, quality_data)

    report = build_promotion_report(
        primary,
        primary_data,
        strict_holdouts=strict,
        quality_evaluation=quality_evaluation,
        quality_lane_present=quality_lane_present,
        minimum_pairs=args.minimum_pairs,
        minimum_strict_pairs=args.minimum_strict_pairs,
        minimum_policy_margin=args.minimum_policy_margin,
        minimum_calibration_improvement=args.minimum_calibration_improvement,
        test_calibration_tolerance=args.test_calibration_tolerance,
        minimum_quality_improvement=args.minimum_quality_improvement,
        minimum_quality_rows_per_head=args.minimum_quality_rows_per_head,
    )
    report_path = Path(args.promotion_report).expanduser()
    _write_json_atomic(report_path, report)
    summary = {
        "promotion_report": str(report_path),
        "report_sha256": report["report_sha256"],
        "passed": report["promotion"]["passed"],
        "decision": report["promotion"]["decision"],
        "failed_gates": [
            gate["name"] for gate in report["promotion"]["gates"] if not gate["passed"]
        ],
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if report["promotion"]["passed"] or args.allow_failed_promotion else 2


if __name__ == "__main__":
    sys.exit(main())


__all__ = [
    "build_promotion_report",
    "current_heuristic_scores",
    "evaluate_split",
    "outcome_metrics",
    "pair_policy_metrics",
    "quality_metrics",
]
