#!/usr/bin/env python3
"""Small, portable candidate-conditioned model router.

The router deliberately has no fixed model-ID output vocabulary.  It scores a
pair of numeric vectors -- one describing the request, one describing a model
from a catalog snapshot -- so a model first seen after training can be ranked.

Architecture (all operations are NumPy):

    t = tanh(normalize(task) @ task_tower + task_bias)
    m = tanh(normalize(model) @ model_tower + model_bias)
    raw = (t * m) @ interaction_head
          + normalize(task) @ task_skip
          + normalize(model) @ model_skip + output_bias

Probability heads use a sigmoid/BCE loss.  Positive quantities such as cost
and latency are learned in log1p space; other continuous heads are standardized
and use a Huber loss.  Optional preference pairs use a Bradley-Terry loss on a
selected head.  Keeping the interaction low-rank makes training and inference
cheap while retaining the important request-by-candidate dependency.

The versioned ``.npz`` export contains only numeric arrays, Unicode strings,
and JSON text.  Loading never requires pickle.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

import numpy as np


FORMAT_VERSION = 1
MODEL_KIND = "chimera-router-numpy"
EPS = 1.0e-8


def _as_float_matrix(value: np.ndarray, name: str) -> np.ndarray:
    array = np.asarray(value, dtype=np.float64)
    if array.ndim == 1:
        array = array[:, None]
    if array.ndim != 2:
        raise ValueError("%s must be a rank-2 matrix, got %r" % (name, array.shape))
    if array.shape[0] == 0 or array.shape[1] == 0:
        raise ValueError("%s cannot be empty" % name)
    return array


def _safe_scale(values: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    finite = np.isfinite(values)
    count = np.sum(finite, axis=0)
    safe_values = np.where(finite, values, 0.0)
    mean = np.sum(safe_values, axis=0) / np.maximum(count, 1)
    variance = np.sum(np.where(finite, (values - mean) ** 2, 0.0), axis=0) / np.maximum(
        count, 1
    )
    scale = np.sqrt(variance)
    mean = np.where(np.isfinite(mean), mean, 0.0)
    scale = np.where(np.isfinite(scale) & (scale > 1.0e-6), scale, 1.0)
    return mean.astype(np.float64), scale.astype(np.float64)


def _sigmoid(x: np.ndarray) -> np.ndarray:
    # This split formulation avoids overflow without depending on scipy.
    x = np.asarray(x, dtype=np.float64)
    out = np.empty_like(x)
    positive = x >= 0.0
    out[positive] = 1.0 / (1.0 + np.exp(-x[positive]))
    exp_x = np.exp(x[~positive])
    out[~positive] = exp_x / (1.0 + exp_x)
    return out


def _default_head_type(name: str, values: np.ndarray) -> str:
    finite = values[np.isfinite(values)]
    bounded = (
        finite.size > 0
        and float(np.min(finite)) >= 0.0
        and float(np.max(finite)) <= 1.0
    )
    probability_words = (
        "quality",
        "success",
        "failure",
        "error",
        "refusal",
        "valid",
        "pass",
        "accept",
        "prob",
        "score",
        "reward",
        "win",
    )
    if bounded and any(word in name.lower() for word in probability_words):
        return "probability"
    if any(
        word in name.lower()
        for word in ("cost", "latency", "duration", "tokens", "seconds")
    ):
        return "positive"
    return "continuous"


@dataclass(frozen=True)
class TrainingResult:
    """Summary returned by :meth:`CandidateRouter.fit`."""

    epochs_run: int
    best_epoch: int
    best_validation_loss: float
    final_training_loss: float
    validation_rows: int


class CandidateRouter:
    """Low-rank two-tower outcome model with manual NumPy gradients."""

    parameter_names = (
        "task_tower",
        "task_bias",
        "model_tower",
        "model_bias",
        "interaction_head",
        "task_skip",
        "model_skip",
        "output_bias",
    )

    def __init__(
        self,
        task_dim: int,
        model_dim: int,
        outcome_names: Sequence[str],
        rank: int = 16,
        seed: int = 7,
        task_feature_names: Optional[Sequence[str]] = None,
        model_feature_names: Optional[Sequence[str]] = None,
        head_types: Optional[Sequence[str]] = None,
    ) -> None:
        if task_dim <= 0 or model_dim <= 0 or rank <= 0:
            raise ValueError("task_dim, model_dim, and rank must be positive")
        if not outcome_names:
            raise ValueError("at least one outcome head is required")

        self.task_dim = int(task_dim)
        self.model_dim = int(model_dim)
        self.rank = int(rank)
        self.outcome_names = tuple(str(name) for name in outcome_names)
        self.output_dim = len(self.outcome_names)
        self.task_feature_names = tuple(
            str(name)
            for name in (task_feature_names or ["task_%d" % i for i in range(task_dim)])
        )
        self.model_feature_names = tuple(
            str(name)
            for name in (
                model_feature_names or ["model_%d" % i for i in range(model_dim)]
            )
        )
        if len(self.task_feature_names) != self.task_dim:
            raise ValueError("task_feature_names length does not match task_dim")
        if len(self.model_feature_names) != self.model_dim:
            raise ValueError("model_feature_names length does not match model_dim")

        if head_types is None:
            # Refined from the labels when fit() is called.
            self.head_types = tuple("continuous" for _ in self.outcome_names)
            self._head_types_explicit = False
        else:
            normalized = tuple(str(value).lower() for value in head_types)
            if len(normalized) != self.output_dim or any(
                value not in ("probability", "continuous", "positive")
                for value in normalized
            ):
                raise ValueError(
                    "head_types must contain probability/continuous/positive for every outcome"
                )
            self.head_types = normalized
            self._head_types_explicit = True

        rng = np.random.default_rng(seed)
        task_std = 1.0 / math.sqrt(max(self.task_dim, 1))
        model_std = 1.0 / math.sqrt(max(self.model_dim, 1))
        head_std = 1.0 / math.sqrt(max(self.rank, 1))
        self.task_tower = rng.normal(0.0, task_std, (self.task_dim, self.rank))
        self.task_bias = np.zeros(self.rank, dtype=np.float64)
        self.model_tower = rng.normal(0.0, model_std, (self.model_dim, self.rank))
        self.model_bias = np.zeros(self.rank, dtype=np.float64)
        self.interaction_head = rng.normal(0.0, head_std, (self.rank, self.output_dim))
        self.task_skip = np.zeros((self.task_dim, self.output_dim), dtype=np.float64)
        self.model_skip = np.zeros((self.model_dim, self.output_dim), dtype=np.float64)
        self.output_bias = np.zeros(self.output_dim, dtype=np.float64)

        self.task_mean = np.zeros(self.task_dim, dtype=np.float64)
        self.task_scale = np.ones(self.task_dim, dtype=np.float64)
        self.model_mean = np.zeros(self.model_dim, dtype=np.float64)
        self.model_scale = np.ones(self.model_dim, dtype=np.float64)
        self.outcome_mean = np.zeros(self.output_dim, dtype=np.float64)
        self.outcome_scale = np.ones(self.output_dim, dtype=np.float64)
        self.task_train_min = np.full(self.task_dim, -np.inf, dtype=np.float64)
        self.task_train_max = np.full(self.task_dim, np.inf, dtype=np.float64)
        self.model_train_min = np.full(self.model_dim, -np.inf, dtype=np.float64)
        self.model_train_max = np.full(self.model_dim, np.inf, dtype=np.float64)
        self.training_metadata: Dict[str, object] = {}

    def _normalise_inputs(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> Tuple[np.ndarray, np.ndarray]:
        task = _as_float_matrix(task_features, "task_features")
        model = _as_float_matrix(model_features, "model_features")
        if task.shape[0] != model.shape[0]:
            raise ValueError(
                "task_features and model_features must have the same row count"
            )
        if task.shape[1] != self.task_dim or model.shape[1] != self.model_dim:
            raise ValueError(
                "feature dimensions do not match model: task %r/%d, model %r/%d"
                % (task.shape, self.task_dim, model.shape, self.model_dim)
            )
        task = np.where(np.isfinite(task), task, self.task_mean)
        model = np.where(np.isfinite(model), model, self.model_mean)
        task_n = np.clip((task - self.task_mean) / self.task_scale, -12.0, 12.0)
        model_n = np.clip((model - self.model_mean) / self.model_scale, -12.0, 12.0)
        return task_n, model_n

    def _forward_normalised(
        self, task_n: np.ndarray, model_n: np.ndarray
    ) -> Tuple[np.ndarray, Tuple[np.ndarray, np.ndarray, np.ndarray]]:
        task_hidden = np.tanh(task_n @ self.task_tower + self.task_bias)
        model_hidden = np.tanh(model_n @ self.model_tower + self.model_bias)
        interaction = task_hidden * model_hidden
        raw = (
            interaction @ self.interaction_head
            + task_n @ self.task_skip
            + model_n @ self.model_skip
            + self.output_bias
        )
        return raw, (task_hidden, model_hidden, interaction)

    def raw_scores(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        """Return pre-link, normalized scores for every outcome head."""

        task_n, model_n = self._normalise_inputs(task_features, model_features)
        raw, _ = self._forward_normalised(task_n, model_n)
        return raw

    def predict(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        """Predict outcomes in their original units."""

        raw = self.raw_scores(task_features, model_features)
        result = np.empty_like(raw)
        for index, head_type in enumerate(self.head_types):
            if head_type == "probability":
                result[:, index] = _sigmoid(raw[:, index])
            elif head_type == "positive":
                log_value = (
                    raw[:, index] * self.outcome_scale[index] + self.outcome_mean[index]
                )
                result[:, index] = np.maximum(
                    np.expm1(np.clip(log_value, -20.0, 30.0)), 0.0
                )
            else:
                result[:, index] = (
                    raw[:, index] * self.outcome_scale[index] + self.outcome_mean[index]
                )
        return result

    def predict_dicts(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> List[Dict[str, float]]:
        predictions = self.predict(task_features, model_features)
        return [
            {name: float(row[index]) for index, name in enumerate(self.outcome_names)}
            for row in predictions
        ]

    def support_distance(
        self, task_features: np.ndarray, model_features: np.ndarray
    ) -> np.ndarray:
        """Return a cheap out-of-support distance for uncertainty penalties.

        Distance is zero inside each feature's observed training range and grows
        in standardized feature units outside it.  It is not a calibrated
        posterior uncertainty estimate, but it makes cold-start/OOD routing
        explicit and inspectable.
        """

        task = _as_float_matrix(task_features, "task_features")
        model = _as_float_matrix(model_features, "model_features")
        if task.shape[0] != model.shape[0]:
            raise ValueError("task/model row counts differ")
        task = np.where(np.isfinite(task), task, self.task_mean)
        model = np.where(np.isfinite(model), model, self.model_mean)
        task_below = np.maximum(self.task_train_min - task, 0.0) / self.task_scale
        task_above = np.maximum(task - self.task_train_max, 0.0) / self.task_scale
        model_below = np.maximum(self.model_train_min - model, 0.0) / self.model_scale
        model_above = np.maximum(model - self.model_train_max, 0.0) / self.model_scale
        squared = np.sum((task_below + task_above) ** 2, axis=1)
        squared += np.sum((model_below + model_above) ** 2, axis=1)
        return np.sqrt(squared / float(self.task_dim + self.model_dim))

    def _backward(
        self,
        task_n: np.ndarray,
        model_n: np.ndarray,
        cache: Tuple[np.ndarray, np.ndarray, np.ndarray],
        draw: np.ndarray,
    ) -> Dict[str, np.ndarray]:
        task_hidden, model_hidden, interaction = cache
        d_interaction = draw @ self.interaction_head.T
        d_task_hidden = d_interaction * model_hidden
        d_model_hidden = d_interaction * task_hidden
        d_task_pre = d_task_hidden * (1.0 - task_hidden * task_hidden)
        d_model_pre = d_model_hidden * (1.0 - model_hidden * model_hidden)
        return {
            "task_tower": task_n.T @ d_task_pre,
            "task_bias": np.sum(d_task_pre, axis=0),
            "model_tower": model_n.T @ d_model_pre,
            "model_bias": np.sum(d_model_pre, axis=0),
            "interaction_head": interaction.T @ draw,
            "task_skip": task_n.T @ draw,
            "model_skip": model_n.T @ draw,
            "output_bias": np.sum(draw, axis=0),
        }

    def _point_loss_and_gradient(
        self,
        task: np.ndarray,
        model: np.ndarray,
        labels: np.ndarray,
        weights: np.ndarray,
        huber_delta: float,
    ) -> Tuple[float, Dict[str, np.ndarray]]:
        task_n, model_n = self._normalise_inputs(task, model)
        raw, cache = self._forward_normalised(task_n, model_n)
        draw = np.zeros_like(raw)
        total_loss = 0.0
        total_weight = 0.0

        for head, head_type in enumerate(self.head_types):
            mask = np.isfinite(labels[:, head]) & (weights > 0.0)
            if not np.any(mask):
                continue
            row_weights = weights[mask]
            denominator = max(float(np.sum(row_weights)), EPS)
            if head_type == "probability":
                target = np.clip(labels[mask, head], 0.0, 1.0)
                logits = raw[mask, head]
                # Stable binary cross entropy: max(x,0)-x*y+log1p(exp(-abs(x))).
                losses = (
                    np.maximum(logits, 0.0)
                    - logits * target
                    + np.log1p(np.exp(-np.abs(logits)))
                )
                total_loss += float(np.sum(losses * row_weights))
                draw[mask, head] = (_sigmoid(logits) - target) * row_weights
            else:
                target_values = labels[mask, head]
                if head_type == "positive":
                    target_values = np.log1p(np.maximum(target_values, 0.0))
                target = (target_values - self.outcome_mean[head]) / self.outcome_scale[
                    head
                ]
                residual = raw[mask, head] - target
                absolute = np.abs(residual)
                quadratic = absolute <= huber_delta
                losses = np.where(
                    quadratic,
                    0.5 * residual * residual,
                    huber_delta * (absolute - 0.5 * huber_delta),
                )
                derivative = np.where(
                    quadratic, residual, huber_delta * np.sign(residual)
                )
                total_loss += float(np.sum(losses * row_weights))
                draw[mask, head] = derivative * row_weights
            total_weight += denominator

        denominator = max(total_weight, EPS)
        draw /= denominator
        return total_loss / denominator, self._backward(task_n, model_n, cache, draw)

    def _pair_loss_and_gradient(
        self,
        task: np.ndarray,
        model: np.ndarray,
        pairs: np.ndarray,
        pair_labels: np.ndarray,
        pair_weights: np.ndarray,
        head: int,
    ) -> Tuple[float, Dict[str, np.ndarray]]:
        if pairs.size == 0:
            return 0.0, {
                name: np.zeros_like(getattr(self, name))
                for name in self.parameter_names
            }
        task_n, model_n = self._normalise_inputs(task, model)
        raw, cache = self._forward_normalised(task_n, model_n)
        left = pairs[:, 0]
        right = pairs[:, 1]
        sign = np.where(pair_labels > 0.5, 1.0, np.where(pair_labels < 0.5, -1.0, 0.0))
        difference = sign * (raw[left, head] - raw[right, head])
        losses = np.logaddexp(0.0, -difference)
        denominator = max(float(np.sum(pair_weights)), EPS)
        loss = float(np.sum(losses * pair_weights) / denominator)
        derivative = -sign * _sigmoid(-difference) * pair_weights / denominator
        draw = np.zeros_like(raw)
        np.add.at(draw[:, head], left, derivative)
        np.add.at(draw[:, head], right, -derivative)
        return loss, self._backward(task_n, model_n, cache, draw)

    @staticmethod
    def _add_gradients(
        first: Dict[str, np.ndarray], second: Dict[str, np.ndarray], scale: float
    ) -> Dict[str, np.ndarray]:
        return {name: first[name] + scale * second[name] for name in first}

    def fit(
        self,
        task_features: np.ndarray,
        model_features: np.ndarray,
        labels: np.ndarray,
        *,
        sample_weights: Optional[np.ndarray] = None,
        train_indices: Optional[np.ndarray] = None,
        validation_indices: Optional[np.ndarray] = None,
        pair_indices: Optional[np.ndarray] = None,
        pair_labels: Optional[np.ndarray] = None,
        pair_weights: Optional[np.ndarray] = None,
        pair_head: Optional[str] = None,
        pair_loss_weight: float = 0.25,
        epochs: int = 200,
        batch_size: int = 256,
        learning_rate: float = 0.01,
        weight_decay: float = 1.0e-5,
        huber_delta: float = 1.0,
        patience: int = 30,
        seed: int = 7,
        verbose: bool = False,
    ) -> TrainingResult:
        """Fit the router with deterministic mini-batch Adam.

        ``pair_indices`` contains global row pairs.  A pair label of 1 means the
        left row is preferred and 0 means the right row is preferred.  Pairs
        that cross out of the training split are ignored.
        """

        task = _as_float_matrix(task_features, "task_features")
        model = _as_float_matrix(model_features, "model_features")
        labels = _as_float_matrix(labels, "labels")
        rows = task.shape[0]
        if model.shape[0] != rows or labels.shape[0] != rows:
            raise ValueError("task, model, and labels must have equal row counts")
        if task.shape[1] != self.task_dim or model.shape[1] != self.model_dim:
            raise ValueError("training feature dimensions do not match router")
        if labels.shape[1] != self.output_dim:
            raise ValueError("label columns do not match outcome_names")
        if rows < 2:
            raise ValueError("at least two training rows are required")

        if train_indices is None:
            train = np.arange(rows, dtype=np.int64)
        else:
            train = np.asarray(train_indices, dtype=np.int64).reshape(-1)
        validation = (
            np.asarray(validation_indices, dtype=np.int64).reshape(-1)
            if validation_indices is not None
            else np.empty(0, dtype=np.int64)
        )
        if train.size == 0:
            raise ValueError("training split is empty")
        if (
            np.any(train < 0)
            or np.any(train >= rows)
            or np.any(validation < 0)
            or np.any(validation >= rows)
        ):
            raise ValueError("split index outside dataset")

        weights = (
            np.ones(rows, dtype=np.float64)
            if sample_weights is None
            else np.asarray(sample_weights, dtype=np.float64).reshape(-1)
        )
        if weights.size != rows:
            raise ValueError("sample_weights length does not match rows")
        weights = np.where(np.isfinite(weights) & (weights > 0.0), weights, 0.0)

        self.task_mean, self.task_scale = _safe_scale(task[train])
        self.model_mean, self.model_scale = _safe_scale(model[train])
        task_finite = np.isfinite(task[train])
        model_finite = np.isfinite(model[train])
        self.task_train_min = np.min(np.where(task_finite, task[train], np.inf), axis=0)
        self.task_train_max = np.max(
            np.where(task_finite, task[train], -np.inf), axis=0
        )
        self.model_train_min = np.min(
            np.where(model_finite, model[train], np.inf), axis=0
        )
        self.model_train_max = np.max(
            np.where(model_finite, model[train], -np.inf), axis=0
        )
        self.task_train_min = np.where(
            np.isfinite(self.task_train_min), self.task_train_min, self.task_mean
        )
        self.task_train_max = np.where(
            np.isfinite(self.task_train_max), self.task_train_max, self.task_mean
        )
        self.model_train_min = np.where(
            np.isfinite(self.model_train_min), self.model_train_min, self.model_mean
        )
        self.model_train_max = np.where(
            np.isfinite(self.model_train_max), self.model_train_max, self.model_mean
        )

        if not self._head_types_explicit:
            self.head_types = tuple(
                _default_head_type(name, labels[train, index])
                for index, name in enumerate(self.outcome_names)
            )
        for index, head_type in enumerate(self.head_types):
            if head_type in ("continuous", "positive"):
                finite = labels[train, index][np.isfinite(labels[train, index])]
                if finite.size:
                    if head_type == "positive":
                        finite = np.log1p(np.maximum(finite, 0.0))
                    mean = float(np.mean(finite))
                    scale = float(np.std(finite))
                    self.outcome_mean[index] = mean
                    self.outcome_scale[index] = scale if scale > 1.0e-6 else 1.0

        pairs = np.empty((0, 2), dtype=np.int64)
        p_labels = np.empty(0, dtype=np.float64)
        p_weights = np.empty(0, dtype=np.float64)
        validation_pairs = np.empty((0, 2), dtype=np.int64)
        validation_pair_labels = np.empty(0, dtype=np.float64)
        validation_pair_weights = np.empty(0, dtype=np.float64)
        if pair_indices is not None:
            all_pairs = np.asarray(pair_indices, dtype=np.int64)
            if all_pairs.ndim != 2 or all_pairs.shape[1] != 2:
                raise ValueError("pair_indices must have shape [pairs, 2]")
            if np.any(all_pairs < 0) or np.any(all_pairs >= rows):
                raise ValueError("pair index outside dataset")
            all_pair_labels = (
                np.ones(all_pairs.shape[0], dtype=np.float64)
                if pair_labels is None
                else np.asarray(pair_labels, dtype=np.float64).reshape(-1)
            )
            all_pair_weights = (
                np.ones(all_pairs.shape[0], dtype=np.float64)
                if pair_weights is None
                else np.asarray(pair_weights, dtype=np.float64).reshape(-1)
            )
            if (
                all_pair_labels.size != all_pairs.shape[0]
                or all_pair_weights.size != all_pairs.shape[0]
            ):
                raise ValueError(
                    "pair labels/weights length does not match pair_indices"
                )
            # Exact ties carry no ordering information.  Excluding them also
            # prevents a constant log(2) from distorting checkpoint selection.
            informative = np.isfinite(all_pair_labels) & (all_pair_labels != 0.5)
            informative &= np.isfinite(all_pair_weights) & (all_pair_weights > 0.0)
            train_mask = np.zeros(rows, dtype=bool)
            train_mask[train] = True
            train_keep = (
                informative & train_mask[all_pairs[:, 0]] & train_mask[all_pairs[:, 1]]
            )
            pairs = all_pairs[train_keep]
            p_labels = all_pair_labels[train_keep]
            p_weights = all_pair_weights[train_keep]
            if validation.size:
                validation_mask = np.zeros(rows, dtype=bool)
                validation_mask[validation] = True
                validation_keep = (
                    informative
                    & validation_mask[all_pairs[:, 0]]
                    & validation_mask[all_pairs[:, 1]]
                )
                validation_pairs = all_pairs[validation_keep]
                validation_pair_labels = all_pair_labels[validation_keep]
                validation_pair_weights = all_pair_weights[validation_keep]

        if pair_head is None:
            preferred = ("quality", "utility", "reward", "success", "score")
            pair_head_index = next(
                (
                    self.outcome_names.index(name)
                    for name in preferred
                    if name in self.outcome_names
                ),
                0,
            )
        else:
            if pair_head not in self.outcome_names:
                raise ValueError("unknown pair_head %r" % pair_head)
            pair_head_index = self.outcome_names.index(pair_head)

        rng = np.random.default_rng(seed)
        moments = {
            name: np.zeros_like(getattr(self, name)) for name in self.parameter_names
        }
        variances = {
            name: np.zeros_like(getattr(self, name)) for name in self.parameter_names
        }
        adam_step = 0
        best_loss = float("inf")
        best_epoch = 0
        best_state = {name: getattr(self, name).copy() for name in self.parameter_names}
        epochs_without_improvement = 0
        final_training_loss = float("inf")

        for epoch in range(1, max(int(epochs), 1) + 1):
            order = train.copy()
            rng.shuffle(order)
            epoch_loss_sum = 0.0
            epoch_batches = 0
            for start in range(0, order.size, max(int(batch_size), 1)):
                batch = order[start : start + max(int(batch_size), 1)]
                point_loss, gradients = self._point_loss_and_gradient(
                    task[batch],
                    model[batch],
                    labels[batch],
                    weights[batch],
                    huber_delta,
                )
                if pairs.size and start == 0:
                    pair_loss, pair_gradients = self._pair_loss_and_gradient(
                        task, model, pairs, p_labels, p_weights, pair_head_index
                    )
                    gradients = self._add_gradients(
                        gradients, pair_gradients, pair_loss_weight
                    )
                    point_loss += pair_loss_weight * pair_loss

                adam_step += 1
                for name in self.parameter_names:
                    parameter = getattr(self, name)
                    gradient = gradients[name]
                    if name not in ("task_bias", "model_bias", "output_bias"):
                        gradient = gradient + weight_decay * parameter
                    moments[name] = 0.9 * moments[name] + 0.1 * gradient
                    variances[name] = (
                        0.999 * variances[name] + 0.001 * gradient * gradient
                    )
                    m_hat = moments[name] / (1.0 - 0.9**adam_step)
                    v_hat = variances[name] / (1.0 - 0.999**adam_step)
                    parameter -= learning_rate * m_hat / (np.sqrt(v_hat) + 1.0e-8)
                epoch_loss_sum += point_loss
                epoch_batches += 1

            final_training_loss = epoch_loss_sum / max(epoch_batches, 1)
            evaluation_rows = validation if validation.size else train
            validation_loss, _ = self._point_loss_and_gradient(
                task[evaluation_rows],
                model[evaluation_rows],
                labels[evaluation_rows],
                weights[evaluation_rows],
                huber_delta,
            )
            if validation_pairs.size:
                validation_pair_loss, _ = self._pair_loss_and_gradient(
                    task,
                    model,
                    validation_pairs,
                    validation_pair_labels,
                    validation_pair_weights,
                    pair_head_index,
                )
                validation_loss += pair_loss_weight * validation_pair_loss
            if validation_loss < best_loss - 1.0e-7:
                best_loss = validation_loss
                best_epoch = epoch
                best_state = {
                    name: getattr(self, name).copy() for name in self.parameter_names
                }
                epochs_without_improvement = 0
            else:
                epochs_without_improvement += 1
            if verbose and (epoch == 1 or epoch % 10 == 0):
                print(
                    "epoch=%d train_loss=%.6f validation_loss=%.6f"
                    % (epoch, final_training_loss, validation_loss),
                    flush=True,
                )
            if (
                validation.size
                and patience > 0
                and epochs_without_improvement >= patience
            ):
                break

        for name, value in best_state.items():
            setattr(self, name, value)
        self.training_metadata.update(
            {
                "best_epoch": best_epoch,
                "best_validation_loss": best_loss,
                "epochs_run": epoch,
                "training_rows": int(train.size),
                "validation_rows": int(validation.size),
                "pair_rows": int(pairs.shape[0]),
                "validation_pair_rows": int(validation_pairs.shape[0]),
                "seed": int(seed),
            }
        )
        return TrainingResult(
            epochs_run=epoch,
            best_epoch=best_epoch,
            best_validation_loss=best_loss,
            final_training_loss=final_training_loss,
            validation_rows=int(validation.size),
        )

    def save(self, path: str, metadata: Optional[Mapping[str, object]] = None) -> None:
        """Write a versioned, pickle-free compressed NumPy artifact."""

        destination = Path(path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        combined_metadata = dict(self.training_metadata)
        if metadata:
            combined_metadata.update(metadata)
        header = {
            "format_version": FORMAT_VERSION,
            "model_kind": MODEL_KIND,
            "task_dim": self.task_dim,
            "model_dim": self.model_dim,
            "rank": self.rank,
            "output_dim": self.output_dim,
            "metadata": combined_metadata,
        }
        arrays: Dict[str, np.ndarray] = {
            "header_json": np.asarray(
                json.dumps(header, sort_keys=True, separators=(",", ":"))
            ),
            "task_feature_names": np.asarray(self.task_feature_names, dtype=np.str_),
            "model_feature_names": np.asarray(self.model_feature_names, dtype=np.str_),
            "outcome_names": np.asarray(self.outcome_names, dtype=np.str_),
            "head_types": np.asarray(self.head_types, dtype=np.str_),
            "task_mean": self.task_mean,
            "task_scale": self.task_scale,
            "model_mean": self.model_mean,
            "model_scale": self.model_scale,
            "outcome_mean": self.outcome_mean,
            "outcome_scale": self.outcome_scale,
            "task_train_min": self.task_train_min,
            "task_train_max": self.task_train_max,
            "model_train_min": self.model_train_min,
            "model_train_max": self.model_train_max,
        }
        arrays.update({name: getattr(self, name) for name in self.parameter_names})
        # Passing a file handle prevents NumPy from silently appending ".npz".
        with destination.open("wb") as handle:
            np.savez_compressed(handle, **arrays)

    @classmethod
    def load(cls, path: str) -> "CandidateRouter":
        """Load and validate a router artifact without enabling pickle."""

        with np.load(path, allow_pickle=False) as archive:
            required = {
                "header_json",
                "task_feature_names",
                "model_feature_names",
                "outcome_names",
                "head_types",
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
            }.union(cls.parameter_names)
            missing = sorted(required.difference(archive.files))
            if missing:
                raise ValueError("router artifact missing: %s" % ", ".join(missing))
            header = json.loads(str(archive["header_json"].item()))
            if header.get("model_kind") != MODEL_KIND:
                raise ValueError("unsupported model kind %r" % header.get("model_kind"))
            if int(header.get("format_version", -1)) != FORMAT_VERSION:
                raise ValueError(
                    "unsupported router format version %r"
                    % header.get("format_version")
                )
            router = cls(
                task_dim=int(header["task_dim"]),
                model_dim=int(header["model_dim"]),
                rank=int(header["rank"]),
                outcome_names=[
                    str(value) for value in archive["outcome_names"].tolist()
                ],
                task_feature_names=[
                    str(value) for value in archive["task_feature_names"].tolist()
                ],
                model_feature_names=[
                    str(value) for value in archive["model_feature_names"].tolist()
                ],
                head_types=[str(value) for value in archive["head_types"].tolist()],
            )
            for name in cls.parameter_names:
                target = getattr(router, name)
                value = np.asarray(archive[name], dtype=np.float64)
                if value.shape != target.shape:
                    raise ValueError(
                        "invalid shape for %s: %r, expected %r"
                        % (name, value.shape, target.shape)
                    )
                setattr(router, name, value.copy())
            for name in (
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
            ):
                setattr(
                    router, name, np.asarray(archive[name], dtype=np.float64).copy()
                )
            router.training_metadata = dict(header.get("metadata") or {})
            return router


def choose_head(
    outcome_names: Iterable[str], candidates: Sequence[str], fallback: int = 0
) -> int:
    """Return the first exact/substring head match, for policy composition."""

    names = [str(name) for name in outcome_names]
    lowered = [name.lower() for name in names]
    for candidate in candidates:
        candidate_l = candidate.lower()
        if candidate_l in lowered:
            return lowered.index(candidate_l)
    for candidate in candidates:
        candidate_l = candidate.lower()
        for index, name in enumerate(lowered):
            if candidate_l in name:
                return index
    return fallback
