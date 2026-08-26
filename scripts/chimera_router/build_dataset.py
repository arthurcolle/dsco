#!/usr/bin/env python3
"""Build a deterministic Chimera routing dataset from DSCO telemetry.

The builder joins prompt-level outcomes in ``~/.dsco/baseline.db`` to a pinned
OpenRouter catalog snapshot.  Generated artifacts contain hashed task vectors,
catalog-derived model descriptors, outcome labels, and exact-prompt comparison
pairs.  V2 also emits a separate weak-supervision catalog-quality lane with
per-cell masks/provenance and benchmark-derived input columns suppressed, plus
model/provider/family cold-start identifiers.  Catalog quality labels are
never appended to historical outcomes.  Artifacts never contain raw prompt
text.  Prompt hashes and hashed features remain potentially sensitive and
should be handled like telemetry, not treated as anonymization.

Example::

    python3 scripts/chimera_router/build_dataset.py \
        --db ~/.dsco/baseline.db \
        --catalog ~/.dsco/openrouter_models.json \
        --output ~/.dsco/chimera_router/dataset.npz
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import hmac
import io
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
import zipfile
from dataclasses import dataclass, field
from typing import (
    Any,
    Dict,
    Iterable,
    List,
    Mapping,
    MutableMapping,
    Optional,
    Sequence,
    Tuple,
)

import numpy as np

try:
    from .features import (
        DEFAULT_MODEL_DIM,
        DEFAULT_TASK_DIM,
        MODEL_BENCHMARK_DERIVED_INDICES,
        MODEL_FAMILY_VERSION,
        TASK_FEATURE_VERSION,
        canonical_model_id,
        feature_contract,
        model_family_id,
        model_feature_names,
        model_features,
        task_feature_names,
        task_features,
    )
except ImportError:  # Direct ``python build_dataset.py`` execution.
    from features import (  # type: ignore
        DEFAULT_MODEL_DIM,
        DEFAULT_TASK_DIM,
        MODEL_BENCHMARK_DERIVED_INDICES,
        MODEL_FAMILY_VERSION,
        TASK_FEATURE_VERSION,
        canonical_model_id,
        feature_contract,
        model_family_id,
        model_feature_names,
        model_features,
        task_feature_names,
        task_features,
    )


SCHEMA_VERSION = "chimera-router-dataset-v2"
LABEL_NAMES: Tuple[str, ...] = (
    "completion_success",
    "provider_failure",
    "tool_error",
    "cost_usd",
    "latency_s",
)
CATALOG_QUALITY_SUPERVISION_VERSION = "openrouter-artificial-analysis-v1"
FEEDBACK_IMPORT_VERSION = "chimera-feedback-import-v1"
FEEDBACK_SCHEMA_VERSION = 3
FEEDBACK_SUPPORTED_SCHEMA_VERSIONS = (2, 3)
FEEDBACK_FEATURE_ENCODING = "float32-le"
CATALOG_QUALITY_LABEL_SPECS: Tuple[Tuple[str, str, str], ...] = (
    (
        "general_intelligence_quality",
        "intelligence_index",
        "openrouter_catalog:benchmarks.artificial_analysis.intelligence_index",
    ),
    (
        "coding_quality",
        "coding_index",
        "openrouter_catalog:benchmarks.artificial_analysis.coding_index",
    ),
    (
        "agentic_quality",
        "agentic_index",
        "openrouter_catalog:benchmarks.artificial_analysis.agentic_index",
    ),
)
PROMPT_TITLES = frozenset({"prompt", "oneshot_prompt"})
PROVIDER_FAILURE_TITLES = frozenset(
    {
        "stream_failed",
        "provider_unavailable",
        "provider_failed",
        "request_failed",
        "authentication_failed",
        "rate_limited",
    }
)
_LOOKUP_NORMALISE_RE = re.compile(r"[^a-z0-9]+")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_BASELINE_LABEL_PROVENANCE: Tuple[str, ...] = (
    "baseline.events.turn_done",
    "baseline.events.provider_failure",
    "baseline.events.tool_error",
    "baseline.events.est_cost_usd",
    "baseline.events.terminal_epoch_minus_prompt_epoch",
)


def _finite_float(value: Any, default: float = 0.0) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return default
    return number if math.isfinite(number) else default


def _safe_json_object(value: Any) -> Dict[str, Any]:
    if not isinstance(value, str) or not value:
        return {}
    try:
        decoded = json.loads(value)
    except (TypeError, ValueError, json.JSONDecodeError):
        return {}
    return decoded if isinstance(decoded, dict) else {}


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def prompt_digest(prompt: str) -> str:
    """Hash exact UTF-8 prompt bytes; no normalization is applied."""

    return _sha256_bytes(prompt.encode("utf-8", "surrogateescape"))


def _stable_fold(value: str, folds: int = 10) -> int:
    digest = hashlib.blake2b(
        value.encode("utf-8", "surrogatepass"), digest_size=8
    ).digest()
    return int.from_bytes(digest, "little") % max(1, folds)


def _holdout_id(kind: str, value: str) -> str:
    """Opaque, deterministic identity for disjoint cold-start partitions."""

    return _sha256_bytes(
        ("chimera-holdout-v1\x1f" + kind + "\x1f" + value).encode(
            "utf-8", "surrogatepass"
        )
    )


def _lookup_key(value: str) -> str:
    return _LOOKUP_NORMALISE_RE.sub("-", value.casefold()).strip("-")


def _provider_from_model_id(model_id: str) -> str:
    canonical = canonical_model_id(model_id)
    if "/" in canonical:
        return canonical.split("/", 1)[0].casefold()
    if ":" in canonical:
        return canonical.split(":", 1)[0].casefold()
    return "unknown"


def _catalog_quality_targets(
    descriptor: Mapping[str, Any],
) -> Tuple[np.ndarray, np.ndarray, List[str]]:
    benchmarks = descriptor.get("benchmarks")
    artificial_analysis: Mapping[str, Any] = {}
    if isinstance(benchmarks, Mapping):
        possible = benchmarks.get("artificial_analysis")
        if isinstance(possible, Mapping):
            artificial_analysis = possible

    labels = np.zeros(len(CATALOG_QUALITY_LABEL_SPECS), dtype=np.float32)
    mask = np.zeros(len(CATALOG_QUALITY_LABEL_SPECS), dtype=np.uint8)
    provenance = [""] * len(CATALOG_QUALITY_LABEL_SPECS)
    for index, (_name, key, source) in enumerate(CATALOG_QUALITY_LABEL_SPECS):
        if key not in artificial_analysis or isinstance(
            artificial_analysis.get(key), bool
        ):
            continue
        try:
            value = float(artificial_analysis.get(key))
        except (TypeError, ValueError, OverflowError):
            continue
        if not math.isfinite(value) or value < 0.0 or value > 100.0:
            continue
        labels[index] = np.float32(value / 100.0)
        mask[index] = np.uint8(1)
        provenance[index] = source
    return labels, mask, provenance


class CatalogIndex:
    """Resolve DSCO model spellings to descriptors without a frozen enum."""

    def __init__(self, payload: bytes) -> None:
        try:
            decoded = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as exc:
            raise ValueError("catalog is not valid UTF-8 JSON") from exc
        if isinstance(decoded, Mapping):
            raw_models = decoded.get("data", [])
        else:
            raw_models = decoded
        if not isinstance(raw_models, list):
            raise ValueError(
                "catalog JSON must be a list or an object containing a data list"
            )

        self.snapshot_sha256 = _sha256_bytes(payload)
        self.models: Dict[str, Dict[str, Any]] = {}
        aliases: MutableMapping[str, List[str]] = {}
        for item in raw_models:
            if not isinstance(item, Mapping):
                continue
            model_id = canonical_model_id(str(item.get("id") or ""))
            if not model_id:
                continue
            descriptor = dict(item)
            descriptor["id"] = model_id
            self.models[model_id] = descriptor
            candidates = {
                model_id.casefold(),
                _lookup_key(model_id),
                model_id.rsplit("/", 1)[-1].casefold(),
                _lookup_key(model_id.rsplit("/", 1)[-1]),
            }
            canonical_slug = canonical_model_id(str(item.get("canonical_slug") or ""))
            if canonical_slug:
                candidates.update(
                    {
                        canonical_slug.casefold(),
                        _lookup_key(canonical_slug),
                        canonical_slug.rsplit("/", 1)[-1].casefold(),
                        _lookup_key(canonical_slug.rsplit("/", 1)[-1]),
                    }
                )
            for alias in candidates:
                if alias:
                    aliases.setdefault(alias, []).append(model_id)
        if not self.models:
            raise ValueError("catalog contains no usable model descriptors")
        self.aliases = {key: sorted(set(values)) for key, values in aliases.items()}

    def _select(self, requested: str, candidates: Sequence[str]) -> Optional[str]:
        unique = list(dict.fromkeys(candidates))
        if not unique:
            return None
        requested_lower = requested.casefold()
        exact = [item for item in unique if item.casefold() == requested_lower]
        if len(exact) == 1:
            return exact[0]

        # Normal routes should not silently resolve to batch-only or moving
        # tilde aliases.  Keep those only when explicitly requested.
        filtered = unique
        if not requested_lower.endswith(":batch"):
            non_batch = [
                item for item in filtered if not item.casefold().endswith(":batch")
            ]
            if non_batch:
                filtered = non_batch
        if not requested_lower.startswith("~"):
            non_tilde = [item for item in filtered if not item.startswith("~")]
            if non_tilde:
                filtered = non_tilde

        if "/" in requested:
            requested_provider = requested.split("/", 1)[0].casefold()
            same_provider = [
                item
                for item in filtered
                if _provider_from_model_id(item) == requested_provider
            ]
            if same_provider:
                filtered = same_provider
        return filtered[0] if len(filtered) == 1 else None

    def resolve(self, requested_model_id: str) -> Tuple[str, Dict[str, Any], bool]:
        requested = canonical_model_id(requested_model_id)
        if not requested:
            requested = "unknown"
        if requested in self.models:
            return requested, self.models[requested], True
        exact_casefold = [
            model_id
            for model_id in self.models
            if model_id.casefold() == requested.casefold()
        ]
        selected = self._select(requested, exact_casefold)
        if selected:
            return selected, self.models[selected], True

        keys = (
            requested.casefold(),
            _lookup_key(requested),
            requested.rsplit("/", 1)[-1].casefold(),
            _lookup_key(requested.rsplit("/", 1)[-1]),
        )
        for key in keys:
            selected = self._select(requested, self.aliases.get(key, []))
            if selected:
                return selected, self.models[selected], True
        # Unknown/local/future IDs still receive provider and slug-segment
        # features; absent catalog prices/capabilities remain explicitly absent.
        return requested, {"id": requested}, False


@dataclass
class Event:
    event_id: int
    instance_id: str
    ts_epoch: float
    category: str
    title: str
    detail: str
    metadata: Dict[str, Any]
    input_tokens: int
    output_tokens: int
    cache_read_tokens: int
    cache_write_tokens: int
    est_cost_usd: float


@dataclass
class Segment:
    instance_id: str
    prompt_event_id: int
    prompt_epoch: float
    prompt: str
    fallback_model: str
    events: List[Event] = field(default_factory=list)


@dataclass
class OutcomeRow:
    prompt_hash: str
    row_id: str
    instance_id: str
    prompt_event_id: int
    event_epoch: float
    model_id: str
    observed_model_id: str
    provider_id: str
    catalog_match: bool
    prompt: str
    descriptor: Dict[str, Any]
    labels: np.ndarray
    label_mask: np.ndarray
    precomputed_task_features: Optional[np.ndarray] = None
    source_kind: str = "baseline"
    label_provenance: Tuple[str, ...] = ()
    label_source_id: str = ""
    label_confidence: float = 1.0
    censored: bool = False


def _table_columns(connection: sqlite3.Connection, table: str) -> set:
    return {str(row[1]) for row in connection.execute("PRAGMA table_info(%s)" % table)}


def _column_expr(columns: set, name: str, fallback: str) -> str:
    return name if name in columns else fallback


def _open_readonly_database(path: Path) -> sqlite3.Connection:
    uri = path.resolve().as_uri() + "?mode=ro"
    connection = sqlite3.connect(uri, uri=True)
    # Some historical tool-result details contain arbitrary command bytes in a
    # SQLite TEXT column.  Surrogate escape keeps the database readable and
    # makes exact prompt hashing round-trip the original bytes.
    connection.text_factory = lambda raw: raw.decode("utf-8", "surrogateescape")
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA query_only=ON")
    connection.execute("BEGIN")
    return connection


def _load_instance_models(connection: sqlite3.Connection) -> Dict[str, str]:
    tables = {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }
    if "instances" not in tables:
        return {}
    columns = _table_columns(connection, "instances")
    if "instance_id" not in columns:
        return {}
    model_expr = "model" if "model" in columns else "''"
    query = (
        "SELECT instance_id, %s AS model FROM instances ORDER BY instance_id"
        % model_expr
    )
    return {
        str(row["instance_id"]): str(row["model"] or "")
        for row in connection.execute(query)
    }


def _iter_events(
    connection: sqlite3.Connection, max_event_id: Optional[int] = None
) -> Iterable[Event]:
    columns = _table_columns(connection, "events")
    required = {"instance_id", "category", "title"}
    missing = sorted(required - columns)
    if missing:
        raise ValueError(
            "events table is missing required columns: %s" % ", ".join(missing)
        )
    event_id_expr = "id" if "id" in columns else "rowid"
    ts_expr = (
        "ts_epoch" if "ts_epoch" in columns else "CAST(%s AS REAL)" % event_id_expr
    )
    expressions = {
        "event_id": event_id_expr,
        "instance_id": "instance_id",
        "ts_epoch": ts_expr,
        "category": "category",
        "title": "title",
        "detail": _column_expr(columns, "detail", "''"),
        "metadata_json": _column_expr(columns, "metadata_json", "NULL"),
        "input_tokens": _column_expr(columns, "input_tokens", "0"),
        "output_tokens": _column_expr(columns, "output_tokens", "0"),
        "cache_read_tokens": _column_expr(columns, "cache_read_tokens", "0"),
        "cache_write_tokens": _column_expr(columns, "cache_write_tokens", "0"),
        "est_cost_usd": _column_expr(columns, "est_cost_usd", "0.0"),
    }
    projection = ", ".join(
        "%s AS %s" % (value, key) for key, value in expressions.items()
    )
    where = " WHERE %s <= ?" % event_id_expr if max_event_id is not None else ""
    query = "SELECT %s FROM events%s ORDER BY instance_id, %s, %s" % (
        projection,
        where,
        ts_expr,
        event_id_expr,
    )
    parameters: Tuple[Any, ...] = (
        (int(max_event_id),) if max_event_id is not None else ()
    )
    for row in connection.execute(query, parameters):
        yield Event(
            event_id=int(row["event_id"]),
            instance_id=str(row["instance_id"] or ""),
            ts_epoch=_finite_float(row["ts_epoch"]),
            category=str(row["category"] or ""),
            title=str(row["title"] or ""),
            detail=str(row["detail"] or ""),
            metadata=_safe_json_object(row["metadata_json"]),
            input_tokens=int(row["input_tokens"] or 0),
            output_tokens=int(row["output_tokens"] or 0),
            cache_read_tokens=int(row["cache_read_tokens"] or 0),
            cache_write_tokens=int(row["cache_write_tokens"] or 0),
            est_cost_usd=max(0.0, _finite_float(row["est_cost_usd"])),
        )


def _is_prompt(event: Event) -> bool:
    return (
        event.category.casefold() == "user" and event.title.casefold() in PROMPT_TITLES
    )


def _is_provider_failure(event: Event) -> bool:
    category = event.category.casefold()
    title = event.title.casefold()
    if title in PROVIDER_FAILURE_TITLES:
        return True
    if category in {"provider_error", "network_error", "auth_error"}:
        return True
    return category == "error" and any(
        token in title
        for token in ("provider", "stream", "request", "auth", "rate_limit")
    )


def _segment_model(segment: Segment) -> str:
    for event in segment.events:
        if event.category.casefold() == "turn":
            model = str(event.metadata.get("model") or "").strip()
            if model:
                return model
    return segment.fallback_model.strip()


def _outcome_row(segment: Segment, catalog: CatalogIndex) -> Optional[OutcomeRow]:
    observed_model = _segment_model(segment)
    if not observed_model:
        return None
    resolved_model, descriptor, matched = catalog.resolve(observed_model)
    done_events = [
        event
        for event in segment.events
        if event.category.casefold() == "turn" and event.title.casefold() == "turn_done"
    ]
    provider_failures = [
        event for event in segment.events if _is_provider_failure(event)
    ]
    terminal_events = done_events + provider_failures
    terminal_observed = bool(terminal_events)
    tool_error = any(
        event.category.casefold() == "tool_error" for event in segment.events
    )
    total_cost = sum(event.est_cost_usd for event in segment.events)
    cost_observed = total_cost > 0.0 or any(
        event.category.casefold() == "turn"
        and (
            "billing" in event.metadata
            or event.input_tokens > 0
            or event.output_tokens > 0
            or event.cache_read_tokens > 0
            or event.cache_write_tokens > 0
        )
        for event in segment.events
    )
    if terminal_events:
        terminal_epoch = max(event.ts_epoch for event in terminal_events)
        latency = max(0.0, terminal_epoch - segment.prompt_epoch)
    else:
        latency = 0.0

    labels = np.asarray(
        [
            float(bool(done_events)),
            float(bool(provider_failures)),
            float(tool_error),
            total_cost,
            latency,
        ],
        dtype=np.float32,
    )
    label_mask = np.asarray(
        [
            terminal_observed,
            terminal_observed,
            terminal_observed,
            cost_observed,
            terminal_observed,
        ],
        dtype=np.uint8,
    )
    p_hash = prompt_digest(segment.prompt)
    row_id = _sha256_bytes(
        (segment.instance_id + "\x1f" + str(segment.prompt_event_id)).encode(
            "utf-8", "surrogatepass"
        )
    )
    provider_id = _provider_from_model_id(resolved_model)
    return OutcomeRow(
        prompt_hash=p_hash,
        row_id=row_id,
        instance_id=segment.instance_id,
        prompt_event_id=segment.prompt_event_id,
        event_epoch=segment.prompt_epoch,
        model_id=resolved_model,
        observed_model_id=canonical_model_id(observed_model),
        provider_id=provider_id,
        catalog_match=matched,
        prompt=segment.prompt,
        descriptor=descriptor,
        labels=labels,
        label_mask=label_mask,
    )


def collect_rows(
    db_path: Path,
    catalog: CatalogIndex,
    min_prompt_chars: int = 1,
    include_incomplete: bool = False,
    max_event_id: Optional[int] = None,
) -> Tuple[List[OutcomeRow], Dict[str, int]]:
    counters = {
        "prompt_events": 0,
        "empty_or_short_prompts": 0,
        "missing_model": 0,
        "incomplete_rows": 0,
    }
    rows: List[OutcomeRow] = []
    connection = _open_readonly_database(db_path)
    try:
        event_columns = _table_columns(connection, "events")
        event_id_expr = "id" if "id" in event_columns else "rowid"
        observed_high_water = int(
            connection.execute(
                "SELECT COALESCE(MAX(%s), 0) FROM events" % event_id_expr
            ).fetchone()[0]
        )
        effective_high_water = (
            observed_high_water
            if max_event_id is None
            else min(max(int(max_event_id), 0), observed_high_water)
        )
        counters["source_event_max_id"] = effective_high_water
        instance_models = _load_instance_models(connection)
        current: Optional[Segment] = None
        current_instance = ""

        def finish(segment: Optional[Segment]) -> None:
            if segment is None:
                return
            row = _outcome_row(segment, catalog)
            if row is None:
                counters["missing_model"] += 1
                return
            if not bool(row.label_mask[0]):
                counters["incomplete_rows"] += 1
                if not include_incomplete:
                    return
            rows.append(row)

        for event in _iter_events(connection, effective_high_water):
            if event.instance_id != current_instance:
                finish(current)
                current = None
                current_instance = event.instance_id
            if _is_prompt(event):
                finish(current)
                counters["prompt_events"] += 1
                if len(event.detail) < min_prompt_chars or not event.detail.strip():
                    counters["empty_or_short_prompts"] += 1
                    current = None
                    continue
                current = Segment(
                    instance_id=event.instance_id,
                    prompt_event_id=event.event_id,
                    prompt_epoch=event.ts_epoch,
                    prompt=event.detail,
                    fallback_model=instance_models.get(event.instance_id, ""),
                )
            elif current is not None:
                current.events.append(event)
        finish(current)
    finally:
        connection.rollback()
        connection.close()

    rows.sort(
        key=lambda row: (
            row.prompt_hash,
            row.model_id,
            row.event_epoch,
            row.row_id,
        )
    )
    return rows, counters


def _feedback_schema_columns(
    connection: sqlite3.Connection, table: str
) -> Dict[str, sqlite3.Row]:
    return {
        str(row["name"]): row
        for row in connection.execute("PRAGMA table_info(%s)" % table)
    }


def _validate_feedback_schema(connection: sqlite3.Connection) -> int:
    required_columns = {
        "router_schema": {"version"},
        "route_decisions": {
            "request_id",
            "decided_at",
            "request_hash",
            "catalog_snapshot_id",
            "chosen_model",
        },
        "route_request_features": {
            "request_id",
            "task_feature_version",
            "task_dim",
            "encoding",
            "feature_sha256",
            "task_features",
        },
        "route_outcomes": {
            "request_id",
            "completed_at",
            "intended_model",
            "actual_model",
            "actual_provider",
            "http_success",
            "task_success",
            "provider_failure",
            "tool_valid",
            "cost_usd",
            "e2e_ms",
            "label_source",
            "label_confidence",
            "censored",
        },
    }
    tables = {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }
    missing_tables = sorted(set(required_columns) - tables)
    if missing_tables:
        raise ValueError(
            "feedback schema is missing tables: %s" % ", ".join(missing_tables)
        )
    for table, required in required_columns.items():
        columns = _feedback_schema_columns(connection, table)
        missing = sorted(required - set(columns))
        if missing:
            raise ValueError(
                "feedback table %s is missing columns: %s" % (table, ", ".join(missing))
            )
    for table in ("route_decisions", "route_request_features", "route_outcomes"):
        columns = _feedback_schema_columns(connection, table)
        if int(columns["request_id"]["pk"] or 0) != 1:
            raise ValueError(
                "feedback table %s must have request_id as its primary key" % table
            )

    versions = [
        int(row[0])
        for row in connection.execute(
            "SELECT version FROM router_schema ORDER BY version"
        )
    ]
    installed_version = max(versions, default=0)
    if installed_version not in FEEDBACK_SUPPORTED_SCHEMA_VERSIONS or versions != list(
        range(1, installed_version + 1)
    ):
        raise ValueError(
            "feedback schema mismatch: installed versions %r, supported versions %r"
            % (versions, FEEDBACK_SUPPORTED_SCHEMA_VERSIONS)
        )
    if installed_version >= 3:
        revision_columns = _feedback_schema_columns(
            connection, "route_outcome_revisions"
        )
        required_revision_columns = {
            "revision_id",
            "request_id",
            "revision_number",
            "recorded_at",
            "supersedes_revision_id",
            "completed_at",
            "intended_model",
            "actual_model",
            "actual_provider",
            "http_success",
            "task_success",
            "provider_failure",
            "tool_valid",
            "cost_usd",
            "e2e_ms",
            "label_source",
            "label_confidence",
            "censored",
        }
        missing = sorted(required_revision_columns - set(revision_columns))
        if missing:
            raise ValueError(
                "feedback table route_outcome_revisions is missing columns: %s"
                % ", ".join(missing)
            )
        if int(revision_columns["revision_id"]["pk"] or 0) != 1:
            raise ValueError(
                "feedback route_outcome_revisions.revision_id must be the primary key"
            )
        triggers = {
            str(row[0])
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='trigger'"
            )
        }
        required_triggers = {
            "route_outcome_revisions_no_update",
            "route_outcome_revisions_no_delete",
        }
        if not required_triggers.issubset(triggers):
            raise ValueError("feedback revision table is missing append-only triggers")
    quick_check = connection.execute("PRAGMA quick_check").fetchone()
    if quick_check is None or str(quick_check[0]).casefold() != "ok":
        raise ValueError("feedback database failed SQLite quick_check")
    foreign_key_failure = connection.execute("PRAGMA foreign_key_check").fetchone()
    if foreign_key_failure is not None:
        raise ValueError("feedback database failed SQLite foreign_key_check")
    return installed_version


def _feedback_timestamp_epoch(value: Any, field: str) -> float:
    if not isinstance(value, str) or not value.strip():
        raise ValueError("feedback %s must be a timezone-aware timestamp" % field)
    candidate = value.strip()
    if candidate.endswith("Z"):
        candidate = candidate[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(candidate)
    except ValueError as exc:
        raise ValueError(
            "feedback %s is not a valid ISO-8601 timestamp" % field
        ) from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ValueError("feedback %s must include a timezone" % field)
    epoch = parsed.astimezone(timezone.utc).timestamp()
    if not math.isfinite(epoch):
        raise ValueError("feedback %s is outside the supported timestamp range" % field)
    return epoch


def _feedback_bool(value: Any, field: str) -> Optional[bool]:
    if value is None:
        return None
    if isinstance(value, bool) or value in (0, 1):
        return bool(value)
    raise ValueError("feedback %s must be null, 0, or 1" % field)


def _feedback_nonnegative(value: Any, field: str) -> Optional[float]:
    if value is None:
        return None
    if isinstance(value, bool):
        raise ValueError("feedback %s must be numeric or null" % field)
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError("feedback %s must be numeric or null" % field) from exc
    if not math.isfinite(number) or number < 0.0:
        raise ValueError("feedback %s must be finite and non-negative" % field)
    return number


def _feedback_opaque_id(namespace: str, value: str) -> str:
    return _sha256_bytes(
        ("chimera-feedback-opaque-v1\x1f" + namespace + "\x1f" + value).encode(
            "utf-8", "surrogatepass"
        )
    )


def _feedback_snapshot_update(digest: "hashlib._Hash", name: str, value: Any) -> None:
    if value is None:
        payload = b""
        kind = b"n"
    elif isinstance(value, (bytes, bytearray, memoryview)):
        payload = bytes(value)
        kind = b"b"
    else:
        payload = str(value).encode("utf-8", "surrogatepass")
        kind = b"s"
    name_bytes = name.encode("ascii")
    digest.update(len(name_bytes).to_bytes(4, "big"))
    digest.update(name_bytes)
    digest.update(kind)
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)


def collect_feedback_rows(
    db_path: Path,
    catalog: CatalogIndex,
    task_dim: int = DEFAULT_TASK_DIM,
    max_revision_id: Optional[int] = None,
    max_outcome_rowid: Optional[int] = None,
    expected_snapshot_sha256: Optional[str] = None,
) -> Tuple[List[OutcomeRow], Dict[str, int], Dict[str, Any]]:
    """Load one pinned logical snapshot of redacted feedback.

    Schema v3 uses the immutable revision high-water.  Schema v2 compatibility
    uses a mutable SQLite rowid selection boundary plus a mandatory-on-replay
    logical SHA guard, because v2 cannot reconstruct superseded outcomes.
    """

    if task_dim != DEFAULT_TASK_DIM:
        raise ValueError(
            "feedback ingestion requires the canonical %d-D task vector"
            % DEFAULT_TASK_DIM
        )
    if max_outcome_rowid is not None and max_outcome_rowid < 0:
        raise ValueError("feedback outcome rowid high-water must be non-negative")
    if max_revision_id is not None and max_revision_id < 0:
        raise ValueError("feedback revision high-water must be non-negative")
    if expected_snapshot_sha256 is not None and not _SHA256_RE.fullmatch(
        expected_snapshot_sha256
    ):
        raise ValueError(
            "expected feedback snapshot SHA-256 must be 64 lowercase hex characters"
        )

    counters: Dict[str, int] = {
        "feedback_outcomes_selected": 0,
        "feedback_rows_imported": 0,
        "feedback_duplicate_request_ids": 0,
        "feedback_rows_without_mapped_labels": 0,
        "feedback_censored_rows": 0,
        "feedback_action_mismatch_rows": 0,
        "feedback_task_success_observations_unmapped": 0,
        "feedback_completion_success_observed": 0,
        "feedback_provider_failure_observed": 0,
        "feedback_tool_error_observed": 0,
        "feedback_cost_usd_observed": 0,
        "feedback_latency_s_observed": 0,
    }
    rows: List[OutcomeRow] = []
    connection = _open_readonly_database(db_path)
    try:
        installed_schema_version = _validate_feedback_schema(connection)
        if installed_schema_version >= 3:
            outcome_source_table = "route_outcome_revisions"
            if max_outcome_rowid is not None:
                raise ValueError(
                    "--max-feedback-outcome-rowid is only valid for schema-v2 compatibility"
                )
            observed_high_water = int(
                connection.execute(
                    "SELECT COALESCE(MAX(revision_id), 0) FROM route_outcome_revisions"
                ).fetchone()[0]
            )
            if max_revision_id is not None and max_revision_id > observed_high_water:
                raise ValueError(
                    "feedback revision high-water %d exceeds observed maximum %d"
                    % (max_revision_id, observed_high_water)
                )
            effective_high_water = (
                observed_high_water if max_revision_id is None else int(max_revision_id)
            )
            counters["source_feedback_max_revision_id"] = effective_high_water
            query = """
                WITH latest AS (
                    SELECT request_id, MAX(revision_id) AS revision_id
                    FROM route_outcome_revisions
                    WHERE revision_id <= ?
                    GROUP BY request_id
                )
                SELECT
                    o.revision_id AS source_record_id,
                    o.revision_number AS source_revision_number,
                    o.recorded_at AS source_recorded_at,
                    o.supersedes_revision_id AS source_supersedes_revision_id,
                    o.request_id AS outcome_request_id,
                    d.request_id AS decision_request_id,
                    f.request_id AS feature_request_id,
                    d.request_hash AS request_hash,
                    d.decided_at AS decided_at,
                    d.catalog_snapshot_id AS catalog_snapshot_id,
                    d.chosen_model AS chosen_model,
                    f.task_feature_version AS task_feature_version,
                    f.task_dim AS task_dim,
                    f.encoding AS feature_encoding,
                    f.feature_sha256 AS feature_sha256,
                    f.task_features AS task_features_blob,
                    o.completed_at AS completed_at,
                    o.intended_model AS intended_model,
                    o.actual_model AS actual_model,
                    o.actual_provider AS actual_provider,
                    o.http_success AS http_success,
                    o.task_success AS task_success,
                    o.provider_failure AS provider_failure,
                    o.tool_valid AS tool_valid,
                    o.cost_usd AS cost_usd,
                    o.e2e_ms AS e2e_ms,
                    o.label_source AS label_source,
                    o.label_confidence AS label_confidence,
                    o.censored AS censored
                FROM latest
                JOIN route_outcome_revisions AS o
                  ON o.request_id = latest.request_id
                 AND o.revision_id = latest.revision_id
                LEFT JOIN route_decisions AS d ON d.request_id = o.request_id
                LEFT JOIN route_request_features AS f ON f.request_id = o.request_id
                ORDER BY o.revision_id, o.request_id COLLATE BINARY
            """
        else:
            outcome_source_table = "route_outcomes"
            if max_revision_id is not None:
                raise ValueError(
                    "--max-feedback-revision-id requires feedback schema v3"
                )
            observed_high_water = int(
                connection.execute(
                    "SELECT COALESCE(MAX(rowid), 0) FROM route_outcomes"
                ).fetchone()[0]
            )
            if (
                max_outcome_rowid is not None
                and int(max_outcome_rowid) > observed_high_water
            ):
                raise ValueError(
                    "feedback outcome rowid high-water %d exceeds observed maximum %d"
                    % (int(max_outcome_rowid), observed_high_water)
                )
            effective_high_water = (
                observed_high_water
                if max_outcome_rowid is None
                else int(max_outcome_rowid)
            )
            counters["source_feedback_outcome_max_rowid"] = effective_high_water
            query = """
            SELECT
                o.rowid AS source_record_id,
                NULL AS source_revision_number,
                NULL AS source_recorded_at,
                NULL AS source_supersedes_revision_id,
                o.request_id AS outcome_request_id,
                d.request_id AS decision_request_id,
                f.request_id AS feature_request_id,
                d.request_hash AS request_hash,
                d.decided_at AS decided_at,
                d.catalog_snapshot_id AS catalog_snapshot_id,
                d.chosen_model AS chosen_model,
                f.task_feature_version AS task_feature_version,
                f.task_dim AS task_dim,
                f.encoding AS feature_encoding,
                f.feature_sha256 AS feature_sha256,
                f.task_features AS task_features_blob,
                o.completed_at AS completed_at,
                o.intended_model AS intended_model,
                o.actual_model AS actual_model,
                o.actual_provider AS actual_provider,
                o.http_success AS http_success,
                o.task_success AS task_success,
                o.provider_failure AS provider_failure,
                o.tool_valid AS tool_valid,
                o.cost_usd AS cost_usd,
                o.e2e_ms AS e2e_ms,
                o.label_source AS label_source,
                o.label_confidence AS label_confidence,
                o.censored AS censored
            FROM route_outcomes AS o
            LEFT JOIN route_decisions AS d ON d.request_id = o.request_id
            LEFT JOIN route_request_features AS f ON f.request_id = o.request_id
            WHERE o.rowid <= ?
            ORDER BY o.rowid, o.request_id COLLATE BINARY
            """
        selected = list(connection.execute(query, (effective_high_water,)))
        counters["feedback_outcomes_selected"] = len(selected)
        snapshot_digest = hashlib.sha256()
        snapshot_digest.update(b"chimera-feedback-logical-snapshot-v1\x00")
        seen_request_ids = set()
        max_completed: Optional[Tuple[float, str]] = None
        catalog_snapshot_ids = set()
        snapshot_fields = (
            "source_record_id",
            "source_revision_number",
            "source_recorded_at",
            "source_supersedes_revision_id",
            "outcome_request_id",
            "decision_request_id",
            "feature_request_id",
            "request_hash",
            "decided_at",
            "catalog_snapshot_id",
            "chosen_model",
            "task_feature_version",
            "task_dim",
            "feature_encoding",
            "feature_sha256",
            "task_features_blob",
            "completed_at",
            "intended_model",
            "actual_model",
            "actual_provider",
            "http_success",
            "task_success",
            "provider_failure",
            "tool_valid",
            "cost_usd",
            "e2e_ms",
            "label_source",
            "label_confidence",
            "censored",
        )
        for selected_row in selected:
            for field_name in snapshot_fields:
                _feedback_snapshot_update(
                    snapshot_digest, field_name, selected_row[field_name]
                )

            source_record_id = int(selected_row["source_record_id"])
            if source_record_id <= 0 or source_record_id > effective_high_water:
                raise ValueError("feedback source record is outside its high-water")
            if installed_schema_version >= 3:
                revision_number = int(selected_row["source_revision_number"] or 0)
                if revision_number <= 0:
                    raise ValueError("feedback revision_number must be positive")
                _feedback_timestamp_epoch(
                    selected_row["source_recorded_at"], "recorded_at"
                )
                supersedes = selected_row["source_supersedes_revision_id"]
                if supersedes is not None and (
                    int(supersedes) <= 0 or int(supersedes) >= source_record_id
                ):
                    raise ValueError("feedback supersedes_revision_id is invalid")

            request_id = str(selected_row["outcome_request_id"] or "")
            if not request_id:
                raise ValueError("feedback outcome has an empty request_id")
            if request_id in seen_request_ids:
                counters["feedback_duplicate_request_ids"] += 1
                continue
            seen_request_ids.add(request_id)
            if str(selected_row["decision_request_id"] or "") != request_id:
                raise ValueError(
                    "feedback outcome %s has no matching route decision"
                    % _feedback_opaque_id("request", request_id)
                )
            if str(selected_row["feature_request_id"] or "") != request_id:
                raise ValueError(
                    "feedback outcome %s has no matching task feature row"
                    % _feedback_opaque_id("request", request_id)
                )

            request_hash = str(selected_row["request_hash"] or "")
            if not _SHA256_RE.fullmatch(request_hash):
                raise ValueError("feedback request_hash is not a lowercase SHA-256")
            if selected_row["task_feature_version"] != TASK_FEATURE_VERSION:
                raise ValueError(
                    "feedback task feature version mismatch: %r, expected %r"
                    % (selected_row["task_feature_version"], TASK_FEATURE_VERSION)
                )
            stored_dim = int(selected_row["task_dim"] or 0)
            if stored_dim != task_dim:
                raise ValueError(
                    "feedback task feature dimension mismatch: %d, expected %d"
                    % (stored_dim, task_dim)
                )
            if selected_row["feature_encoding"] != FEEDBACK_FEATURE_ENCODING:
                raise ValueError(
                    "feedback task feature encoding mismatch: %r"
                    % selected_row["feature_encoding"]
                )
            blob_value = selected_row["task_features_blob"]
            if not isinstance(blob_value, (bytes, bytearray, memoryview)):
                raise ValueError("feedback task feature payload is not a BLOB")
            feature_payload = bytes(blob_value)
            if len(feature_payload) != task_dim * np.dtype("<f4").itemsize:
                raise ValueError(
                    "feedback task feature payload has %d bytes, expected %d"
                    % (len(feature_payload), task_dim * 4)
                )
            stored_hash = str(selected_row["feature_sha256"] or "")
            computed_hash = _sha256_bytes(feature_payload)
            if not _SHA256_RE.fullmatch(stored_hash) or not hmac.compare_digest(
                stored_hash, computed_hash
            ):
                raise ValueError("feedback task feature SHA-256 mismatch")
            task_vector = np.frombuffer(feature_payload, dtype="<f4").astype(
                np.float32, copy=True
            )
            if task_vector.shape != (task_dim,) or not np.all(np.isfinite(task_vector)):
                raise ValueError(
                    "feedback task feature vector is non-finite or malformed"
                )

            completed_at = str(selected_row["completed_at"] or "")
            completed_epoch = _feedback_timestamp_epoch(completed_at, "completed_at")
            _feedback_timestamp_epoch(selected_row["decided_at"], "decided_at")
            completed_key = (completed_epoch, completed_at)
            if max_completed is None or completed_key > max_completed:
                max_completed = completed_key

            chosen_model = canonical_model_id(str(selected_row["chosen_model"] or ""))
            intended_model = canonical_model_id(
                str(selected_row["intended_model"] or "")
            )
            actual_model = canonical_model_id(str(selected_row["actual_model"] or ""))
            observed_model = actual_model or intended_model or chosen_model
            if not chosen_model or not intended_model or not observed_model:
                raise ValueError("feedback outcome contains an empty model identifier")
            if observed_model != chosen_model:
                counters["feedback_action_mismatch_rows"] += 1
            resolved_model, descriptor, matched = catalog.resolve(observed_model)

            labels = np.zeros(len(LABEL_NAMES), dtype=np.float32)
            label_mask = np.zeros(len(LABEL_NAMES), dtype=np.uint8)
            provenance = [""] * len(LABEL_NAMES)
            http_success = _feedback_bool(
                selected_row["http_success"], "route_outcomes.http_success"
            )
            if http_success is not None:
                labels[0] = np.float32(http_success)
                label_mask[0] = np.uint8(1)
                provenance[0] = "feedback.%s.http_success" % outcome_source_table
                counters["feedback_completion_success_observed"] += 1
            task_success = _feedback_bool(
                selected_row["task_success"], "route_outcomes.task_success"
            )
            if task_success is not None:
                counters["feedback_task_success_observations_unmapped"] += 1
            provider_failure = _feedback_bool(
                selected_row["provider_failure"],
                "route_outcomes.provider_failure",
            )
            if provider_failure is not None:
                labels[1] = np.float32(provider_failure)
                label_mask[1] = np.uint8(1)
                provenance[1] = "feedback.%s.provider_failure" % outcome_source_table
                counters["feedback_provider_failure_observed"] += 1
            tool_valid = _feedback_bool(
                selected_row["tool_valid"], "route_outcomes.tool_valid"
            )
            if tool_valid is not None:
                labels[2] = np.float32(not tool_valid)
                label_mask[2] = np.uint8(1)
                provenance[2] = "feedback.derived:not(%s.tool_valid)" % (
                    outcome_source_table,
                )
                counters["feedback_tool_error_observed"] += 1
            cost_usd = _feedback_nonnegative(
                selected_row["cost_usd"], "route_outcomes.cost_usd"
            )
            if cost_usd is not None:
                labels[3] = np.float32(cost_usd)
                label_mask[3] = np.uint8(1)
                provenance[3] = "feedback.%s.cost_usd" % outcome_source_table
                counters["feedback_cost_usd_observed"] += 1
            e2e_ms = _feedback_nonnegative(
                selected_row["e2e_ms"], "route_outcomes.e2e_ms"
            )
            if e2e_ms is not None:
                labels[4] = np.float32(e2e_ms / 1000.0)
                label_mask[4] = np.uint8(1)
                provenance[4] = "feedback.%s.e2e_ms/1000" % outcome_source_table
                counters["feedback_latency_s_observed"] += 1

            confidence = _feedback_nonnegative(
                selected_row["label_confidence"],
                "route_outcomes.label_confidence",
            )
            if confidence is None or confidence > 1.0:
                raise ValueError("feedback label_confidence must be in [0, 1]")
            censored = _feedback_bool(
                selected_row["censored"], "route_outcomes.censored"
            )
            if censored:
                counters["feedback_censored_rows"] += 1
            if not np.any(label_mask):
                counters["feedback_rows_without_mapped_labels"] += 1
                continue
            label_source = str(selected_row["label_source"] or "")
            if not label_source:
                raise ValueError("feedback label_source cannot be empty")
            opaque_request = _feedback_opaque_id("request", request_id)
            feedback_group = _sha256_bytes(
                ("chimera-feedback-request-group-v1\x1f" + request_hash).encode("ascii")
            )
            snapshot_id = selected_row["catalog_snapshot_id"]
            if snapshot_id is not None:
                catalog_snapshot_ids.add(
                    _feedback_opaque_id("catalog-snapshot", str(snapshot_id))
                )
            rows.append(
                OutcomeRow(
                    prompt_hash=feedback_group,
                    row_id=_feedback_opaque_id("row", request_id),
                    instance_id=opaque_request,
                    prompt_event_id=-source_record_id,
                    event_epoch=completed_epoch,
                    model_id=resolved_model,
                    observed_model_id=observed_model,
                    provider_id=_provider_from_model_id(resolved_model),
                    catalog_match=matched,
                    prompt="",
                    descriptor=descriptor,
                    labels=labels,
                    label_mask=label_mask,
                    precomputed_task_features=task_vector,
                    source_kind="feedback_v%d" % installed_schema_version,
                    label_provenance=tuple(provenance),
                    label_source_id=_feedback_opaque_id("label-source", label_source),
                    label_confidence=float(confidence),
                    censored=bool(censored),
                )
            )

        logical_sha256 = snapshot_digest.hexdigest()
        if expected_snapshot_sha256 is not None and not hmac.compare_digest(
            expected_snapshot_sha256, logical_sha256
        ):
            raise ValueError(
                "feedback logical snapshot SHA-256 mismatch: got %s, expected %s"
                % (logical_sha256, expected_snapshot_sha256)
            )
        counters["feedback_rows_imported"] = len(rows)
        snapshot: Dict[str, Any] = {
            "import_version": FEEDBACK_IMPORT_VERSION,
            "schema_version": installed_schema_version,
            "database_filename": db_path.name,
            "outcome_source_table": outcome_source_table,
            "included_max_completed_at": (
                None if max_completed is None else max_completed[1]
            ),
            "logical_snapshot_sha256": logical_sha256,
            "task_feature_version": TASK_FEATURE_VERSION,
            "task_dim": task_dim,
            "feature_encoding": FEEDBACK_FEATURE_ENCODING,
            "catalog_snapshot_sha256": catalog.snapshot_sha256,
            "decision_catalog_snapshot_ids": sorted(catalog_snapshot_ids),
        }
        if installed_schema_version >= 3:
            snapshot.update(
                {
                    "included_revision_id": effective_high_water,
                    "high_water_semantics": "inclusive immutable append-only revision_id",
                }
            )
        else:
            snapshot.update(
                {
                    "included_outcome_max_rowid": effective_high_water,
                    "high_water_semantics": "mutable compatibility boundary; logical snapshot SHA is required for exact replay",
                }
            )
    finally:
        connection.rollback()
        connection.close()

    rows.sort(
        key=lambda row: (
            row.prompt_hash,
            row.model_id,
            row.event_epoch,
            row.row_id,
        )
    )
    return rows, counters, snapshot


def _unicode_array(values: Sequence[str]) -> np.ndarray:
    width = max((len(value) for value in values), default=1)
    return np.asarray(values, dtype="<U%d" % max(1, width))


def _cold_start_arrays(
    model_ids: Sequence[str], provider_ids: Sequence[str], prefix: str = ""
) -> Dict[str, np.ndarray]:
    if len(model_ids) != len(provider_ids):
        raise ValueError("model/provider holdout inputs must have equal lengths")
    families = [model_family_id(model_id) for model_id in model_ids]
    return {
        prefix + "family_ids": _unicode_array(families),
        prefix + "model_holdout_ids": _unicode_array(
            [_holdout_id("model", model_id) for model_id in model_ids]
        ),
        prefix + "provider_holdout_ids": _unicode_array(
            [_holdout_id("provider", provider_id) for provider_id in provider_ids]
        ),
        prefix + "family_holdout_ids": _unicode_array(
            [_holdout_id("family", family) for family in families]
        ),
        prefix + "model_holdout_fold": np.asarray(
            [_stable_fold(model_id) for model_id in model_ids], dtype=np.uint8
        ),
        prefix + "provider_holdout_fold": np.asarray(
            [_stable_fold(provider_id) for provider_id in provider_ids], dtype=np.uint8
        ),
        prefix + "family_holdout_fold": np.asarray(
            [_stable_fold(family) for family in families], dtype=np.uint8
        ),
    }


def _pair_preference(
    left_labels: np.ndarray,
    right_labels: np.ndarray,
    common_mask: np.ndarray,
) -> float:
    # Lexicographic policy: completing is more important than failure modes;
    # cost and latency only break otherwise equal outcomes.  Pair deltas/masks
    # are also emitted so trainers can use a different utility policy.
    comparisons = (
        (0, 1.0, 0.0),
        (1, -1.0, 0.0),
        (2, -1.0, 0.0),
        (3, -1.0, 1e-12),
        (4, -1.0, 1e-6),
    )
    for index, direction, tolerance in comparisons:
        if not common_mask[index]:
            continue
        delta = float(left_labels[index] - right_labels[index]) * direction
        if delta > tolerance:
            return 1.0
        if delta < -tolerance:
            return 0.0
    return 0.5


def build_pairs(
    rows: Sequence[OutcomeRow],
    group_ids: np.ndarray,
    max_pairs_per_group: int = 0,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    by_group: MutableMapping[int, List[int]] = {}
    for index, group_id in enumerate(group_ids.tolist()):
        by_group.setdefault(int(group_id), []).append(index)

    pair_records: List[Tuple[int, int, str]] = []
    for group_id in sorted(by_group):
        indices = by_group[group_id]
        group_pairs: List[Tuple[int, int, str]] = []
        for offset, left in enumerate(indices):
            for right in indices[offset + 1 :]:
                if rows[left].model_id == rows[right].model_id:
                    continue
                rank = _sha256_bytes(
                    (
                        rows[left].prompt_hash
                        + "\x1f"
                        + rows[left].row_id
                        + "\x1f"
                        + rows[right].row_id
                    ).encode("ascii")
                )
                group_pairs.append((left, right, rank))
        if max_pairs_per_group > 0 and len(group_pairs) > max_pairs_per_group:
            group_pairs.sort(key=lambda item: (item[2], item[0], item[1]))
            group_pairs = group_pairs[:max_pairs_per_group]
        pair_records.extend(group_pairs)
    pair_records.sort(key=lambda item: (item[0], item[1]))

    pair_indices = np.empty((len(pair_records), 2), dtype=np.int64)
    pair_deltas = np.empty((len(pair_records), len(LABEL_NAMES)), dtype=np.float32)
    pair_label_mask = np.empty((len(pair_records), len(LABEL_NAMES)), dtype=np.uint8)
    pair_preference = np.empty(len(pair_records), dtype=np.float32)
    for pair_index, (left, right, _rank) in enumerate(pair_records):
        pair_indices[pair_index] = (left, right)
        common_mask = np.bitwise_and(rows[left].label_mask, rows[right].label_mask)
        pair_label_mask[pair_index] = common_mask
        pair_deltas[pair_index] = np.where(
            common_mask,
            rows[left].labels - rows[right].labels,
            np.float32(0.0),
        )
        pair_preference[pair_index] = _pair_preference(
            rows[left].labels, rows[right].labels, common_mask
        )
    return pair_indices, pair_deltas, pair_label_mask, pair_preference


def _temporal_split(rows: Sequence[OutcomeRow]) -> np.ndarray:
    """Assign prompt groups, rather than rows, to an 80/10/10 time split."""

    group_epoch: Dict[str, float] = {}
    for row in rows:
        group_epoch[row.prompt_hash] = min(
            row.event_epoch,
            group_epoch.get(row.prompt_hash, row.event_epoch),
        )
    ordered = sorted(group_epoch, key=lambda key: (group_epoch[key], key))
    count = len(ordered)
    train_end = int(math.floor(count * 0.8))
    validation_end = int(math.floor(count * 0.9))
    assignments: Dict[str, int] = {}
    for index, prompt_hash in enumerate(ordered):
        assignments[prompt_hash] = (
            0 if index < train_end else 1 if index < validation_end else 2
        )
    return np.asarray([assignments[row.prompt_hash] for row in rows], dtype=np.uint8)


def arrays_from_rows(
    rows: Sequence[OutcomeRow],
    catalog: CatalogIndex,
    task_dim: int,
    model_dim: int,
    max_pairs_per_group: int,
) -> Dict[str, np.ndarray]:
    count = len(rows)
    task_matrix = np.empty((count, task_dim), dtype=np.float32)
    model_matrix = np.empty((count, model_dim), dtype=np.float32)
    labels = np.empty((count, len(LABEL_NAMES)), dtype=np.float32)
    label_mask = np.empty((count, len(LABEL_NAMES)), dtype=np.uint8)
    task_cache: Dict[str, np.ndarray] = {}
    model_cache: Dict[Tuple[str, bool], np.ndarray] = {}

    for index, row in enumerate(rows):
        if row.precomputed_task_features is None and row.prompt_hash in task_cache:
            candidate_task_features = task_cache[row.prompt_hash]
        elif row.precomputed_task_features is None:
            candidate_task_features = task_features(row.prompt, task_dim)
        else:
            candidate_task_features = np.asarray(
                row.precomputed_task_features, dtype=np.float32
            ).reshape(-1)
            if candidate_task_features.shape != (task_dim,) or not np.all(
                np.isfinite(candidate_task_features)
            ):
                raise ValueError(
                    "precomputed task feature vector is non-finite or has the wrong dimension"
                )
        if row.prompt_hash in task_cache:
            if not np.array_equal(task_cache[row.prompt_hash], candidate_task_features):
                raise ValueError(
                    "one task group contains inconsistent task feature vectors"
                )
        else:
            task_cache[row.prompt_hash] = candidate_task_features.copy()
        task_matrix[index] = task_cache[row.prompt_hash]
        model_key = (row.model_id, row.catalog_match)
        if model_key not in model_cache:
            model_cache[model_key] = model_features(
                row.descriptor,
                model_dim,
                requested_model_id=row.observed_model_id,
                catalog_match=row.catalog_match,
            )
        model_matrix[index] = model_cache[model_key]
        labels[index] = row.labels
        label_mask[index] = row.label_mask

    unique_prompt_hashes = sorted({row.prompt_hash for row in rows})
    group_lookup = {
        prompt_hash: index for index, prompt_hash in enumerate(unique_prompt_hashes)
    }
    group_ids = np.asarray(
        [group_lookup[row.prompt_hash] for row in rows], dtype=np.int64
    )
    pair_indices, pair_deltas, pair_pair_mask, pair_preference = build_pairs(
        rows, group_ids, max_pairs_per_group
    )
    catalog_ids = sorted(catalog.models)
    catalog_provider_ids = [
        _provider_from_model_id(model_id) for model_id in catalog_ids
    ]
    catalog_matrix = np.empty((len(catalog_ids), model_dim), dtype=np.float32)
    quality_matrix = np.empty((len(catalog_ids), model_dim), dtype=np.float32)
    quality_labels = np.zeros(
        (len(catalog_ids), len(CATALOG_QUALITY_LABEL_SPECS)), dtype=np.float32
    )
    quality_label_mask = np.zeros_like(quality_labels, dtype=np.uint8)
    quality_provenance: List[List[str]] = []
    for index, model_id in enumerate(catalog_ids):
        descriptor = catalog.models[model_id]
        catalog_matrix[index] = model_features(
            descriptor,
            model_dim,
            requested_model_id=model_id,
            catalog_match=True,
        )
        quality_matrix[index] = model_features(
            descriptor,
            model_dim,
            requested_model_id=model_id,
            catalog_match=True,
            include_benchmark_priors=False,
        )
        target, target_mask, provenance = _catalog_quality_targets(descriptor)
        quality_labels[index] = target
        quality_label_mask[index] = target_mask
        quality_provenance.append(provenance)

    # This assertion makes the anti-leak contract executable.  Future feature
    # revisions must update MODEL_BENCHMARK_DERIVED_INDICES deliberately.
    if np.any(
        quality_matrix[:, np.asarray(MODEL_BENCHMARK_DERIVED_INDICES, dtype=np.int64)]
        != 0.0
    ):
        raise ValueError("catalog quality features contain benchmark-derived inputs")

    row_model_ids = [row.model_id for row in rows]
    row_provider_ids = [row.provider_id for row in rows]
    arrays: Dict[str, np.ndarray] = {
        "schema_version": _unicode_array([SCHEMA_VERSION]),
        "catalog_sha256": _unicode_array([catalog.snapshot_sha256]),
        "task_features": task_matrix,
        "model_features": model_matrix,
        "labels": labels,
        "label_mask": label_mask,
        "label_names": _unicode_array(LABEL_NAMES),
        "task_feature_names": _unicode_array(task_feature_names(task_dim)),
        "model_feature_names": _unicode_array(model_feature_names(model_dim)),
        "model_ids": _unicode_array(row_model_ids),
        "observed_model_ids": _unicode_array([row.observed_model_id for row in rows]),
        "provider_ids": _unicode_array(row_provider_ids),
        "prompt_hashes": _unicode_array([row.prompt_hash for row in rows]),
        "row_ids": _unicode_array([row.row_id for row in rows]),
        "instance_ids": _unicode_array([row.instance_id for row in rows]),
        "prompt_event_ids": np.asarray(
            [row.prompt_event_id for row in rows], dtype=np.int64
        ),
        "event_epochs": np.asarray([row.event_epoch for row in rows], dtype=np.float64),
        "catalog_match": np.asarray(
            [row.catalog_match for row in rows], dtype=np.uint8
        ),
        "catalog_ids": _unicode_array(catalog_ids),
        "catalog_features": catalog_matrix,
        "catalog_provider_ids": _unicode_array(catalog_provider_ids),
        "catalog_quality_catalog_sha256": _unicode_array([catalog.snapshot_sha256]),
        "catalog_quality_catalog_indices": np.arange(len(catalog_ids), dtype=np.int64),
        "catalog_quality_supervision_version": _unicode_array(
            [CATALOG_QUALITY_SUPERVISION_VERSION]
        ),
        "catalog_quality_model_ids": _unicode_array(catalog_ids),
        "catalog_quality_provider_ids": _unicode_array(catalog_provider_ids),
        "catalog_quality_features": quality_matrix,
        "catalog_quality_feature_names": _unicode_array(model_feature_names(model_dim)),
        "catalog_quality_suppressed_feature_indices": np.asarray(
            MODEL_BENCHMARK_DERIVED_INDICES, dtype=np.int16
        ),
        "catalog_quality_labels": quality_labels,
        "catalog_quality_label_mask": quality_label_mask,
        "catalog_quality_label_names": _unicode_array(
            [spec[0] for spec in CATALOG_QUALITY_LABEL_SPECS]
        ),
        "catalog_quality_label_provenance": _unicode_array(
            [source for row_sources in quality_provenance for source in row_sources]
        ).reshape(len(catalog_ids), len(CATALOG_QUALITY_LABEL_SPECS)),
        "group_ids": group_ids,
        "temporal_split": _temporal_split(rows),
        "pair_indices": pair_indices,
        "pair_deltas": pair_deltas,
        "pair_label_mask": pair_pair_mask,
        "pair_preference": pair_preference,
    }
    if any(row.source_kind != "baseline" for row in rows):
        row_provenance = []
        for row in rows:
            declared = (
                row.label_provenance
                if len(row.label_provenance) == len(LABEL_NAMES)
                else _BASELINE_LABEL_PROVENANCE
            )
            row_provenance.append(
                [
                    declared[index] if bool(row.label_mask[index]) else ""
                    for index in range(len(LABEL_NAMES))
                ]
            )
        arrays.update(
            {
                "row_sources": _unicode_array([row.source_kind for row in rows]),
                "row_label_provenance": _unicode_array(
                    [value for values in row_provenance for value in values]
                ).reshape(count, len(LABEL_NAMES)),
                "row_label_source_ids": _unicode_array(
                    [
                        row.label_source_id
                        or _feedback_opaque_id("label-source", "baseline_events")
                        for row in rows
                    ]
                ),
                "row_label_confidence": np.asarray(
                    [row.label_confidence for row in rows], dtype=np.float32
                ),
                "row_censored": np.asarray(
                    [row.censored for row in rows], dtype=np.uint8
                ),
            }
        )
    arrays.update(_cold_start_arrays(row_model_ids, row_provider_ids))
    arrays.update(_cold_start_arrays(catalog_ids, catalog_provider_ids, "catalog_"))
    arrays.update(
        _cold_start_arrays(catalog_ids, catalog_provider_ids, "catalog_quality_")
    )
    return arrays


def save_npz_deterministic(path: Path, arrays: Mapping[str, np.ndarray]) -> None:
    """Write NPZ with sorted members and a fixed ZIP timestamp."""

    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
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
                info = zipfile.ZipInfo(
                    filename=name + ".npy", date_time=(1980, 1, 1, 0, 0, 0)
                )
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o600 << 16
                archive.writestr(
                    info, buffer.getvalue(), compress_type=zipfile.ZIP_DEFLATED
                )
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def attach_feedback_snapshot_arrays(
    arrays: MutableMapping[str, np.ndarray], snapshot: Mapping[str, Any]
) -> None:
    """Pin feedback source provenance inside the NPZ, not only its sidecar."""

    if "row_sources" not in arrays:
        row_count = int(arrays["labels"].shape[0])
        label_mask = np.asarray(arrays["label_mask"], dtype=np.uint8)
        provenance = np.empty((row_count, len(LABEL_NAMES)), dtype=object)
        for row_index in range(row_count):
            for label_index, source in enumerate(_BASELINE_LABEL_PROVENANCE):
                provenance[row_index, label_index] = (
                    source if bool(label_mask[row_index, label_index]) else ""
                )
        arrays["row_sources"] = _unicode_array(["baseline"] * row_count)
        arrays["row_label_provenance"] = _unicode_array(
            provenance.reshape(-1).tolist()
        ).reshape(row_count, len(LABEL_NAMES))
        arrays["row_label_source_ids"] = _unicode_array(
            [
                _feedback_opaque_id("label-source", "baseline_events")
                for _ in range(row_count)
            ]
        )
        arrays["row_label_confidence"] = np.ones(row_count, dtype=np.float32)
        arrays["row_censored"] = np.zeros(row_count, dtype=np.uint8)
    arrays["feedback_import_version"] = _unicode_array(
        [str(snapshot["import_version"])]
    )
    arrays["feedback_schema_version"] = np.asarray(
        [int(snapshot["schema_version"])], dtype=np.int16
    )
    arrays["feedback_source_logical_sha256"] = _unicode_array(
        [str(snapshot["logical_snapshot_sha256"])]
    )
    arrays["feedback_task_feature_version"] = _unicode_array(
        [str(snapshot["task_feature_version"])]
    )
    arrays["feedback_task_dim"] = np.asarray(
        [int(snapshot["task_dim"])], dtype=np.int32
    )
    if "included_outcome_max_rowid" in snapshot:
        arrays["feedback_source_max_outcome_rowid"] = np.asarray(
            [int(snapshot["included_outcome_max_rowid"])], dtype=np.int64
        )
    if "included_revision_id" in snapshot:
        arrays["feedback_source_max_revision_id"] = np.asarray(
            [int(snapshot["included_revision_id"])], dtype=np.int64
        )


def _write_json_deterministic(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (
        json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    ).encode("utf-8")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
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


def build_metadata(
    arrays: Mapping[str, np.ndarray],
    catalog: CatalogIndex,
    counters: Mapping[str, int],
    task_dim: int,
    model_dim: int,
    max_pairs_per_group: int,
    npz_sha256: str,
    source_db: Path,
    source_catalog: Path,
    feedback_snapshot: Optional[Mapping[str, Any]] = None,
) -> Dict[str, Any]:
    labels = arrays["labels"]
    model_ids = arrays["model_ids"]
    provider_ids = arrays["provider_ids"]
    prompt_hashes = arrays["prompt_hashes"]
    temporal = arrays["temporal_split"]
    quality_mask = arrays["catalog_quality_label_mask"]
    family_ids = arrays["family_ids"]
    metadata: Dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "artifact_sha256": npz_sha256,
        "catalog_snapshot_sha256": catalog.snapshot_sha256,
        "source": {
            "database_filename": source_db.name,
            "catalog_filename": source_catalog.name,
        },
        "counts": {
            **{key: int(value) for key, value in counters.items()},
            "rows": int(labels.shape[0]),
            "prompt_groups": int(len(set(prompt_hashes.tolist()))),
            "models": int(len(set(model_ids.tolist()))),
            "providers": int(len(set(provider_ids.tolist()))),
            "families": int(len(set(family_ids.tolist()))),
            "catalog_models": int(len(catalog.models)),
            "catalog_matched_rows": int(np.sum(arrays["catalog_match"])),
            "catalog_quality_labeled_models": int(
                np.sum(np.any(quality_mask != 0, axis=1))
            ),
            "catalog_quality_observed_labels": int(np.sum(quality_mask)),
            "pairs": int(arrays["pair_indices"].shape[0]),
            "temporal_train_rows": int(np.sum(temporal == 0)),
            "temporal_validation_rows": int(np.sum(temporal == 1)),
            "temporal_test_rows": int(np.sum(temporal == 2)),
        },
        "label_contract": {
            "names": list(LABEL_NAMES),
            "units": ["boolean", "boolean", "boolean", "USD", "seconds"],
            "mask_semantics": "1 means outcome was observed; 0 means unknown, not negative",
            "latency_definition": "prompt event to final turn_done or provider-failure event",
            "cost_definition": "sum of baseline est_cost_usd within the prompt segment",
            "catalog_quality_labels_included": False,
        },
        "catalog_quality_contract": {
            "supervision_version": CATALOG_QUALITY_SUPERVISION_VERSION,
            "kind": "weak_catalog_benchmark_supervision",
            "catalog_snapshot_sha256": catalog.snapshot_sha256,
            "names": [spec[0] for spec in CATALOG_QUALITY_LABEL_SPECS],
            "units": ["[0,1] fraction", "[0,1] fraction", "[0,1] fraction"],
            "source_transform": "finite source indices in [0,100] divided by 100; out-of-range values are masked unknown",
            "features": "catalog_quality_features",
            "labels": "catalog_quality_labels",
            "mask": "catalog_quality_label_mask",
            "provenance": "catalog_quality_label_provenance",
            "mask_semantics": "1 means the exact finite source field existed; 0 means unknown, not zero",
            "anti_leak": {
                "benchmark_priors_in_features": False,
                "suppressed_model_feature_indices": list(
                    MODEL_BENCHMARK_DERIVED_INDICES
                ),
                "suppressed_model_feature_names": [
                    model_feature_names(model_dim)[index]
                    for index in MODEL_BENCHMARK_DERIVED_INDICES
                ],
            },
            "separation": "never concatenate these weak labels to historical labels; train and evaluate as an auxiliary catalog lane",
        },
        "pair_contract": {
            "grouping": "exact SHA-256 of unnormalized UTF-8 prompt bytes",
            "indices": "unordered row pairs with the same prompt hash and different resolved model IDs",
            "deltas": "left labels minus right labels; missing dimensions are zero",
            "preference": "1 left, 0 right, 0.5 tie under completion/failure/tool-error/cost/latency lexicographic order",
            "max_pairs_per_group": int(max_pairs_per_group),
        },
        "split_contract": {
            "temporal_split": {"0": "train", "1": "validation", "2": "test"},
            "temporal_grouping": "exact prompt groups assigned together by first observation time",
            "model_holdout_fold": "stable blake2b(model_id) modulo 10; reserve one or more folds for cold-start evaluation",
            "provider_holdout_fold": "stable blake2b(provider_id) modulo 10",
            "family_holdout_fold": "stable blake2b(versioned family_id) modulo 10",
            "holdout_ids": "SHA-256 domain-separated opaque IDs are emitted for model, provider, and family",
            "model_family_version": MODEL_FAMILY_VERSION,
            "catalog_quality_holdouts": "quality-prefixed model/provider/family IDs and folds align one-to-one with catalog_quality_model_ids",
        },
        "security": {
            "raw_prompts_emitted": False,
            "warning": "prompt hashes and hashed features are data-minimizing, not anonymized; treat them as sensitive telemetry",
        },
        "arrays": {
            name: {"dtype": str(array.dtype), "shape": list(array.shape)}
            for name, array in sorted(arrays.items())
        },
    }
    if feedback_snapshot is not None:
        feedback_outcome_table = str(feedback_snapshot["outcome_source_table"])
        metadata["source"]["feedback_database_filename"] = str(
            feedback_snapshot["database_filename"]
        )
        metadata["feedback_contract"] = {
            **dict(feedback_snapshot),
            "historical_array_merge": True,
            "row_source_array": "row_sources",
            "label_provenance_array": "row_label_provenance",
            "label_source_ids": "domain-separated SHA-256 IDs; raw label_source text is not emitted",
            "label_confidence_array": "row_label_confidence",
            "label_mapping": {
                "completion_success": feedback_outcome_table + ".http_success",
                "provider_failure": feedback_outcome_table + ".provider_failure",
                "tool_error": "1 - " + feedback_outcome_table + ".tool_valid",
                "cost_usd": feedback_outcome_table + ".cost_usd",
                "latency_s": feedback_outcome_table + ".e2e_ms / 1000",
            },
            "task_success_mapping": "not mapped: semantic task success is not the baseline terminal-completion target",
            "mask_semantics": "1 only when the exact mapped source field is non-null; explicit false and zero remain observed",
            "censor_semantics": "censored is emitted separately and does not erase explicitly observed fields",
            "candidate_semantics": "only the actually used/intended/chosen model receives an outcome row; unchosen exposures are never negative labels",
            "task_grouping": "domain-separated SHA-256 of the stored canonical request hash; repeated requests stay in one temporal group",
            "raw_prompts_emitted": False,
            "warning": "request hashes and task features are data-minimizing, not anonymous",
        }
    metadata.update(feature_contract(task_dim, model_dim))
    return metadata


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--db",
        type=Path,
        default=Path("~/.dsco/baseline.db"),
        help="DSCO baseline SQLite database (default: ~/.dsco/baseline.db)",
    )
    parser.add_argument(
        "--catalog",
        type=Path,
        default=Path("~/.dsco/openrouter_models.json"),
        help="pinned OpenRouter models JSON snapshot",
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="output .npz dataset"
    )
    parser.add_argument(
        "--metadata",
        type=Path,
        help="metadata JSON path (default: OUTPUT with .json suffix)",
    )
    parser.add_argument("--task-dim", type=int, default=DEFAULT_TASK_DIM)
    parser.add_argument("--model-dim", type=int, default=DEFAULT_MODEL_DIM)
    parser.add_argument("--min-prompt-chars", type=int, default=1)
    parser.add_argument(
        "--max-event-id",
        type=int,
        help="pin the inclusive events.id high-water mark for reproducible rebuilds",
    )
    parser.add_argument(
        "--feedback-db",
        type=Path,
        help="optional redacted Chimera feedback SQLite database",
    )
    parser.add_argument(
        "--max-feedback-revision-id",
        type=int,
        help="pin the inclusive immutable schema-v3 feedback revision high-water",
    )
    parser.add_argument(
        "--max-feedback-outcome-rowid",
        type=int,
        help="schema-v2 compatibility cutoff; pair with --expected-feedback-snapshot-sha256",
    )
    parser.add_argument(
        "--expected-feedback-snapshot-sha256",
        help="fail unless the selected feedback logical snapshot has this SHA-256",
    )
    parser.add_argument(
        "--include-incomplete",
        action="store_true",
        help="retain rows without turn_done/provider failure using zero label masks",
    )
    parser.add_argument(
        "--max-pairs-per-group",
        type=int,
        default=0,
        help="deterministic cap per exact-prompt group; 0 keeps all pairs",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    db_path = args.db.expanduser().resolve()
    catalog_path = args.catalog.expanduser().resolve()
    feedback_path = (
        None if args.feedback_db is None else args.feedback_db.expanduser().resolve()
    )
    output_path = args.output.expanduser().resolve()
    if output_path.suffix != ".npz":
        output_path = output_path.with_suffix(
            output_path.suffix + ".npz" if output_path.suffix else ".npz"
        )
    metadata_path = (
        args.metadata.expanduser().resolve()
        if args.metadata
        else output_path.with_suffix(".json")
    )
    if not db_path.is_file():
        raise SystemExit("baseline database not found: %s" % db_path)
    if not catalog_path.is_file():
        raise SystemExit("OpenRouter catalog not found: %s" % catalog_path)
    if feedback_path is not None and not feedback_path.is_file():
        raise SystemExit("feedback database not found: %s" % feedback_path)
    if args.min_prompt_chars < 0:
        raise SystemExit("--min-prompt-chars must be non-negative")
    if args.max_pairs_per_group < 0:
        raise SystemExit("--max-pairs-per-group must be non-negative")
    if args.max_event_id is not None and args.max_event_id < 0:
        raise SystemExit("--max-event-id must be non-negative")
    if args.max_feedback_revision_id is not None and args.max_feedback_revision_id < 0:
        raise SystemExit("--max-feedback-revision-id must be non-negative")
    if (
        args.max_feedback_outcome_rowid is not None
        and args.max_feedback_outcome_rowid < 0
    ):
        raise SystemExit("--max-feedback-outcome-rowid must be non-negative")
    if feedback_path is None and (
        args.max_feedback_revision_id is not None
        or args.max_feedback_outcome_rowid is not None
        or args.expected_feedback_snapshot_sha256 is not None
    ):
        raise SystemExit("feedback snapshot options require --feedback-db")
    # Validate dimensions and make the vectorizer contract fail fast.
    task_feature_names(args.task_dim)
    model_feature_names(args.model_dim)

    catalog_payload = catalog_path.read_bytes()
    catalog = CatalogIndex(catalog_payload)
    rows, counters = collect_rows(
        db_path,
        catalog,
        min_prompt_chars=args.min_prompt_chars,
        include_incomplete=args.include_incomplete,
        max_event_id=args.max_event_id,
    )
    feedback_snapshot: Optional[Dict[str, Any]] = None
    if feedback_path is not None:
        try:
            feedback_rows, feedback_counters, feedback_snapshot = collect_feedback_rows(
                feedback_path,
                catalog,
                task_dim=args.task_dim,
                max_revision_id=args.max_feedback_revision_id,
                max_outcome_rowid=args.max_feedback_outcome_rowid,
                expected_snapshot_sha256=args.expected_feedback_snapshot_sha256,
            )
        except (OSError, sqlite3.Error, ValueError) as exc:
            raise SystemExit("feedback import failed: %s" % exc) from exc
        rows.extend(feedback_rows)
        counters.update(feedback_counters)
        rows.sort(
            key=lambda row: (
                row.prompt_hash,
                row.model_id,
                row.event_epoch,
                row.row_id,
            )
        )
    if not rows:
        raise SystemExit("no labeled prompt/model rows found in baseline database")
    arrays = arrays_from_rows(
        rows,
        catalog,
        task_dim=args.task_dim,
        model_dim=args.model_dim,
        max_pairs_per_group=args.max_pairs_per_group,
    )
    if feedback_snapshot is not None:
        attach_feedback_snapshot_arrays(arrays, feedback_snapshot)
    # Drop the only in-memory raw prompt references before writing artifacts.
    for row in rows:
        row.prompt = ""
    save_npz_deterministic(output_path, arrays)
    artifact_hash = _sha256_file(output_path)
    metadata = build_metadata(
        arrays,
        catalog,
        counters,
        args.task_dim,
        args.model_dim,
        args.max_pairs_per_group,
        artifact_hash,
        db_path,
        catalog_path,
        feedback_snapshot,
    )
    _write_json_deterministic(metadata_path, metadata)
    summary = {
        "artifact_sha256": artifact_hash,
        "catalog_snapshot_sha256": catalog.snapshot_sha256,
        "catalog_quality_labeled_models": metadata["counts"][
            "catalog_quality_labeled_models"
        ],
        "metadata": str(metadata_path),
        "models": metadata["counts"]["models"],
        "output": str(output_path),
        "pairs": metadata["counts"]["pairs"],
        "providers": metadata["counts"]["providers"],
        "rows": metadata["counts"]["rows"],
        "source_event_max_id": metadata["counts"]["source_event_max_id"],
    }
    if feedback_snapshot is not None:
        summary.update(
            {
                "feedback_rows": metadata["counts"]["feedback_rows_imported"],
                "feedback_snapshot_sha256": feedback_snapshot[
                    "logical_snapshot_sha256"
                ],
            }
        )
        if "included_revision_id" in feedback_snapshot:
            summary["feedback_max_revision_id"] = feedback_snapshot[
                "included_revision_id"
            ]
        elif "included_outcome_max_rowid" in feedback_snapshot:
            summary["feedback_max_outcome_rowid"] = feedback_snapshot[
                "included_outcome_max_rowid"
            ]
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
