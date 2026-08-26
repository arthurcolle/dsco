#!/usr/bin/env python3
"""Train and export the portable Chimera candidate router.

Expected dataset contract (``build_dataset.py`` v1):

* ``task_features``: float [N, task_dim]
* ``model_features``: float [N, model_dim]
* ``labels``: float [N, O]
* ``label_mask``: optional bool/uint8 [N, O]
* ``group_ids``: optional [N], grouping the same prompt/request
* ``pair_indices``: optional int [P, 2]
* ``pair_preference``: optional float [P], 1=left, 0=right, .5=tie

The loader accepts a few conservative legacy aliases, but never enables pickle.
Feature and label names may live in the NPZ or its JSON sidecar.  The default
five labels match the first dataset schema:
``completion_success, provider_failure, tool_error, cost_usd, latency_s``.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import numpy as np

try:
    from .features import MODEL_FEATURE_VERSION, TASK_FEATURE_VERSION
    from .model import CandidateRouter
except ImportError:  # Direct execution: python scripts/chimera_router/train.py
    from features import MODEL_FEATURE_VERSION, TASK_FEATURE_VERSION
    from model import CandidateRouter


DEFAULT_LABEL_NAMES = (
    "completion_success",
    "provider_failure",
    "tool_error",
    "cost_usd",
    "latency_s",
)


def _first_array(
    archive: Mapping[str, np.ndarray], names: Sequence[str]
) -> Optional[np.ndarray]:
    for name in names:
        if name in archive:
            return np.asarray(archive[name])
    return None


def _load_sidecar(
    dataset_path: Path, explicit: Optional[str]
) -> Tuple[Dict[str, Any], Optional[Path]]:
    candidates: List[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.extend(
        [
            dataset_path.with_suffix(".json"),
            dataset_path.with_suffix(".meta.json"),
            Path(str(dataset_path) + ".json"),
        ]
    )
    seen = set()
    for candidate in candidates:
        resolved = str(candidate.expanduser())
        if resolved in seen:
            continue
        seen.add(resolved)
        if candidate.is_file():
            with candidate.open("r", encoding="utf-8") as handle:
                value = json.load(handle)
            if not isinstance(value, dict):
                raise ValueError(
                    "metadata sidecar must contain a JSON object: %s" % candidate
                )
            return value, candidate
    if explicit:
        raise FileNotFoundError("metadata sidecar not found: %s" % explicit)
    return {}, None


def _names_from_metadata(
    metadata: Mapping[str, Any], key: str, dimension: int
) -> Optional[List[str]]:
    possible: Any = metadata.get(key)
    if possible is None and isinstance(metadata.get("features"), dict):
        short = "task" if key.startswith("task") else "model"
        possible = metadata["features"].get(short)
    if isinstance(possible, dict):
        # Accept either name -> index or index -> name mappings.
        if all(isinstance(value, int) for value in possible.values()):
            ordered: List[Optional[str]] = [None] * dimension
            for name, index in possible.items():
                if 0 <= int(index) < dimension:
                    ordered[int(index)] = str(name)
            if all(value is not None for value in ordered):
                return [str(value) for value in ordered]
        if all(str(name).isdigit() for name in possible):
            ordered_values = [possible.get(str(index)) for index in range(dimension)]
            if all(value is not None for value in ordered_values):
                return [str(value) for value in ordered_values]
    if isinstance(possible, list) and len(possible) == dimension:
        return [str(value) for value in possible]
    return None


def _string_array(array: np.ndarray) -> List[str]:
    return [str(value) for value in np.asarray(array).reshape(-1).tolist()]


def load_dataset(
    path: str, metadata_path: Optional[str] = None
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, Dict[str, Any]]:
    """Load the dataset and return task/model/labels/weights plus auxiliary data."""

    dataset_path = Path(path).expanduser()
    metadata, sidecar_path = _load_sidecar(dataset_path, metadata_path)
    with np.load(str(dataset_path), allow_pickle=False) as archive:
        task = _first_array(
            archive, ("task_features", "request_features", "X_task", "tasks")
        )
        model = _first_array(
            archive, ("model_features", "candidate_features", "X_model", "models")
        )
        labels = _first_array(archive, ("labels", "pointwise_labels", "outcomes", "y"))
        if task is None or model is None or labels is None:
            raise ValueError(
                "dataset must contain task_features, model_features, and labels; found %s"
                % ", ".join(sorted(archive.files))
            )
        task = np.asarray(task, dtype=np.float64)
        model = np.asarray(model, dtype=np.float64)
        labels = np.asarray(labels, dtype=np.float64)
        if labels.ndim == 1:
            labels = labels[:, None]
        if task.ndim != 2 or model.ndim != 2 or labels.ndim != 2:
            raise ValueError("task_features, model_features, and labels must be rank-2")
        if task.shape[0] != model.shape[0] or task.shape[0] != labels.shape[0]:
            raise ValueError("pointwise arrays must have identical row counts")

        label_mask = _first_array(
            archive, ("label_mask", "labels_mask", "outcome_mask")
        )
        if label_mask is not None:
            mask = np.asarray(label_mask, dtype=bool)
            if mask.shape != labels.shape:
                raise ValueError(
                    "label_mask shape %r does not match labels %r"
                    % (mask.shape, labels.shape)
                )
            labels = np.where(mask, labels, np.nan)

        weights_array = _first_array(
            archive, ("sample_weights", "pointwise_weights", "weights")
        )
        weights = (
            np.ones(task.shape[0], dtype=np.float64)
            if weights_array is None
            else np.asarray(weights_array, dtype=np.float64).reshape(-1)
        )
        if weights.size != task.shape[0]:
            raise ValueError("sample weight length does not match rows")

        task_names_array = _first_array(
            archive, ("task_feature_names", "request_feature_names")
        )
        model_names_array = _first_array(
            archive, ("model_feature_names", "candidate_feature_names")
        )
        label_names_array = _first_array(archive, ("label_names", "outcome_names"))
        task_names = (
            _string_array(task_names_array)
            if task_names_array is not None
            else _names_from_metadata(metadata, "task_feature_names", task.shape[1])
        )
        model_names = (
            _string_array(model_names_array)
            if model_names_array is not None
            else _names_from_metadata(metadata, "model_feature_names", model.shape[1])
        )
        label_names = (
            _string_array(label_names_array)
            if label_names_array is not None
            else metadata.get("label_names")
        )
        if not isinstance(label_names, list) or len(label_names) != labels.shape[1]:
            label_names = (
                list(DEFAULT_LABEL_NAMES)
                if labels.shape[1] == len(DEFAULT_LABEL_NAMES)
                else ["outcome_%d" % index for index in range(labels.shape[1])]
            )
        task_names = task_names or [
            "task_%03d" % index for index in range(task.shape[1])
        ]
        model_names = model_names or [
            "model_%03d" % index for index in range(model.shape[1])
        ]
        if len(task_names) != task.shape[1] or len(model_names) != model.shape[1]:
            raise ValueError("feature-name count does not match feature matrix")

        group_ids = _first_array(
            archive, ("group_ids", "prompt_group_ids", "request_group_ids")
        )
        if group_ids is None:
            group_ids = _first_array(archive, ("prompt_hashes", "request_hashes"))
        pairs = _first_array(archive, ("pair_indices", "pairs", "preference_pairs"))
        preferences = _first_array(
            archive, ("pair_preference", "pair_labels", "preferences")
        )
        pair_weights = _first_array(archive, ("pair_weights", "preference_weights"))
        pair_deltas = _first_array(archive, ("pair_deltas",))
        model_ids = _first_array(archive, ("model_ids", "candidate_ids"))
        provider_ids = _first_array(archive, ("provider_ids",))
        family_ids = _first_array(archive, ("family_ids", "model_family_ids"))
        instance_ids = _first_array(archive, ("instance_ids",))
        catalog_ids = _first_array(archive, ("catalog_ids", "catalog_model_ids"))
        catalog_features = _first_array(
            archive, ("catalog_features", "catalog_model_features")
        )
        temporal_split = _first_array(archive, ("temporal_split", "split_ids"))
        event_epochs = _first_array(archive, ("event_epochs", "timestamps"))
        model_holdout_ids = _first_array(archive, ("model_holdout_ids",))
        provider_holdout_ids = _first_array(archive, ("provider_holdout_ids",))
        family_holdout_ids = _first_array(archive, ("family_holdout_ids",))
        model_holdout_fold = _first_array(archive, ("model_holdout_fold",))
        provider_holdout_fold = _first_array(archive, ("provider_holdout_fold",))
        family_holdout_fold = _first_array(archive, ("family_holdout_fold",))
        catalog_provider_ids = _first_array(archive, ("catalog_provider_ids",))
        catalog_family_ids = _first_array(archive, ("catalog_family_ids",))

        quality_model_ids = _first_array(archive, ("catalog_quality_model_ids",))
        quality_features = _first_array(archive, ("catalog_quality_features",))
        quality_labels = _first_array(archive, ("catalog_quality_labels",))
        quality_label_mask = _first_array(archive, ("catalog_quality_label_mask",))
        quality_label_names = _first_array(archive, ("catalog_quality_label_names",))
        quality_label_provenance = _first_array(
            archive, ("catalog_quality_label_provenance",)
        )
        quality_catalog_indices = _first_array(
            archive, ("catalog_quality_catalog_indices",)
        )
        quality_model_holdout_ids = _first_array(
            archive, ("catalog_quality_model_holdout_ids",)
        )
        quality_provider_holdout_ids = _first_array(
            archive, ("catalog_quality_provider_holdout_ids",)
        )
        quality_family_holdout_ids = _first_array(
            archive, ("catalog_quality_family_holdout_ids",)
        )
        quality_model_holdout_fold = _first_array(
            archive, ("catalog_quality_model_holdout_fold",)
        )
        quality_provider_holdout_fold = _first_array(
            archive, ("catalog_quality_provider_holdout_fold",)
        )
        quality_family_holdout_fold = _first_array(
            archive, ("catalog_quality_family_holdout_fold",)
        )

        auxiliary: Dict[str, Any] = {
            "dataset_path": str(dataset_path),
            "metadata_path": str(sidecar_path) if sidecar_path else None,
            "metadata": metadata,
            "task_feature_names": task_names,
            "model_feature_names": model_names,
            "label_names": [str(value) for value in label_names],
            "group_ids": (
                None if group_ids is None else np.asarray(group_ids).reshape(-1)
            ),
            "pair_indices": (
                None if pairs is None else np.asarray(pairs, dtype=np.int64)
            ),
            "pair_preference": (
                None
                if preferences is None
                else np.asarray(preferences, dtype=np.float64).reshape(-1)
            ),
            "pair_weights": (
                None
                if pair_weights is None
                else np.asarray(pair_weights, dtype=np.float64).reshape(-1)
            ),
            "pair_deltas": (
                None
                if pair_deltas is None
                else np.asarray(pair_deltas, dtype=np.float64)
            ),
            "model_ids": None if model_ids is None else np.asarray(model_ids),
            "provider_ids": None if provider_ids is None else np.asarray(provider_ids),
            "family_ids": None if family_ids is None else np.asarray(family_ids),
            "instance_ids": None if instance_ids is None else np.asarray(instance_ids),
            "catalog_ids": None if catalog_ids is None else np.asarray(catalog_ids),
            "catalog_features": (
                None
                if catalog_features is None
                else np.asarray(catalog_features, dtype=np.float64)
            ),
            "temporal_split": (
                None
                if temporal_split is None
                else np.asarray(temporal_split, dtype=np.int64).reshape(-1)
            ),
            "event_epochs": None
            if event_epochs is None
            else np.asarray(event_epochs, dtype=np.float64).reshape(-1),
            "model_holdout_ids": None
            if model_holdout_ids is None
            else np.asarray(model_holdout_ids),
            "provider_holdout_ids": None
            if provider_holdout_ids is None
            else np.asarray(provider_holdout_ids),
            "family_holdout_ids": None
            if family_holdout_ids is None
            else np.asarray(family_holdout_ids),
            "model_holdout_fold": None
            if model_holdout_fold is None
            else np.asarray(model_holdout_fold, dtype=np.int64).reshape(-1),
            "provider_holdout_fold": None
            if provider_holdout_fold is None
            else np.asarray(provider_holdout_fold, dtype=np.int64).reshape(-1),
            "family_holdout_fold": None
            if family_holdout_fold is None
            else np.asarray(family_holdout_fold, dtype=np.int64).reshape(-1),
            "catalog_provider_ids": None
            if catalog_provider_ids is None
            else np.asarray(catalog_provider_ids),
            "catalog_family_ids": None
            if catalog_family_ids is None
            else np.asarray(catalog_family_ids),
            "catalog_quality_model_ids": None
            if quality_model_ids is None
            else np.asarray(quality_model_ids),
            "catalog_quality_features": None
            if quality_features is None
            else np.asarray(quality_features, dtype=np.float64),
            "catalog_quality_labels": None
            if quality_labels is None
            else np.asarray(quality_labels, dtype=np.float64),
            "catalog_quality_label_mask": None
            if quality_label_mask is None
            else np.asarray(quality_label_mask, dtype=bool),
            "catalog_quality_label_names": None
            if quality_label_names is None
            else np.asarray(quality_label_names),
            "catalog_quality_label_provenance": None
            if quality_label_provenance is None
            else np.asarray(quality_label_provenance),
            "catalog_quality_catalog_indices": None
            if quality_catalog_indices is None
            else np.asarray(quality_catalog_indices, dtype=np.int64).reshape(-1),
            "catalog_quality_model_holdout_ids": None
            if quality_model_holdout_ids is None
            else np.asarray(quality_model_holdout_ids),
            "catalog_quality_provider_holdout_ids": None
            if quality_provider_holdout_ids is None
            else np.asarray(quality_provider_holdout_ids),
            "catalog_quality_family_holdout_ids": None
            if quality_family_holdout_ids is None
            else np.asarray(quality_family_holdout_ids),
            "catalog_quality_model_holdout_fold": None
            if quality_model_holdout_fold is None
            else np.asarray(quality_model_holdout_fold, dtype=np.int64).reshape(-1),
            "catalog_quality_provider_holdout_fold": None
            if quality_provider_holdout_fold is None
            else np.asarray(quality_provider_holdout_fold, dtype=np.int64).reshape(-1),
            "catalog_quality_family_holdout_fold": None
            if quality_family_holdout_fold is None
            else np.asarray(quality_family_holdout_fold, dtype=np.int64).reshape(-1),
        }
    return task, model, labels, weights, auxiliary


def grouped_split(
    rows: int, group_ids: Optional[np.ndarray], validation_fraction: float, seed: int
) -> Tuple[np.ndarray, np.ndarray]:
    """Make a deterministic group-disjoint train/validation split."""

    if not 0.0 <= validation_fraction < 1.0:
        raise ValueError("validation_fraction must be in [0, 1)")
    all_rows = np.arange(rows, dtype=np.int64)
    if validation_fraction == 0.0 or rows < 3:
        return all_rows, np.empty(0, dtype=np.int64)
    rng = np.random.default_rng(seed)
    if group_ids is not None:
        groups = np.asarray(group_ids).reshape(-1)
        if groups.size != rows:
            raise ValueError("group_ids length does not match rows")
        unique_groups = np.unique(groups)
        if unique_groups.size >= 2:
            shuffled = unique_groups.copy()
            rng.shuffle(shuffled)
            count = min(
                max(int(round(unique_groups.size * validation_fraction)), 1),
                unique_groups.size - 1,
            )
            validation_groups = shuffled[:count]
            validation_mask = np.isin(groups, validation_groups)
            return all_rows[~validation_mask], all_rows[validation_mask]
    shuffled_rows = all_rows.copy()
    rng.shuffle(shuffled_rows)
    count = min(max(int(round(rows * validation_fraction)), 1), rows - 1)
    validation = np.sort(shuffled_rows[:count])
    training = np.sort(shuffled_rows[count:])
    return training, validation


def _head_types(names: Sequence[str], labels: np.ndarray) -> List[str]:
    probability_tokens = (
        "success",
        "failure",
        "error",
        "quality",
        "valid",
        "refusal",
        "prob",
    )
    result: List[str] = []
    for index, name in enumerate(names):
        finite = labels[:, index][np.isfinite(labels[:, index])]
        is_bounded = (
            finite.size > 0
            and float(np.min(finite)) >= 0.0
            and float(np.max(finite)) <= 1.0
        )
        result.append(
            "probability"
            if is_bounded and any(token in name.lower() for token in probability_tokens)
            else (
                "positive"
                if any(
                    token in name.lower()
                    for token in ("cost", "latency", "duration", "tokens", "seconds")
                )
                else "continuous"
            )
        )
    return result


def evaluate(
    router: CandidateRouter,
    task: np.ndarray,
    model: np.ndarray,
    labels: np.ndarray,
    indices: np.ndarray,
    pairs: Optional[np.ndarray],
    preferences: Optional[np.ndarray],
    pair_head: Optional[str],
) -> Dict[str, Any]:
    evaluation_indices = (
        indices if indices.size else np.arange(task.shape[0], dtype=np.int64)
    )
    predicted = router.predict(task[evaluation_indices], model[evaluation_indices])
    metrics: Dict[str, Any] = {"rows": int(evaluation_indices.size), "heads": {}}
    for head, name in enumerate(router.outcome_names):
        actual = labels[evaluation_indices, head]
        mask = np.isfinite(actual)
        if not np.any(mask):
            continue
        residual = predicted[mask, head] - actual[mask]
        head_metrics: Dict[str, float] = {
            "mae": float(np.mean(np.abs(residual))),
            "rmse": float(np.sqrt(np.mean(residual * residual))),
            "count": int(np.sum(mask)),
        }
        if router.head_types[head] == "probability":
            head_metrics["brier"] = float(np.mean(residual * residual))
        metrics["heads"][name] = head_metrics

    if (
        pairs is not None
        and preferences is not None
        and pair_head in router.outcome_names
    ):
        pair_array = np.asarray(pairs, dtype=np.int64)
        pref = np.asarray(preferences, dtype=np.float64).reshape(-1)
        membership = np.zeros(task.shape[0], dtype=bool)
        membership[evaluation_indices] = True
        keep = (
            membership[pair_array[:, 0]] & membership[pair_array[:, 1]] & (pref != 0.5)
        )
        if np.any(keep):
            raw = router.raw_scores(task, model)[
                :, router.outcome_names.index(pair_head)
            ]
            chosen_left = raw[pair_array[keep, 0]] > raw[pair_array[keep, 1]]
            expected_left = pref[keep] > 0.5
            metrics["pair_accuracy"] = float(np.mean(chosen_left == expected_left))
            metrics["pair_count"] = int(np.sum(keep))
            left_rate = float(np.mean(expected_left))
            metrics["pair_majority_baseline"] = max(left_rate, 1.0 - left_rate)
    return metrics


def _json_safe(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(key): _json_safe(child) for key, child in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(child) for child in value]
    if isinstance(value, np.ndarray):
        return [_json_safe(child) for child in value.tolist()]
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def _feature_version(metadata: Mapping[str, Any], name: str) -> Optional[str]:
    """Read a feature version from the sidecar's supported nesting shapes."""

    containers: List[Mapping[str, Any]] = [metadata]
    for key in ("feature_contract", "features"):
        child = metadata.get(key)
        if isinstance(child, Mapping):
            containers.append(child)
    for container in containers:
        value = container.get(name)
        if value is not None:
            return str(value)
    return None


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="build_dataset.py NPZ")
    parser.add_argument(
        "--metadata", help="optional JSON sidecar (auto-discovered by default)"
    )
    parser.add_argument("--output", required=True, help="versioned router .npz output")
    parser.add_argument(
        "--rank", type=int, default=24, help="interaction rank (default: 24)"
    )
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--batch-size", type=int, default=512)
    parser.add_argument("--learning-rate", type=float, default=0.005)
    parser.add_argument("--weight-decay", type=float, default=1.0e-5)
    parser.add_argument("--validation-fraction", type=float, default=0.15)
    parser.add_argument(
        "--split",
        choices=("auto", "temporal", "group-random"),
        default="auto",
        help="use dataset temporal split when present (default: auto)",
    )
    parser.add_argument("--patience", type=int, default=30)
    parser.add_argument("--pair-loss-weight", type=float, default=0.25)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--quiet", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    np.seterr(over="raise", invalid="raise")
    task, model, base_labels, weights, auxiliary = load_dataset(
        args.dataset, args.metadata
    )
    dataset_task_version = _feature_version(
        auxiliary["metadata"], "task_feature_version"
    )
    dataset_model_version = _feature_version(
        auxiliary["metadata"], "model_feature_version"
    )
    if (
        dataset_task_version is not None
        and dataset_task_version != TASK_FEATURE_VERSION
    ):
        raise ValueError(
            "dataset task feature version %r does not match trainer %r"
            % (dataset_task_version, TASK_FEATURE_VERSION)
        )
    if (
        dataset_model_version is not None
        and dataset_model_version != MODEL_FEATURE_VERSION
    ):
        raise ValueError(
            "dataset model feature version %r does not match trainer %r"
            % (dataset_model_version, MODEL_FEATURE_VERSION)
        )
    temporal_split = auxiliary["temporal_split"]
    test_indices = np.empty(0, dtype=np.int64)
    if args.split in ("auto", "temporal") and temporal_split is not None:
        if temporal_split.size != task.shape[0]:
            raise ValueError("temporal_split length does not match rows")
        train_indices = np.flatnonzero(temporal_split == 0).astype(np.int64)
        validation_indices = np.flatnonzero(temporal_split == 1).astype(np.int64)
        test_indices = np.flatnonzero(temporal_split == 2).astype(np.int64)
        if train_indices.size == 0:
            raise ValueError("temporal training split is empty")
        split_kind = "temporal"
    elif args.split == "temporal":
        raise ValueError(
            "--split temporal requested, but dataset has no temporal_split"
        )
    else:
        train_indices, validation_indices = grouped_split(
            task.shape[0], auxiliary["group_ids"], args.validation_fraction, args.seed
        )
        split_kind = "group-random"

    labels = base_labels
    outcome_names = list(auxiliary["label_names"])
    pairs = auxiliary["pair_indices"]
    preferences = auxiliary["pair_preference"]
    pair_head: Optional[str] = None
    use_pairs = (
        pairs is not None and preferences is not None and args.pair_loss_weight > 0.0
    )
    if use_pairs:
        # A separate pair-only score shares the towers but does not corrupt the
        # calibration of primitive success/failure/cost/latency heads.
        pair_head = "preference_score"
        labels = np.column_stack(
            (labels, np.full(labels.shape[0], np.nan, dtype=np.float64))
        )
        outcome_names.append(pair_head)

    head_types = _head_types(outcome_names, labels)
    router = CandidateRouter(
        task_dim=task.shape[1],
        model_dim=model.shape[1],
        outcome_names=outcome_names,
        rank=args.rank,
        seed=args.seed,
        task_feature_names=auxiliary["task_feature_names"],
        model_feature_names=auxiliary["model_feature_names"],
        head_types=head_types,
    )
    result = router.fit(
        task,
        model,
        labels,
        sample_weights=weights,
        train_indices=train_indices,
        validation_indices=validation_indices,
        pair_indices=pairs if use_pairs else None,
        pair_labels=preferences if use_pairs else None,
        pair_weights=auxiliary["pair_weights"] if use_pairs else None,
        pair_head=pair_head,
        pair_loss_weight=args.pair_loss_weight,
        epochs=args.epochs,
        batch_size=args.batch_size,
        learning_rate=args.learning_rate,
        weight_decay=args.weight_decay,
        patience=args.patience,
        seed=args.seed,
        verbose=not args.quiet,
    )
    metrics = evaluate(
        router,
        task,
        model,
        labels,
        validation_indices,
        pairs,
        preferences,
        pair_head,
    )
    test_metrics = (
        evaluate(
            router, task, model, labels, test_indices, pairs, preferences, pair_head
        )
        if test_indices.size
        else None
    )
    export_metadata = {
        "dataset_schema": _json_safe(auxiliary["metadata"].get("schema_version")),
        "dataset_metadata": _json_safe(auxiliary["metadata"]),
        "dataset_rows": int(task.shape[0]),
        "catalog_models": (
            int(len(auxiliary["catalog_ids"]))
            if auxiliary["catalog_ids"] is not None
            else None
        ),
        "task_feature_version": dataset_task_version or TASK_FEATURE_VERSION,
        "model_feature_version": dataset_model_version or MODEL_FEATURE_VERSION,
        "split_kind": split_kind,
        "training_result": asdict(result),
        "validation_metrics": metrics,
        "test_metrics": test_metrics,
    }
    router.save(args.output, metadata=export_metadata)
    summary = {
        "artifact": str(Path(args.output).expanduser()),
        "format": "chimera-router-numpy-v1",
        "task_dim": router.task_dim,
        "model_dim": router.model_dim,
        "rank": router.rank,
        "outcomes": list(router.outcome_names),
        "train_rows": int(train_indices.size),
        "validation_rows": int(validation_indices.size),
        "test_rows": int(test_indices.size),
        "split_kind": split_kind,
        "training": asdict(result),
        "validation": metrics,
        "test": test_metrics,
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
