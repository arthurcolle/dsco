#!/usr/bin/env python3
"""Deterministic ensemble, calibration, and catalog-quality training for Chimera.

The v2 router remains candidate-conditioned.  Diversity comes from seeded,
group-bootstrap member fits rather than a fixed model-ID vocabulary.  Binary
calibration is learned only from the validation split and stored beside the
members in one pickle-free, deterministic NPZ artifact.

``RouterEnsemble.load`` also accepts a v1 single-model artifact and wraps it as
an identity-calibrated one-member ensemble, preserving existing callers.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import numpy as np

try:
    from .features import MODEL_FEATURE_VERSION, TASK_FEATURE_VERSION
    from .model import CandidateRouter, MODEL_KIND
    from .train import _feature_version, _head_types, grouped_split, load_dataset
except ImportError:  # Direct execution.
    from features import MODEL_FEATURE_VERSION, TASK_FEATURE_VERSION
    from model import CandidateRouter, MODEL_KIND
    from train import _feature_version, _head_types, grouped_split, load_dataset


ENSEMBLE_KIND = "chimera-router-ensemble"
ENSEMBLE_FORMAT_VERSION = 2
QUALITY_TASK_FEATURE_VERSION = "chimera-catalog-quality-constant-v1"
EPS = 1.0e-12

_STATE_ARRAYS = (
    "task_mean",
    "task_scale",
    "model_mean",
    "model_scale",
    "outcome_mean",
    "outcome_scale",
    "task_train_min",
    "task_train_max",
    "model_train_min",
    "model_train_max",
)


def _sigmoid(value: np.ndarray) -> np.ndarray:
    value = np.asarray(value, dtype=np.float64)
    result = np.empty_like(value)
    positive = value >= 0.0
    result[positive] = 1.0 / (1.0 + np.exp(-value[positive]))
    exp_value = np.exp(value[~positive])
    result[~positive] = exp_value / (1.0 + exp_value)
    return result


def _logit(value: np.ndarray) -> np.ndarray:
    clipped = np.clip(np.asarray(value, dtype=np.float64), 1.0e-6, 1.0 - 1.0e-6)
    return np.log(clipped) - np.log1p(-clipped)


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


def _indices_sha256(indices: np.ndarray) -> str:
    canonical = np.sort(np.asarray(indices, dtype="<i8").reshape(-1))
    return hashlib.sha256(canonical.tobytes()).hexdigest()


def _stable_fold(value: str, folds: int = 10) -> int:
    digest = hashlib.blake2b(str(value).encode("utf-8"), digest_size=8).digest()
    return int.from_bytes(digest, "little") % folds


def _provider_id(model_id: str) -> str:
    value = str(model_id).removeprefix("openrouter/")
    return value.split("/", 1)[0] if "/" in value else "unknown"


def _family_id(model_id: str) -> str:
    """Conservative fallback used only when the v2 dataset lacks family IDs."""

    value = str(model_id).removeprefix("openrouter/").casefold()
    provider, slug = value.split("/", 1) if "/" in value else ("unknown", value)
    pieces = [piece for piece in slug.replace("_", "-").split("-") if piece]
    family: List[str] = []
    for piece in pieces:
        if any(character.isdigit() for character in piece) or piece in {
            "preview",
            "latest",
            "beta",
            "free",
            "instruct",
        }:
            break
        family.append(piece)
        if len(family) >= 2:
            break
    return provider + "/" + ("-".join(family) if family else slug)


def _write_npz_deterministic(path: Path, arrays: Mapping[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(
            temporary,
            mode="w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
        ) as archive:
            for name in sorted(arrays):
                array = np.asarray(arrays[name])
                if array.dtype.hasobject:
                    raise ValueError("object arrays are forbidden: %s" % name)
                buffer = io.BytesIO()
                np.lib.format.write_array(buffer, array, allow_pickle=False)
                info = zipfile.ZipInfo(name + ".npy", date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o600 << 16
                archive.writestr(info, buffer.getvalue())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _fit_platt_brier(
    probabilities: np.ndarray, labels: np.ndarray
) -> Tuple[float, float, float, float]:
    """Fit monotone Platt parameters with deterministic Brier selection."""

    probability = np.clip(
        np.asarray(probabilities, dtype=np.float64), 1.0e-6, 1.0 - 1.0e-6
    )
    target = np.asarray(labels, dtype=np.float64)
    x = _logit(probability)
    before = float(np.mean((probability - target) ** 2))
    prevalence = float(np.clip(np.mean(target), 1.0e-6, 1.0 - 1.0e-6))
    prevalence_logit = float(_logit(np.asarray([prevalence]))[0])

    best_a, best_b, best_loss = 1.0, 0.0, before
    slopes = (0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0)
    offsets = (-1.5, -0.75, -0.25, 0.0, 0.25, 0.75, 1.5)
    for slope in slopes:
        center = prevalence_logit - slope * float(np.mean(x))
        intercepts = (0.0,) + tuple(center + offset for offset in offsets)
        for intercept in intercepts:
            calibrated = _sigmoid(slope * x + intercept)
            loss = float(np.mean((calibrated - target) ** 2))
            candidate = (loss, abs(slope - 1.0) + abs(intercept), slope, intercept)
            incumbent = (best_loss, abs(best_a - 1.0) + abs(best_b), best_a, best_b)
            if candidate < incumbent:
                best_loss, best_a, best_b = loss, slope, intercept

    step_a, step_b = 0.25, 0.25
    for _ in range(8):
        for delta_a in (-step_a, 0.0, step_a):
            for delta_b in (-step_b, 0.0, step_b):
                slope = min(max(best_a + delta_a, 0.0), 8.0)
                intercept = min(max(best_b + delta_b, -12.0), 12.0)
                calibrated = _sigmoid(slope * x + intercept)
                loss = float(np.mean((calibrated - target) ** 2))
                candidate = (loss, abs(slope - 1.0) + abs(intercept), slope, intercept)
                incumbent = (best_loss, abs(best_a - 1.0) + abs(best_b), best_a, best_b)
                if candidate < incumbent:
                    best_loss, best_a, best_b = loss, slope, intercept
        step_a *= 0.5
        step_b *= 0.5
    if (
        not all(math.isfinite(value) for value in (best_a, best_b, best_loss))
        or best_loss >= before - 1.0e-12
    ):
        return 1.0, 0.0, before, before
    return best_a, best_b, before, best_loss


@dataclass
class BinaryCalibrator:
    slopes: np.ndarray
    intercepts: np.ndarray
    active: np.ndarray
    head_types: Tuple[str, ...]
    metadata: Dict[str, Any]

    @classmethod
    def identity(cls, head_types: Sequence[str]) -> "BinaryCalibrator":
        count = len(tuple(head_types))
        return cls(
            slopes=np.ones(count, dtype=np.float64),
            intercepts=np.zeros(count, dtype=np.float64),
            active=np.zeros(count, dtype=bool),
            head_types=tuple(str(value) for value in head_types),
            metadata={"source": "identity", "heads": {}},
        )

    @classmethod
    def fit(
        cls,
        predictions: np.ndarray,
        labels: np.ndarray,
        head_types: Sequence[str],
        outcome_names: Sequence[str],
        *,
        minimum_rows: int = 20,
        validation_indices_hash: Optional[str] = None,
    ) -> "BinaryCalibrator":
        predicted = np.asarray(predictions, dtype=np.float64)
        target = np.asarray(labels, dtype=np.float64)
        if predicted.shape != target.shape or predicted.ndim != 2:
            raise ValueError(
                "calibration predictions/labels must have equal rank-2 shapes"
            )
        calibrator = cls.identity(head_types)
        head_metadata: Dict[str, Any] = {}
        for index, (name, head_type) in enumerate(zip(outcome_names, head_types)):
            mask = np.isfinite(predicted[:, index]) & np.isfinite(target[:, index])
            observed = target[mask, index]
            binary = mask.copy()
            if observed.size:
                binary[mask] = (observed == 0.0) | (observed == 1.0)
            count = int(np.sum(binary))
            if head_type != "probability" or count < minimum_rows:
                head_metadata[str(name)] = {
                    "active": False,
                    "rows": count,
                    "reason": "not_binary_or_insufficient_rows",
                }
                continue
            slope, intercept, before, after = _fit_platt_brier(
                predicted[binary, index], target[binary, index]
            )
            calibrator.slopes[index] = slope
            calibrator.intercepts[index] = intercept
            calibrator.active[index] = after < before - 1.0e-12
            head_metadata[str(name)] = {
                "active": bool(calibrator.active[index]),
                "rows": count,
                "slope": slope,
                "intercept": intercept,
                "brier_before": before,
                "brier_after": after,
                "brier_improvement": before - after,
            }
        calibrator.metadata = {
            "source": "validation_only",
            "validation_indices_sha256": validation_indices_hash,
            "heads": head_metadata,
        }
        return calibrator

    def transform(self, predictions: np.ndarray) -> np.ndarray:
        result = np.asarray(predictions, dtype=np.float64).copy()
        if result.ndim != 2 or result.shape[1] != self.slopes.size:
            raise ValueError("prediction shape does not match calibrator")
        for index, head_type in enumerate(self.head_types):
            if head_type == "probability":
                result[:, index] = _sigmoid(
                    self.slopes[index] * _logit(result[:, index])
                    + self.intercepts[index]
                )
        return result

    def transform_members(self, predictions: np.ndarray) -> np.ndarray:
        member_values = np.asarray(predictions, dtype=np.float64)
        if member_values.ndim != 3:
            raise ValueError(
                "member prediction tensor must have shape [members, rows, heads]"
            )
        return np.stack([self.transform(member) for member in member_values], axis=0)


class RouterEnsemble:
    """A compatible set of candidate routers plus validation-only calibration."""

    def __init__(
        self,
        members: Sequence[CandidateRouter],
        calibrator: Optional[BinaryCalibrator] = None,
        metadata: Optional[Mapping[str, Any]] = None,
    ) -> None:
        if not members:
            raise ValueError("ensemble requires at least one member")
        self.members = tuple(members)
        first = self.members[0]
        for member in self.members[1:]:
            if (
                member.task_dim != first.task_dim
                or member.model_dim != first.model_dim
                or member.outcome_names != first.outcome_names
                or member.head_types != first.head_types
                or member.task_feature_names != first.task_feature_names
                or member.model_feature_names != first.model_feature_names
            ):
                raise ValueError("ensemble members have incompatible contracts")
        self.task_dim = first.task_dim
        self.model_dim = first.model_dim
        self.outcome_names = first.outcome_names
        self.head_types = first.head_types
        self.task_feature_names = first.task_feature_names
        self.model_feature_names = first.model_feature_names
        self.calibrator = calibrator or BinaryCalibrator.identity(self.head_types)
        self.calibrator.slopes = np.asarray(
            self.calibrator.slopes, dtype=np.float64
        ).reshape(-1)
        self.calibrator.intercepts = np.asarray(
            self.calibrator.intercepts, dtype=np.float64
        ).reshape(-1)
        self.calibrator.active = np.asarray(self.calibrator.active, dtype=bool).reshape(
            -1
        )
        expected_calibrator_shape = (len(self.outcome_names),)
        if (
            self.calibrator.slopes.shape != expected_calibrator_shape
            or self.calibrator.intercepts.shape != expected_calibrator_shape
            or self.calibrator.active.shape != expected_calibrator_shape
            or tuple(self.calibrator.head_types) != self.head_types
        ):
            raise ValueError("calibrator contract does not match ensemble heads")
        if np.any(self.calibrator.slopes < 0.0):
            raise ValueError("calibrator slopes must be nonnegative")
        self.training_metadata = dict(metadata or {})
        self._validate_finite_parameters()

    def _validate_finite_parameters(self) -> None:
        for member_index, member in enumerate(self.members):
            support_bounds = (
                "task_train_min",
                "task_train_max",
                "model_train_min",
                "model_train_max",
            )
            for name in member.parameter_names + tuple(
                item for item in _STATE_ARRAYS if item not in support_bounds
            ):
                value = np.asarray(getattr(member, name), dtype=np.float64)
                if not np.all(np.isfinite(value)):
                    raise ValueError(
                        "non-finite member %d array %s" % (member_index, name)
                    )
                if name in ("task_scale", "model_scale", "outcome_scale") and np.any(
                    value <= 0.0
                ):
                    raise ValueError(
                        "nonpositive member %d array %s" % (member_index, name)
                    )
            # An untrained legacy CandidateRouter intentionally uses +/-inf to
            # mean "unknown support bounds". Preserve that v1 compatibility,
            # while still rejecting NaNs and inverted learned ranges.
            for minimum_name, maximum_name in (
                ("task_train_min", "task_train_max"),
                ("model_train_min", "model_train_max"),
            ):
                minimum = np.asarray(getattr(member, minimum_name), dtype=np.float64)
                maximum = np.asarray(getattr(member, maximum_name), dtype=np.float64)
                if np.any(np.isnan(minimum)) or np.any(np.isnan(maximum)):
                    raise ValueError(
                        "NaN member %d support bounds %s/%s"
                        % (member_index, minimum_name, maximum_name)
                    )
                finite_range = np.isfinite(minimum) & np.isfinite(maximum)
                legacy_unknown_range = np.isneginf(minimum) & np.isposinf(maximum)
                if np.any(~(finite_range | legacy_unknown_range)) or np.any(
                    finite_range & (minimum > maximum)
                ):
                    raise ValueError(
                        "invalid member %d support bounds %s/%s"
                        % (member_index, minimum_name, maximum_name)
                    )
        for name, value in (
            ("calibration slopes", self.calibrator.slopes),
            ("calibration intercepts", self.calibrator.intercepts),
        ):
            if not np.all(np.isfinite(value)):
                raise ValueError("non-finite %s" % name)

    def _model_features_for_inference(self, model_features: np.ndarray) -> np.ndarray:
        """Apply artifact-owned feature suppression required by auxiliary heads.

        Catalog quality is deliberately trained without the Artificial Analysis
        descriptor columns that supply its weak labels.  Enforcing the same
        projection inside the loaded artifact prevents a route caller from
        accidentally reintroducing those columns at inference time.
        """

        columns = self.training_metadata.get("benchmark_feature_columns_zeroed")
        if columns is None:
            return model_features
        indices = np.asarray(columns, dtype=np.int64).reshape(-1)
        if indices.size == 0:
            return model_features
        result = np.asarray(model_features, dtype=np.float64).copy()
        was_vector = result.ndim == 1
        if was_vector:
            result = result[None, :]
        if result.ndim != 2 or result.shape[1] != self.model_dim:
            raise ValueError("model feature shape does not match ensemble")
        if np.any(indices < 0) or np.any(indices >= self.model_dim):
            raise ValueError("invalid benchmark feature suppression column")
        result[:, indices] = 0.0
        return result[0] if was_vector else result

    def predict_members(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        model_features = self._model_features_for_inference(model_features)
        tensor = np.stack(
            [member.predict(task_features, model_features) for member in self.members],
            axis=0,
        )
        if not np.all(np.isfinite(tensor)):
            raise FloatingPointError("non-finite ensemble prediction")
        return tensor

    def predict_uncalibrated(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        return np.mean(self.predict_members(task_features, model_features), axis=0)

    def predict(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        return np.mean(
            self.calibrator.transform_members(
                self.predict_members(task_features, model_features)
            ),
            axis=0,
        )

    def predict_distribution(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> Dict[str, np.ndarray]:
        """Return calibrated point moments plus conservative epistemic spread.

        A prevalence-only Platt map may have slope zero.  Such calibration can
        improve the mean without making independently bootstrapped members
        agree, so the routing-facing ``std`` never falls below raw member
        disagreement. ``calibrated_member_std`` exposes the literal calibrated
        ensemble moment for diagnostics.
        """

        member_values = self.predict_members(task_features, model_features)
        calibrated_members = self.calibrator.transform_members(member_values)
        calibrated_std = np.std(calibrated_members, axis=0, ddof=0)
        uncalibrated_std = np.std(member_values, axis=0, ddof=0)
        result = {
            "mean": np.mean(calibrated_members, axis=0),
            "std": np.maximum(calibrated_std, uncalibrated_std),
            "calibrated_member_std": calibrated_std,
            "uncalibrated_mean": np.mean(member_values, axis=0),
            "uncalibrated_std": uncalibrated_std,
        }
        if not all(np.all(np.isfinite(value)) for value in result.values()):
            raise FloatingPointError("non-finite ensemble distribution")
        return result

    def raw_scores(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        model_features = self._model_features_for_inference(model_features)
        result = np.mean(
            np.stack(
                [
                    member.raw_scores(task_features, model_features)
                    for member in self.members
                ],
                axis=0,
            ),
            axis=0,
        )
        if not np.all(np.isfinite(result)):
            raise FloatingPointError("non-finite ensemble raw score")
        return result

    def support_distance(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        model_features = self._model_features_for_inference(model_features)
        result = np.mean(
            np.stack(
                [
                    member.support_distance(task_features, model_features)
                    for member in self.members
                ],
                axis=0,
            ),
            axis=0,
        )
        if not np.all(np.isfinite(result)):
            raise FloatingPointError("non-finite ensemble support distance")
        return result

    def save(self, path: str) -> None:
        destination = Path(path).expanduser()
        header = {
            "format_version": ENSEMBLE_FORMAT_VERSION,
            "model_kind": ENSEMBLE_KIND,
            "member_count": len(self.members),
            "member_ranks": [member.rank for member in self.members],
            "task_dim": self.task_dim,
            "model_dim": self.model_dim,
            "output_dim": len(self.outcome_names),
            "metadata": _json_safe(self.training_metadata),
            "member_metadata": [
                _json_safe(member.training_metadata) for member in self.members
            ],
            "calibration_metadata": _json_safe(self.calibrator.metadata),
        }
        arrays: Dict[str, np.ndarray] = {
            "header_json": np.asarray(
                json.dumps(header, sort_keys=True, separators=(",", ":"))
            ),
            "task_feature_names": np.asarray(self.task_feature_names, dtype=np.str_),
            "model_feature_names": np.asarray(self.model_feature_names, dtype=np.str_),
            "outcome_names": np.asarray(self.outcome_names, dtype=np.str_),
            "head_types": np.asarray(self.head_types, dtype=np.str_),
            "calibration_slopes": np.asarray(self.calibrator.slopes, dtype=np.float64),
            "calibration_intercepts": np.asarray(
                self.calibrator.intercepts, dtype=np.float64
            ),
            "calibration_active": np.asarray(self.calibrator.active, dtype=np.uint8),
        }
        for member_index, member in enumerate(self.members):
            prefix = "member_%03d_" % member_index
            for name in member.parameter_names + _STATE_ARRAYS:
                arrays[prefix + name] = np.asarray(
                    getattr(member, name), dtype=np.float64
                )
        _write_npz_deterministic(destination, arrays)

    @classmethod
    def load(cls, path: str) -> "RouterEnsemble":
        source = str(Path(path).expanduser())
        with np.load(source, allow_pickle=False) as archive:
            if "header_json" not in archive:
                raise ValueError("router artifact has no header_json")
            header = json.loads(str(archive["header_json"].item()))
            kind = header.get("model_kind")
            if kind == MODEL_KIND:
                single = CandidateRouter.load(source)
                return cls(
                    [single],
                    metadata={
                        "model_kind": MODEL_KIND,
                        "single_model_compatibility": True,
                        **dict(single.training_metadata),
                    },
                )
            if (
                kind != ENSEMBLE_KIND
                or int(header.get("format_version", -1)) != ENSEMBLE_FORMAT_VERSION
            ):
                raise ValueError(
                    "unsupported ensemble artifact %r v%r"
                    % (kind, header.get("format_version"))
                )
            task_names = tuple(
                str(value) for value in archive["task_feature_names"].tolist()
            )
            model_names = tuple(
                str(value) for value in archive["model_feature_names"].tolist()
            )
            outcome_names = tuple(
                str(value) for value in archive["outcome_names"].tolist()
            )
            head_types = tuple(str(value) for value in archive["head_types"].tolist())
            member_metadata = header.get("member_metadata") or []
            members: List[CandidateRouter] = []
            for member_index in range(int(header["member_count"])):
                prefix = "member_%03d_" % member_index
                rank = int(header["member_ranks"][member_index])
                member = CandidateRouter(
                    task_dim=int(header["task_dim"]),
                    model_dim=int(header["model_dim"]),
                    outcome_names=outcome_names,
                    rank=rank,
                    task_feature_names=task_names,
                    model_feature_names=model_names,
                    head_types=head_types,
                )
                for name in member.parameter_names + _STATE_ARRAYS:
                    key = prefix + name
                    if key not in archive:
                        raise ValueError("ensemble artifact missing %s" % key)
                    target = np.asarray(getattr(member, name))
                    value = np.asarray(archive[key], dtype=np.float64)
                    if value.shape != target.shape:
                        raise ValueError(
                            "invalid shape for %s: %r expected %r"
                            % (key, value.shape, target.shape)
                        )
                    setattr(member, name, value.copy())
                member.training_metadata = dict(
                    member_metadata[member_index]
                    if member_index < len(member_metadata)
                    else {}
                )
                members.append(member)
            calibrator = BinaryCalibrator(
                slopes=np.asarray(archive["calibration_slopes"], dtype=np.float64),
                intercepts=np.asarray(
                    archive["calibration_intercepts"], dtype=np.float64
                ),
                active=np.asarray(archive["calibration_active"], dtype=bool),
                head_types=head_types,
                metadata=dict(header.get("calibration_metadata") or {}),
            )
            return cls(members, calibrator, metadata=header.get("metadata") or {})


@dataclass(frozen=True)
class RouterTrainingData:
    task: np.ndarray
    model: np.ndarray
    labels: np.ndarray
    weights: np.ndarray
    train_indices: np.ndarray
    validation_indices: np.ndarray
    test_indices: np.ndarray
    outcome_names: Tuple[str, ...]
    head_types: Tuple[str, ...]
    pair_indices: Optional[np.ndarray]
    pair_preferences: Optional[np.ndarray]
    pair_weights: Optional[np.ndarray]
    pair_head: Optional[str]
    auxiliary: Mapping[str, Any]
    split_name: str


def _fallback_ids(
    auxiliary: Mapping[str, Any], rows: int
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    model_ids = auxiliary.get("model_holdout_ids")
    if model_ids is None:
        model_ids = auxiliary.get("model_ids")
    if model_ids is None:
        model_ids = np.asarray(["row-%d" % index for index in range(rows)])
    model_ids = np.asarray(model_ids).astype(str)
    provider_ids = auxiliary.get("provider_holdout_ids")
    if provider_ids is None:
        provider_ids = auxiliary.get("provider_ids")
    if provider_ids is None:
        provider_ids = np.asarray([_provider_id(value) for value in model_ids])
    provider_ids = np.asarray(provider_ids).astype(str)
    family_ids = auxiliary.get("family_holdout_ids")
    if family_ids is None:
        family_ids = auxiliary.get("family_ids")
    if family_ids is None:
        family_ids = np.asarray([_family_id(value) for value in model_ids])
    return (
        model_ids,
        np.asarray(provider_ids).astype(str),
        np.asarray(family_ids).astype(str),
    )


def holdout_folds(auxiliary: Mapping[str, Any], rows: int, axis: str) -> np.ndarray:
    supplied = auxiliary.get(axis + "_holdout_fold")
    if supplied is not None:
        result = np.asarray(supplied, dtype=np.int64).reshape(-1)
        if result.size != rows:
            raise ValueError("%s_holdout_fold length does not match rows" % axis)
        return result
    model_ids, provider_ids, family_ids = _fallback_ids(auxiliary, rows)
    values = {"model": model_ids, "provider": provider_ids, "family": family_ids}[axis]
    return np.asarray([_stable_fold(value) for value in values], dtype=np.int64)


def split_indices(
    auxiliary: Mapping[str, Any], rows: int, axis: str = "temporal", seed: int = 7
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, str]:
    if axis == "temporal":
        temporal = auxiliary.get("temporal_split")
        if temporal is not None:
            values = np.asarray(temporal, dtype=np.int64).reshape(-1)
            if values.size != rows:
                raise ValueError("temporal_split length does not match rows")
            return (
                np.flatnonzero(values == 0).astype(np.int64),
                np.flatnonzero(values == 1).astype(np.int64),
                np.flatnonzero(values == 2).astype(np.int64),
                "temporal",
            )
        training, validation = grouped_split(
            rows, auxiliary.get("group_ids"), 0.15, seed
        )
        return (
            training,
            validation,
            np.empty(0, dtype=np.int64),
            "group-random-fallback",
        )
    if axis not in ("model", "provider", "family"):
        raise ValueError("unknown split axis %r" % axis)
    folds = holdout_folds(auxiliary, rows, axis)
    return (
        np.flatnonzero(folds <= 7).astype(np.int64),
        np.flatnonzero(folds == 8).astype(np.int64),
        np.flatnonzero(folds == 9).astype(np.int64),
        axis + "-holdout",
    )


def prepare_router_training_data(
    dataset_path: str,
    metadata_path: Optional[str] = None,
    *,
    split_axis: str = "temporal",
    seed: int = 7,
    pair_loss_weight: float = 0.25,
) -> RouterTrainingData:
    task, model, base_labels, weights, auxiliary = load_dataset(
        dataset_path, metadata_path
    )
    train_indices, validation_indices, test_indices, split_name = split_indices(
        auxiliary, task.shape[0], split_axis, seed
    )
    if train_indices.size == 0 or validation_indices.size == 0:
        raise ValueError(
            "%s split needs non-empty train and validation rows" % split_name
        )
    labels = base_labels
    outcome_names = list(auxiliary["label_names"])
    pair_indices = auxiliary.get("pair_indices")
    preferences = auxiliary.get("pair_preference")
    pair_head: Optional[str] = None
    if pair_indices is not None and preferences is not None and pair_loss_weight > 0.0:
        pair_head = "preference_score"
        labels = np.column_stack(
            (labels, np.full(labels.shape[0], np.nan, dtype=np.float64))
        )
        outcome_names.append(pair_head)
    else:
        pair_indices = None
        preferences = None
    return RouterTrainingData(
        task=task,
        model=model,
        labels=labels,
        weights=weights,
        train_indices=train_indices,
        validation_indices=validation_indices,
        test_indices=test_indices,
        outcome_names=tuple(outcome_names),
        head_types=tuple(_head_types(outcome_names, labels)),
        pair_indices=pair_indices,
        pair_preferences=preferences,
        pair_weights=auxiliary.get("pair_weights"),
        pair_head=pair_head,
        auxiliary=auxiliary,
        split_name=split_name,
    )


def _bootstrap_row_weights(
    train_indices: np.ndarray,
    groups: Optional[np.ndarray],
    base_weights: np.ndarray,
    seed: int,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    rng = np.random.default_rng(seed)
    if groups is None:
        group_values = np.arange(base_weights.size, dtype=np.int64)
    else:
        group_values = np.asarray(groups).reshape(-1)
        if group_values.size != base_weights.size:
            raise ValueError("bootstrap group length does not match rows")
    train_groups = np.unique(group_values[train_indices])
    draws = rng.choice(train_groups, size=train_groups.size, replace=True)
    unique_draws, counts = np.unique(draws, return_counts=True)
    count_lookup = {
        value: int(count)
        for value, count in zip(unique_draws.tolist(), counts.tolist())
    }
    multipliers = np.asarray(
        [count_lookup.get(value, 0) for value in group_values], dtype=np.float64
    )
    selected = train_indices[multipliers[train_indices] > 0.0]
    # Bootstrap only the training partition. Validation/test weights remain
    # untouched so checkpoint selection and calibration see real held-out loss.
    weights = np.asarray(base_weights, dtype=np.float64).copy()
    weights[train_indices] *= multipliers[train_indices]
    return selected, weights, multipliers


def fit_router_ensemble(
    data: RouterTrainingData,
    *,
    members: int = 5,
    rank: int = 24,
    epochs: int = 200,
    batch_size: int = 512,
    learning_rate: float = 0.005,
    weight_decay: float = 1.0e-5,
    patience: int = 30,
    pair_loss_weight: float = 0.25,
    calibrate_binary: bool = True,
    seed: int = 7,
    verbose: bool = False,
) -> RouterEnsemble:
    if members <= 0:
        raise ValueError("members must be positive")
    trained: List[CandidateRouter] = []
    results: List[Dict[str, Any]] = []
    groups = data.auxiliary.get("group_ids")
    pairs = data.pair_indices
    base_pair_weights = (
        np.ones(pairs.shape[0], dtype=np.float64)
        if pairs is not None and data.pair_weights is None
        else None
        if pairs is None
        else np.asarray(data.pair_weights, dtype=np.float64).reshape(-1)
    )
    for member_index in range(members):
        member_seed = int(seed + 1009 * member_index)
        selected, sample_weights, multipliers = _bootstrap_row_weights(
            data.train_indices, groups, data.weights, member_seed
        )
        pair_weights = base_pair_weights
        if pairs is not None and base_pair_weights is not None:
            pair_weights = base_pair_weights.copy()
            train_multiplier = np.minimum(
                multipliers[pairs[:, 0]], multipliers[pairs[:, 1]]
            )
            validation_membership = np.zeros(data.task.shape[0], dtype=bool)
            validation_membership[data.validation_indices] = True
            validation_pairs = (
                validation_membership[pairs[:, 0]] & validation_membership[pairs[:, 1]]
            )
            pair_weights *= np.where(validation_pairs, 1.0, train_multiplier)
        router = CandidateRouter(
            task_dim=data.task.shape[1],
            model_dim=data.model.shape[1],
            outcome_names=data.outcome_names,
            rank=rank,
            seed=member_seed,
            task_feature_names=data.auxiliary["task_feature_names"],
            model_feature_names=data.auxiliary["model_feature_names"],
            head_types=data.head_types,
        )
        result = router.fit(
            data.task,
            data.model,
            data.labels,
            sample_weights=sample_weights,
            train_indices=selected,
            validation_indices=data.validation_indices,
            pair_indices=pairs,
            pair_labels=data.pair_preferences,
            pair_weights=pair_weights,
            pair_head=data.pair_head,
            pair_loss_weight=pair_loss_weight,
            epochs=epochs,
            batch_size=batch_size,
            learning_rate=learning_rate,
            weight_decay=weight_decay,
            patience=patience,
            seed=member_seed,
            verbose=verbose,
        )
        router.training_metadata.update(
            {
                "ensemble_member": member_index,
                "bootstrap_seed": member_seed,
                "bootstrap_selected_rows": int(selected.size),
                "split_name": data.split_name,
            }
        )
        results.append(_json_safe(result.__dict__))
        trained.append(router)

    uncalibrated = np.mean(
        np.stack(
            [
                member.predict(
                    data.task[data.validation_indices],
                    data.model[data.validation_indices],
                )
                for member in trained
            ],
            axis=0,
        ),
        axis=0,
    )
    if calibrate_binary:
        calibrator = BinaryCalibrator.fit(
            uncalibrated,
            data.labels[data.validation_indices],
            data.head_types,
            data.outcome_names,
            validation_indices_hash=_indices_sha256(data.validation_indices),
        )
    else:
        calibrator = BinaryCalibrator.identity(data.head_types)
        calibrator.metadata = {
            "source": "identity_nonbinary_quality_labels",
            "validation_indices_sha256": _indices_sha256(data.validation_indices),
            "heads": {},
        }
    dataset_metadata = data.auxiliary.get("metadata") or {}
    metadata = {
        "artifact_role": "operational_router",
        "task_feature_version": _feature_version(
            dataset_metadata, "task_feature_version"
        )
        or TASK_FEATURE_VERSION,
        "model_feature_version": _feature_version(
            dataset_metadata, "model_feature_version"
        )
        or MODEL_FEATURE_VERSION,
        "dataset_artifact_sha256": dataset_metadata.get("artifact_sha256"),
        "catalog_snapshot_sha256": dataset_metadata.get("catalog_snapshot_sha256"),
        "split_name": data.split_name,
        "train_indices_sha256": _indices_sha256(data.train_indices),
        "validation_indices_sha256": _indices_sha256(data.validation_indices),
        "test_indices_sha256": _indices_sha256(data.test_indices),
        "train_rows": int(data.train_indices.size),
        "validation_rows": int(data.validation_indices.size),
        "test_rows": int(data.test_indices.size),
        "member_results": results,
        "training_config": {
            "members": members,
            "rank": rank,
            "epochs": epochs,
            "batch_size": batch_size,
            "learning_rate": learning_rate,
            "weight_decay": weight_decay,
            "patience": patience,
            "pair_loss_weight": pair_loss_weight,
            "calibrate_binary": calibrate_binary,
            "seed": seed,
        },
        "uncertainty_semantics": "max(calibrated_member_std, uncalibrated_member_std)",
    }
    return RouterEnsemble(trained, calibrator, metadata)


def _quality_leakage_columns(feature_names: Sequence[str]) -> List[int]:
    tokens = (
        "benchmark_",
        "artificial_analysis",
        "intelligence_index",
        "coding_index",
        "agentic_index",
        "has_benchmark",
    )
    return [
        index
        for index, name in enumerate(feature_names)
        if any(token in str(name).casefold() for token in tokens)
    ]


def prepare_catalog_quality_data(
    dataset_path: str,
    metadata_path: Optional[str] = None,
    *,
    split_axis: str = "family",
    seed: int = 7,
) -> Optional[RouterTrainingData]:
    _task, _model, _labels, _weights, auxiliary = load_dataset(
        dataset_path, metadata_path
    )
    features = auxiliary.get("catalog_quality_features")
    labels = auxiliary.get("catalog_quality_labels")
    model_ids = auxiliary.get("catalog_quality_model_ids")
    if features is None or labels is None or model_ids is None:
        return None
    features = np.asarray(features, dtype=np.float64).copy()
    labels = np.asarray(labels, dtype=np.float64).copy()
    model_ids = np.asarray(model_ids).astype(str)
    if features.ndim != 2 or labels.ndim != 2 or features.shape[0] != labels.shape[0]:
        raise ValueError("catalog quality arrays have incompatible shapes")
    mask = auxiliary.get("catalog_quality_label_mask")
    if mask is not None:
        mask_array = np.asarray(mask, dtype=bool)
        if mask_array.shape != labels.shape:
            raise ValueError("catalog quality label mask shape mismatch")
        labels = np.where(mask_array, labels, np.nan)
    observed_quality = labels[np.isfinite(labels)]
    if observed_quality.size and (
        float(np.min(observed_quality)) < 0.0 or float(np.max(observed_quality)) > 1.0
    ):
        raise ValueError(
            "catalog quality labels must be normalized to [0, 1]; observed range %.6g..%.6g"
            % (float(np.min(observed_quality)), float(np.max(observed_quality)))
        )
    label_names_array = auxiliary.get("catalog_quality_label_names")
    label_names = (
        tuple(str(value) for value in np.asarray(label_names_array).tolist())
        if label_names_array is not None
        else ("general_intelligence_quality", "coding_quality", "agentic_quality")
    )
    if len(label_names) != labels.shape[1]:
        raise ValueError("catalog quality label names do not match columns")
    feature_names = tuple(str(value) for value in auxiliary["model_feature_names"])
    leakage_columns = _quality_leakage_columns(feature_names)
    leakage_nonzero = (
        int(np.count_nonzero(features[:, leakage_columns])) if leakage_columns else 0
    )
    if leakage_columns:
        features[:, leakage_columns] = 0.0

    quality_model_holdout_ids = auxiliary.get("catalog_quality_model_holdout_ids")
    if quality_model_holdout_ids is None:
        quality_model_holdout_ids = model_ids
    quality_aux: Dict[str, Any] = {
        "dataset_path": auxiliary.get("dataset_path"),
        "metadata_path": auxiliary.get("metadata_path"),
        "metadata": auxiliary.get("metadata") or {},
        "task_feature_names": ["numeric:bias"],
        "model_feature_names": feature_names,
        "group_ids": model_ids,
        "model_ids": model_ids,
        "model_holdout_ids": quality_model_holdout_ids,
        "provider_holdout_ids": auxiliary.get("catalog_quality_provider_holdout_ids"),
        "family_holdout_ids": auxiliary.get("catalog_quality_family_holdout_ids"),
        "model_holdout_fold": auxiliary.get("catalog_quality_model_holdout_fold"),
        "provider_holdout_fold": auxiliary.get("catalog_quality_provider_holdout_fold"),
        "family_holdout_fold": auxiliary.get("catalog_quality_family_holdout_fold"),
        "quality_label_provenance": auxiliary.get("catalog_quality_label_provenance"),
        "quality_catalog_indices": auxiliary.get("catalog_quality_catalog_indices"),
        "quality_leakage_columns_zeroed": leakage_columns,
        "quality_leakage_nonzero_before_zeroing": leakage_nonzero,
    }
    if quality_aux["provider_holdout_ids"] is None:
        quality_aux["provider_holdout_ids"] = np.asarray(
            [_provider_id(value) for value in model_ids]
        )
    if quality_aux["family_holdout_ids"] is None:
        quality_aux["family_holdout_ids"] = np.asarray(
            [_family_id(value) for value in model_ids]
        )
    rows = features.shape[0]
    train_indices, validation_indices, test_indices, split_name = split_indices(
        quality_aux, rows, split_axis, seed
    )
    if not train_indices.size or not validation_indices.size:
        training, validation = grouped_split(rows, model_ids, 0.15, seed)
        train_indices, validation_indices, test_indices = (
            training,
            validation,
            np.empty(0, dtype=np.int64),
        )
        split_name = "quality-group-random-fallback"
    return RouterTrainingData(
        task=np.ones((rows, 1), dtype=np.float64),
        model=features,
        labels=labels,
        weights=np.ones(rows, dtype=np.float64),
        train_indices=train_indices,
        validation_indices=validation_indices,
        test_indices=test_indices,
        outcome_names=label_names,
        head_types=tuple("probability" for _ in label_names),
        pair_indices=None,
        pair_preferences=None,
        pair_weights=None,
        pair_head=None,
        auxiliary=quality_aux,
        split_name=split_name,
    )


def fit_catalog_quality_ensemble(
    data: RouterTrainingData,
    **kwargs: Any,
) -> RouterEnsemble:
    ensemble = fit_router_ensemble(
        data, pair_loss_weight=0.0, calibrate_binary=False, **kwargs
    )
    provenance = data.auxiliary.get("quality_label_provenance")
    provenance_counts: Dict[str, int] = {}
    if provenance is not None:
        for value in np.asarray(provenance).reshape(-1).astype(str):
            if value:
                provenance_counts[value] = provenance_counts.get(value, 0) + 1
    ensemble.training_metadata.update(
        {
            "artifact_role": "catalog_quality_predictor",
            "task_feature_version": QUALITY_TASK_FEATURE_VERSION,
            "label_source": "OpenRouter Artificial Analysis weak supervision",
            "label_provenance_counts": provenance_counts,
            "benchmark_feature_columns_zeroed": list(
                data.auxiliary.get("quality_leakage_columns_zeroed")
                if data.auxiliary.get("quality_leakage_columns_zeroed") is not None
                else []
            ),
            "benchmark_feature_nonzero_values_removed": int(
                data.auxiliary.get("quality_leakage_nonzero_before_zeroing") or 0
            ),
        }
    )
    return ensemble


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="v1/v2 build_dataset NPZ")
    parser.add_argument("--metadata", help="optional sidecar JSON")
    parser.add_argument("--output", required=True, help="ensemble .npz")
    parser.add_argument("--members", type=int, default=5)
    parser.add_argument("--rank", type=int, default=24)
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--batch-size", type=int, default=512)
    parser.add_argument("--learning-rate", type=float, default=0.005)
    parser.add_argument("--weight-decay", type=float, default=1.0e-5)
    parser.add_argument("--patience", type=int, default=30)
    parser.add_argument("--pair-loss-weight", type=float, default=0.25)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument(
        "--quality-output", help="quality ensemble path; defaults beside output"
    )
    parser.add_argument(
        "--quality-split", choices=("model", "provider", "family"), default="family"
    )
    parser.add_argument("--skip-quality", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    data = prepare_router_training_data(
        args.dataset,
        args.metadata,
        split_axis="temporal",
        seed=args.seed,
        pair_loss_weight=args.pair_loss_weight,
    )
    ensemble = fit_router_ensemble(
        data,
        members=args.members,
        rank=args.rank,
        epochs=args.epochs,
        batch_size=args.batch_size,
        learning_rate=args.learning_rate,
        weight_decay=args.weight_decay,
        patience=args.patience,
        pair_loss_weight=args.pair_loss_weight,
        seed=args.seed,
        verbose=not args.quiet,
    )
    ensemble.save(args.output)
    summary: Dict[str, Any] = {
        "artifact": str(Path(args.output).expanduser()),
        "format": "chimera-router-ensemble-v2",
        "members": len(ensemble.members),
        "split": data.split_name,
        "train_rows": int(data.train_indices.size),
        "validation_rows": int(data.validation_indices.size),
        "test_rows": int(data.test_indices.size),
        "calibration": ensemble.calibrator.metadata,
    }
    if not args.skip_quality:
        quality_data = prepare_catalog_quality_data(
            args.dataset, args.metadata, split_axis=args.quality_split, seed=args.seed
        )
        if quality_data is None:
            summary["quality_artifact"] = {
                "status": "skipped",
                "reason": "dataset_has_no_catalog_quality_lane",
            }
        else:
            quality_path = (
                Path(args.quality_output).expanduser()
                if args.quality_output
                else Path(args.output)
                .expanduser()
                .with_name(Path(args.output).stem + ".quality.npz")
            )
            quality_ensemble = fit_catalog_quality_ensemble(
                quality_data,
                members=args.members,
                rank=min(args.rank, 16),
                epochs=args.epochs,
                batch_size=min(args.batch_size, 256),
                learning_rate=args.learning_rate,
                weight_decay=args.weight_decay,
                patience=args.patience,
                seed=args.seed + 50021,
                verbose=not args.quiet,
            )
            quality_ensemble.save(str(quality_path))
            summary["quality_artifact"] = {
                "status": "written",
                "artifact": str(quality_path),
                "members": len(quality_ensemble.members),
                "split": quality_data.split_name,
                "label_source": quality_ensemble.training_metadata["label_source"],
                "benchmark_feature_columns_zeroed": quality_ensemble.training_metadata[
                    "benchmark_feature_columns_zeroed"
                ],
            }
    print(json.dumps(_json_safe(summary), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())


__all__ = [
    "BinaryCalibrator",
    "RouterEnsemble",
    "RouterTrainingData",
    "fit_catalog_quality_ensemble",
    "fit_router_ensemble",
    "holdout_folds",
    "prepare_catalog_quality_data",
    "prepare_router_training_data",
    "split_indices",
]
