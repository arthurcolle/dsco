#!/usr/bin/env python3
"""Rank a live model catalog with a trained Chimera router.

This module performs no network requests.  Give it a current OpenRouter-style
catalog (or any list of compatible descriptor dictionaries), a request, and a
versioned router ``.npz``.  Prices and policy remain live inputs rather than
being frozen into the learned weights.

The public integration entry point is :func:`route_request`.  Explicit model
overrides are authoritative: when supplied, the requested ID is returned even
if it is absent from the catalog or violates normal automatic-routing filters.
Violations are reported so honoring an override never becomes invisible.
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import fnmatch
import json
import math
import sys
import time
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

import numpy as np

try:
    from .features import (
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        canonical_model_id,
        model_features,
        task_features,
    )
    from .ensemble import QUALITY_TASK_FEATURE_VERSION, RouterEnsemble
    from .model import CandidateRouter, choose_head
except ImportError:  # Direct execution.
    from features import (
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        canonical_model_id,
        model_features,
        task_features,
    )
    from ensemble import QUALITY_TASK_FEATURE_VERSION, RouterEnsemble
    from model import CandidateRouter, choose_head


ROUTE_SCHEMA_VERSION = 2
RouterModel = Union[CandidateRouter, RouterEnsemble]


def _as_float(value: Any, default: Optional[float] = None) -> Optional[float]:
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return default
    return number if math.isfinite(number) else default


def _as_bool(value: Any) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return bool(value)


def _mapping(value: Any) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _string_tuple(value: Any) -> Tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        return (value,)
    if isinstance(value, Sequence):
        return tuple(str(item) for item in value if item is not None)
    return ()


def _nested(mapping: Mapping[str, Any], *path: str) -> Any:
    current: Any = mapping
    for component in path:
        if not isinstance(current, Mapping):
            return None
        current = current.get(component)
    return current


def _first(mapping: Mapping[str, Any], paths: Sequence[Tuple[str, ...]]) -> Any:
    for path in paths:
        value = _nested(mapping, *path)
        if value is not None:
            return value
    return None


@dataclass(frozen=True)
class RoutingPolicy:
    """External utility weights and hard eligibility constraints.

    Weights operate in natural units: cost is USD/request and latency is
    seconds.  Callers should choose them for their own economics; these
    defaults intentionally favor a successful cheap response.
    """

    # ``completion_success`` in the bootstrap telemetry means that a turn
    # reached a terminal response, not that the answer was semantically good.
    # Keep that learned operational signal deliberately smaller than the live
    # catalog quality prior until outcome logging contains real task scores.
    success_weight: float = 0.05
    quality_weight: float = 0.0
    benchmark_weight: float = 0.75
    preference_weight: float = 0.01
    cost_weight: float = 25.0
    latency_weight: float = 0.0005
    failure_weight: float = 0.05
    uncertainty_weight: float = 0.05
    # Use a lower confidence bound for metadata-only quality predictions.
    # Live catalog benchmarks remain authoritative when present.
    quality_lcb_z: float = 1.0
    missing_metadata_penalty: float = 0.05
    min_context: int = 0
    max_cost_usd: Optional[float] = None
    max_latency_s: Optional[float] = None
    max_failure_probability: Optional[float] = None
    quality_floor: Optional[float] = None
    required_modalities: Tuple[str, ...] = ("text",)
    required_output_modalities: Tuple[str, ...] = ("text",)
    # OpenRouter may describe media generators as ``text+audio`` or
    # ``text+image`` outputs because they also return auxiliary text.  A plain
    # text request must not silently route to one of those generators.  The
    # caller can opt in when it genuinely accepts additional output media.
    allow_additional_output_modalities: bool = False
    required_parameters: Tuple[str, ...] = ()
    allowed_models: Tuple[str, ...] = ()
    denied_models: Tuple[str, ...] = ()
    allowed_providers: Tuple[str, ...] = ()
    denied_providers: Tuple[str, ...] = ()
    required_region: Optional[str] = None
    zdr_required: bool = False
    require_known_price: bool = True
    include_expired: bool = False
    allow_batch: bool = False
    # Nested OpenRouter virtual routers obscure the actual selected model and
    # defeat this router's own cost/quality accounting. They require opt-in.
    allow_router_models: bool = False

    @classmethod
    def from_mapping(cls, value: Optional[Mapping[str, Any]]) -> "RoutingPolicy":
        if value is None:
            return cls()
        raw: Dict[str, Any] = dict(value)
        weights = raw.get("weights")
        if isinstance(weights, Mapping):
            aliases = {
                "success": "success_weight",
                "quality": "quality_weight",
                "benchmark": "benchmark_weight",
                "preference": "preference_weight",
                "cost": "cost_weight",
                "latency": "latency_weight",
                "failure": "failure_weight",
                "uncertainty": "uncertainty_weight",
            }
            for name, target in aliases.items():
                if name in weights and target not in raw:
                    raw[target] = weights[name]
        aliases = {
            "lambda_cost": "cost_weight",
            "lambda_latency": "latency_weight",
            "lambda_failure": "failure_weight",
            "lambda_uncertainty": "uncertainty_weight",
            "latency_sla_s": "max_latency_s",
            "min_context_length": "min_context",
            "input_modalities": "required_modalities",
        }
        for source, target in aliases.items():
            if source in raw and target not in raw:
                raw[target] = raw[source]
        if "latency_sla_ms" in raw and "max_latency_s" not in raw:
            milliseconds = _as_float(raw["latency_sla_ms"])
            raw["max_latency_s"] = (
                None if milliseconds is None else milliseconds / 1000.0
            )

        accepted = {field.name for field in fields(cls)}
        kwargs: Dict[str, Any] = {name: raw[name] for name in accepted if name in raw}
        for tuple_name in (
            "required_modalities",
            "required_output_modalities",
            "required_parameters",
            "allowed_models",
            "denied_models",
            "allowed_providers",
            "denied_providers",
        ):
            if tuple_name in kwargs:
                kwargs[tuple_name] = _string_tuple(kwargs[tuple_name])
        numeric_names = (
            "success_weight",
            "quality_weight",
            "preference_weight",
            "benchmark_weight",
            "cost_weight",
            "latency_weight",
            "failure_weight",
            "uncertainty_weight",
            "quality_lcb_z",
            "missing_metadata_penalty",
            "max_cost_usd",
            "max_latency_s",
            "max_failure_probability",
            "quality_floor",
        )
        for name in numeric_names:
            if name in kwargs and kwargs[name] is not None:
                converted = _as_float(kwargs[name])
                if converted is None:
                    raise ValueError("policy %s must be numeric" % name)
                kwargs[name] = converted
        if kwargs.get("quality_lcb_z", 0.0) < 0.0:
            raise ValueError("policy quality_lcb_z must be nonnegative")
        if "min_context" in kwargs:
            kwargs["min_context"] = max(int(kwargs["min_context"]), 0)
        for bool_name in (
            "zdr_required",
            "require_known_price",
            "include_expired",
            "allow_batch",
            "allow_additional_output_modalities",
            "allow_router_models",
        ):
            if bool_name in kwargs:
                kwargs[bool_name] = _as_bool(kwargs[bool_name])
        return cls(**kwargs)


def load_catalog(path: str) -> List[Dict[str, Any]]:
    """Load a JSON catalog list or an object containing ``data``/``models``."""

    if path == "-":
        payload = json.load(sys.stdin)
    else:
        with Path(path).expanduser().open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    if isinstance(payload, Mapping):
        for key in ("data", "models", "catalog"):
            if isinstance(payload.get(key), list):
                payload = payload[key]
                break
        else:
            # Also accept {model_id: descriptor} fixtures.
            if all(isinstance(item, Mapping) for item in payload.values()):
                payload = [
                    dict(item, id=item.get("id", model_id))
                    for model_id, item in payload.items()
                ]
    if not isinstance(payload, list):
        raise ValueError("catalog must be a JSON list or an object with data/models")
    result: List[Dict[str, Any]] = []
    for index, value in enumerate(payload):
        if not isinstance(value, Mapping):
            raise ValueError("catalog row %d is not an object" % index)
        result.append(dict(value))
    return result


def _model_id(model: Mapping[str, Any]) -> str:
    return str(
        model.get("id") or model.get("canonical_slug") or model.get("slug") or ""
    ).strip()


def _provider(model_id: str) -> str:
    canonical = canonical_model_id(model_id)
    return canonical.split("/", 1)[0] if "/" in canonical else "unknown"


def _matches(model_id: str, patterns: Iterable[str]) -> bool:
    full = model_id.casefold()
    canonical = canonical_model_id(model_id).casefold()
    return any(
        fnmatch.fnmatchcase(full, str(pattern).casefold())
        or fnmatch.fnmatchcase(canonical, canonical_model_id(str(pattern)).casefold())
        for pattern in patterns
    )


def _modalities(model: Mapping[str, Any], direction: str) -> Tuple[str, ...]:
    architecture = _mapping(model.get("architecture"))
    values = architecture.get("%s_modalities" % direction)
    if values is not None:
        return tuple(value.casefold() for value in _string_tuple(values))
    modality = str(architecture.get("modality") or "").casefold()
    if "->" in modality:
        side = modality.split("->", 1)[0 if direction == "input" else 1]
        return tuple(part.strip() for part in side.split("+") if part.strip())
    return ()


def _supported_parameters(model: Mapping[str, Any]) -> set:
    supported = {
        value.casefold() for value in _string_tuple(model.get("supported_parameters"))
    }
    # Treat common catalog synonyms as the same required capability.
    if "tool_choice" in supported:
        supported.add("tools")
    if "tools" in supported:
        supported.add("tool_choice")
    return supported


def _context_length(model: Mapping[str, Any]) -> Optional[int]:
    values = (
        model.get("context_length"),
        _nested(model, "top_provider", "context_length"),
        model.get("context_window"),
    )
    finite = [
        int(value)
        for value in (_as_float(item) for item in values)
        if value is not None and value > 0
    ]
    return max(finite) if finite else None


def _expiration_epoch(value: Any) -> Optional[float]:
    numeric = _as_float(value)
    if numeric is not None and numeric > 0:
        return numeric
    if not isinstance(value, str) or not value.strip():
        return None
    text = value.strip().replace("Z", "+00:00")
    try:
        parsed = _datetime.datetime.fromisoformat(text)
    except ValueError:
        try:
            parsed = _datetime.datetime.strptime(text[:10], "%Y-%m-%d").replace(
                tzinfo=_datetime.timezone.utc
            )
        except ValueError:
            return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=_datetime.timezone.utc)
    return parsed.timestamp()


def _price(model: Mapping[str, Any], key: str) -> Optional[float]:
    pricing = _mapping(model.get("pricing"))
    aliases = {
        "prompt": ("prompt", "input", "input_token"),
        "completion": ("completion", "output", "output_token"),
        "request": ("request",),
    }
    for name in aliases[key]:
        if name in pricing:
            value = _as_float(pricing.get(name))
            if value is not None and value >= 0.0:
                return value
    return None


def estimated_live_cost(
    model: Mapping[str, Any], prompt_tokens: int, completion_tokens: int
) -> Optional[float]:
    prompt_price = _price(model, "prompt")
    completion_price = _price(model, "completion")
    request_price = _price(model, "request") or 0.0
    if prompt_price is None or completion_price is None:
        return None
    return (
        max(int(prompt_tokens), 0) * prompt_price
        + max(int(completion_tokens), 0) * completion_price
        + request_price
    )


def _observed_latency(model: Mapping[str, Any]) -> Optional[float]:
    value = _first(
        model,
        (
            ("latency_s",),
            ("latency_p95_s",),
            ("metrics", "latency_p95_s"),
            ("metrics", "latency_s"),
            ("telemetry", "latency_p95_s"),
        ),
    )
    seconds = _as_float(value)
    if seconds is not None and seconds >= 0.0:
        return seconds
    milliseconds = _as_float(
        _first(
            model,
            (
                ("latency_ms",),
                ("latency_p95_ms",),
                ("metrics", "latency_p95_ms"),
                ("telemetry", "latency_p95_ms"),
            ),
        )
    )
    return (
        milliseconds / 1000.0
        if milliseconds is not None and milliseconds >= 0.0
        else None
    )


def _observed_failure(model: Mapping[str, Any]) -> Optional[float]:
    value = _first(
        model,
        (
            ("failure_probability",),
            ("error_rate",),
            ("error_rate_ewma",),
            ("metrics", "error_rate_ewma"),
            ("telemetry", "error_rate_ewma"),
        ),
    )
    probability = _as_float(value)
    return min(max(probability, 0.0), 1.0) if probability is not None else None


def _benchmark_quality(
    model: Mapping[str, Any], *, code_task: bool, agentic_task: bool
) -> Tuple[Optional[float], Dict[str, Optional[float]]]:
    """Return a task-conditioned quality prior from current catalog metadata.

    These indices are live metadata, not labels learned from historical DSCO
    completion events. Missing components are omitted and the remaining
    weights are renormalized; a model with no benchmark stays explicitly
    unknown rather than receiving a fabricated zero-quality label.
    """

    aa = _mapping(_nested(model, "benchmarks", "artificial_analysis"))
    values: Dict[str, Optional[float]] = {}
    for name in ("intelligence_index", "coding_index", "agentic_index"):
        value = _as_float(aa.get(name))
        values[name] = None if value is None else min(max(value / 100.0, 0.0), 1.0)

    score = _weighted_task_quality(
        {
            "general_intelligence_quality": values["intelligence_index"],
            "coding_quality": values["coding_index"],
            "agentic_quality": values["agentic_index"],
        },
        code_task=code_task,
        agentic_task=agentic_task,
    )
    return score, values


def _task_quality_weights(*, code_task: bool, agentic_task: bool) -> Dict[str, float]:
    """Weights shared by observed and learned catalog-quality components."""

    if code_task and agentic_task:
        return {
            "general_intelligence_quality": 0.20,
            "coding_quality": 0.45,
            "agentic_quality": 0.35,
        }
    if code_task:
        return {
            "general_intelligence_quality": 0.25,
            "coding_quality": 0.75,
            "agentic_quality": 0.0,
        }
    if agentic_task:
        return {
            "general_intelligence_quality": 0.35,
            "coding_quality": 0.0,
            "agentic_quality": 0.65,
        }
    return {
        "general_intelligence_quality": 1.0,
        "coding_quality": 0.0,
        "agentic_quality": 0.0,
    }


def _weighted_task_quality(
    values: Mapping[str, Optional[float]], *, code_task: bool, agentic_task: bool
) -> Optional[float]:
    weights = _task_quality_weights(code_task=code_task, agentic_task=agentic_task)
    denominator = sum(
        weight for name, weight in weights.items() if values[name] is not None
    )
    if denominator <= 0.0:
        return None
    score = sum(
        weight * float(values[name])
        for name, weight in weights.items()
        if values[name] is not None
    )
    return score / denominator


def _named_task_flag(
    router: CandidateRouter, task_vector: np.ndarray, name: str
) -> bool:
    try:
        index = router.task_feature_names.index(name)
    except ValueError:
        return False
    return bool(index < task_vector.size and task_vector[index] >= 0.5)


def _zdr_available(model: Mapping[str, Any]) -> Optional[bool]:
    value = _first(
        model,
        (
            ("zdr_available",),
            ("zero_data_retention",),
            ("data_policy", "zero_data_retention"),
            ("privacy", "zdr"),
        ),
    )
    return None if value is None else _as_bool(value)


def _regions(model: Mapping[str, Any]) -> Tuple[str, ...]:
    value = _first(
        model, (("regions",), ("data_policy", "regions"), ("top_provider", "regions"))
    )
    return tuple(item.casefold() for item in _string_tuple(value))


def _request_prompt(request: Mapping[str, Any]) -> str:
    prompt = request.get("prompt")
    if isinstance(prompt, str):
        return prompt
    messages = request.get("messages")
    if isinstance(messages, list):
        chunks: List[str] = []
        for message in messages:
            if not isinstance(message, Mapping):
                continue
            content = message.get("content")
            if isinstance(content, str):
                chunks.append("%s: %s" % (message.get("role", "unknown"), content))
            elif isinstance(content, list):
                for part in content:
                    if isinstance(part, Mapping) and isinstance(part.get("text"), str):
                        chunks.append(
                            "%s: %s" % (message.get("role", "unknown"), part["text"])
                        )
        return "\n".join(chunks)
    return ""


def _request_task_vector(router: RouterModel, request: Mapping[str, Any]) -> np.ndarray:
    supplied = request.get("task_features")
    if supplied is not None:
        vector = np.asarray(supplied, dtype=np.float64).reshape(-1)
        if vector.size != router.task_dim:
            raise ValueError(
                "request task_features has %d values, router expects %d"
                % (vector.size, router.task_dim)
            )
        return vector
    return np.asarray(
        task_features(_request_prompt(request), dim=router.task_dim), dtype=np.float64
    )


def _candidate_vector(
    router: RouterModel,
    model: Mapping[str, Any],
    *,
    include_benchmark_priors: bool = True,
) -> np.ndarray:
    if include_benchmark_priors:
        supplied = model.get("_chimera_model_features_v2")
        if supplied is None:
            supplied = model.get("features")
        if supplied is None:
            supplied = model.get("model_features")
    else:
        # This cache is generated internally from the anti-leak vectorizer.
        # Never reuse the operational vector, whose benchmark columns may
        # contain the weak-supervision targets used by the quality artifact.
        supplied = model.get("_chimera_quality_features_v2")
    if supplied is not None:
        vector = np.asarray(supplied, dtype=np.float64).reshape(-1)
        if vector.size == router.model_dim:
            return vector
        # A stale cached vector must not take down routing for the entire live
        # catalog. Recompute from the versioned descriptor contract instead.
    return np.asarray(
        model_features(
            model,
            dim=router.model_dim,
            requested_model_id=_model_id(model),
            catalog_match=True,
            include_benchmark_priors=include_benchmark_priors,
        ),
        dtype=np.float64,
    )


def _base_rejections(
    model: Mapping[str, Any],
    policy: RoutingPolicy,
    now_epoch: float,
    minimum_context: int,
) -> List[str]:
    model_id = _model_id(model)
    canonical = canonical_model_id(model_id)
    provider = _provider(model_id).casefold()
    reasons: List[str] = []
    if not model_id:
        reasons.append("missing_model_id")
    if model_id.casefold().startswith("openrouter/") and not policy.allow_router_models:
        reasons.append("nested_router_not_allowed")
    if canonical.casefold().endswith(":batch") and not policy.allow_batch:
        reasons.append("batch_variant_not_allowed")
    if policy.allowed_models and not _matches(model_id, policy.allowed_models):
        reasons.append("not_in_allowed_models")
    if policy.denied_models and _matches(model_id, policy.denied_models):
        reasons.append("denied_model")
    if policy.allowed_providers and provider not in {
        value.casefold() for value in policy.allowed_providers
    }:
        reasons.append("not_in_allowed_providers")
    if provider in {value.casefold() for value in policy.denied_providers}:
        reasons.append("denied_provider")

    if minimum_context > 0:
        context = _context_length(model)
        if context is None:
            reasons.append("unknown_context_length")
        elif context < minimum_context:
            reasons.append("context_too_small")

    required_modalities = {value.casefold() for value in policy.required_modalities}
    if required_modalities:
        available_modalities = set(_modalities(model, "input"))
        missing_modalities = sorted(
            required_modalities.difference(available_modalities)
        )
        reasons.extend("missing_modality:%s" % value for value in missing_modalities)

    required_outputs = {value.casefold() for value in policy.required_output_modalities}
    if required_outputs:
        available_outputs = set(_modalities(model, "output"))
        missing_outputs = sorted(required_outputs.difference(available_outputs))
        reasons.extend(
            "missing_output_modality:%s" % value for value in missing_outputs
        )
        if not policy.allow_additional_output_modalities:
            unexpected_outputs = sorted(available_outputs.difference(required_outputs))
            reasons.extend(
                "unexpected_output_modality:%s" % value for value in unexpected_outputs
            )

    supported = _supported_parameters(model)
    missing_parameters = sorted(
        value.casefold()
        for value in policy.required_parameters
        if value.casefold() not in supported
    )
    reasons.extend("missing_parameter:%s" % value for value in missing_parameters)

    if policy.zdr_required and _zdr_available(model) is not True:
        reasons.append("zdr_unavailable_or_unknown")
    if policy.required_region:
        regions = set(_regions(model))
        if policy.required_region.casefold() not in regions:
            reasons.append("region_unavailable_or_unknown")

    expiration = _expiration_epoch(model.get("expiration_date"))
    if (
        expiration is not None
        and expiration <= now_epoch
        and not policy.include_expired
    ):
        reasons.append("expired")
    if model.get("available") is False or str(model.get("status") or "").casefold() in (
        "disabled",
        "offline",
        "unavailable",
        "deprecated",
    ):
        reasons.append("unavailable")
    return reasons


def _find_override(
    catalog: Sequence[Mapping[str, Any]], requested: str
) -> Optional[Mapping[str, Any]]:
    requested_full = requested.strip().casefold()
    requested_canonical = canonical_model_id(requested).casefold()
    for model in catalog:
        candidate = _model_id(model)
        if candidate.casefold() == requested_full:
            return model
    for model in catalog:
        if canonical_model_id(_model_id(model)).casefold() == requested_canonical:
            return model
    return None


def _merge_policy(
    request: Mapping[str, Any], policy: Optional[Mapping[str, Any]]
) -> RoutingPolicy:
    merged: Dict[str, Any] = {}
    embedded = request.get("routing_policy") or request.get("policy")
    if isinstance(embedded, Mapping):
        merged.update(embedded)
    # Common request-level constraints are accepted without nesting.
    for name in (
        "min_context",
        "min_context_length",
        "max_cost_usd",
        "max_latency_s",
        "latency_sla_ms",
        "max_failure_probability",
        "quality_floor",
        "required_modalities",
        "input_modalities",
        "required_output_modalities",
        "output_modalities",
        "required_parameters",
        "allowed_models",
        "denied_models",
        "allowed_providers",
        "denied_providers",
        "required_region",
        "zdr_required",
        "require_known_price",
        "allow_batch",
    ):
        if name in request:
            merged[name] = request[name]
    if "output_modalities" in merged and "required_output_modalities" not in merged:
        merged["required_output_modalities"] = merged["output_modalities"]
    if policy:
        merged.update(policy)
    return RoutingPolicy.from_mapping(merged)


def _artifact_feature_version(router: RouterModel, name: str) -> Optional[str]:
    metadata: Mapping[str, Any] = router.training_metadata
    containers: List[Mapping[str, Any]] = [metadata]
    for key in ("feature_contract", "dataset_metadata"):
        child = metadata.get(key)
        if isinstance(child, Mapping):
            containers.append(child)
            nested = child.get("feature_contract")
            if isinstance(nested, Mapping):
                containers.append(nested)
    for container in containers:
        value = container.get(name)
        if value is not None:
            return str(value)
    return None


def _validate_feature_contract(router: RouterModel) -> None:
    artifact_task = _artifact_feature_version(router, "task_feature_version")
    artifact_model = _artifact_feature_version(router, "model_feature_version")
    if artifact_task is None or artifact_model is None:
        raise ValueError(
            "router artifact has no feature-version contract; retrain/export it with current train.py"
        )
    if artifact_task != TASK_FEATURE_VERSION or artifact_model != MODEL_FEATURE_VERSION:
        raise ValueError(
            "router feature contract mismatch: artifact task=%r model=%r, runtime task=%r model=%r"
            % (
                artifact_task,
                artifact_model,
                TASK_FEATURE_VERSION,
                MODEL_FEATURE_VERSION,
            )
        )


def _validate_quality_feature_contract(router: RouterModel) -> None:
    artifact_task = _artifact_feature_version(router, "task_feature_version")
    artifact_model = _artifact_feature_version(router, "model_feature_version")
    artifact_role = str(router.training_metadata.get("artifact_role") or "")
    if artifact_task != QUALITY_TASK_FEATURE_VERSION:
        raise ValueError(
            "quality router task feature mismatch: artifact %r, runtime %r"
            % (artifact_task, QUALITY_TASK_FEATURE_VERSION)
        )
    if artifact_model != MODEL_FEATURE_VERSION:
        raise ValueError(
            "quality router model feature mismatch: artifact %r, runtime %r"
            % (artifact_model, MODEL_FEATURE_VERSION)
        )
    if artifact_role != "catalog_quality_predictor" or router.task_dim != 1:
        raise ValueError("quality router is not a catalog-quality artifact")


def _load_router_artifact(value: Union[RouterModel, str]) -> RouterModel:
    if isinstance(value, str):
        return RouterEnsemble.load(value)
    if isinstance(value, (CandidateRouter, RouterEnsemble)):
        return value
    raise TypeError(
        "router must be a CandidateRouter, RouterEnsemble, or artifact path"
    )


def _prediction_distribution(
    router: RouterModel, task: np.ndarray, model: np.ndarray
) -> Tuple[np.ndarray, np.ndarray]:
    if isinstance(router, RouterEnsemble):
        distribution = router.predict_distribution(task, model)
        return (
            np.asarray(distribution["mean"], dtype=np.float64),
            np.asarray(distribution["std"], dtype=np.float64),
        )
    mean = np.asarray(router.predict(task, model), dtype=np.float64)
    return mean, np.zeros_like(mean)


def _catalog_quality_predictions(
    router: Optional[RouterModel],
    catalog: Sequence[Mapping[str, Any]],
    *,
    code_task: bool,
    agentic_task: bool,
) -> Tuple[List[Optional[float]], List[Optional[float]], List[Dict[str, float]]]:
    rows = len(catalog)
    if router is None:
        return [None] * rows, [None] * rows, [{} for _ in range(rows)]
    _validate_quality_feature_contract(router)
    candidates = np.vstack(
        [
            _candidate_vector(router, descriptor, include_benchmark_priors=False)
            for descriptor in catalog
        ]
    )
    tasks = np.ones((rows, 1), dtype=np.float64)
    mean, std = _prediction_distribution(router, tasks, candidates)
    name_to_index = {name: index for index, name in enumerate(router.outcome_names)}
    required = (
        "general_intelligence_quality",
        "coding_quality",
        "agentic_quality",
    )
    if any(name not in name_to_index for name in required):
        raise ValueError("quality router is missing general/coding/agentic heads")
    weights = _task_quality_weights(code_task=code_task, agentic_task=agentic_task)
    quality: List[Optional[float]] = []
    uncertainty: List[Optional[float]] = []
    components: List[Dict[str, float]] = []
    for row in range(rows):
        values = {
            name: min(max(float(mean[row, name_to_index[name]]), 0.0), 1.0)
            for name in required
        }
        component_std = {
            name: max(float(std[row, name_to_index[name]]), 0.0) for name in required
        }
        quality.append(
            _weighted_task_quality(
                values, code_task=code_task, agentic_task=agentic_task
            )
        )
        uncertainty.append(
            sum(weights[name] * component_std[name] for name in required)
        )
        components.append(values)
    return quality, uncertainty, components


def route_request(
    router: Union[RouterModel, str],
    request: Mapping[str, Any],
    catalog: Sequence[Mapping[str, Any]],
    *,
    quality_router: Optional[Union[RouterModel, str]] = None,
    policy: Optional[Mapping[str, Any]] = None,
    explicit_model: Optional[str] = None,
    top_k: int = 10,
    now_epoch: Optional[float] = None,
) -> Dict[str, Any]:
    """Rank ``catalog`` and return a JSON-serializable route decision.

    ``explicit_model`` wins over request/policy overrides.  If the model is not
    in the catalog, the result still selects the requested ID, with
    ``catalog_match=false`` and no learned prediction.  This preserves the
    host CLI/API's explicit model contract.
    """

    model_router = _load_router_artifact(router)
    catalog_quality_router = (
        None if quality_router is None else _load_router_artifact(quality_router)
    )
    catalog_rows = [dict(value) for value in catalog if isinstance(value, Mapping)]
    routing_policy = _merge_policy(request, policy)
    current_time = time.time() if now_epoch is None else float(now_epoch)

    override_value = explicit_model
    if not override_value:
        override_value = request.get("explicit_model_override")  # type: ignore[assignment]
    if not override_value and isinstance(request.get("routing"), Mapping):
        override_value = request["routing"].get("explicit_model_override")  # type: ignore[index,assignment]
    if override_value:
        override_id = str(override_value).strip()
        matched = _find_override(catalog_rows, override_id)
        if matched is None:
            return {
                "schema_version": ROUTE_SCHEMA_VERSION,
                "selected_model": override_id,
                "fallback_models": [],
                "explicit_override": True,
                "catalog_match": False,
                "override_violations": ["model_not_in_catalog"],
                "candidates": [],
                "rejected_count": 0,
            }
        # An override should not be made dependent on unrelated catalog rows.
        # Score just the forced descriptor for observability and violations.
        catalog_rows = [dict(matched)]
    else:
        override_id = ""
        matched = None

    if not catalog_rows:
        return {
            "schema_version": ROUTE_SCHEMA_VERSION,
            "selected_model": None,
            "fallback_models": [],
            "explicit_override": False,
            "catalog_match": False,
            "error": "empty_catalog",
            "candidates": [],
            "rejected_count": 0,
        }

    # Dimensions alone are insufficient: a future vectorizer could retain the
    # same widths while changing hash namespaces or transforms.
    _validate_feature_contract(model_router)
    task_vector = _request_task_vector(model_router, request)
    code_task = _named_task_flag(model_router, task_vector, "bool:code_task")
    agentic_task = _named_task_flag(
        model_router, task_vector, "bool:action_request"
    ) or any(
        value.casefold() in {"tools", "tool_choice"}
        for value in routing_policy.required_parameters
    )
    candidate_vectors = np.vstack(
        [_candidate_vector(model_router, row) for row in catalog_rows]
    )
    task_matrix = np.repeat(task_vector[None, :], len(catalog_rows), axis=0)
    predictions, prediction_std = _prediction_distribution(
        model_router, task_matrix, candidate_vectors
    )
    raw_predictions = model_router.raw_scores(task_matrix, candidate_vectors)
    support_distances = model_router.support_distance(task_matrix, candidate_vectors)

    name_to_index = {
        name: index for index, name in enumerate(model_router.outcome_names)
    }
    success_index = choose_head(
        model_router.outcome_names,
        ("completion_success", "success", "quality", "reward"),
        0,
    )
    quality_index = choose_head(model_router.outcome_names, ("quality",), success_index)
    provider_failure_index = choose_head(
        model_router.outcome_names, ("provider_failure", "failure", "error"), -1
    )
    tool_error_index = choose_head(model_router.outcome_names, ("tool_error",), -1)
    cost_index = choose_head(model_router.outcome_names, ("cost_usd", "cost"), -1)
    latency_index = choose_head(
        model_router.outcome_names, ("latency_s", "latency"), -1
    )
    preference_index = name_to_index.get("preference_score", -1)
    catalog_quality, catalog_quality_std, catalog_quality_components = (
        _catalog_quality_predictions(
            catalog_quality_router,
            catalog_rows,
            code_task=code_task,
            agentic_task=agentic_task,
        )
    )

    supplied_prompt_tokens = _as_float(
        request.get("estimated_prompt_tokens"), _as_float(request.get("prompt_tokens"))
    )
    if supplied_prompt_tokens is None:
        # Catalog prices are per token, but this dependency-free router does
        # not carry every vendor tokenizer. UTF-8 bytes/4 is an inspectable,
        # conservative-enough estimate for routing (billing uses actual usage).
        prompt_tokens = max(
            int(math.ceil(len(_request_prompt(request).encode("utf-8")) / 4.0)), 1
        )
        prompt_token_source = "utf8_bytes_div4"
    else:
        prompt_tokens = max(int(supplied_prompt_tokens), 0)
        prompt_token_source = "request"
    supplied_completion_tokens = _as_float(
        request.get("estimated_completion_tokens"),
        _as_float(
            request.get("max_tokens"), _as_float(request.get("completion_tokens"))
        ),
    )
    if supplied_completion_tokens is None:
        completion_tokens = 512
        completion_token_source = "default_512"
    else:
        completion_tokens = max(int(supplied_completion_tokens), 0)
        completion_token_source = "request"
    explicit_min_context = int(_as_float(request.get("min_context"), 0.0) or 0)
    minimum_context = max(
        routing_policy.min_context,
        explicit_min_context,
        prompt_tokens + completion_tokens if prompt_tokens or completion_tokens else 0,
    )

    ranked: List[Dict[str, Any]] = []
    for index, model in enumerate(catalog_rows):
        model_id = _model_id(model)
        prediction = {
            name: float(predictions[index, head])
            for head, name in enumerate(model_router.outcome_names)
        }
        reasons = _base_rejections(model, routing_policy, current_time, minimum_context)
        live_cost = estimated_live_cost(model, prompt_tokens, completion_tokens)
        predicted_cost = (
            max(float(predictions[index, cost_index]), 0.0) if cost_index >= 0 else None
        )
        cost = live_cost if live_cost is not None else predicted_cost
        observed_latency = _observed_latency(model)
        predicted_latency = (
            max(float(predictions[index, latency_index]), 0.0)
            if latency_index >= 0
            else None
        )
        latency = (
            observed_latency if observed_latency is not None else predicted_latency
        )

        provider_failure = (
            min(max(float(predictions[index, provider_failure_index]), 0.0), 1.0)
            if provider_failure_index >= 0
            else 0.0
        )
        tool_error = (
            min(max(float(predictions[index, tool_error_index]), 0.0), 1.0)
            if tool_error_index >= 0
            else 0.0
        )
        predicted_failure = 1.0 - (1.0 - provider_failure) * (1.0 - tool_error)
        observed_failure = _observed_failure(model)
        failure = max(predicted_failure, observed_failure or 0.0)
        success = min(max(float(predictions[index, success_index]), 0.0), 1.0)
        quality = min(max(float(predictions[index, quality_index]), 0.0), 1.0)
        preference = (
            math.tanh(float(raw_predictions[index, preference_index]))
            if preference_index >= 0
            else 0.0
        )
        benchmark_quality, benchmark_components = _benchmark_quality(
            model, code_task=code_task, agentic_task=agentic_task
        )
        learned_catalog_quality = catalog_quality[index]
        learned_catalog_uncertainty = catalog_quality_std[index]
        learned_catalog_lcb = (
            min(
                max(
                    float(learned_catalog_quality)
                    - routing_policy.quality_lcb_z
                    * float(learned_catalog_uncertainty or 0.0),
                    0.0,
                ),
                1.0,
            )
            if learned_catalog_quality is not None
            else None
        )
        quality_prior = (
            benchmark_quality if benchmark_quality is not None else learned_catalog_lcb
        )
        quality_source = (
            "live_catalog_benchmark"
            if benchmark_quality is not None
            else (
                "semantic_quality_ensemble"
                if learned_catalog_quality is not None
                else "operational_completion_proxy"
            )
        )
        probability_indices = [
            head
            for head in (success_index, provider_failure_index, tool_error_index)
            if head >= 0
        ]
        epistemic_uncertainty = (
            max(float(prediction_std[index, head]) for head in probability_indices)
            if probability_indices
            else 0.0
        )
        combined_uncertainty = (
            float(support_distances[index])
            + epistemic_uncertainty
            + float(learned_catalog_uncertainty or 0.0)
        )

        if routing_policy.require_known_price and live_cost is None:
            reasons.append("unknown_live_price")
        if routing_policy.max_cost_usd is not None:
            if live_cost is None:
                if "unknown_live_price" not in reasons:
                    reasons.append("unknown_live_price")
            elif live_cost > routing_policy.max_cost_usd:
                reasons.append("cost_above_limit")
        if routing_policy.max_latency_s is not None:
            if latency is None:
                reasons.append("unknown_latency")
            elif latency > routing_policy.max_latency_s:
                reasons.append("latency_above_sla")
        if (
            routing_policy.max_failure_probability is not None
            and failure > routing_policy.max_failure_probability
        ):
            reasons.append("failure_probability_above_limit")
        quality_signal = quality_prior if quality_prior is not None else success
        if (
            routing_policy.quality_floor is not None
            and quality_signal < routing_policy.quality_floor
        ):
            reasons.append("quality_below_floor")

        missing_count = (
            int(live_cost is None) + int(latency is None) + int(quality_prior is None)
        )
        utility = (
            routing_policy.success_weight * success
            + routing_policy.quality_weight * quality
            + routing_policy.benchmark_weight * (quality_prior or 0.0)
            + routing_policy.preference_weight * preference
            - routing_policy.cost_weight * (cost or 0.0)
            - routing_policy.latency_weight * (latency or 0.0)
            - routing_policy.failure_weight * failure
            - routing_policy.uncertainty_weight * combined_uncertainty
            - routing_policy.missing_metadata_penalty * missing_count
        )
        ranked.append(
            {
                "model_id": model_id,
                "canonical_model_id": canonical_model_id(model_id),
                "feasible": not reasons,
                "rejection_reasons": reasons,
                "utility": float(utility),
                "predictions": prediction,
                "expected_cost_usd": None if cost is None else float(cost),
                "cost_source": (
                    "live_catalog"
                    if live_cost is not None
                    else "learned"
                    if predicted_cost is not None
                    else None
                ),
                "expected_latency_s": None if latency is None else float(latency),
                "latency_source": (
                    "live_catalog"
                    if observed_latency is not None
                    else "learned"
                    if predicted_latency is not None
                    else None
                ),
                "failure_probability": float(failure),
                "benchmark_quality": benchmark_quality,
                "benchmark_components": benchmark_components,
                "catalog_quality_prediction": learned_catalog_quality,
                "catalog_quality_lcb": learned_catalog_lcb,
                "catalog_quality_components": catalog_quality_components[index],
                "catalog_quality_uncertainty": learned_catalog_uncertainty,
                "quality_prior": quality_signal,
                "quality_source": quality_source,
                "epistemic_uncertainty": epistemic_uncertainty,
                "combined_uncertainty": combined_uncertainty,
                "support_distance": float(support_distances[index]),
            }
        )

    ranked.sort(
        key=lambda item: (
            not item["feasible"],
            -item["utility"],
            item["canonical_model_id"],
        )
    )
    feasible = [item for item in ranked if item["feasible"]]
    selected = feasible[0]["model_id"] if feasible else None
    override_violations: List[str] = []
    if matched is not None:
        matched_id = _model_id(matched)
        selected = matched_id
        match_row = next(item for item in ranked if item["model_id"] == matched_id)
        override_violations = list(match_row["rejection_reasons"])
        match_row["selected_by_override"] = True
        # Show the forced candidate first without pretending its utility won.
        ranked.remove(match_row)
        ranked.insert(0, match_row)

    top_count = max(int(top_k), 1)
    fallback_models = [
        item["model_id"] for item in feasible if item["model_id"] != selected
    ][: max(top_count - 1, 0)]
    result: Dict[str, Any] = {
        "schema_version": ROUTE_SCHEMA_VERSION,
        "selected_model": selected,
        "fallback_models": fallback_models,
        "explicit_override": matched is not None,
        "catalog_match": selected is not None,
        "candidates": ranked[:top_count],
        "feasible_count": len(feasible),
        "rejected_count": len(ranked) - len(feasible),
        "policy": asdict(routing_policy),
        "router_kind": (
            "ensemble" if isinstance(model_router, RouterEnsemble) else "single"
        ),
        "ensemble_members": (
            len(model_router.members) if isinstance(model_router, RouterEnsemble) else 1
        ),
        "quality_router": catalog_quality_router is not None,
        "token_estimates": {
            "prompt_tokens": prompt_tokens,
            "prompt_source": prompt_token_source,
            "completion_tokens": completion_tokens,
            "completion_source": completion_token_source,
        },
    }
    if override_violations:
        result["override_violations"] = override_violations
    if selected is None:
        result["error"] = "no_feasible_model"
    return result


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--router", required=True, help="trained router .npz")
    parser.add_argument(
        "--quality-router",
        help="optional semantic catalog-quality ensemble .npz",
    )
    parser.add_argument("--catalog", required=True, help="catalog JSON, or - for stdin")
    request_group = parser.add_mutually_exclusive_group(required=True)
    request_group.add_argument("--request", help="request/policy JSON")
    request_group.add_argument("--prompt", help="literal prompt")
    parser.add_argument("--model", help="authoritative explicit model override")
    parser.add_argument("--policy", help="JSON policy overlay")
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--prompt-tokens", type=int)
    parser.add_argument("--completion-tokens", type=int)
    parser.add_argument("--max-cost-usd", type=float)
    parser.add_argument("--max-latency-s", type=float)
    parser.add_argument("--benchmark-weight", type=float)
    parser.add_argument("--cost-weight", type=float)
    parser.add_argument("--latency-weight", type=float)
    parser.add_argument("--failure-weight", type=float)
    return parser


def _load_json_object(path: str) -> Dict[str, Any]:
    with Path(path).expanduser().open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, Mapping):
        raise ValueError("expected a JSON object: %s" % path)
    return dict(value)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    catalog = load_catalog(args.catalog)
    request = (
        _load_json_object(args.request)
        if args.request
        else {"prompt": args.prompt or ""}
    )
    if args.prompt_tokens is not None:
        request["estimated_prompt_tokens"] = args.prompt_tokens
    if args.completion_tokens is not None:
        request["estimated_completion_tokens"] = args.completion_tokens
    policy = _load_json_object(args.policy) if args.policy else {}
    for name in (
        "max_cost_usd",
        "max_latency_s",
        "benchmark_weight",
        "cost_weight",
        "latency_weight",
        "failure_weight",
    ):
        value = getattr(args, name)
        if value is not None:
            policy[name] = value
    result = route_request(
        args.router,
        request,
        catalog,
        quality_router=args.quality_router,
        policy=policy,
        explicit_model=args.model,
        top_k=args.top_k,
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result.get("selected_model") else 2


if __name__ == "__main__":
    sys.exit(main())


__all__ = [
    "ROUTE_SCHEMA_VERSION",
    "RoutingPolicy",
    "estimated_live_cost",
    "load_catalog",
    "route_request",
]
