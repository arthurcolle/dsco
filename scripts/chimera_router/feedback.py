#!/usr/bin/env python3
"""Redacted decision and delayed-outcome storage for Chimera Router.

The store intentionally never persists raw prompts or responses.  It records a
SHA-256 digest of the canonical request plus the structured route exposure and
outcome fields needed for counterfactual evaluation and future retraining.

This module is a storage boundary, not an inference proxy.  It performs no
network calls and does not hold provider credentials.
"""

from __future__ import annotations

import hashlib
import json
import math
import sqlite3
import threading
import uuid
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterator, Mapping, Optional, Sequence

import numpy as np


FEEDBACK_SCHEMA_VERSION = 3
DEFAULT_SCHEMA_PATH = Path(__file__).with_name("feedback_schema.sql")


LATEST_OUTCOMES_AS_OF_SQL = """
SELECT revisions.*
FROM route_outcome_revisions AS revisions
JOIN (
    SELECT request_id, MAX(revision_id) AS revision_id
    FROM route_outcome_revisions
    WHERE revision_id <= ?
    GROUP BY request_id
) AS latest
  ON latest.request_id = revisions.request_id
 AND latest.revision_id = revisions.revision_id
ORDER BY revisions.request_id
"""


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    )


def request_digest(request: Mapping[str, Any]) -> str:
    """Return the stable digest stored instead of request content."""

    payload = _canonical_json(dict(request)).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _utc_now() -> str:
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def _provider(model_id: Optional[str]) -> Optional[str]:
    if not model_id:
        return None
    value = str(model_id).strip()
    if "/" not in value:
        return None
    return value.split("/", 1)[0] or None


def _optional_float(value: Any, *, nonnegative: bool = False) -> Optional[float]:
    if value is None or isinstance(value, bool):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(result) or (nonnegative and result < 0.0):
        return None
    return result


def _optional_int(value: Any, *, nonnegative: bool = False) -> Optional[int]:
    if value is None or isinstance(value, bool):
        return None
    try:
        result = int(value)
    except (TypeError, ValueError):
        return None
    if nonnegative and result < 0:
        return None
    return result


def _optional_bool(value: Any) -> Optional[int]:
    if value is None:
        return None
    if isinstance(value, bool):
        return int(value)
    if value in (0, 1):
        return int(value)
    raise ValueError("boolean outcome fields must be true, false, 0, 1, or null")


class FeedbackStore:
    """Small thread-safe SQLite feedback sink.

    A short-lived SQLite connection is used per transaction.  That keeps the
    class safe under ``ThreadingHTTPServer`` without sharing cursor state while
    WAL mode permits concurrent readers.
    """

    def __init__(self, path: Path, schema_path: Path = DEFAULT_SCHEMA_PATH) -> None:
        self.path = Path(path).expanduser().resolve()
        self.schema_path = Path(schema_path).expanduser().resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        schema = self.schema_path.read_text(encoding="utf-8")
        with self._connect() as connection:
            schema_table = connection.execute(
                "SELECT 1 FROM sqlite_master "
                "WHERE type = 'table' AND name = 'router_schema'"
            ).fetchone()
            if schema_table is not None:
                row = connection.execute(
                    "SELECT MAX(version) FROM router_schema"
                ).fetchone()
                installed = int(row[0]) if row and row[0] is not None else 0
                if installed > FEEDBACK_SCHEMA_VERSION:
                    raise ValueError(
                        "feedback schema is newer than this binary: "
                        "installed %d, supported %d"
                        % (installed, FEEDBACK_SCHEMA_VERSION)
                    )
            connection.executescript(schema)
            row = connection.execute(
                "SELECT MAX(version) FROM router_schema"
            ).fetchone()
            installed = int(row[0]) if row and row[0] is not None else 0
            if installed != FEEDBACK_SCHEMA_VERSION:
                raise ValueError(
                    "feedback schema mismatch: installed %d, supported %d"
                    % (installed, FEEDBACK_SCHEMA_VERSION)
                )

    @contextmanager
    def _connect(self) -> Iterator[sqlite3.Connection]:
        connection = sqlite3.connect(str(self.path), timeout=10.0)
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 10000")
        try:
            yield connection
        except BaseException:
            connection.rollback()
            raise
        else:
            connection.commit()
        finally:
            connection.close()

    def register_catalog(
        self,
        *,
        snapshot_id: str,
        content_sha256: str,
        model_count: int,
        source_url: str,
        captured_at: Optional[str] = None,
        schema_version: int = 1,
    ) -> None:
        if not snapshot_id or not content_sha256:
            raise ValueError("catalog snapshot ID and content hash are required")
        if model_count < 0:
            raise ValueError("catalog model_count cannot be negative")
        with self._lock, self._connect() as connection:
            connection.execute(
                """
                INSERT OR IGNORE INTO catalog_snapshots(
                    snapshot_id, captured_at, source_url, content_sha256,
                    schema_version, model_count
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    str(snapshot_id),
                    captured_at or _utc_now(),
                    str(source_url),
                    str(content_sha256),
                    int(schema_version),
                    int(model_count),
                ),
            )

    def record_decision(
        self,
        request: Mapping[str, Any],
        decision: Mapping[str, Any],
        *,
        checkpoint_id: str,
        catalog_snapshot_id: Optional[str],
        policy_id: str = "chimera-default",
        logging_propensity: Optional[float] = None,
        tenant_hash: Optional[str] = None,
        request_id: Optional[str] = None,
        task_features: Optional[Sequence[float]] = None,
        task_feature_version: Optional[str] = None,
    ) -> str:
        """Persist one decision and every candidate exposed to the scorer."""

        selected = decision.get("selected_model")
        if not selected:
            raise ValueError("cannot record a decision without a selected model")
        propensity = _optional_float(logging_propensity)
        if propensity is not None and not 0.0 < propensity <= 1.0:
            raise ValueError("logging_propensity must be in (0, 1]")
        identifier = str(request_id or uuid.uuid4())
        candidates_value = decision.get("candidates")
        candidates: Sequence[Any] = (
            candidates_value if isinstance(candidates_value, list) else []
        )
        chosen_score = 0.0
        for candidate in candidates:
            if isinstance(candidate, Mapping) and candidate.get("model_id") == selected:
                chosen_score = _optional_float(candidate.get("utility")) or 0.0
                break
        fallback = decision.get("fallback_models")
        fallback_models = fallback if isinstance(fallback, list) else []
        constraints = {
            "policy": decision.get("policy") or {},
            "token_estimates": decision.get("token_estimates") or {},
            "schema_version": decision.get("schema_version"),
        }
        feature_payload: Optional[bytes] = None
        feature_hash: Optional[str] = None
        feature_dim = 0
        if task_features is not None:
            if not task_feature_version:
                raise ValueError(
                    "task_feature_version is required when task_features are stored"
                )
            feature_array = np.asarray(task_features, dtype="<f4").reshape(-1)
            if feature_array.size == 0 or not np.all(np.isfinite(feature_array)):
                raise ValueError("task_features must be a non-empty finite vector")
            feature_payload = feature_array.tobytes(order="C")
            feature_hash = hashlib.sha256(feature_payload).hexdigest()
            feature_dim = int(feature_array.size)

        with self._lock, self._connect() as connection:
            connection.execute(
                """
                INSERT INTO route_decisions(
                    request_id, request_hash, tenant_hash, policy_id,
                    checkpoint_id, catalog_snapshot_id, explicit_override,
                    constraints_json, chosen_model, chosen_provider,
                    chosen_score, confidence, logging_propensity,
                    fallback_chain_json
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    identifier,
                    request_digest(request),
                    tenant_hash,
                    str(policy_id),
                    str(checkpoint_id),
                    catalog_snapshot_id,
                    (
                        str(selected)
                        if bool(decision.get("explicit_override"))
                        else None
                    ),
                    _canonical_json(constraints),
                    str(selected),
                    _provider(str(selected)),
                    float(chosen_score),
                    _optional_float(decision.get("confidence")),
                    propensity,
                    _canonical_json(fallback_models),
                ),
            )
            if feature_payload is not None:
                connection.execute(
                    """
                    INSERT INTO route_request_features(
                        request_id, task_feature_version, task_dim, encoding,
                        feature_sha256, task_features
                    ) VALUES (?, ?, ?, 'float32-le', ?, ?)
                    """,
                    (
                        identifier,
                        str(task_feature_version),
                        feature_dim,
                        feature_hash,
                        sqlite3.Binary(feature_payload),
                    ),
                )

            exposure_rows = []
            for rank, value in enumerate(candidates):
                if not isinstance(value, Mapping):
                    continue
                model_id = str(value.get("model_id") or "").strip()
                if not model_id:
                    continue
                predictions = value.get("predictions")
                predictions = predictions if isinstance(predictions, Mapping) else {}
                predicted_quality = _optional_float(value.get("benchmark_quality"))
                if predicted_quality is None:
                    predicted_quality = _optional_float(predictions.get("quality"))
                if predicted_quality is None:
                    predicted_quality = _optional_float(
                        predictions.get("completion_success")
                    )
                rejection = value.get("rejection_reasons")
                rejection_reasons = rejection if isinstance(rejection, list) else []
                exposure_rows.append(
                    (
                        identifier,
                        int(rank),
                        model_id,
                        _provider(model_id),
                        int(bool(value.get("feasible"))),
                        (
                            _canonical_json(rejection_reasons)
                            if rejection_reasons
                            else None
                        ),
                        predicted_quality,
                        _optional_float(value.get("failure_probability")),
                        (
                            None
                            if _optional_float(value.get("expected_latency_s")) is None
                            else 1000.0
                            * float(_optional_float(value.get("expected_latency_s")))
                        ),
                        _optional_float(
                            value.get("expected_cost_usd"), nonnegative=True
                        ),
                        _optional_float(value.get("utility")),
                        propensity if model_id == str(selected) else None,
                        int(model_id == str(selected)),
                    )
                )
            connection.executemany(
                """
                INSERT INTO candidate_exposures(
                    request_id, rank, model_id, provider, feasible,
                    rejection_reason, predicted_quality, predicted_failure,
                    predicted_latency_ms, predicted_cost_usd, score,
                    logging_propensity, chosen
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                exposure_rows,
            )
        return identifier

    def outcome_revision_high_water(self) -> int:
        """Return the largest immutable outcome revision currently visible."""

        with self._lock, self._connect() as connection:
            row = connection.execute(
                "SELECT COALESCE(MAX(revision_id), 0) FROM route_outcome_revisions"
            ).fetchone()
        return int(row[0]) if row else 0

    def latest_outcomes_as_of(
        self, revision_high_water: Optional[int] = None
    ) -> Dict[str, Any]:
        """Return one deterministic latest revision per request at a high-water.

        Callers should persist the returned ``revision_high_water`` alongside a
        derived dataset.  Later corrections cannot change an as-of replay.
        """

        if revision_high_water is not None:
            if isinstance(revision_high_water, bool):
                raise ValueError("revision_high_water must be a nonnegative integer")
            try:
                high_water = int(revision_high_water)
            except (TypeError, ValueError) as error:
                raise ValueError(
                    "revision_high_water must be a nonnegative integer"
                ) from error
            if high_water < 0 or high_water != revision_high_water:
                raise ValueError("revision_high_water must be a nonnegative integer")
        else:
            high_water = None

        with self._lock, self._connect() as connection:
            if high_water is None:
                row = connection.execute(
                    "SELECT COALESCE(MAX(revision_id), 0) FROM route_outcome_revisions"
                ).fetchone()
                high_water = int(row[0]) if row else 0
            cursor = connection.execute(LATEST_OUTCOMES_AS_OF_SQL, (high_water,))
            columns = [description[0] for description in cursor.description]
            outcomes = [dict(zip(columns, row)) for row in cursor.fetchall()]
        return {
            "revision_high_water": high_water,
            "outcomes": outcomes,
        }

    def record_outcome(self, payload: Mapping[str, Any]) -> Dict[str, Any]:
        """Append a delayed outcome revision for an existing decision."""

        request_id = str(payload.get("request_id") or "").strip()
        label_source = str(payload.get("label_source") or "").strip()
        confidence = _optional_float(payload.get("label_confidence"))
        if not request_id:
            raise ValueError("request_id is required")
        if not label_source:
            raise ValueError("label_source is required")
        if confidence is None or not 0.0 <= confidence <= 1.0:
            raise ValueError("label_confidence must be in [0, 1]")

        with self._lock, self._connect() as connection:
            # Serialize revision-number allocation across independent service
            # processes, not only threads sharing this FeedbackStore instance.
            connection.execute("BEGIN IMMEDIATE")
            decision = connection.execute(
                "SELECT chosen_model FROM route_decisions WHERE request_id = ?",
                (request_id,),
            ).fetchone()
            if decision is None:
                raise KeyError("unknown request_id")
            previous = connection.execute(
                "SELECT revision_id, revision_number "
                "FROM route_outcome_revisions WHERE request_id = ? "
                "ORDER BY revision_id DESC LIMIT 1",
                (request_id,),
            ).fetchone()
            supersedes_revision_id = int(previous[0]) if previous else None
            revision_number = int(previous[1]) + 1 if previous else 1
            recorded_at = _utc_now()
            intended_model = str(payload.get("intended_model") or decision[0])
            outcome_values = (
                request_id,
                str(payload.get("completed_at") or recorded_at),
                intended_model,
                (
                    None
                    if payload.get("actual_model") is None
                    else str(payload.get("actual_model"))
                ),
                (
                    None
                    if payload.get("actual_provider") is None
                    else str(payload.get("actual_provider"))
                ),
                (
                    None
                    if payload.get("generation_id") is None
                    else str(payload.get("generation_id"))
                ),
                _optional_bool(payload.get("http_success")),
                _optional_bool(payload.get("task_success")),
                _optional_bool(payload.get("provider_failure")),
                _optional_bool(payload.get("tool_valid")),
                _optional_bool(payload.get("schema_valid")),
                _optional_bool(payload.get("refusal")),
                _optional_bool(payload.get("user_retry")),
                _optional_bool(payload.get("accepted_or_used")),
                _optional_int(payload.get("prompt_tokens"), nonnegative=True),
                _optional_int(payload.get("completion_tokens"), nonnegative=True),
                _optional_int(payload.get("reasoning_tokens"), nonnegative=True),
                _optional_int(payload.get("cache_read_tokens"), nonnegative=True),
                _optional_int(payload.get("cache_write_tokens"), nonnegative=True),
                _optional_float(payload.get("cost_usd"), nonnegative=True),
                _optional_float(payload.get("ttft_ms"), nonnegative=True),
                _optional_float(payload.get("e2e_ms"), nonnegative=True),
                (
                    None
                    if payload.get("finish_reason") is None
                    else str(payload.get("finish_reason"))
                ),
                label_source,
                confidence,
                int(_optional_bool(payload.get("censored")) or 0),
            )
            revision_cursor = connection.execute(
                """
                INSERT INTO route_outcome_revisions(
                    request_id, revision_number, recorded_at,
                    supersedes_revision_id, completed_at, intended_model,
                    actual_model, actual_provider, generation_id, http_success,
                    task_success, provider_failure, tool_valid, schema_valid,
                    refusal, user_retry, accepted_or_used, prompt_tokens,
                    completion_tokens, reasoning_tokens, cache_read_tokens,
                    cache_write_tokens, cost_usd, ttft_ms, e2e_ms,
                    finish_reason, label_source, label_confidence, censored
                ) VALUES (
                    ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                    ?, ?, ?, ?, ?, ?, ?, ?, ?
                )
                """,
                (
                    request_id,
                    revision_number,
                    recorded_at,
                    supersedes_revision_id,
                    *outcome_values[1:],
                ),
            )
            revision_id = int(revision_cursor.lastrowid)
            connection.execute(
                """
                INSERT INTO route_outcomes(
                    request_id, completed_at, intended_model, actual_model,
                    actual_provider, generation_id, http_success, task_success,
                    provider_failure, tool_valid, schema_valid, refusal,
                    user_retry, accepted_or_used, prompt_tokens,
                    completion_tokens, reasoning_tokens, cache_read_tokens,
                    cache_write_tokens, cost_usd, ttft_ms, e2e_ms,
                    finish_reason, label_source, label_confidence, censored
                ) VALUES (
                    ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                    ?, ?, ?, ?, ?, ?
                )
                ON CONFLICT(request_id) DO UPDATE SET
                    completed_at=excluded.completed_at,
                    intended_model=excluded.intended_model,
                    actual_model=excluded.actual_model,
                    actual_provider=excluded.actual_provider,
                    generation_id=excluded.generation_id,
                    http_success=excluded.http_success,
                    task_success=excluded.task_success,
                    provider_failure=excluded.provider_failure,
                    tool_valid=excluded.tool_valid,
                    schema_valid=excluded.schema_valid,
                    refusal=excluded.refusal,
                    user_retry=excluded.user_retry,
                    accepted_or_used=excluded.accepted_or_used,
                    prompt_tokens=excluded.prompt_tokens,
                    completion_tokens=excluded.completion_tokens,
                    reasoning_tokens=excluded.reasoning_tokens,
                    cache_read_tokens=excluded.cache_read_tokens,
                    cache_write_tokens=excluded.cache_write_tokens,
                    cost_usd=excluded.cost_usd,
                    ttft_ms=excluded.ttft_ms,
                    e2e_ms=excluded.e2e_ms,
                    finish_reason=excluded.finish_reason,
                    label_source=excluded.label_source,
                    label_confidence=excluded.label_confidence,
                    censored=excluded.censored
                """,
                outcome_values,
            )
        return {
            "recorded": True,
            "request_id": request_id,
            "revision_id": revision_id,
            "revision_number": revision_number,
            "supersedes_revision_id": supersedes_revision_id,
        }


__all__ = [
    "DEFAULT_SCHEMA_PATH",
    "FEEDBACK_SCHEMA_VERSION",
    "FeedbackStore",
    "LATEST_OUTCOMES_AS_OF_SQL",
    "request_digest",
]
