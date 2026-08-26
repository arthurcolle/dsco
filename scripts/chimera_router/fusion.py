#!/usr/bin/env python3
"""Build the Chimera Router DuckDB fusion warehouse.

SQLite remains the concurrent OLTP source of truth.  This builder takes
consistent SQLite backups, removes raw prompt/response/process-content fields,
loads a typed DuckDB analytical read model off to the side, runs quality gates,
and atomically publishes the completed database.

The implementation deliberately uses the DuckDB CLI instead of the Python
package.  The repository therefore needs no new Python dependency and works on
the current dsco development machines where ``duckdb`` is already installed.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from typing import Any, Dict, Iterator, Mapping, Optional, Sequence
import zlib


FUSION_SCHEMA_VERSION = "chimera-router-fusion-v2"
SCHEMA_PATH = Path(__file__).with_name("fusion_schema.sql")
REGISTRY_PATH = Path(__file__).with_name("fusion_sources.json")
DEFAULT_DUCKDB = Path("/opt/homebrew/bin/duckdb")
DUCKDB_SECURITY_PREAMBLE = """
SET allow_community_extensions=false;
SET autoinstall_known_extensions=false;
SET autoload_known_extensions=false;
SET allow_unsigned_extensions=false;
SET threads=1;
SET lock_configuration=true;
"""
FORBIDDEN_EXTERNAL_FIELDS = frozenset(
    {
        "prompt",
        "response",
        "messages",
        "conversation",
        "conversations",
        "raw_prompt",
        "raw_response",
        "text",
        "content",
    }
)


class FusionError(RuntimeError):
    """Raised for a fail-closed fusion or validation error."""


def _utc_now() -> str:
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def _epoch_iso(value: Any) -> Optional[str]:
    if value is None or value == "":
        return None
    if isinstance(value, str):
        stripped = value.strip()
        if not stripped:
            return None
        try:
            number = float(stripped)
        except ValueError:
            return stripped
    else:
        try:
            number = float(value)
        except (TypeError, ValueError, OverflowError):
            return None
    if not math.isfinite(number):
        return None
    magnitude = abs(number)
    if magnitude >= 1e14:
        number /= 1_000_000.0
    elif magnitude >= 1e11:
        number /= 1_000.0
    try:
        return (
            datetime.fromtimestamp(number, tz=timezone.utc)
            .isoformat(timespec="milliseconds")
            .replace("+00:00", "Z")
        )
    except (OverflowError, OSError, ValueError):
        return None


def _file_time(path: Path) -> str:
    return _epoch_iso(path.stat().st_mtime) or _utc_now()


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    )


def _json_object(value: Any) -> Dict[str, Any]:
    if isinstance(value, Mapping):
        return dict(value)
    if not isinstance(value, str) or not value:
        return {}
    try:
        decoded = json.loads(value)
    except (TypeError, ValueError, json.JSONDecodeError):
        return {}
    return dict(decoded) if isinstance(decoded, Mapping) else {}


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _sha256_text(value: Optional[str]) -> Optional[str]:
    if value is None or value == "":
        return None
    return _sha256_bytes(str(value).encode("utf-8", "surrogateescape"))


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _finite(value: Any) -> Optional[float]:
    if value is None or isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return number if math.isfinite(number) else None


def _integer(value: Any) -> Optional[int]:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return None


def _boolean(value: Any) -> Optional[bool]:
    if value is None:
        return None
    if isinstance(value, bool):
        return value
    if value in (0, 1, "0", "1"):
        return bool(int(value))
    if isinstance(value, str):
        lowered = value.casefold()
        if lowered in {"true", "yes", "ok", "success"}:
            return True
        if lowered in {"false", "no", "failed", "failure"}:
            return False
    return None


def _model_id(value: Any) -> Optional[str]:
    if value is None:
        return None
    result = str(value).strip().casefold()
    return result or None


def _provider(value: Any, model_id: Optional[str] = None) -> Optional[str]:
    if value is not None and str(value).strip():
        return str(value).strip().casefold()
    if model_id and "/" in model_id:
        return model_id.split("/", 1)[0]
    return None


def _safe_source_uri(path: Path) -> str:
    resolved = path.expanduser().resolve()
    home = Path.home().resolve()
    try:
        return "~/" + resolved.relative_to(home).as_posix()
    except ValueError:
        return "local://" + resolved.name


def _sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _table_exists(connection: sqlite3.Connection, table: str) -> bool:
    row = connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)
    ).fetchone()
    return row is not None


def _snapshot_id(source_key: str, content_sha256: str) -> str:
    payload = (
        "chimera-fusion-snapshot-v1\x1f" + source_key + "\x1f" + content_sha256
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


@dataclass(frozen=True)
class SourceSnapshot:
    snapshot_id: str
    source_key: str
    content_sha256: str
    fingerprint_mode: str
    captured_at: str
    ingested_at: str
    byte_size: int
    row_count: int
    high_watermark: Optional[str]
    schema_version: str
    source_uri: Optional[str]
    artifact_uri: Optional[str]
    provenance_json: str

    def as_row(self) -> Dict[str, Any]:
        return dict(self.__dict__)


class StagingArea:
    """Write canonical newline-delimited JSON files for DuckDB bulk loading."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.paths: Dict[str, Path] = {}
        self.handles: Dict[str, Any] = {}
        self.counts: Dict[str, int] = {}

    def write(self, table: str, row: Mapping[str, Any]) -> None:
        if table not in self.handles:
            path = self.root / f"{table}.jsonl"
            self.paths[table] = path
            self.handles[table] = path.open("w", encoding="utf-8", newline="\n")
            self.counts[table] = 0
        self.handles[table].write(_canonical_json(dict(row)) + "\n")
        self.counts[table] += 1

    def close(self) -> None:
        for handle in self.handles.values():
            handle.close()
        self.handles.clear()

    def total(self, tables: Sequence[str]) -> int:
        return sum(self.counts.get(table, 0) for table in tables)


class DuckDBCLI:
    def __init__(self, executable: Path) -> None:
        self.executable = executable.expanduser().resolve()
        if not self.executable.exists():
            candidate = shutil.which(str(executable)) or shutil.which("duckdb")
            if not candidate:
                raise FusionError(f"DuckDB CLI not found: {executable}")
            self.executable = Path(candidate).resolve()

    def execute(
        self,
        database: Path,
        sql: str,
        *,
        readonly: bool = False,
        json_output: bool = False,
    ) -> str:
        command = [str(self.executable)]
        if readonly:
            command.append("-readonly")
        command.append(str(database))
        if json_output:
            command.append("-json")
        command.extend(["-c", sql])
        command[-1] = DUCKDB_SECURITY_PREAMBLE + "\n" + command[-1]
        result = subprocess.run(
            command,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise FusionError(
                "DuckDB command failed: "
                + (result.stderr.strip() or result.stdout.strip() or "unknown error")
            )
        return result.stdout

    def query_json(
        self, database: Path, sql: str, *, readonly: bool = True
    ) -> list[Dict[str, Any]]:
        output = self.execute(
            database, sql, readonly=readonly, json_output=True
        ).strip()
        if not output:
            return []
        try:
            decoded = json.loads(output)
        except json.JSONDecodeError as exc:
            raise FusionError(f"DuckDB returned invalid JSON: {exc}") from exc
        if not isinstance(decoded, list):
            raise FusionError("DuckDB JSON query did not return a row array")
        return [dict(row) for row in decoded]

    def version(self) -> str:
        result = subprocess.run(
            [str(self.executable), "--version"],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise FusionError("unable to read DuckDB version")
        return result.stdout.strip()


@contextmanager
def _warehouse_lock(path: Path) -> Iterator[None]:
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+") as handle:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise FusionError(f"another fusion build holds {lock_path}") from exc
        yield


def _consistent_sqlite_copy(source: Path, destination: Path) -> Path:
    source = source.expanduser().resolve()
    if not source.is_file():
        raise FusionError(f"SQLite source does not exist: {source}")
    uri = source.as_uri() + "?mode=ro"
    reader = sqlite3.connect(uri, uri=True, timeout=30.0)
    writer = sqlite3.connect(str(destination), timeout=30.0)
    try:
        reader.execute("PRAGMA busy_timeout=30000")
        reader.backup(writer, pages=4096)
        writer.commit()
    finally:
        writer.close()
        reader.close()
    return destination


def _source_snapshot(
    *,
    source_key: str,
    content_sha256: str,
    fingerprint_mode: str,
    captured_at: str,
    byte_size: int,
    row_count: int,
    high_watermark: Optional[str],
    schema_version: str,
    source_uri: Optional[str],
    provenance: Mapping[str, Any],
) -> SourceSnapshot:
    return SourceSnapshot(
        snapshot_id=_snapshot_id(source_key, content_sha256),
        source_key=source_key,
        content_sha256=content_sha256,
        fingerprint_mode=fingerprint_mode,
        captured_at=captured_at,
        ingested_at=_utc_now(),
        byte_size=max(0, int(byte_size)),
        row_count=max(0, int(row_count)),
        high_watermark=high_watermark,
        schema_version=schema_version,
        source_uri=source_uri,
        artifact_uri=None,
        provenance_json=_canonical_json(dict(provenance)),
    )


def _export_baseline(
    source: Path, copy_path: Path, staging: StagingArea
) -> SourceSnapshot:
    source = source.expanduser().resolve()
    _consistent_sqlite_copy(source, copy_path)
    content_sha = _sha256_file(copy_path)
    snapshot_id = _snapshot_id("dsco_baseline", content_sha)
    tables = ["baseline_instance", "baseline_event", "baseline_trace_span"]
    connection = sqlite3.connect(str(copy_path))
    connection.text_factory = lambda value: value.decode("utf-8", "surrogateescape")
    connection.row_factory = sqlite3.Row
    try:
        high_water = connection.execute("SELECT MAX(id) FROM events").fetchone()[0]
        for row in connection.execute(
            "SELECT instance_id,parent_instance_id,pid,model,mode,started_at,ended_at "
            "FROM instances ORDER BY instance_id"
        ):
            staging.write(
                "baseline_instance",
                {
                    "source_snapshot_id": snapshot_id,
                    "instance_id": row["instance_id"],
                    "parent_instance_id": row["parent_instance_id"],
                    "pid": _integer(row["pid"]),
                    "model_id": _model_id(row["model"]),
                    "mode": row["mode"],
                    "started_at": _epoch_iso(row["started_at"]),
                    "ended_at": _epoch_iso(row["ended_at"]),
                },
            )
        for row in connection.execute(
            "SELECT id,instance_id,ts,ts_epoch,category,title,detail,metadata_json,"
            "input_tokens,output_tokens,cache_read_tokens,cache_write_tokens,"
            "est_cost_usd FROM events ORDER BY id"
        ):
            metadata = _json_object(row["metadata_json"])
            model = _model_id(metadata.get("model"))
            staging.write(
                "baseline_event",
                {
                    "source_snapshot_id": snapshot_id,
                    "event_id": _integer(row["id"]),
                    "instance_id": row["instance_id"],
                    "event_at": _epoch_iso(row["ts"]),
                    "event_epoch": _finite(row["ts_epoch"]),
                    "category": row["category"],
                    "title": row["title"],
                    "detail_sha256": _sha256_text(row["detail"]),
                    "metadata_sha256": _sha256_text(row["metadata_json"]),
                    "provider_id": _provider(metadata.get("provider"), model),
                    "model_id": model,
                    "auth_mode": metadata.get("auth_mode"),
                    "billing_mode": metadata.get("billing"),
                    "input_tokens": _integer(row["input_tokens"]),
                    "output_tokens": _integer(row["output_tokens"]),
                    "cache_read_tokens": _integer(row["cache_read_tokens"]),
                    "cache_write_tokens": _integer(row["cache_write_tokens"]),
                    "estimated_cost_usd": _finite(row["est_cost_usd"]),
                },
            )
        for row in connection.execute(
            "SELECT span_id,trace_id,parent_span,name,start_epoch,end_epoch,status,"
            "metadata_json FROM trace_spans ORDER BY span_id"
        ):
            staging.write(
                "baseline_trace_span",
                {
                    "source_snapshot_id": snapshot_id,
                    "span_id": row["span_id"],
                    "trace_id": row["trace_id"],
                    "parent_span_id": row["parent_span"],
                    "name": row["name"],
                    "started_at": _epoch_iso(row["start_epoch"]),
                    "ended_at": _epoch_iso(row["end_epoch"]),
                    "status": row["status"],
                    "metadata_sha256": _sha256_text(row["metadata_json"]),
                },
            )
    finally:
        connection.close()
    return _source_snapshot(
        source_key="dsco_baseline",
        content_sha256=content_sha,
        fingerprint_mode="sqlite_backup_sha256",
        captured_at=_file_time(source),
        byte_size=copy_path.stat().st_size,
        row_count=staging.total(tables),
        high_watermark=str(high_water) if high_water is not None else None,
        schema_version="baseline-sqlite-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "sqlite_online_backup",
            "raw_detail_retained": False,
            "raw_metadata_retained": False,
        },
    )


def _usage_value(usage: Mapping[str, Any], *names: str) -> Optional[int]:
    for name in names:
        value = usage.get(name)
        if value is not None:
            return _integer(value)
    return None


def _export_chronicle(
    source: Path, copy_path: Path, staging: StagingArea
) -> SourceSnapshot:
    source = source.expanduser().resolve()
    _consistent_sqlite_copy(source, copy_path)
    content_sha = _sha256_file(copy_path)
    snapshot_id = _snapshot_id("dsco_chronicle", content_sha)
    tables = [
        "chronicle_session",
        "chronicle_event",
        "chronicle_span",
        "chronicle_blob",
        "chronicle_edge",
        "chronicle_training_example",
    ]
    connection = sqlite3.connect(str(copy_path))
    connection.text_factory = lambda value: value.decode("utf-8", "surrogateescape")
    connection.row_factory = sqlite3.Row
    try:
        high_water = connection.execute("SELECT MAX(wall_time) FROM events").fetchone()[
            0
        ]
        for row in connection.execute("SELECT * FROM sessions ORDER BY session_id"):
            staging.write(
                "chronicle_session",
                {
                    "source_snapshot_id": snapshot_id,
                    "session_id": row["session_id"],
                    "installation_id": row["installation_id"],
                    "instance_id": row["instance_id"],
                    "provider_id": _provider(row["provider"], _model_id(row["model"])),
                    "model_id": _model_id(row["model"]),
                    "mode": row["mode"],
                    "started_at": _epoch_iso(row["started_at"]),
                    "ended_at": _epoch_iso(row["ended_at"]),
                    "policy_sha256": _sha256_text(row["policy_json"]),
                },
            )
        for row in connection.execute("SELECT * FROM events ORDER BY event_id"):
            payload = _json_object(row["payload_json"])
            usage = payload.get("usage")
            usage = dict(usage) if isinstance(usage, Mapping) else {}
            model = _model_id(payload.get("model"))
            staging.write(
                "chronicle_event",
                {
                    "source_snapshot_id": snapshot_id,
                    "event_id": row["event_id"],
                    "installation_id": row["installation_id"],
                    "session_id": row["session_id"],
                    "trace_id": row["trace_id"],
                    "span_id": row["span_id"],
                    "parent_span_id": row["parent_span_id"],
                    "sequence_number": _integer(row["seq"]),
                    "event_at": _epoch_iso(row["wall_time"]),
                    "event_type": row["event_type"],
                    "actor_type": row["actor_type"],
                    "actor_id": row["actor_id"],
                    "payload_hash": row["payload_hash"]
                    or _sha256_text(row["payload_json"]),
                    "event_hash": row["event_hash"],
                    "sensitivity": row["sensitivity"],
                    "sync_state": row["sync_state"],
                    "provider_id": _provider(payload.get("provider"), model),
                    "model_id": model,
                    "tool_name": payload.get("tool_name") or payload.get("tool"),
                    "generation_id_sha256": _sha256_text(payload.get("generation_id")),
                    "finish_reason": payload.get("finish_reason"),
                    "ok": _boolean(payload.get("ok")),
                    "latency_ms": _finite(payload.get("latency_ms")),
                    "cost_usd": _finite(payload.get("cost_usd")),
                    "input_tokens": _usage_value(
                        usage, "input_tokens", "prompt_tokens"
                    ),
                    "output_tokens": _usage_value(
                        usage, "output_tokens", "completion_tokens"
                    ),
                    "cache_read_tokens": _usage_value(
                        usage, "cache_read_tokens", "cached_tokens"
                    ),
                    "cache_write_tokens": _usage_value(usage, "cache_write_tokens"),
                    "request_blob_sha256": payload.get("request_blob_sha256"),
                    "output_blob_sha256": payload.get("output_blob_sha256"),
                },
            )
        for row in connection.execute("SELECT * FROM spans ORDER BY span_id"):
            staging.write(
                "chronicle_span",
                {
                    "source_snapshot_id": snapshot_id,
                    "span_id": row["span_id"],
                    "trace_id": row["trace_id"],
                    "parent_span_id": row["parent_span_id"],
                    "span_type": row["span_type"],
                    "name": row["name"],
                    "started_at": _epoch_iso(row["started_at"]),
                    "ended_at": _epoch_iso(row["ended_at"]),
                    "status": row["status"],
                    "payload_sha256": _sha256_text(row["payload_json"]),
                },
            )
        for row in connection.execute("SELECT * FROM blobs ORDER BY sha256"):
            staging.write(
                "chronicle_blob",
                {
                    "source_snapshot_id": snapshot_id,
                    "sha256": row["sha256"],
                    "byte_length": _integer(row["byte_len"]) or 0,
                    "content_type": row["content_type"],
                    "logical_type": row["logical_type"],
                    "codec": row["codec"],
                    "encryption": row["encryption"],
                    "sensitivity": row["sensitivity"],
                    "created_at": _epoch_iso(row["created_at"]),
                },
            )
        for row in connection.execute("SELECT * FROM edges ORDER BY edge_id"):
            staging.write(
                "chronicle_edge",
                {
                    "source_snapshot_id": snapshot_id,
                    "edge_id": row["edge_id"],
                    "from_id": row["from_id"],
                    "to_id": row["to_id"],
                    "relation": row["relation"],
                    "confidence": _finite(row["confidence"]),
                    "metadata_sha256": _sha256_text(row["metadata_json"]),
                    "created_at": _epoch_iso(row["created_at"]),
                },
            )
        for row in connection.execute(
            "SELECT * FROM training_examples ORDER BY example_id"
        ):
            staging.write(
                "chronicle_training_example",
                {
                    "source_snapshot_id": snapshot_id,
                    "example_id": row["example_id"],
                    "source_trace_id": row["source_trace_id"],
                    "task_type": row["task_type"],
                    "dataset_type": row["dataset_type"],
                    "quality_score": _finite(row["quality_score"]),
                    "consent_state": row["consent_state"],
                    "redaction_state": row["redaction_state"],
                    "input_blob_sha256": row["input_blob"],
                    "output_blob_sha256": row["output_blob"],
                    "label_blob_sha256": row["label_blob"],
                    "metadata_sha256": _sha256_text(row["metadata_json"]),
                    "created_at": _epoch_iso(row["created_at"]),
                },
            )
    finally:
        connection.close()
    return _source_snapshot(
        source_key="dsco_chronicle",
        content_sha256=content_sha,
        fingerprint_mode="sqlite_backup_sha256",
        captured_at=_file_time(source),
        byte_size=copy_path.stat().st_size,
        row_count=staging.total(tables),
        high_watermark=str(high_water) if high_water is not None else None,
        schema_version="chronicle-sqlite-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "sqlite_online_backup",
            "raw_payload_retained": False,
            "blob_paths_retained": False,
        },
    )


def _export_evals(
    source: Path, copy_path: Path, staging: StagingArea
) -> SourceSnapshot:
    source = source.expanduser().resolve()
    _consistent_sqlite_copy(source, copy_path)
    content_sha = _sha256_file(copy_path)
    snapshot_id = _snapshot_id("openrouter_local_evals", content_sha)
    tables = ["openrouter_eval_model", "openrouter_eval"]
    connection = sqlite3.connect(str(copy_path))
    connection.text_factory = lambda value: value.decode("utf-8", "surrogateescape")
    connection.row_factory = sqlite3.Row
    try:
        high_water = connection.execute("SELECT MAX(id) FROM evals").fetchone()[0]
        for row in connection.execute("SELECT * FROM models ORDER BY id"):
            model = _model_id(row["id"])
            staging.write(
                "openrouter_eval_model",
                {
                    "source_snapshot_id": snapshot_id,
                    "model_id": model,
                    "name": row["name"],
                    "provider_id": _provider(row["provider"], model),
                    "created_at": _epoch_iso(row["created"]),
                    "context_length": _integer(row["context_length"]),
                    "prompt_price_usd_token": _finite(row["prompt_price"]),
                    "completion_price_usd_token": _finite(row["completion_price"]),
                    "last_seen_at": _epoch_iso(row["last_seen"]),
                },
            )
        for row in connection.execute("SELECT * FROM evals ORDER BY id"):
            staging.write(
                "openrouter_eval",
                {
                    "source_snapshot_id": snapshot_id,
                    "eval_id": _integer(row["id"]),
                    "model_id": _model_id(row["model_id"]),
                    "run_at": _epoch_iso(row["run_date"]),
                    "tier": _integer(row["tier"]),
                    "target_tokens": _integer(row["target_tokens"]),
                    "actual_tokens": _integer(row["actual_tokens"]),
                    "latency_ms": _finite(row["latency_ms"]),
                    "http_status": _integer(row["http_status"]),
                    "ok": _boolean(row["ok"]),
                    "snippet_sha256": _sha256_text(row["snippet"]),
                    "error_sha256": _sha256_text(row["error_msg"]),
                },
            )
    finally:
        connection.close()
    return _source_snapshot(
        source_key="openrouter_local_evals",
        content_sha256=content_sha,
        fingerprint_mode="sqlite_backup_sha256",
        captured_at=_file_time(source),
        byte_size=copy_path.stat().st_size,
        row_count=staging.total(tables),
        high_watermark=str(high_water) if high_water is not None else None,
        schema_version="openrouter-evals-sqlite-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "sqlite_online_backup",
            "raw_snippets_retained": False,
            "raw_errors_retained": False,
        },
    )


def _catalog_models(payload: Any) -> list[Mapping[str, Any]]:
    if isinstance(payload, Mapping):
        data = payload.get("data")
        if isinstance(data, list):
            return [item for item in data if isinstance(item, Mapping)]
    if isinstance(payload, list):
        return [item for item in payload if isinstance(item, Mapping)]
    raise FusionError("catalog must be an array or an object containing data[]")


def _catalog_price(pricing: Mapping[str, Any], *keys: str) -> Optional[str]:
    for key in keys:
        if key in pricing:
            value = pricing.get(key)
            if value is None or isinstance(value, bool):
                return None
            try:
                number = Decimal(str(value))
            except (InvalidOperation, ValueError):
                return None
            if not number.is_finite():
                return None
            return format(number, "f")
    return None


def _required_nonnegative_decimal(value: Any, *, field: str, row_label: str) -> str:
    if value is None or isinstance(value, bool):
        raise FusionError(f"{row_label} requires numeric {field}")
    try:
        number = Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise FusionError(f"{row_label} has invalid {field}") from exc
    if not number.is_finite() or number < 0:
        raise FusionError(f"{row_label} has negative or non-finite {field}")
    return format(number, "f")


def _positive_integer_or_none(
    value: Any, *, field: str, row_label: str
) -> Optional[int]:
    if value is None:
        return None
    result = _integer(value)
    if result is None or result <= 0:
        raise FusionError(f"{row_label} has invalid {field}")
    return result


def _export_catalog(source: Path, staging: StagingArea) -> SourceSnapshot:
    source = source.expanduser().resolve()
    raw = source.read_bytes()
    content_sha = _sha256_bytes(raw)
    source_key = "openrouter_catalog"
    snapshot_id = _snapshot_id(source_key, content_sha)
    try:
        payload = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FusionError(f"invalid catalog JSON {source}: {exc}") from exc
    models = _catalog_models(payload)
    captured_at = _file_time(source)
    tables = ["catalog_model_snapshot", "model_alias", "benchmark_observation"]
    starting_rows = staging.total(tables)
    for descriptor in sorted(models, key=lambda item: str(item.get("id", ""))):
        model = _model_id(descriptor.get("id"))
        if not model:
            continue
        architecture = descriptor.get("architecture")
        architecture = dict(architecture) if isinstance(architecture, Mapping) else {}
        top_provider = descriptor.get("top_provider")
        top_provider = dict(top_provider) if isinstance(top_provider, Mapping) else {}
        pricing = descriptor.get("pricing")
        pricing = dict(pricing) if isinstance(pricing, Mapping) else {}
        raw_record = _canonical_json(descriptor)
        canonical_slug = _model_id(descriptor.get("canonical_slug"))
        alias_target = _model_id(descriptor.get("alias_target"))
        provider_id = _provider(None, canonical_slug or model)
        staging.write(
            "catalog_model_snapshot",
            {
                "source_snapshot_id": snapshot_id,
                "captured_at": captured_at,
                "model_id": model,
                "canonical_slug": canonical_slug,
                "alias_target": alias_target,
                "provider_id": provider_id,
                "name": descriptor.get("name"),
                "description": descriptor.get("description"),
                "created_at": _epoch_iso(descriptor.get("created")),
                "expiration_at": _epoch_iso(descriptor.get("expiration_date")),
                "knowledge_cutoff": descriptor.get("knowledge_cutoff"),
                "hugging_face_id": descriptor.get("hugging_face_id"),
                "context_length": _integer(
                    descriptor.get("context_length")
                    or top_provider.get("context_length")
                ),
                "max_completion_tokens": _integer(
                    top_provider.get("max_completion_tokens")
                ),
                "tokenizer": architecture.get("tokenizer"),
                "instruct_type": architecture.get("instruct_type"),
                "input_modalities_json": _canonical_json(
                    architecture.get("input_modalities") or []
                ),
                "output_modalities_json": _canonical_json(
                    architecture.get("output_modalities") or []
                ),
                "supported_parameters_json": _canonical_json(
                    descriptor.get("supported_parameters") or []
                ),
                "default_parameters_json": _canonical_json(
                    descriptor.get("default_parameters") or {}
                ),
                "prompt_price_usd_token": _catalog_price(pricing, "prompt"),
                "completion_price_usd_token": _catalog_price(pricing, "completion"),
                "cache_read_price_usd_token": _catalog_price(
                    pricing, "input_cache_read", "cache_read"
                ),
                "cache_write_price_usd_token": _catalog_price(
                    pricing, "input_cache_write", "cache_write"
                ),
                "image_price_usd": _catalog_price(pricing, "image"),
                "request_price_usd": _catalog_price(pricing, "request"),
                "is_moderated": _boolean(top_provider.get("is_moderated")),
                "raw_record_sha256": _sha256_text(raw_record),
            },
        )
        if canonical_slug and canonical_slug != model:
            staging.write(
                "model_alias",
                {
                    "source_snapshot_id": snapshot_id,
                    "alias_model_id": model,
                    "canonical_model_id": canonical_slug,
                    "alias_kind": "canonical_slug",
                },
            )
        if alias_target and alias_target != model:
            staging.write(
                "model_alias",
                {
                    "source_snapshot_id": snapshot_id,
                    "alias_model_id": model,
                    "canonical_model_id": alias_target,
                    "alias_kind": "alias_target",
                },
            )
        benchmarks = descriptor.get("benchmarks")
        benchmarks = dict(benchmarks) if isinstance(benchmarks, Mapping) else {}
        artificial = benchmarks.get("artificial_analysis")
        artificial = dict(artificial) if isinstance(artificial, Mapping) else {}
        for metric_name, metric_value in sorted(artificial.items()):
            numeric = _finite(metric_value)
            if numeric is None:
                continue
            observation_key = f"{model}\x1fartificial_analysis\x1f{metric_name}"
            staging.write(
                "benchmark_observation",
                {
                    "source_snapshot_id": snapshot_id,
                    "observation_id": _sha256_text(observation_key),
                    "observed_at": captured_at,
                    "model_id": model,
                    "provider_id": provider_id,
                    "benchmark": "openrouter_artificial_analysis",
                    "task": None,
                    "metric_name": str(metric_name),
                    "metric_value": numeric,
                    "metric_unit": "score_0_100",
                    "split": None,
                    "sample_count": None,
                    "standard_error": None,
                    "evaluation_version": None,
                    "provenance_url": "https://openrouter.ai/api/v1/models",
                    "license_spdx": None,
                    "raw_record_sha256": _sha256_text(
                        _canonical_json(
                            {
                                "model_id": model,
                                "metric": metric_name,
                                "value": numeric,
                            }
                        )
                    ),
                    "metadata_json": "{}",
                },
            )
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="file_sha256",
        captured_at=captured_at,
        byte_size=len(raw),
        row_count=staging.total(tables) - starting_rows,
        high_watermark=str(len(models)),
        schema_version="openrouter-model-catalog-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "immutable_file",
            "model_count": len(models),
            "source_api": "https://openrouter.ai/api/v1/models?output_modalities=all",
        },
    )


def _export_feedback(
    source: Path, copy_path: Path, staging: StagingArea
) -> SourceSnapshot:
    source = source.expanduser().resolve()
    _consistent_sqlite_copy(source, copy_path)
    content_sha = _sha256_file(copy_path)
    snapshot_id = _snapshot_id("chimera_router_feedback", content_sha)
    tables = [
        "route_decision",
        "route_candidate_exposure",
        "route_outcome_revision",
        "route_pairwise_comparison",
    ]
    connection = sqlite3.connect(str(copy_path))
    connection.text_factory = lambda value: value.decode("utf-8", "surrogateescape")
    connection.row_factory = sqlite3.Row
    try:
        version = 1
        if _table_exists(connection, "router_schema"):
            row = connection.execute(
                "SELECT MAX(version) FROM router_schema"
            ).fetchone()
            version = int(row[0] or 1)
        if version > 3:
            raise FusionError(
                f"feedback schema {version} is newer than supported schema 3"
            )
        feature_table = _table_exists(connection, "route_request_features")
        feature_join = (
            "LEFT JOIN route_request_features AS feature "
            "ON feature.request_id = decision.request_id"
            if feature_table
            else ""
        )
        feature_columns = (
            ",feature.task_feature_version,feature.task_dim,feature.encoding,"
            "feature.feature_sha256,hex(feature.task_features) AS task_features_hex"
            if feature_table
            else ",NULL AS task_feature_version,NULL AS task_dim,NULL AS encoding,"
            "NULL AS feature_sha256,NULL AS task_features_hex"
        )
        decision_sql = (
            "SELECT decision.*"
            + feature_columns
            + " FROM route_decisions AS decision "
            + feature_join
            + " ORDER BY decision.request_id"
        )
        for row in connection.execute(decision_sql):
            staging.write(
                "route_decision",
                {
                    "source_snapshot_id": snapshot_id,
                    "request_id": row["request_id"],
                    "decided_at": _epoch_iso(row["decided_at"]),
                    "request_hash": row["request_hash"],
                    "tenant_hash": row["tenant_hash"],
                    "policy_id": row["policy_id"],
                    "checkpoint_id": row["checkpoint_id"],
                    "catalog_snapshot_id": row["catalog_snapshot_id"],
                    "explicit_override": row["explicit_override"],
                    "constraints_json": row["constraints_json"] or "{}",
                    "chosen_model": _model_id(row["chosen_model"]),
                    "chosen_provider": _provider(
                        row["chosen_provider"], _model_id(row["chosen_model"])
                    ),
                    "chosen_score": _finite(row["chosen_score"]),
                    "confidence": _finite(row["confidence"]),
                    "logging_propensity": _finite(row["logging_propensity"]),
                    "fallback_chain_json": row["fallback_chain_json"] or "[]",
                    "task_feature_version": row["task_feature_version"],
                    "task_dimension": _integer(row["task_dim"]),
                    "task_feature_encoding": row["encoding"],
                    "task_feature_sha256": row["feature_sha256"],
                    "task_features_hex": row["task_features_hex"],
                },
            )
        if _table_exists(connection, "candidate_exposures"):
            for row in connection.execute(
                "SELECT * FROM candidate_exposures ORDER BY request_id,rank"
            ):
                model = _model_id(row["model_id"])
                staging.write(
                    "route_candidate_exposure",
                    {
                        "source_snapshot_id": snapshot_id,
                        "request_id": row["request_id"],
                        "rank": _integer(row["rank"]),
                        "model_id": model,
                        "provider_id": _provider(row["provider"], model),
                        "feasible": bool(row["feasible"]),
                        "rejection_reason": row["rejection_reason"],
                        "predicted_quality": _finite(row["predicted_quality"]),
                        "predicted_failure": _finite(row["predicted_failure"]),
                        "predicted_latency_ms": _finite(row["predicted_latency_ms"]),
                        "predicted_cost_usd": _finite(row["predicted_cost_usd"]),
                        "score": _finite(row["score"]),
                        "logging_propensity": _finite(row["logging_propensity"]),
                        "chosen": bool(row["chosen"]),
                    },
                )
        high_water: Optional[int] = None
        if _table_exists(connection, "route_outcome_revisions"):
            revision_rows = connection.execute(
                "SELECT * FROM route_outcome_revisions ORDER BY revision_id"
            )
            for row in revision_rows:
                high_water = max(high_water or 0, int(row["revision_id"]))
                _write_outcome_revision(staging, snapshot_id, row)
        elif _table_exists(connection, "route_outcomes"):
            for synthetic_id, row in enumerate(
                connection.execute("SELECT * FROM route_outcomes ORDER BY request_id"),
                start=1,
            ):
                projected = dict(row)
                projected.update(
                    {
                        "revision_id": synthetic_id,
                        "revision_number": 1,
                        "recorded_at": row["completed_at"],
                        "supersedes_revision_id": None,
                    }
                )
                high_water = synthetic_id
                _write_outcome_revision(staging, snapshot_id, projected)
        if _table_exists(connection, "pairwise_comparisons"):
            for row in connection.execute(
                "SELECT * FROM pairwise_comparisons ORDER BY comparison_id"
            ):
                staging.write(
                    "route_pairwise_comparison",
                    {
                        "source_snapshot_id": snapshot_id,
                        "comparison_id": row["comparison_id"],
                        "request_hash": row["request_hash"],
                        "catalog_snapshot_id": row["catalog_snapshot_id"],
                        "model_a": _model_id(row["model_a"]),
                        "model_b": _model_id(row["model_b"]),
                        "winner": row["winner"],
                        "score_a": _finite(row["score_a"]),
                        "score_b": _finite(row["score_b"]),
                        "source": row["source"],
                        "confidence": _finite(row["confidence"]),
                        "created_at": _epoch_iso(row["created_at"]),
                    },
                )
    finally:
        connection.close()
    return _source_snapshot(
        source_key="chimera_router_feedback",
        content_sha256=content_sha,
        fingerprint_mode="sqlite_backup_sha256",
        captured_at=_file_time(source),
        byte_size=copy_path.stat().st_size,
        row_count=staging.total(tables),
        high_watermark=str(high_water) if high_water is not None else None,
        schema_version=f"chimera-feedback-sqlite-v{version}",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "sqlite_online_backup",
            "outcome_revision_high_water": high_water,
            "raw_prompt_retained": False,
            "task_features_retained": feature_table,
            "task_features_are_sensitive": feature_table,
        },
    )


def _write_outcome_revision(
    staging: StagingArea,
    snapshot_id: str,
    row: Mapping[str, Any],
) -> None:
    staging.write(
        "route_outcome_revision",
        {
            "source_snapshot_id": snapshot_id,
            "revision_id": _integer(row["revision_id"]),
            "request_id": row["request_id"],
            "revision_number": _integer(row["revision_number"]),
            "recorded_at": _epoch_iso(row["recorded_at"]),
            "supersedes_revision_id": _integer(row["supersedes_revision_id"]),
            "completed_at": _epoch_iso(row["completed_at"]),
            "intended_model": _model_id(row["intended_model"]),
            "actual_model": _model_id(row["actual_model"]),
            "actual_provider": _provider(
                row["actual_provider"], _model_id(row["actual_model"])
            ),
            "generation_id_sha256": _sha256_text(row["generation_id"]),
            "http_success": _boolean(row["http_success"]),
            "task_success": _boolean(row["task_success"]),
            "provider_failure": _boolean(row["provider_failure"]),
            "tool_valid": _boolean(row["tool_valid"]),
            "schema_valid": _boolean(row["schema_valid"]),
            "refusal": _boolean(row["refusal"]),
            "user_retry": _boolean(row["user_retry"]),
            "accepted_or_used": _boolean(row["accepted_or_used"]),
            "prompt_tokens": _integer(row["prompt_tokens"]),
            "completion_tokens": _integer(row["completion_tokens"]),
            "reasoning_tokens": _integer(row["reasoning_tokens"]),
            "cache_read_tokens": _integer(row["cache_read_tokens"]),
            "cache_write_tokens": _integer(row["cache_write_tokens"]),
            "cost_usd": _finite(row["cost_usd"]),
            "ttft_ms": _finite(row["ttft_ms"]),
            "e2e_ms": _finite(row["e2e_ms"]),
            "finish_reason": row["finish_reason"],
            "label_source": row["label_source"],
            "label_confidence": _finite(row["label_confidence"]),
            "censored": bool(row["censored"]),
        },
    )


def _runtime_record(
    snapshot_id: str,
    source_name: str,
    raw: bytes,
    value: Mapping[str, Any],
) -> Optional[Dict[str, Any]]:
    observed_at = _epoch_iso(value.get("ts"))
    if not observed_at:
        return None
    return {
        "source_snapshot_id": snapshot_id,
        "source_record_sha256": _sha256_bytes(
            source_name.encode("utf-8", "surrogatepass") + b"\x00" + raw
        ),
        "observed_at": observed_at,
        "pid": _integer(value.get("pid")),
        "parent_pid": _integer(value.get("ppid")),
        "sequence_number": _integer(value.get("seq")),
        "event": value.get("event"),
        "phase": value.get("phase"),
        "signal": value.get("sig"),
        "supervised": value.get("supervised"),
        "cpu_user_ms": _integer(value.get("cpu_user_ms")),
        "cpu_system_ms": _integer(value.get("cpu_sys_ms")),
        "rss_mb": _finite(value.get("rss_mb")),
        "peak_rss_mb": _finite(value.get("peak_rss_mb") or value.get("maxrss_mb")),
        "rss_delta_mb": _finite(value.get("rss_delta_mb")),
        "memory_pressure": _integer(value.get("mem_pressure")),
        "memory_restart": _integer(value.get("mem_restart")),
        "thread_count": _integer(value.get("thread_count")),
        "fd_count": _integer(value.get("fd_count")),
        "voluntary_switches": _integer(value.get("ctx_switches_vol")),
        "involuntary_switches": _integer(value.get("ctx_switches_invol")),
        "minor_faults": _integer(value.get("minor_faults")),
        "major_faults": _integer(value.get("major_faults")),
        "uptime_s": _finite(value.get("uptime_s")),
    }


def _child_metric_record(
    snapshot_id: str,
    source_name: str,
    raw: bytes,
    line_number: int,
    value: Mapping[str, Any],
) -> Optional[Dict[str, Any]]:
    observed_at = _epoch_iso(value.get("ts"))
    if not observed_at:
        return None
    record_hash = _sha256_bytes(
        source_name.encode("utf-8", "surrogatepass")
        + b"\x00"
        + raw
        + b"\x1fline="
        + str(line_number).encode("ascii")
    )
    return {
        "source_snapshot_id": snapshot_id,
        "source_record_sha256": record_hash,
        "observed_at": observed_at,
        "child_pid": _integer(value.get("child_pid")),
        "supervisor_pid": _integer(value.get("supervisor_pid")),
        "rss_mb": _finite(value.get("rss_mb")),
        "peak_rss_mb": _finite(value.get("peak_rss_mb")),
        "memory_pressure": _integer(value.get("mem_pressure")),
        "uptime_s": _finite(value.get("uptime_s")),
    }


def _export_metric_glob(
    root: Path,
    pattern: str,
    source_key: str,
    target_table: str,
    staging: StagingArea,
) -> Optional[SourceSnapshot]:
    paths = sorted(
        root.expanduser().resolve().glob(pattern), key=lambda path: path.name
    )
    if not paths:
        return None
    digest = hashlib.sha256()
    total_bytes = 0
    malformed = 0
    minimum_ts: Optional[str] = None
    maximum_ts: Optional[str] = None
    # The snapshot ID is content-addressed, so hash the full source set before
    # emitting rows carrying that ID.  This costs one extra sequential read but
    # avoids a weak mtime-only fingerprint for operational telemetry.
    for path in paths:
        raw = path.read_bytes()
        total_bytes += len(raw)
        file_hash = _sha256_bytes(raw)
        digest.update(path.name.encode("utf-8", "surrogatepass"))
        digest.update(b"\x00")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\n")
    content_sha = digest.hexdigest()
    snapshot_id = _snapshot_id(source_key, content_sha)
    for path in paths:
        raw = path.read_bytes()
        if target_table == "runtime_process_metric":
            try:
                decoded = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError):
                malformed += 1
                continue
            if not isinstance(decoded, Mapping):
                malformed += 1
                continue
            record = _runtime_record(snapshot_id, path.name, raw, decoded)
            if record is None:
                malformed += 1
                continue
            staging.write(target_table, record)
            observed = record["observed_at"]
            minimum_ts = min(minimum_ts, observed) if minimum_ts else observed
            maximum_ts = max(maximum_ts, observed) if maximum_ts else observed
        else:
            for line_number, line in enumerate(raw.splitlines(), start=1):
                if not line.strip():
                    continue
                try:
                    decoded = json.loads(line)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    malformed += 1
                    continue
                if not isinstance(decoded, Mapping):
                    malformed += 1
                    continue
                record = _child_metric_record(
                    snapshot_id, path.name, line, line_number, decoded
                )
                if record is None:
                    malformed += 1
                    continue
                staging.write(target_table, record)
                observed = record["observed_at"]
                minimum_ts = min(minimum_ts, observed) if minimum_ts else observed
                maximum_ts = max(maximum_ts, observed) if maximum_ts else observed
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_file_content_sha256",
        captured_at=max((_file_time(path) for path in paths), default=_utc_now()),
        byte_size=total_bytes,
        row_count=staging.counts.get(target_table, 0),
        high_watermark=maximum_ts,
        schema_version=f"{source_key}-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "ordered_filename_and_content_hash",
            "file_count": len(paths),
            "malformed_records": malformed,
            "minimum_observed_at": minimum_ts,
            "maximum_observed_at": maximum_ts,
            "excluded_fields": ["cmdline", "cwd", "detail", "exe", "node"],
        },
    )


def _content_addressed_paths(
    paths: Sequence[Path], root: Path
) -> tuple[str, int, Dict[Path, str]]:
    digest = hashlib.sha256()
    total_bytes = 0
    file_hashes: Dict[Path, str] = {}
    for path in paths:
        raw = path.read_bytes()
        total_bytes += len(raw)
        file_sha = _sha256_bytes(raw)
        file_hashes[path] = file_sha
        try:
            identity = path.relative_to(root).as_posix()
        except ValueError:
            identity = path.name
        digest.update(identity.encode("utf-8", "surrogatepass"))
        digest.update(b"\x00")
        digest.update(file_sha.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest(), total_bytes, file_hashes


def _fingerprint_file_set(paths: Sequence[Path], root: Path) -> tuple[str, int, str]:
    """Fingerprint many files without retaining a per-path hash dictionary."""
    digest = hashlib.sha256()
    total_bytes = 0
    latest_mtime = 0.0
    for path in paths:
        raw = path.read_bytes()
        total_bytes += len(raw)
        file_sha = _sha256_bytes(raw)
        try:
            identity = path.relative_to(root).as_posix()
        except ValueError:
            identity = path.name
        digest.update(identity.encode("utf-8", "surrogatepass"))
        digest.update(b"\x00")
        digest.update(file_sha.encode("ascii"))
        digest.update(b"\n")
        latest_mtime = max(latest_mtime, path.stat().st_mtime)
    return (
        digest.hexdigest(),
        total_bytes,
        _epoch_iso(latest_mtime) or _utc_now(),
    )


def _export_incident_metrics(
    incidents_dir: Path, staging: StagingArea
) -> Optional[SourceSnapshot]:
    root = incidents_dir.expanduser().resolve()
    paths = sorted(root.glob("*.json"), key=lambda path: path.name)
    if not paths:
        return None
    source_key = "dsco_incident_metrics"
    content_sha, total_bytes, captured_at = _fingerprint_file_set(paths, root)
    snapshot_id = _snapshot_id(source_key, content_sha)
    malformed = 0
    minimum_ts: Optional[str] = None
    maximum_ts: Optional[str] = None
    starting_rows = staging.counts.get("runtime_incident", 0)
    for path in paths:
        raw = path.read_bytes()
        try:
            value = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            malformed += 1
            continue
        if not isinstance(value, Mapping):
            malformed += 1
            continue
        observed_at = _epoch_iso(value.get("ts"))
        if observed_at is None:
            malformed += 1
            continue
        staging.write(
            "runtime_incident",
            {
                "source_snapshot_id": snapshot_id,
                "source_record_sha256": _sha256_bytes(
                    b"dsco:incident-record:v1\x00"
                    + path.relative_to(root).as_posix().encode("utf-8", "surrogatepass")
                    + b"\x00"
                    + _sha256_bytes(raw).encode("ascii")
                ),
                "observed_at": observed_at,
                "supervisor_pid": _integer(value.get("supervisor_pid")),
                "child_pid": _integer(value.get("child_pid")),
                "incident_class": value.get("class"),
                "signal_name": value.get("signal"),
                "signal_number": _integer(value.get("signal_num")),
                "exit_code": _integer(value.get("exit_code")),
                "action": value.get("action"),
                "restart_count": _integer(value.get("restart_count")),
                "next_delay_ms": _integer(value.get("next_delay_ms")),
                "uptime_s": _finite(value.get("uptime_s")),
                "peak_rss_mb": _finite(value.get("peak_rss_mb")),
                "memory_budget_mb": _finite(value.get("mem_budget_mb")),
                "memory_soft_limit_mb": _finite(value.get("mem_soft_mb")),
                "memory_pressure": _integer(value.get("mem_pressure")),
                "poll_ms": _integer(value.get("poll_ms")),
                "preempted": _boolean(value.get("preempted")),
                "tracer_reaped": _boolean(value.get("tracer_reaped")),
                "resume_after_crash": _boolean(value.get("resume_after_crash")),
                "memory_restart": _boolean(value.get("mem_restart")),
                "last_heartbeat_pid": _integer(value.get("last_heartbeat_pid")),
                "last_heartbeat_age_s": _finite(value.get("last_heartbeat_age_s")),
                "last_heartbeat_pid_matches_child": _boolean(
                    value.get("last_heartbeat_pid_matches_child")
                ),
                "crash_log_present": _boolean(value.get("crash_log_present")),
                "debugger_backtrace_present": _boolean(
                    value.get("debugger_backtrace_present")
                ),
            },
        )
        minimum_ts = min(minimum_ts, observed_at) if minimum_ts else observed_at
        maximum_ts = max(maximum_ts, observed_at) if maximum_ts else observed_at
    row_count = staging.counts.get("runtime_incident", 0) - starting_rows
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_relative_path_and_content_sha256",
        captured_at=maximum_ts or captured_at,
        byte_size=total_bytes,
        row_count=row_count,
        high_watermark=maximum_ts,
        schema_version="dsco-runtime-incident-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "streamed_ordered_relative_path_and_content_hash",
            "file_count": len(paths),
            "malformed_or_empty_files": malformed,
            "minimum_observed_at": minimum_ts,
            "maximum_observed_at": maximum_ts,
            "excluded_fields": [
                "child_cmdline",
                "paths",
                "last_heartbeat",
                "crash_log_excerpt",
                "debugger_backtrace_excerpt",
            ],
            "raw_process_and_crash_text_retained": False,
        },
    )


def _fingerprint_swarm_results(
    paths: Sequence[Path], root: Path
) -> tuple[str, int, Dict[Path, str], Dict[Path, int], str]:
    digest = hashlib.sha256()
    total_bytes = 0
    file_hashes: Dict[Path, str] = {}
    mtimes_ns: Dict[Path, int] = {}
    latest_mtime_ns = 0
    for path in paths:
        raw = path.read_bytes()
        relative_path = path.relative_to(root).as_posix()
        file_sha = _sha256_bytes(raw)
        mtime_ns = path.stat().st_mtime_ns
        file_hashes[path] = file_sha
        mtimes_ns[path] = mtime_ns
        total_bytes += len(raw)
        latest_mtime_ns = max(latest_mtime_ns, mtime_ns)
        digest.update(relative_path.encode("utf-8", "surrogatepass"))
        digest.update(b"\x00")
        digest.update(file_sha.encode("ascii"))
        digest.update(b"\x00")
        digest.update(str(mtime_ns).encode("ascii"))
        digest.update(b"\n")
    return (
        digest.hexdigest(),
        total_bytes,
        file_hashes,
        mtimes_ns,
        _epoch_iso(latest_mtime_ns / 1_000_000_000) or _utc_now(),
    )


def _export_swarm_child_results(
    swarm_dir: Path, staging: StagingArea
) -> Optional[SourceSnapshot]:
    root = swarm_dir.expanduser().resolve()
    paths = sorted(
        (
            path
            for path in root.glob("*/child-*.RESULT.json")
            if path.is_file() and not path.is_symlink()
        ),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    if not paths:
        return None
    source_key = "dsco_swarm_child_results"
    (
        content_sha,
        total_bytes,
        file_hashes,
        mtimes_ns,
        snapshot_captured_at,
    ) = _fingerprint_swarm_results(paths, root)
    snapshot_id = _snapshot_id(source_key, content_sha)
    malformed = 0
    maximum_ts: Optional[str] = None
    starting_rows = staging.counts.get("swarm_child_result", 0)
    for path in paths:
        raw = path.read_bytes()
        try:
            value = json.loads(raw, parse_float=Decimal)
        except (UnicodeDecodeError, json.JSONDecodeError):
            malformed += 1
            continue
        if not isinstance(value, Mapping):
            malformed += 1
            continue
        child_match = re.fullmatch(r"child-(\d+)\.RESULT\.json", path.name)
        parent_pid = _integer(path.parent.name)
        child_id = _integer(value.get("id"))
        task = value.get("task")
        output = value.get("output")
        status_value = value.get("status")
        exit_code = _integer(value.get("exit_code"))
        try:
            duration = Decimal(str(value.get("duration_s")))
            cost = Decimal(str(value.get("cost_usd")))
        except InvalidOperation:
            malformed += 1
            continue
        if (
            child_match is None
            or parent_pid is None
            or child_id is None
            or child_id != int(child_match.group(1))
            or not isinstance(task, str)
            or not isinstance(output, str)
            or not isinstance(status_value, str)
            or not status_value.strip()
            or exit_code is None
            or not duration.is_finite()
            or duration < 0
            or not cost.is_finite()
            or cost < 0
        ):
            malformed += 1
            continue
        status = status_value.strip().casefold()
        if status not in {"done", "error", "killed"}:
            malformed += 1
            continue
        model_value = value.get("model")
        requested_model_id = (
            model_value.strip() if isinstance(model_value, str) else None
        )
        requested_model_id = requested_model_id or None
        mtime_ns = mtimes_ns[path]
        completed_at = _epoch_iso(mtime_ns / 1_000_000_000) or _utc_now()
        duration_ms = int((duration * 1000).quantize(Decimal("1")))
        started_at = _epoch_iso(mtime_ns / 1_000_000_000 - float(duration))
        cost_six_places = cost.quantize(Decimal("0.000001"))
        task_bytes = task.encode("utf-8", "surrogatepass")
        output_bytes = output.encode("utf-8", "surrogatepass")
        task_maybe_truncated = len(task) >= 127 or len(task_bytes) >= 127
        output_maybe_truncated = len(output_bytes) >= 524287
        process_success = status == "done" and exit_code == 0
        status_exit_consistent = process_success or (
            status in {"error", "killed"} and exit_code != 0
        )
        likely_degenerate_row = (
            status == "done"
            and not output_bytes
            and duration_ms < 100
            and cost_six_places == 0
        )
        relative_path = path.relative_to(root).as_posix()
        source_record_sha = _sha256_bytes(
            b"dsco:swarm-record:v1\x00"
            + relative_path.encode("utf-8", "surrogatepass")
            + b"\x00"
            + file_hashes[path].encode("ascii")
        )
        staging.write(
            "swarm_child_result",
            {
                "source_snapshot_id": snapshot_id,
                "source_record_sha256": source_record_sha,
                "source_file_sha256": file_hashes[path],
                "swarm_run_key_sha256": _sha256_bytes(
                    b"dsco:swarm-run:v1\x00"
                    + path.parent.name.encode("utf-8", "surrogatepass")
                ),
                "completed_at_approx": completed_at,
                "started_at_approx": started_at,
                "source_file_mtime_ns": mtime_ns,
                "timestamp_basis": "file_mtime",
                "parent_pid": parent_pid,
                "child_id": child_id,
                "id_matches_filename": True,
                "task_sha256": _sha256_bytes(b"dsco:swarm-task:v1\x00" + task_bytes),
                "task_utf8_bytes": len(task_bytes),
                "task_maybe_truncated": task_maybe_truncated,
                "requested_model_id": requested_model_id,
                "normalized_model_id": requested_model_id.casefold()
                if requested_model_id
                else None,
                "model_basis": "parent_requested_unverified",
                "status": status,
                "exit_code": exit_code,
                "process_success": process_success,
                "outcome_kind": "process_exit_only",
                "duration_ms": duration_ms,
                "cost_usd_recorded": format(cost_six_places, "f"),
                "cost_basis": "reported_or_estimated_undisclosed",
                "cost_nonzero": cost_six_places > 0,
                "output_present": bool(output_bytes),
                "output_sha256": _sha256_bytes(output_bytes) if output_bytes else None,
                "output_utf8_bytes": len(output_bytes),
                "output_maybe_truncated": output_maybe_truncated,
                "operational_label_usable": status_exit_consistent
                and not task_maybe_truncated
                and not likely_degenerate_row,
                "semantic_label_usable": False,
            },
        )
        maximum_ts = max(maximum_ts, completed_at) if maximum_ts else completed_at
    row_count = staging.counts.get("swarm_child_result", 0) - starting_rows
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_relative_path_content_and_mtime_sha256",
        captured_at=snapshot_captured_at,
        byte_size=total_bytes,
        row_count=row_count,
        high_watermark=maximum_ts,
        schema_version="dsco-swarm-child-result-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "ordered_relative_path_content_and_mtime_hash",
            "file_count": len(paths),
            "malformed_or_mismatched_records": malformed,
            "mutable_last_write_wins_slot_files": True,
            "raw_tasks_outputs_errors_retained": False,
            "comparison_group": "domain_separated_task_sha256",
            "timestamp_basis": "file_mtime_approximation",
            "model_basis": "parent_requested_unverified",
            "cost_basis": "reported_or_estimated_undisclosed",
            "semantic_correctness_available": False,
        },
    )


def _export_transcript_metrics(
    sessions_dir: Path, staging: StagingArea
) -> Optional[SourceSnapshot]:
    root = sessions_dir.expanduser().resolve()
    paths = sorted(
        (
            path
            for path in root.glob("transcript-*.jsonl")
            if path.name != "transcript-last.jsonl" and not path.is_symlink()
        ),
        key=lambda path: path.name,
    )
    if not paths:
        return None
    source_key = "dsco_transcript_metrics"
    content_sha, total_bytes, file_hashes = _content_addressed_paths(paths, root)
    snapshot_id = _snapshot_id(source_key, content_sha)
    malformed = 0
    maximum_ts: Optional[str] = None
    pid_pattern = re.compile(r"^transcript-(\d+)\.jsonl$")
    starting_rows = staging.counts.get("transcript_turn_metric", 0)
    for path in paths:
        match = pid_pattern.match(path.name)
        source_pid = int(match.group(1)) if match else None
        file_sha = file_hashes[path]
        for line_number, line in enumerate(path.read_bytes().splitlines(), start=1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except (UnicodeDecodeError, json.JSONDecodeError):
                malformed += 1
                continue
            if not isinstance(row, Mapping):
                malformed += 1
                continue
            observed_at = _epoch_iso(row.get("ts"))
            if not observed_at:
                malformed += 1
                continue
            model = _model_id(row.get("model"))
            record_sha = _sha256_bytes(
                file_sha.encode("ascii")
                + b"\x1f"
                + str(line_number).encode("ascii")
                + b"\x1f"
                + line
            )
            staging.write(
                "transcript_turn_metric",
                {
                    "source_snapshot_id": snapshot_id,
                    "source_record_sha256": record_sha,
                    "source_file_sha256": file_sha,
                    "source_row_number": line_number,
                    "source_pid": source_pid,
                    "observed_at": observed_at,
                    "turn_number": _integer(row.get("turn")),
                    "model_id": model,
                    "provider_id": _provider(None, model),
                    "input_tokens": _integer(row.get("in")),
                    "output_tokens": _integer(row.get("out")),
                    "cache_read_tokens": _integer(row.get("cache_read")),
                    "cache_write_tokens": _integer(row.get("cache_write")),
                    "cost_usd": _finite(row.get("cost")),
                    "stop_reason": row.get("stop"),
                },
            )
            maximum_ts = max(maximum_ts, observed_at) if maximum_ts else observed_at
    row_count = staging.counts.get("transcript_turn_metric", 0) - starting_rows
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_file_content_sha256",
        captured_at=max((_file_time(path) for path in paths), default=_utc_now()),
        byte_size=total_bytes,
        row_count=row_count,
        high_watermark=maximum_ts,
        schema_version="dsco-transcript-metrics-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "ordered_relative_path_and_content_hash",
            "file_count": len(paths),
            "malformed_records": malformed,
            "raw_prompt_response_retained": False,
            "symlink_aliases_excluded": True,
        },
    )


def _export_tool_traces(
    traces_dir: Path, staging: StagingArea
) -> Optional[SourceSnapshot]:
    root = traces_dir.expanduser().resolve()
    paths = sorted(root.glob("*/tools.jsonl"), key=lambda path: path.as_posix())
    if not paths:
        return None
    source_key = "dsco_tool_traces"
    content_sha, total_bytes, file_hashes = _content_addressed_paths(paths, root)
    snapshot_id = _snapshot_id(source_key, content_sha)
    malformed = 0
    maximum_ts: Optional[str] = None
    session_pattern = re.compile(r"^sess_(\d+)_(\d+)$")
    starting_rows = staging.counts.get("tool_trace_metric", 0)
    for path in paths:
        file_sha = file_hashes[path]
        match = session_pattern.match(path.parent.name)
        if match:
            session_started_at = _epoch_iso(match.group(1))
            source_pid = int(match.group(2))
            trace_session_key = path.parent.name
        else:
            session_started_at = None
            source_pid = None
            trace_session_key = "fallback:" + _sha256_text(path.parent.name)[:24]
        for line_number, line in enumerate(path.read_bytes().splitlines(), start=1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except (UnicodeDecodeError, json.JSONDecodeError):
                malformed += 1
                continue
            if not isinstance(row, Mapping):
                malformed += 1
                continue
            observed_at = _epoch_iso(row.get("ts"))
            tool_name = str(row.get("tool") or "").strip()
            if not observed_at or not tool_name:
                malformed += 1
                continue
            record_sha = _sha256_bytes(
                file_sha.encode("ascii")
                + b"\x1f"
                + str(line_number).encode("ascii")
                + b"\x1f"
                + line
            )
            staging.write(
                "tool_trace_metric",
                {
                    "source_snapshot_id": snapshot_id,
                    "source_record_sha256": record_sha,
                    "source_file_sha256": file_sha,
                    "source_row_number": line_number,
                    "trace_session_key": trace_session_key,
                    "source_pid": source_pid,
                    "session_started_at": session_started_at,
                    "observed_at": observed_at,
                    "tool_name": tool_name,
                    "ok": _boolean(row.get("ok")),
                    "latency_ms": _finite(row.get("ms")),
                    "token_count": _integer(row.get("tok")),
                },
            )
            maximum_ts = max(maximum_ts, observed_at) if maximum_ts else observed_at
    row_count = staging.counts.get("tool_trace_metric", 0) - starting_rows
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_file_content_sha256",
        captured_at=max((_file_time(path) for path in paths), default=_utc_now()),
        byte_size=total_bytes,
        row_count=row_count,
        high_watermark=maximum_ts,
        schema_version="dsco-tool-trace-metrics-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "ordered_relative_path_and_content_hash",
            "file_count": len(paths),
            "malformed_records": malformed,
            "raw_tool_args_results_retained": False,
            "deduplication": "deduplicated_tool_trace view retains first exact metric row",
        },
    )


def _same_path(value: Any, expected: Path) -> bool:
    if not isinstance(value, str) or not value.strip():
        return False
    try:
        return Path(value).expanduser().resolve() == expected.expanduser().resolve()
    except (OSError, RuntimeError):
        return False


def _export_run_manifests(
    runs_dir: Path, staging: StagingArea
) -> tuple[Optional[SourceSnapshot], set[str]]:
    root = runs_dir.expanduser().resolve()
    paths = sorted(root.glob("*/manifest.json"), key=lambda path: path.parent.name)
    if not paths:
        return None, set()
    primary_root = Path("~/.dsco/chronicle").expanduser().resolve()
    primary_db = primary_root / "indexes/chronicle.sqlite"
    selected: list[tuple[Path, Mapping[str, Any], bytes]] = []
    invalid = 0
    alternate = 0
    for path in paths:
        raw = path.read_bytes()
        try:
            manifest = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            invalid += 1
            continue
        if not isinstance(manifest, Mapping):
            invalid += 1
            continue
        if not (
            _same_path(manifest.get("chronicle_root"), primary_root)
            and _same_path(manifest.get("db_path"), primary_db)
        ):
            alternate += 1
            continue
        run_id = str(manifest.get("run_id") or "").strip()
        if not run_id or run_id != path.parent.name:
            invalid += 1
            continue
        selected.append((path, manifest, raw))
    if not selected:
        return None, set()
    source_key = "dsco_run_manifests"
    digest = hashlib.sha256()
    total_bytes = 0
    for path, _manifest, raw in selected:
        total_bytes += len(raw)
        digest.update(path.parent.name.encode("utf-8"))
        digest.update(b"\x00")
        digest.update(_sha256_bytes(raw).encode("ascii"))
        digest.update(b"\n")
    content_sha = digest.hexdigest()
    snapshot_id = _snapshot_id(source_key, content_sha)
    maximum_ts: Optional[str] = None
    run_ids: set[str] = set()
    starting_rows = staging.counts.get("run_manifest_snapshot", 0)
    for _path, manifest, raw in selected:
        run_id = str(manifest["run_id"])
        run_ids.add(run_id)
        updated_at = _epoch_iso(manifest.get("updated_at"))
        started_at = _epoch_iso(manifest.get("started_at"))
        maximum_ts = (
            max(
                maximum_ts or updated_at or started_at or "",
                updated_at or started_at or "",
            )
            or maximum_ts
        )
        staging.write(
            "run_manifest_snapshot",
            {
                "source_snapshot_id": snapshot_id,
                "run_id": run_id,
                "session_id": manifest.get("session_id"),
                "installation_id": manifest.get("installation_id"),
                "status": manifest.get("status"),
                "started_at": started_at,
                "ended_at": _epoch_iso(manifest.get("ended_at")),
                "updated_at": updated_at,
                "schema_version": str(manifest.get("schema_version") or ""),
                "capture_mode": manifest.get("capture_mode"),
                "instance_id": manifest.get("instance_id"),
                "sequence_high_water": _integer(manifest.get("seq")),
                "manifest_sha256": _sha256_bytes(raw),
                "primary_chronicle": True,
            },
        )
    row_count = staging.counts.get("run_manifest_snapshot", 0) - starting_rows
    return (
        _source_snapshot(
            source_key=source_key,
            content_sha256=content_sha,
            fingerprint_mode="ordered_run_id_and_content_sha256",
            captured_at=max(
                (_file_time(path) for path, _manifest, _raw in selected),
                default=_utc_now(),
            ),
            byte_size=total_bytes,
            row_count=row_count,
            high_watermark=maximum_ts,
            schema_version="dsco-run-manifest-v1",
            source_uri=_safe_source_uri(root),
            provenance={
                "snapshot_method": "ordered_run_id_and_content_hash",
                "primary_manifest_count": len(selected),
                "alternate_test_manifests_excluded": alternate,
                "invalid_manifests_excluded": invalid,
                "path_fields_retained": False,
            },
        ),
        run_ids,
    )


def _mapping(value: Any) -> Dict[str, Any]:
    return dict(value) if isinstance(value, Mapping) else {}


def _first_value(*values: Any) -> Any:
    for value in values:
        if value is not None:
            return value
    return None


def _wal_event_row(
    *,
    snapshot_id: str,
    file_sha: str,
    frame_offset: int,
    frame_length: int,
    frame_crc32: int,
    payload_bytes: bytes,
    outer: Mapping[str, Any],
) -> Dict[str, Any]:
    envelope = _mapping(outer.get("payload"))
    inner = _mapping(envelope.get("payload"))
    event_type = str(outer.get("type") or "")
    event_name = envelope.get("event")
    if not inner and event_type in {
        "RUN_START",
        "RUN_END",
        "run.started",
        "run.completed",
        "run.receipt",
        "tool.call",
        "tool.result",
        "turn.started",
        "turn.checkpoint",
    }:
        inner = envelope
    agent = _mapping(envelope.get("agent"))
    trace = _mapping(envelope.get("trace"))
    principal = _mapping(envelope.get("principal"))
    envelope_cost = _mapping(envelope.get("cost"))
    inner_cost = _mapping(inner.get("cost"))
    cost = {**envelope_cost, **inner_cost}
    callbacks = _mapping(inner.get("callbacks"))
    model = _model_id(_first_value(agent.get("model"), inner.get("model")))
    provider = _provider(agent.get("provider"), model)
    return {
        "source_snapshot_id": snapshot_id,
        "source_file_sha256": file_sha,
        "frame_offset": frame_offset,
        "frame_length": frame_length,
        "frame_crc32": frame_crc32,
        "record_version": _integer(outer.get("v")),
        "event_type": event_type,
        "run_id": str(outer.get("run_id") or ""),
        "sequence_number": _integer(outer.get("seq")),
        "event_at": _epoch_iso(outer.get("wall_ms")),
        "payload_sha256": _sha256_bytes(payload_bytes),
        "event_schema": envelope.get("schema"),
        "event_id": envelope.get("event_id"),
        "event_name": event_name,
        "status": _first_value(envelope.get("status"), inner.get("status")),
        "severity": envelope.get("severity"),
        "phase": envelope.get("phase"),
        "category": envelope.get("category"),
        "principal_subject_sha256": _sha256_text(principal.get("subject")),
        "principal_tier": principal.get("tier"),
        "agent_model": model,
        "agent_provider": provider,
        "agent_topology": agent.get("topology"),
        "agent_worker_id": agent.get("worker_id"),
        "trace_id": trace.get("trace_id"),
        "span_id": trace.get("span_id"),
        "parent_span_id": trace.get("parent_span_id"),
        "cost_usd_delta": _finite(cost.get("usd_delta")),
        "cost_usd_total": _finite(cost.get("usd_total")),
        "input_tokens": _integer(
            _first_value(cost.get("input_tokens"), inner.get("input_tokens"))
        ),
        "output_tokens": _integer(
            _first_value(cost.get("output_tokens"), inner.get("output_tokens"))
        ),
        "cache_read_tokens": _integer(
            _first_value(cost.get("cache_read_tokens"), inner.get("cache_read"))
        ),
        "cache_write_tokens": _integer(
            _first_value(cost.get("cache_write_tokens"), inner.get("cache_write"))
        ),
        "reasoning_tokens": _integer(cost.get("reasoning_tokens")),
        "tool_name": inner.get("tool"),
        "tool_id": inner.get("tool_id"),
        "tool_ok": _boolean(inner.get("ok")),
        "tool_latency_ms": _finite(inner.get("latency_ms")),
        "tool_cached": _boolean(inner.get("cached")),
        "tool_args_bytes": _integer(inner.get("args_bytes")),
        "tool_result_bytes": _integer(inner.get("result_bytes")),
        "turn_number": _integer(inner.get("turn")),
        "conversation_count": _integer(inner.get("conv_count")),
        "stop_reason": inner.get("stop_reason"),
        "duration_ms": _integer(inner.get("duration_ms")),
        "journal_records": _integer(inner.get("journal_records")),
        "callback_enqueued": _integer(callbacks.get("enqueued")),
        "callback_pending": _integer(callbacks.get("pending")),
        "callback_delivered": _integer(callbacks.get("delivered")),
        "callback_dead_lettered": _integer(callbacks.get("dead_lettered")),
    }


def _export_run_wal(
    runs_dir: Path, primary_run_ids: set[str], staging: StagingArea
) -> Optional[SourceSnapshot]:
    root = runs_dir.expanduser().resolve()
    paths = sorted(
        (
            root / run_id / "journal.wal"
            for run_id in primary_run_ids
            if (root / run_id / "journal.wal").is_file()
        ),
        key=lambda path: path.parent.name,
    )
    if not paths:
        return None
    source_key = "dsco_run_wal"
    content_sha, total_bytes, file_hashes = _content_addressed_paths(paths, root)
    snapshot_id = _snapshot_id(source_key, content_sha)
    corrupt_tails = 0
    valid_frames = 0
    maximum_ts: Optional[str] = None
    starting_rows = staging.counts.get("run_wal_event", 0)
    for path in paths:
        raw = path.read_bytes()
        offset = 0
        while offset < len(raw):
            frame_offset = offset
            if len(raw) - offset < 8:
                corrupt_tails += 1
                break
            frame_length = int.from_bytes(raw[offset : offset + 4], "little")
            expected_crc = int.from_bytes(raw[offset + 4 : offset + 8], "little")
            offset += 8
            if frame_length <= 0 or frame_length > 32 * 1024 * 1024:
                corrupt_tails += 1
                break
            if len(raw) - offset < frame_length:
                corrupt_tails += 1
                break
            payload_bytes = raw[offset : offset + frame_length]
            offset += frame_length
            if zlib.crc32(payload_bytes) & 0xFFFFFFFF != expected_crc:
                corrupt_tails += 1
                break
            try:
                outer = json.loads(payload_bytes)
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise FusionError(
                    f"valid-CRC WAL frame is invalid JSON: {path}"
                ) from exc
            if not isinstance(outer, Mapping):
                raise FusionError(f"valid-CRC WAL frame is not an object: {path}")
            run_id = str(outer.get("run_id") or "")
            if run_id != path.parent.name:
                raise FusionError(
                    f"WAL run_id mismatch: directory={path.parent.name} frame={run_id}"
                )
            row = _wal_event_row(
                snapshot_id=snapshot_id,
                file_sha=file_hashes[path],
                frame_offset=frame_offset,
                frame_length=frame_length,
                frame_crc32=expected_crc,
                payload_bytes=payload_bytes,
                outer=outer,
            )
            if row["sequence_number"] is None or row["event_at"] is None:
                raise FusionError(f"WAL frame lacks sequence or timestamp: {path}")
            staging.write("run_wal_event", row)
            valid_frames += 1
            maximum_ts = (
                max(maximum_ts, row["event_at"]) if maximum_ts else row["event_at"]
            )
    row_count = staging.counts.get("run_wal_event", 0) - starting_rows
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_run_id_and_content_sha256",
        captured_at=max((_file_time(path) for path in paths), default=_utc_now()),
        byte_size=total_bytes,
        row_count=row_count,
        high_watermark=maximum_ts,
        schema_version="dsco-run-wal-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "ordered_run_id_and_content_hash",
            "wal_file_count": len(paths),
            "wal_files_without_primary_manifest_excluded": max(
                0, len(list(root.glob("*/journal.wal"))) - len(paths)
            ),
            "valid_frames": valid_frames,
            "corrupt_or_truncated_tails": corrupt_tails,
            "alternate_test_runs_excluded": True,
            "raw_args_results_payloads_retained": False,
        },
    )


def _contains_forbidden_key(value: Any) -> Optional[str]:
    if isinstance(value, Mapping):
        for key, nested in value.items():
            if str(key).casefold() in FORBIDDEN_EXTERNAL_FIELDS:
                return str(key)
            found = _contains_forbidden_key(nested)
            if found:
                return found
    elif isinstance(value, list):
        for nested in value:
            found = _contains_forbidden_key(nested)
            if found:
                return found
    return None


def _external_rows(path: Path) -> tuple[bytes, list[Mapping[str, Any]]]:
    raw = path.expanduser().resolve().read_bytes()
    rows: list[Mapping[str, Any]] = []
    for line_number, line in enumerate(raw.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            decoded = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise FusionError(
                f"invalid JSONL {path} line {line_number}: {exc}"
            ) from exc
        if not isinstance(decoded, Mapping):
            raise FusionError(f"{path} line {line_number} is not an object")
        forbidden = _contains_forbidden_key(decoded)
        if forbidden:
            raise FusionError(
                f"{path} line {line_number} contains forbidden raw-content field "
                f"{forbidden!r}; derive hashes/features in a governed adapter first"
            )
        rows.append(decoded)
    return raw, rows


def _export_external_benchmarks(source: Path, staging: StagingArea) -> SourceSnapshot:
    raw, records = _external_rows(source)
    if not records:
        raise FusionError(f"benchmark JSONL is empty: {source}")
    source_keys = {str(record.get("source_key", "")).strip() for record in records}
    if len(source_keys) != 1 or not next(iter(source_keys)):
        raise FusionError(
            "benchmark JSONL must contain exactly one nonempty source_key"
        )
    source_key = next(iter(source_keys))
    content_sha = _sha256_bytes(raw)
    snapshot_id = _snapshot_id(source_key, content_sha)
    starting_rows = staging.counts.get("benchmark_observation", 0)
    for index, record in enumerate(records):
        model = _model_id(record.get("model_id"))
        metric_name = str(record.get("metric_name", "")).strip()
        benchmark = str(record.get("benchmark", "")).strip()
        metric_value = _finite(record.get("metric_value"))
        if not model or not metric_name or not benchmark or metric_value is None:
            raise FusionError(
                f"benchmark row {index + 1} requires model_id, benchmark, "
                "metric_name, and finite metric_value"
            )
        raw_record = _canonical_json(record)
        observation_id = str(record.get("observation_id") or "").strip()
        if not observation_id:
            observation_id = (
                _sha256_text(f"{source_key}\x1f{index}\x1f{raw_record}") or ""
            )
        known = {
            "source_key",
            "observation_id",
            "observed_at",
            "model_id",
            "provider_id",
            "benchmark",
            "task",
            "metric_name",
            "metric_value",
            "metric_unit",
            "split",
            "sample_count",
            "standard_error",
            "evaluation_version",
            "provenance_url",
            "license_spdx",
        }
        metadata = {key: value for key, value in record.items() if key not in known}
        staging.write(
            "benchmark_observation",
            {
                "source_snapshot_id": snapshot_id,
                "observation_id": observation_id,
                "observed_at": _epoch_iso(record.get("observed_at")),
                "model_id": model,
                "provider_id": _provider(record.get("provider_id"), model),
                "benchmark": benchmark,
                "task": record.get("task"),
                "metric_name": metric_name,
                "metric_value": metric_value,
                "metric_unit": record.get("metric_unit"),
                "split": record.get("split"),
                "sample_count": _integer(record.get("sample_count")),
                "standard_error": _finite(record.get("standard_error")),
                "evaluation_version": record.get("evaluation_version"),
                "provenance_url": record.get("provenance_url"),
                "license_spdx": record.get("license_spdx"),
                "raw_record_sha256": _sha256_text(raw_record),
                "metadata_json": _canonical_json(metadata),
            },
        )
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="file_sha256",
        captured_at=_file_time(source),
        byte_size=len(raw),
        row_count=staging.counts.get("benchmark_observation", 0) - starting_rows,
        high_watermark=str(len(records)),
        schema_version="normalized-benchmark-jsonl-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "immutable_file",
            "raw_prompt_response_retained": False,
        },
    )


def _export_external_preferences(source: Path, staging: StagingArea) -> SourceSnapshot:
    raw, records = _external_rows(source)
    if not records:
        raise FusionError(f"preference JSONL is empty: {source}")
    source_keys = {str(record.get("source_key", "")).strip() for record in records}
    if len(source_keys) != 1 or not next(iter(source_keys)):
        raise FusionError(
            "preference JSONL must contain exactly one nonempty source_key"
        )
    source_key = next(iter(source_keys))
    content_sha = _sha256_bytes(raw)
    snapshot_id = _snapshot_id(source_key, content_sha)
    starting_rows = staging.counts.get("preference_observation", 0)
    for index, record in enumerate(records):
        model_a = _model_id(record.get("model_a"))
        model_b = _model_id(record.get("model_b"))
        winner = str(record.get("winner", "")).casefold()
        if not model_a or not model_b or winner not in {"a", "b", "tie", "both_bad"}:
            raise FusionError(
                f"preference row {index + 1} requires model_a/model_b and "
                "winner in a,b,tie,both_bad"
            )
        raw_record = _canonical_json(record)
        comparison_id = str(record.get("comparison_id") or "").strip()
        if not comparison_id:
            comparison_id = (
                _sha256_text(f"{source_key}\x1f{index}\x1f{raw_record}") or ""
            )
        known = {
            "source_key",
            "comparison_id",
            "observed_at",
            "request_hash",
            "task_type",
            "domain",
            "language",
            "model_a",
            "model_b",
            "winner",
            "turn_count",
            "sample_weight",
            "confidence",
            "provenance_url",
            "license_spdx",
        }
        metadata = {key: value for key, value in record.items() if key not in known}
        staging.write(
            "preference_observation",
            {
                "source_snapshot_id": snapshot_id,
                "comparison_id": comparison_id,
                "observed_at": _epoch_iso(record.get("observed_at")),
                "request_hash": record.get("request_hash"),
                "task_type": record.get("task_type"),
                "domain": record.get("domain"),
                "language": record.get("language"),
                "model_a": model_a,
                "model_b": model_b,
                "winner": winner,
                "turn_count": _integer(record.get("turn_count")),
                "sample_weight": _finite(record.get("sample_weight")) or 1.0,
                "confidence": _finite(record.get("confidence")),
                "provenance_url": record.get("provenance_url"),
                "license_spdx": record.get("license_spdx"),
                "raw_record_sha256": _sha256_text(raw_record),
                "metadata_json": _canonical_json(metadata),
            },
        )
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="file_sha256",
        captured_at=_file_time(source),
        byte_size=len(raw),
        row_count=staging.counts.get("preference_observation", 0) - starting_rows,
        high_watermark=str(len(records)),
        schema_version="normalized-preference-jsonl-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "immutable_file",
            "raw_prompt_response_retained": False,
        },
    )


def _number_from_nested(value: Any, *keys: str) -> Optional[float]:
    numeric = _finite(value)
    if numeric is not None:
        return numeric
    if isinstance(value, Mapping):
        for key in keys:
            numeric = _finite(value.get(key))
            if numeric is not None:
                return numeric
    return None


def _export_endpoint_snapshot(source: Path, staging: StagingArea) -> SourceSnapshot:
    source = source.expanduser().resolve()
    raw = source.read_bytes()
    content_sha = _sha256_bytes(raw)
    source_key = "openrouter_endpoints"
    snapshot_id = _snapshot_id(source_key, content_sha)
    try:
        payload = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FusionError(f"invalid endpoint JSON {source}: {exc}") from exc
    data = payload.get("data") if isinstance(payload, Mapping) else payload
    default_model: Optional[str] = None
    endpoints: list[Mapping[str, Any]] = []
    if isinstance(data, Mapping):
        default_model = _model_id(data.get("id") or data.get("model_id"))
        possible = data.get("endpoints")
        if isinstance(possible, list):
            endpoints = [item for item in possible if isinstance(item, Mapping)]
        elif all(key in data for key in ("provider", "pricing")):
            endpoints = [data]
    elif isinstance(data, list):
        endpoints = [item for item in data if isinstance(item, Mapping)]
    if not endpoints:
        raise FusionError(f"endpoint snapshot contains no endpoints: {source}")
    captured_at = _file_time(source)
    starting_rows = staging.counts.get("provider_endpoint_snapshot", 0)
    for index, endpoint in enumerate(endpoints):
        model = _model_id(
            endpoint.get("model_id") or endpoint.get("model") or default_model
        )
        provider = _provider(
            endpoint.get("provider_slug")
            or endpoint.get("provider_name")
            or endpoint.get("provider"),
            model,
        )
        if not model or not provider:
            raise FusionError(
                f"endpoint row {index + 1} requires model_id and provider"
            )
        pricing = endpoint.get("pricing")
        pricing = dict(pricing) if isinstance(pricing, Mapping) else {}
        performance = endpoint.get("performance")
        performance = dict(performance) if isinstance(performance, Mapping) else {}
        uptime = endpoint.get("uptime")
        uptime = dict(uptime) if isinstance(uptime, Mapping) else {}
        raw_record = _canonical_json(endpoint)
        endpoint_tag = str(
            endpoint.get("tag") or endpoint.get("endpoint_tag") or "default"
        )
        staging.write(
            "provider_endpoint_snapshot",
            {
                "source_snapshot_id": snapshot_id,
                "captured_at": captured_at,
                "model_id": model,
                "provider_id": provider,
                "endpoint_tag": endpoint_tag,
                "quantization": endpoint.get("quantization"),
                "context_length": _integer(endpoint.get("context_length")),
                "max_prompt_tokens": _integer(endpoint.get("max_prompt_tokens")),
                "max_completion_tokens": _integer(
                    endpoint.get("max_completion_tokens")
                ),
                "prompt_price_usd_token": _catalog_price(pricing, "prompt"),
                "completion_price_usd_token": _catalog_price(pricing, "completion"),
                "uptime_5m": _number_from_nested(
                    endpoint.get("uptime_5m") or uptime.get("5m"), "value", "rate"
                ),
                "uptime_30m": _number_from_nested(
                    endpoint.get("uptime_30m") or uptime.get("30m"), "value", "rate"
                ),
                "uptime_1d": _number_from_nested(
                    endpoint.get("uptime_1d") or uptime.get("1d"), "value", "rate"
                ),
                "latency_30m_ms": _number_from_nested(
                    endpoint.get("latency_30m")
                    or endpoint.get("latency_last_30m")
                    or performance.get("latency_30m"),
                    "p50",
                    "median",
                    "value",
                ),
                "throughput_30m_tps": _number_from_nested(
                    endpoint.get("throughput_30m")
                    or endpoint.get("throughput_last_30m")
                    or performance.get("throughput_30m"),
                    "p50",
                    "median",
                    "value",
                ),
                "status": endpoint.get("status"),
                "supported_parameters_json": _canonical_json(
                    endpoint.get("supported_parameters") or []
                ),
                "raw_record_sha256": _sha256_text(raw_record),
            },
        )
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="file_sha256",
        captured_at=captured_at,
        byte_size=len(raw),
        row_count=(staging.counts.get("provider_endpoint_snapshot", 0) - starting_rows),
        high_watermark=str(len(endpoints)),
        schema_version="openrouter-endpoint-snapshot-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "immutable_file",
            "source_api": "https://openrouter.ai/api/v1/models/:author/:slug/endpoints",
        },
    )


def _string_list(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [str(item) for item in value if isinstance(item, (str, int, float))]


def _safe_scalar_subset(
    value: Mapping[str, Any], fields: Sequence[str]
) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for field in fields:
        candidate = value.get(field)
        if isinstance(candidate, (str, bool, int)):
            result[field] = candidate
        elif isinstance(candidate, float) and math.isfinite(candidate):
            result[field] = candidate
    return result


def _safe_string_mapping(value: Any, *, field: str, row_label: str) -> Dict[str, str]:
    if value is None:
        return {}
    if not isinstance(value, Mapping):
        raise FusionError(f"{row_label} has invalid {field}")
    result: Dict[str, str] = {}
    for key, candidate in value.items():
        if not isinstance(candidate, (str, int, float)) or isinstance(candidate, bool):
            raise FusionError(f"{row_label} has invalid {field}.{key}")
        if isinstance(candidate, float) and not math.isfinite(candidate):
            raise FusionError(f"{row_label} has non-finite {field}.{key}")
        result[str(key)] = str(candidate)
    return result


def _safe_numeric_mapping(
    value: Any, *, field: str, row_label: str
) -> Dict[str, float]:
    if value is None:
        return {}
    if not isinstance(value, Mapping):
        raise FusionError(f"{row_label} has invalid {field}")
    result: Dict[str, float] = {}
    for key, candidate in value.items():
        number = _finite(candidate)
        if number is None or number < 0:
            raise FusionError(f"{row_label} has invalid {field}.{key}")
        result[str(key)] = number
    return result


def _optional_nonnegative_float(
    value: Mapping[str, Any], field: str, *, row_label: str
) -> Optional[float]:
    if field not in value:
        return None
    result = _finite(value.get(field))
    if result is None or result < 0:
        raise FusionError(f"{row_label} has invalid {field}")
    return result


def _export_native_model_catalog(source: Path, staging: StagingArea) -> SourceSnapshot:
    source = source.expanduser().resolve()
    raw = source.read_bytes()
    content_sha = _sha256_bytes(raw)
    source_key = "oh_my_pi_model_catalog"
    snapshot_id = _snapshot_id(source_key, content_sha)
    try:
        payload = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FusionError(f"invalid native model catalog JSON {source}: {exc}") from exc
    if not isinstance(payload, Mapping) or not payload:
        raise FusionError("native model catalog must be a nonempty provider object")

    captured_at = _file_time(source)
    starting_rows = staging.counts.get("native_model_offer_snapshot", 0)
    normalized_id_counts: Dict[tuple[str, str], int] = {}
    provider_count = 0
    dynamic_price_sentinels = 0
    for provider_key, provider_models in sorted(
        payload.items(), key=lambda item: str(item[0])
    ):
        provider_id = str(provider_key).strip()
        if not provider_id or not isinstance(provider_models, Mapping):
            raise FusionError(f"native catalog provider {provider_key!r} is invalid")
        provider_count += 1
        for model_key, raw_descriptor in sorted(
            provider_models.items(), key=lambda item: str(item[0])
        ):
            if not isinstance(raw_descriptor, Mapping):
                raise FusionError(
                    f"native catalog {provider_id}/{model_key} is not an object"
                )
            descriptor = dict(raw_descriptor)
            row_label = f"native catalog {provider_id}/{model_key}"
            model_id = descriptor.get("id")
            descriptor_provider = descriptor.get("provider")
            if not isinstance(model_id, str) or model_id != str(model_key):
                raise FusionError(f"{row_label} key does not match descriptor id")
            if (
                not isinstance(descriptor_provider, str)
                or descriptor_provider != provider_id
            ):
                raise FusionError(
                    f"{row_label} provider does not match parent provider"
                )
            name = descriptor.get("name")
            api = descriptor.get("api")
            if not isinstance(name, str) or not name.strip():
                raise FusionError(f"{row_label} requires name")
            if not isinstance(api, str) or not api.strip():
                raise FusionError(f"{row_label} requires api")
            reasoning = descriptor.get("reasoning")
            if not isinstance(reasoning, bool):
                raise FusionError(f"{row_label} requires boolean reasoning")
            input_modalities = descriptor.get("input")
            if not isinstance(input_modalities, list) or any(
                not isinstance(item, str) or not item.strip()
                for item in input_modalities
            ):
                raise FusionError(f"{row_label} requires string input modalities")
            pricing = descriptor.get("cost")
            if not isinstance(pricing, Mapping):
                raise FusionError(f"{row_label} requires cost object")
            costs: Dict[str, Optional[str]] = {}
            row_uses_dynamic_price = False
            for key in ("input", "output", "cacheRead", "cacheWrite"):
                raw_price = pricing.get(key)
                if (
                    provider_id == "openrouter"
                    and model_id == "openrouter/auto"
                    and key in {"input", "output"}
                    and str(raw_price) == "-1000000"
                ):
                    costs[key] = None
                    row_uses_dynamic_price = True
                    dynamic_price_sentinels += 1
                else:
                    costs[key] = _required_nonnegative_decimal(
                        raw_price, field=f"cost.{key}", row_label=row_label
                    )
            if row_uses_dynamic_price:
                pricing_status = "dynamic_unavailable"
            elif any(
                Decimal(value) > 0 for value in costs.values() if value is not None
            ):
                pricing_status = "quoted_nonzero"
            else:
                pricing_status = "zero_unspecified"
            supports_tools = descriptor.get("supportsTools")
            if "supportsTools" in descriptor and not isinstance(supports_tools, bool):
                raise FusionError(f"{row_label} has invalid supportsTools")
            request_model_id = descriptor.get("requestModelId")
            if request_model_id is not None and not isinstance(request_model_id, str):
                raise FusionError(f"{row_label} has invalid requestModelId")

            thinking_value = descriptor.get("thinking")
            if thinking_value is not None and not isinstance(thinking_value, Mapping):
                raise FusionError(f"{row_label} has invalid thinking")
            thinking = _mapping(thinking_value)
            thinking_efforts_raw = thinking.get("efforts", [])
            if not isinstance(thinking_efforts_raw, list) or any(
                not isinstance(item, str) for item in thinking_efforts_raw
            ):
                raise FusionError(f"{row_label} has invalid thinking.efforts")
            thinking_config = _safe_scalar_subset(
                thinking,
                ("mode", "requiresEffort", "supportsDisplay", "suppressWhenOff"),
            )
            thinking_config["effortRouting"] = _safe_string_mapping(
                thinking.get("effortRouting"),
                field="thinking.effortRouting",
                row_label=row_label,
            )
            thinking_config["effortMap"] = _safe_string_mapping(
                thinking.get("effortMap"),
                field="thinking.effortMap",
                row_label=row_label,
            )
            thinking_config["effortBudgets"] = _safe_numeric_mapping(
                thinking.get("effortBudgets"),
                field="thinking.effortBudgets",
                row_label=row_label,
            )

            compat_value = descriptor.get("compat")
            if compat_value is not None and not isinstance(compat_value, Mapping):
                raise FusionError(f"{row_label} has invalid compat")
            compat = _mapping(compat_value)
            compat_capabilities = _safe_scalar_subset(
                compat,
                (
                    "supportsDeveloperRole",
                    "reasoningContentField",
                    "supportsReasoningEffort",
                    "supportsUsageInStreaming",
                    "supportsStore",
                    "supportsToolChoice",
                    "thinkingFormat",
                    "includeEncryptedReasoning",
                    "requiresReasoningContentForToolCalls",
                    "escapeBuiltinToolNames",
                    "filterReasoningHistory",
                    "omitReasoningEffort",
                    "supportsImageDetailOriginal",
                    "allowsSyntheticReasoningContentForToolCalls",
                    "replayUnsignedThinking",
                    "maxTokensField",
                    "requiresAssistantContentForToolCalls",
                    "supportsForcedToolChoice",
                    "disableAdaptiveThinking",
                    "officialEndpoint",
                    "requiresThinkingEnabled",
                    "requiresToolResultId",
                    "streamIdleTimeoutMs",
                    "supportsEagerToolInputStreaming",
                    "supportsLongCacheRetention",
                    "supportsMidConversationSystem",
                    "supportsSamplingParams",
                    "disableReasoningOnToolChoice",
                ),
            )
            compat_capabilities["reasoningEffortMap"] = _safe_string_mapping(
                compat.get("reasoningEffortMap"),
                field="compat.reasoningEffortMap",
                row_label=row_label,
            )

            remote_value = descriptor.get("remoteCompaction")
            if remote_value is not None and not isinstance(remote_value, Mapping):
                raise FusionError(f"{row_label} has invalid remoteCompaction")
            remote = _mapping(remote_value)
            normalized_key = (provider_id.casefold(), model_id.casefold())
            normalized_id_counts[normalized_key] = (
                normalized_id_counts.get(normalized_key, 0) + 1
            )
            raw_record = _canonical_json(descriptor)
            staging.write(
                "native_model_offer_snapshot",
                {
                    "source_snapshot_id": snapshot_id,
                    "captured_at": captured_at,
                    "inference_provider_id": provider_id,
                    "model_id": model_id,
                    "normalized_model_id": model_id.casefold(),
                    "model_namespace": model_id.split("/", 1)[0]
                    if "/" in model_id
                    else None,
                    "request_model_id": request_model_id,
                    "display_name": name,
                    "api": api,
                    "context_window": _positive_integer_or_none(
                        descriptor.get("contextWindow"),
                        field="contextWindow",
                        row_label=row_label,
                    ),
                    "max_tokens": _positive_integer_or_none(
                        descriptor.get("maxTokens"),
                        field="maxTokens",
                        row_label=row_label,
                    ),
                    "input_modalities_json": _canonical_json(input_modalities),
                    "reasoning": reasoning,
                    "supports_tools_declared": supports_tools,
                    "native_tools_usable": supports_tools is not False,
                    "input_price_usd_million": costs["input"],
                    "output_price_usd_million": costs["output"],
                    "cache_read_price_usd_million": costs["cacheRead"],
                    "cache_write_price_usd_million": costs["cacheWrite"],
                    "pricing_status": pricing_status,
                    "premium_multiplier": _optional_nonnegative_float(
                        descriptor, "premiumMultiplier", row_label=row_label
                    ),
                    "priority": _optional_nonnegative_float(
                        descriptor, "priority", row_label=row_label
                    ),
                    "thinking_mode": thinking.get("mode"),
                    "thinking_efforts_json": _canonical_json(thinking_efforts_raw),
                    "thinking_config_json": _canonical_json(thinking_config),
                    "compat_capabilities_json": _canonical_json(compat_capabilities),
                    "apply_patch_tool_type": descriptor.get("applyPatchToolType"),
                    "omit_max_output_tokens": _boolean(
                        descriptor.get("omitMaxOutputTokens")
                    ),
                    "prefer_websockets": _boolean(descriptor.get("preferWebsockets")),
                    "context_promotion_target": descriptor.get(
                        "contextPromotionTarget"
                    ),
                    "reasoning_mode": descriptor.get("reasoningMode"),
                    "use_responses_lite": _boolean(descriptor.get("useResponsesLite")),
                    "remote_compaction_enabled": _boolean(remote.get("enabled")),
                    "remote_compaction_v2_streaming": _boolean(
                        remote.get("v2StreamingEnabled")
                    ),
                    "remote_compaction_api": remote.get("api"),
                    "raw_record_sha256": _sha256_text(raw_record),
                },
            )

    row_count = staging.counts.get("native_model_offer_snapshot", 0) - starting_rows
    normalized_case_collisions = sum(
        count - 1 for count in normalized_id_counts.values() if count > 1
    )
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="file_sha256",
        captured_at=captured_at,
        byte_size=len(raw),
        row_count=row_count,
        high_watermark=f"providers={provider_count};offers={row_count}",
        schema_version="oh-my-pi-model-catalog-v1",
        source_uri=_safe_source_uri(source),
        provenance={
            "snapshot_method": "immutable_generated_catalog_file",
            "provider_count": provider_count,
            "offer_count": row_count,
            "cost_unit": "usd_per_million_tokens",
            "case_sensitive_identity": ["inference_provider_id", "model_id"],
            "normalized_case_collision_rows": normalized_case_collisions,
            "zero_price_semantics": "unresolved_free_or_unknown",
            "dynamic_price_sentinel_fields_mapped_to_null": dynamic_price_sentinels,
            "excluded_fields": [
                "baseUrl",
                "headers",
                "compat.extraBody",
                "compat.signingEndpoint",
                "free_text",
            ],
            "raw_excluded_fields_retained": False,
        },
    )


def _export_provider_metadata(
    providers_dir: Path, staging: StagingArea
) -> Optional[SourceSnapshot]:
    root = providers_dir.expanduser().resolve()
    paths = sorted(root.glob("*.json"), key=lambda path: path.name)
    if not paths:
        return None
    source_key = "dsco_provider_metadata"
    content_sha, total_bytes, file_hashes = _content_addressed_paths(paths, root)
    snapshot_id = _snapshot_id(source_key, content_sha)
    starting_rows = staging.counts.get("provider_capability_snapshot", 0)
    seen: set[str] = set()
    last_reviewed_values: list[str] = []
    for path in paths:
        try:
            descriptor = json.loads(path.read_bytes())
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise FusionError(f"invalid provider metadata JSON {path}: {exc}") from exc
        if not isinstance(descriptor, Mapping):
            raise FusionError(f"provider metadata is not an object: {path}")
        provider_id = str(descriptor.get("provider") or "").strip().casefold()
        if not provider_id or provider_id in seen:
            raise FusionError(f"invalid or duplicate provider ID in {path}")
        seen.add(provider_id)
        reviewed = str(descriptor.get("last_reviewed") or "")
        if reviewed:
            if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", reviewed):
                raise FusionError(
                    f"provider metadata has invalid review date in {path}"
                )
            last_reviewed_values.append(reviewed)
        dsco = _mapping(descriptor.get("dsco"))
        confidence = _finite(descriptor.get("confidence"))
        risk = dsco.get("risk")
        if confidence is None or not 0 <= confidence <= 1:
            raise FusionError(f"provider metadata has invalid confidence in {path}")
        if risk not in {"low", "medium", "high"}:
            raise FusionError(f"provider metadata has invalid risk in {path}")
        wire_api_value = descriptor.get("wire_apis")
        if not isinstance(wire_api_value, list) or any(
            not isinstance(item, Mapping) for item in wire_api_value
        ):
            raise FusionError(f"provider metadata has invalid wire_apis in {path}")
        wire_apis = [_mapping(item) for item in wire_api_value]
        input_modalities: set[str] = set()
        output_modalities: set[str] = set()
        streaming_supported = False
        tools_supported = False
        parallel_tools = False
        strict_schema = False
        for wire_index, wire in enumerate(wire_apis, start=1):
            modalities_value = wire.get("modalities")
            streaming_value = wire.get("streaming")
            tools_value = wire.get("tools")
            if not isinstance(modalities_value, Mapping):
                raise FusionError(
                    f"provider metadata {path} wire API {wire_index} lacks modalities"
                )
            if not isinstance(streaming_value, Mapping):
                raise FusionError(
                    f"provider metadata {path} wire API {wire_index} lacks streaming"
                )
            if not isinstance(tools_value, Mapping):
                raise FusionError(
                    f"provider metadata {path} wire API {wire_index} lacks tools"
                )
            modalities = _mapping(modalities_value)
            streaming_capability = _mapping(streaming_value)
            tools = _mapping(tools_value)
            if not isinstance(modalities.get("input"), list) or not isinstance(
                modalities.get("output"), list
            ):
                raise FusionError(
                    f"provider metadata {path} wire API {wire_index} has invalid modalities"
                )
            required_booleans = {
                "streaming.supported": streaming_capability.get("supported"),
                "tools.supported": tools.get("supported"),
                "tools.parallel_tool_calls": tools.get("parallel_tool_calls"),
                "tools.strict_schema": tools.get("strict_schema"),
            }
            invalid_boolean = next(
                (
                    field
                    for field, value in required_booleans.items()
                    if not isinstance(value, bool)
                ),
                None,
            )
            if invalid_boolean:
                raise FusionError(
                    f"provider metadata {path} wire API {wire_index} has invalid "
                    f"{invalid_boolean}"
                )
            input_modalities.update(
                item.casefold() for item in _string_list(modalities.get("input"))
            )
            output_modalities.update(
                item.casefold() for item in _string_list(modalities.get("output"))
            )
            streaming_supported = (
                streaming_supported or streaming_capability["supported"]
            )
            tools_supported = tools_supported or tools["supported"]
            parallel_tools = parallel_tools or tools["parallel_tool_calls"]
            strict_schema = strict_schema or tools["strict_schema"]
        reasoning = _mapping(descriptor.get("reasoning"))
        prompt_cache = _mapping(descriptor.get("prompt_caching"))
        streaming = _mapping(descriptor.get("streaming"))
        cost_management = _mapping(descriptor.get("cost_management"))
        usage = _mapping(descriptor.get("usage_accounting"))
        api_coverage = _mapping(descriptor.get("api_coverage"))
        mechanisms = [
            item
            for item in prompt_cache.get("mechanisms", [])
            if isinstance(item, Mapping)
        ]
        levers = [
            item
            for item in cost_management.get("levers", [])
            if isinstance(item, Mapping)
        ]
        endpoint_groups = [
            item
            for item in api_coverage.get("endpoint_groups", [])
            if isinstance(item, Mapping)
        ]
        hosted_tools = [
            item
            for item in api_coverage.get("hosted_tools", [])
            if isinstance(item, Mapping)
        ]
        staging.write(
            "provider_capability_snapshot",
            {
                "source_snapshot_id": snapshot_id,
                "captured_at": _file_time(path),
                "provider_id": provider_id,
                "display_name": descriptor.get("display_name"),
                "confidence": confidence,
                "last_reviewed_date": reviewed or None,
                "provider_profile": dsco.get("provider_profile"),
                "risk": risk,
                "implemented_features_json": _canonical_json(
                    _string_list(dsco.get("implemented_features"))
                ),
                "missing_features_json": _canonical_json(
                    _string_list(dsco.get("missing_features"))
                ),
                "test_count": len(_string_list(dsco.get("tests"))),
                "wire_api_count": len(wire_apis),
                "supports_text_input": "text" in input_modalities,
                "supports_text_output": "text" in output_modalities,
                "supports_image_input": "image" in input_modalities,
                "supports_image_output": "image" in output_modalities,
                "supports_audio_input": "audio" in input_modalities,
                "supports_audio_output": "audio" in output_modalities,
                "supports_streaming": streaming_supported,
                "supports_tools": tools_supported,
                "supports_parallel_tools": parallel_tools,
                "supports_strict_schema": strict_schema,
                "reasoning_supported": _boolean(reasoning.get("supported")),
                "reasoning_field": reasoning.get("field"),
                "reasoning_efforts_json": _canonical_json(
                    _string_list(reasoning.get("efforts"))
                ),
                "rejects_unsupported_reasoning": _boolean(
                    reasoning.get("unsupported_values_rejected")
                ),
                "prompt_cache_status": prompt_cache.get("status"),
                "prompt_cache_min_tokens": _integer(
                    prompt_cache.get("minimum_cacheable_tokens")
                ),
                "prompt_cache_ttl": prompt_cache.get("ttl"),
                "prompt_cache_mechanism_count": len(mechanisms),
                "request_max_retries": _integer(streaming.get("request_max_retries")),
                "stream_max_retries": _integer(streaming.get("stream_max_retries")),
                "recommended_idle_timeout_ms": _integer(
                    streaming.get("idle_timeout_ms_recommended")
                ),
                "cost_management_status": cost_management.get("status"),
                "cost_lever_count": len(levers),
                "reports_cached_tokens": bool(_string_list(usage.get("cached_tokens"))),
                "reports_reasoning_tokens": bool(
                    _string_list(usage.get("reasoning_tokens"))
                ),
                "reports_audio_tokens": bool(_string_list(usage.get("audio_tokens"))),
                "reports_image_tokens": bool(_string_list(usage.get("image_tokens"))),
                "endpoint_group_count": len(endpoint_groups),
                "hosted_tool_count": len(hosted_tools),
                "raw_record_sha256": file_hashes[path],
            },
        )
    row_count = staging.counts.get("provider_capability_snapshot", 0) - starting_rows
    return _source_snapshot(
        source_key=source_key,
        content_sha256=content_sha,
        fingerprint_mode="ordered_filename_and_content_sha256",
        captured_at=max((_file_time(path) for path in paths), default=_utc_now()),
        byte_size=total_bytes,
        row_count=row_count,
        high_watermark=max(last_reviewed_values, default=None),
        schema_version="dsco-provider-capability-v1",
        source_uri=_safe_source_uri(root),
        provenance={
            "snapshot_method": "ordered_filename_and_content_hash",
            "provider_count": row_count,
            "excluded_objects": [
                "auth",
                "docs",
                "client_side_headers",
                "urls",
                "notes",
                "quirks",
                "free_text_policies",
            ],
            "automatic_catalog_prefix_join": False,
        },
    )


def _load_registry(path: Path) -> list[Dict[str, Any]]:
    try:
        decoded = json.loads(path.expanduser().resolve().read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FusionError(f"cannot read source registry {path}: {exc}") from exc
    rows = decoded.get("sources") if isinstance(decoded, Mapping) else decoded
    if not isinstance(rows, list):
        raise FusionError("source registry must be an array or contain sources[]")
    required = {
        "source_key",
        "display_name",
        "source_class",
        "adapter",
        "format",
        "grain",
        "stable_ids_json",
        "pii_risk",
        "trust_tier",
        "default_enabled",
    }
    result: list[Dict[str, Any]] = []
    seen: set[str] = set()
    for index, candidate in enumerate(rows):
        if not isinstance(candidate, Mapping):
            raise FusionError(f"source registry row {index + 1} is not an object")
        missing = sorted(required - set(candidate))
        if missing:
            raise FusionError(
                f"source registry row {index + 1} is missing {', '.join(missing)}"
            )
        row = dict(candidate)
        source_key = str(row["source_key"]).strip()
        if not source_key or source_key in seen:
            raise FusionError(f"invalid or duplicate source_key {source_key!r}")
        seen.add(source_key)
        stable_ids = row["stable_ids_json"]
        if not isinstance(stable_ids, str):
            stable_ids = _canonical_json(stable_ids)
        else:
            try:
                json.loads(stable_ids)
            except json.JSONDecodeError as exc:
                raise FusionError(
                    f"stable_ids_json for {source_key} is invalid JSON"
                ) from exc
        result.append(
            {
                "source_key": source_key,
                "display_name": str(row["display_name"]),
                "source_class": str(row["source_class"]),
                "adapter": str(row["adapter"]),
                "format": str(row["format"]),
                "grain": str(row["grain"]),
                "stable_ids_json": stable_ids,
                "source_url": row.get("source_url"),
                "license_spdx": row.get("license_spdx"),
                "license_url": row.get("license_url"),
                "pii_risk": str(row["pii_risk"]),
                "trust_tier": str(row["trust_tier"]),
                "default_enabled": bool(row["default_enabled"]),
                "refresh_cadence": row.get("refresh_cadence"),
                "notes": row.get("notes"),
            }
        )
    return result


def _load_staged(
    cli: DuckDBCLI,
    database: Path,
    staging: StagingArea,
    *,
    build_id: str,
    built_at: str,
) -> None:
    schema_sql = SCHEMA_PATH.read_text(encoding="utf-8")
    cli.execute(database, schema_sql)
    statements = [
        "BEGIN TRANSACTION",
        "INSERT INTO warehouse_metadata VALUES "
        f"('schema_version',{_sql_literal(FUSION_SCHEMA_VERSION)}),"
        f"('build_id',{_sql_literal(build_id)}),"
        f"('built_at',{_sql_literal(built_at)}),"
        "('raw_prompt_response_retained','false'),"
        "('oltp_writer','sqlite'),"
        "('olap_reader','duckdb')",
    ]
    for table, path in sorted(staging.paths.items()):
        if staging.counts.get(table, 0) <= 0:
            continue
        statements.append(
            f"INSERT INTO {table} BY NAME SELECT * FROM read_json_auto("
            f"{_sql_literal(str(path))}, format='newline_delimited', "
            "sample_size=-1, maximum_object_size=67108864, union_by_name=true)"
        )
    statements.extend(["COMMIT", "CHECKPOINT"])
    cli.execute(database, ";\n".join(statements) + ";")


@dataclass(frozen=True)
class QualityCheck:
    name: str
    dimension: str
    severity: str
    passed: bool
    observed: Optional[float]
    expected: str
    detail: str


def _scalar(cli: DuckDBCLI, database: Path, sql: str) -> Any:
    rows = cli.query_json(database, sql)
    if not rows or not rows[0]:
        return None
    return next(iter(rows[0].values()))


def _quality_checks(cli: DuckDBCLI, database: Path) -> list[QualityCheck]:
    checks: list[QualityCheck] = []

    def add(
        name: str,
        dimension: str,
        severity: str,
        observed: Any,
        predicate: Any,
        expected: str,
        detail: str,
    ) -> None:
        numeric = _finite(observed)
        checks.append(
            QualityCheck(
                name=name,
                dimension=dimension,
                severity=severity,
                passed=bool(predicate(observed)),
                observed=numeric,
                expected=expected,
                detail=detail,
            )
        )

    snapshots = _scalar(cli, database, "SELECT count(*) AS value FROM source_snapshot")
    add(
        "source_snapshots_present",
        "completeness",
        "error",
        snapshots,
        lambda value: int(value or 0) > 0,
        "> 0",
        "At least one content-addressed source snapshot must be present.",
    )
    unregistered = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM source_snapshot AS snapshot "
        "LEFT JOIN source_registry AS registry USING(source_key) "
        "WHERE registry.source_key IS NULL",
    )
    add(
        "all_snapshots_registered",
        "provenance",
        "error",
        unregistered,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Every ingested source must have ownership, license, grain, and trust metadata.",
    )
    duplicate_catalog = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM (SELECT source_snapshot_id,model_id,count(*) n "
        "FROM catalog_model_snapshot GROUP BY 1,2 HAVING n>1)",
    )
    add(
        "catalog_model_key_unique",
        "uniqueness",
        "error",
        duplicate_catalog,
        lambda value: int(value or 0) == 0,
        "= 0",
        "A model ID may occur only once inside a catalog snapshot.",
    )
    invalid_native_offers = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM native_model_offer_snapshot "
        "WHERE trim(inference_provider_id)='' OR trim(model_id)='' "
        "OR context_window <= 0 OR max_tokens <= 0 "
        "OR input_price_usd_million < 0 OR output_price_usd_million < 0 "
        "OR cache_read_price_usd_million < 0 "
        "OR cache_write_price_usd_million < 0 "
        "OR pricing_status NOT IN "
        "('quoted_nonzero','zero_unspecified','dynamic_unavailable')",
    )
    add(
        "native_model_offer_domains",
        "validity",
        "error",
        invalid_native_offers,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Native provider offers require exact identities, positive limits, and valid pricing semantics.",
    )
    event_orphans = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM baseline_event AS event "
        "LEFT JOIN baseline_instance AS instance "
        "ON event.source_snapshot_id=instance.source_snapshot_id "
        "AND event.instance_id=instance.instance_id "
        "WHERE instance.instance_id IS NULL",
    )
    add(
        "baseline_event_instance_integrity",
        "referential_integrity",
        "error",
        event_orphans,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Baseline events must resolve to an instance in the same SQLite snapshot.",
    )
    candidate_orphans = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM route_candidate_exposure AS candidate "
        "LEFT JOIN route_decision AS decision "
        "ON candidate.source_snapshot_id=decision.source_snapshot_id "
        "AND candidate.request_id=decision.request_id "
        "WHERE decision.request_id IS NULL",
    )
    add(
        "candidate_decision_integrity",
        "referential_integrity",
        "error",
        candidate_orphans,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Every candidate exposure must resolve to its route decision.",
    )
    temporal_leaks = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM route_training_fact "
        "WHERE asof_catalog_captured_at > decided_at",
    )
    add(
        "route_catalog_asof_no_future_leakage",
        "temporal_integrity",
        "error",
        temporal_leaks,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Catalog features joined to a decision may not come from the future.",
    )
    invalid_preferences = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM preference_observation "
        "WHERE model_a=model_b OR winner NOT IN ('a','b','tie','both_bad')",
    )
    add(
        "preference_pair_validity",
        "validity",
        "error",
        invalid_preferences,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Preference rows require two distinct models and a supported winner label.",
    )
    invalid_metrics = _scalar(
        cli,
        database,
        "SELECT (SELECT count(*) FROM openrouter_eval WHERE latency_ms < 0) + "
        "(SELECT count(*) FROM route_outcome_revision WHERE cost_usd < 0 OR "
        "ttft_ms < 0 OR e2e_ms < 0) + "
        "(SELECT count(*) FROM runtime_process_metric WHERE rss_mb < 0 OR "
        "peak_rss_mb < 0 OR uptime_s < 0) + "
        "(SELECT count(*) FROM runtime_incident WHERE uptime_s < 0 "
        "OR peak_rss_mb < 0 OR memory_budget_mb < 0 "
        "OR memory_soft_limit_mb < 0 OR restart_count < 0 "
        "OR next_delay_ms < 0 OR poll_ms < 0) + "
        "(SELECT count(*) FROM swarm_child_result WHERE duration_ms < 0 "
        "OR cost_usd_recorded < 0 OR status NOT IN ('done','error','killed') "
        "OR semantic_label_usable) AS value",
    )
    add(
        "nonnegative_operational_metrics",
        "validity",
        "error",
        invalid_metrics,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Latency, cost, memory, uptime, and retry observations require valid domains.",
    )
    invalid_provider_capabilities = _scalar(
        cli,
        database,
        "SELECT count(*) AS value FROM provider_capability_snapshot "
        "WHERE confidence IS NULL OR confidence < 0 OR confidence > 1 "
        "OR risk IS NULL OR risk NOT IN ('low','medium','high')",
    )
    add(
        "provider_capability_domains",
        "validity",
        "error",
        invalid_provider_capabilities,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Provider confidence must be in [0,1] and risk must be low, medium, or high.",
    )
    wal_orphans = _scalar(
        cli,
        database,
        "SELECT count(DISTINCT wal.run_id) AS value FROM run_wal_event AS wal "
        "LEFT JOIN run_manifest_snapshot AS manifest USING(run_id) "
        "WHERE manifest.run_id IS NULL",
    )
    add(
        "wal_manifest_integrity",
        "referential_integrity",
        "error",
        wal_orphans,
        lambda value: int(value or 0) == 0,
        "= 0",
        "Every governed WAL run must resolve to a primary Chronicle manifest.",
    )
    manifest_count = int(
        _scalar(
            cli,
            database,
            "SELECT count(*) AS value FROM run_manifest_snapshot",
        )
        or 0
    )
    manifest_session_matches = int(
        _scalar(
            cli,
            database,
            "SELECT count(DISTINCT manifest.run_id) AS value "
            "FROM run_manifest_snapshot AS manifest "
            "JOIN chronicle_session AS session ON session.session_id=manifest.session_id",
        )
        or 0
    )
    manifest_session_coverage = (
        manifest_session_matches / manifest_count if manifest_count else 1.0
    )
    add(
        "run_manifest_chronicle_coverage",
        "join_coverage",
        "warning",
        manifest_session_coverage,
        lambda value: float(value or 0.0) >= 0.95,
        ">= 0.95",
        f"{manifest_session_matches} of {manifest_count} primary run manifests join Chronicle sessions.",
    )
    observed_models = int(
        _scalar(
            cli,
            database,
            "SELECT count(DISTINCT normalized_model_id) AS value FROM ("
            "SELECT lower(trim(model_id)) normalized_model_id FROM baseline_event "
            "WHERE model_id IS NOT NULL AND trim(model_id)<>'' "
            "UNION ALL SELECT lower(trim(model_id)) FROM openrouter_eval "
            "WHERE model_id IS NOT NULL AND trim(model_id)<>'')",
        )
        or 0
    )
    covered_models = int(
        _scalar(
            cli,
            database,
            "SELECT count(DISTINCT observed.normalized_model_id) AS value FROM ("
            "SELECT lower(trim(model_id)) normalized_model_id FROM baseline_event "
            "WHERE model_id IS NOT NULL AND trim(model_id)<>'' "
            "UNION SELECT lower(trim(model_id)) FROM openrouter_eval "
            "WHERE model_id IS NOT NULL AND trim(model_id)<>'') observed "
            "JOIN (SELECT DISTINCT lower(trim(model_id)) normalized_model_id "
            "FROM latest_catalog_model) catalog USING(normalized_model_id)",
        )
        or 0
    )
    coverage = covered_models / observed_models if observed_models else 1.0
    add(
        "observed_model_catalog_coverage",
        "join_coverage",
        "warning",
        coverage,
        lambda value: float(value or 0.0) >= 0.50,
        ">= 0.50",
        f"{covered_models} of {observed_models} observed model IDs resolve to the latest catalog.",
    )
    return checks


def _persist_checks(
    cli: DuckDBCLI, database: Path, run_id: str, checks: Sequence[QualityCheck]
) -> None:
    checked_at = _utc_now()
    values = []
    for check in checks:
        observed = "NULL" if check.observed is None else repr(check.observed)
        values.append(
            "("
            + ",".join(
                [
                    _sql_literal(run_id),
                    _sql_literal(checked_at),
                    _sql_literal(check.name),
                    _sql_literal(check.dimension),
                    _sql_literal(check.severity),
                    "TRUE" if check.passed else "FALSE",
                    observed,
                    _sql_literal(check.expected),
                    _sql_literal(check.detail),
                ]
            )
            + ")"
        )
    if values:
        cli.execute(
            database,
            "INSERT INTO data_quality_result VALUES "
            + ",".join(values)
            + "; CHECKPOINT;",
        )


def _summary(cli: DuckDBCLI, database: Path) -> Dict[str, Any]:
    rows = cli.query_json(
        database,
        "SELECT "
        "(SELECT value FROM warehouse_metadata WHERE key='schema_version') schema_version,"
        "(SELECT value FROM warehouse_metadata WHERE key='build_id') build_id,"
        "(SELECT count(*) FROM source_registry) registered_sources,"
        "(SELECT count(*) FROM source_snapshot) ingested_snapshots,"
        "(SELECT coalesce(sum(row_count),0) FROM source_snapshot) declared_source_rows,"
        "(SELECT count(*) FROM baseline_event) baseline_events,"
        "(SELECT count(*) FROM chronicle_event) chronicle_events,"
        "(SELECT count(*) FROM runtime_process_metric) runtime_metrics,"
        "(SELECT count(*) FROM child_process_metric) child_metrics,"
        "(SELECT count(*) FROM runtime_incident) runtime_incidents,"
        "(SELECT count(*) FROM incident_instance_match "
        "WHERE child_instance_id IS NOT NULL) incident_child_pid_matches,"
        "(SELECT count(*) FROM incident_instance_match "
        "WHERE child_instance_id IS NULL AND supervisor_instance_id IS NOT NULL) "
        "incident_supervisor_only_matches,"
        "(SELECT count(*) FROM swarm_child_result) swarm_child_results,"
        "(SELECT count(*) FROM swarm_comparison_group) swarm_comparison_groups,"
        "(SELECT count(*) FROM transcript_turn_metric) transcript_turn_metrics,"
        "(SELECT count(*) FROM tool_trace_metric) tool_trace_metrics,"
        "(SELECT count(*) FROM deduplicated_tool_trace) deduplicated_tool_trace_metrics,"
        "(SELECT count(*) FROM run_manifest_snapshot) run_manifests,"
        "(SELECT count(*) FROM run_wal_event) run_wal_events,"
        "(SELECT count(*) FROM catalog_model_snapshot) catalog_models,"
        "(SELECT count(*) FROM native_model_offer_snapshot) native_model_offers,"
        "(SELECT count(DISTINCT inference_provider_id) "
        "FROM native_model_offer_snapshot) native_catalog_providers,"
        "(SELECT count(*) FROM (SELECT inference_provider_id,normalized_model_id "
        "FROM native_model_offer_snapshot GROUP BY 1,2 HAVING count(*)>1)) "
        "native_normalized_collision_groups,"
        "(SELECT count(*) FROM native_model_offer_snapshot "
        "WHERE pricing_status='zero_unspecified') native_zero_unspecified_prices,"
        "(SELECT count(*) FROM provider_endpoint_snapshot) provider_endpoints,"
        "(SELECT count(*) FROM provider_capability_snapshot) provider_capabilities,"
        "(SELECT count(*) FROM benchmark_observation) benchmark_observations,"
        "(SELECT count(*) FROM preference_observation) preference_observations,"
        "(SELECT count(*) FROM openrouter_eval) local_evals,"
        "(SELECT count(*) FROM route_decision) route_decisions,"
        "(SELECT count(*) FROM route_outcome_revision) outcome_revisions,"
        "(SELECT count(*) FROM model_identity) resolved_model_identities,"
        "(SELECT count(*) FROM model_universe) historical_model_universe,"
        "(SELECT count(*) FROM current_routable_model_universe) "
        "routable_model_universe,"
        "(SELECT count(*) FROM data_quality_result WHERE severity='error' AND NOT passed) failed_errors,"
        "(SELECT count(*) FROM data_quality_result WHERE severity='warning' AND NOT passed) failed_warnings",
    )
    return rows[0] if rows else {}


PARQUET_RELATIONS: tuple[tuple[str, str], ...] = (
    ("source_snapshot", "source_key,snapshot_id"),
    ("catalog_model_snapshot", "captured_at,model_id,source_snapshot_id"),
    (
        "native_model_offer_snapshot",
        "captured_at,inference_provider_id,model_id,source_snapshot_id",
    ),
    (
        "model_provider_offer",
        "source_family,provider_id,model_id,captured_at,source_snapshot_id,endpoint_tag",
    ),
    ("provider_endpoint_snapshot", "captured_at,model_id,provider_id,endpoint_tag"),
    ("provider_capability_snapshot", "captured_at,provider_id"),
    ("benchmark_observation", "source_snapshot_id,observation_id"),
    ("preference_observation", "source_snapshot_id,comparison_id"),
    ("model_evidence", "model_id"),
    ("model_universe", "normalized_model_id,model_id"),
    ("current_routable_model_universe", "normalized_model_id,model_id"),
    ("route_training_fact", "decided_at,request_id"),
    ("runtime_incident", "observed_at,source_record_sha256"),
    ("incident_daily", "day,incident_class"),
    ("incident_instance_match", "observed_at,source_record_sha256"),
    ("swarm_child_result", "completed_at_approx,parent_pid,child_id"),
    ("swarm_comparison_group", "task_sha256"),
    ("transcript_turn_metric", "observed_at,source_file_sha256,source_row_number"),
    ("deduplicated_tool_trace", "observed_at,trace_session_key,source_row_number"),
    ("run_execution_fact", "started_at,run_id"),
    ("run_wal_event", "event_at,run_id,sequence_number"),
    ("process_health_hourly", "hour"),
)


def _fusion_snapshot_id(snapshots: Sequence[SourceSnapshot]) -> str:
    contract = {
        "fusion_schema_version": FUSION_SCHEMA_VERSION,
        "fusion_schema_sha256": _sha256_file(SCHEMA_PATH),
        "sources": [
            {
                "source_key": snapshot.source_key,
                "content_sha256": snapshot.content_sha256,
                "captured_at": snapshot.captured_at,
                "schema_version": snapshot.schema_version,
                "high_watermark": snapshot.high_watermark,
            }
            for snapshot in sorted(
                snapshots, key=lambda item: (item.source_key, item.snapshot_id)
            )
        ],
    }
    return _sha256_text(_canonical_json(contract)) or ""


def _publish_parquet_snapshot(
    cli: DuckDBCLI,
    database: Path,
    lake_dir: Path,
    fusion_snapshot_id: str,
    snapshots: Sequence[SourceSnapshot],
) -> Dict[str, Any]:
    lake_dir = lake_dir.expanduser().resolve()
    snapshots_root = lake_dir / "snapshots"
    snapshots_root.mkdir(parents=True, exist_ok=True)
    destination = snapshots_root / fusion_snapshot_id
    if not destination.exists():
        temporary = Path(tempfile.mkdtemp(prefix=".building-", dir=str(snapshots_root)))
        try:
            files: list[Dict[str, Any]] = []
            for relation, order_by in PARQUET_RELATIONS:
                output = temporary / f"{relation}.parquet"
                cli.execute(
                    database,
                    f"COPY (SELECT * FROM {relation} ORDER BY {order_by}) "
                    f"TO {_sql_literal(str(output))} "
                    "(FORMAT PARQUET, COMPRESSION ZSTD);",
                )
                count = int(
                    _scalar(cli, database, f"SELECT count(*) AS value FROM {relation}")
                    or 0
                )
                schema_rows = cli.query_json(
                    database,
                    "SELECT column_name,data_type FROM duckdb_columns() "
                    f"WHERE table_name={_sql_literal(relation)} ORDER BY column_index",
                )
                files.append(
                    {
                        "relation": relation,
                        "file": output.name,
                        "row_count": count,
                        "byte_size": output.stat().st_size,
                        "sha256": _sha256_file(output),
                        "schema_sha256": _sha256_text(_canonical_json(schema_rows)),
                    }
                )
            manifest = {
                "schema_version": "chimera-fusion-parquet-manifest-v1",
                "fusion_snapshot_id": fusion_snapshot_id,
                "created_at": _utc_now(),
                "duckdb_version": cli.version(),
                "fusion_schema_version": FUSION_SCHEMA_VERSION,
                "fusion_schema_sha256": _sha256_file(SCHEMA_PATH),
                "sources": [snapshot.as_row() for snapshot in snapshots],
                "files": files,
            }
            manifest_bytes = (_canonical_json(manifest) + "\n").encode("utf-8")
            (temporary / "manifest.json").write_bytes(manifest_bytes)
            manifest_sha = _sha256_bytes(manifest_bytes)
            (temporary / "manifest.sha256").write_text(
                manifest_sha + "  manifest.json\n", encoding="ascii"
            )
            os.replace(temporary, destination)
        except BaseException:
            shutil.rmtree(temporary, ignore_errors=True)
            raise
    manifest_path = destination / "manifest.json"
    manifest_sha = _sha256_file(manifest_path)
    return {
        "fusion_snapshot_id": fusion_snapshot_id,
        "parquet_snapshot": str(destination),
        "parquet_manifest_sha256": manifest_sha,
    }


def _publish_current_pointer(
    lake_dir: Path, parquet_summary: Mapping[str, Any]
) -> None:
    published_root = lake_dir.expanduser().resolve() / "published"
    published_root.mkdir(parents=True, exist_ok=True)
    fusion_snapshot_id = str(parquet_summary["fusion_snapshot_id"])
    pointer = {
        "schema_version": "chimera-fusion-current-pointer-v1",
        "fusion_snapshot_id": fusion_snapshot_id,
        "manifest_sha256": parquet_summary["parquet_manifest_sha256"],
        "relative_path": f"../snapshots/{fusion_snapshot_id}/manifest.json",
        "published_at": _utc_now(),
    }
    pointer_temp = published_root / f".current-{os.getpid()}.json"
    pointer_temp.write_text(_canonical_json(pointer) + "\n", encoding="utf-8")
    os.replace(pointer_temp, published_root / "current.json")


def _build(args: argparse.Namespace) -> int:
    warehouse = args.warehouse.expanduser().resolve()
    warehouse.parent.mkdir(parents=True, exist_ok=True)
    cli = DuckDBCLI(args.duckdb)
    registry_rows = _load_registry(args.registry)
    registry_keys = {row["source_key"] for row in registry_rows}
    built_at = _utc_now()
    build_id = hashlib.sha256(
        (FUSION_SCHEMA_VERSION + "\x1f" + built_at).encode("utf-8")
    ).hexdigest()
    with (
        _warehouse_lock(warehouse),
        tempfile.TemporaryDirectory(
            prefix="chimera-fusion-", dir=str(warehouse.parent)
        ) as temporary,
    ):
        temporary_root = Path(temporary)
        staging = StagingArea(temporary_root / "staging")
        staging.root.mkdir(parents=True, exist_ok=True)
        snapshots: list[SourceSnapshot] = []
        try:
            if args.baseline_db and args.baseline_db.expanduser().is_file():
                snapshots.append(
                    _export_baseline(
                        args.baseline_db,
                        temporary_root / "baseline.sqlite",
                        staging,
                    )
                )
            if args.chronicle_db and args.chronicle_db.expanduser().is_file():
                snapshots.append(
                    _export_chronicle(
                        args.chronicle_db,
                        temporary_root / "chronicle.sqlite",
                        staging,
                    )
                )
            if args.evals_db and args.evals_db.expanduser().is_file():
                snapshots.append(
                    _export_evals(
                        args.evals_db,
                        temporary_root / "openrouter_evals.sqlite",
                        staging,
                    )
                )
            if args.feedback_db and args.feedback_db.expanduser().is_file():
                snapshots.append(
                    _export_feedback(
                        args.feedback_db,
                        temporary_root / "feedback.sqlite",
                        staging,
                    )
                )
            for catalog in args.catalog:
                snapshots.append(_export_catalog(catalog, staging))
            if not args.skip_native_model_catalog:
                for native_catalog in args.native_model_catalog:
                    snapshots.append(
                        _export_native_model_catalog(native_catalog, staging)
                    )
            for endpoint in args.endpoint_json:
                snapshots.append(_export_endpoint_snapshot(endpoint, staging))
            if not args.skip_provider_metadata:
                provider_metadata = _export_provider_metadata(
                    args.provider_metadata_dir, staging
                )
                if provider_metadata is not None:
                    snapshots.append(provider_metadata)
            for benchmark in args.benchmark_jsonl:
                snapshots.append(_export_external_benchmarks(benchmark, staging))
            for preference in args.preference_jsonl:
                snapshots.append(_export_external_preferences(preference, staging))
            if not args.skip_session_metrics:
                transcript = _export_transcript_metrics(args.sessions_dir, staging)
                tool_trace = _export_tool_traces(args.tool_traces_dir, staging)
                snapshots.extend(
                    item for item in (transcript, tool_trace) if item is not None
                )
            if args.include_incident_metrics:
                incidents = _export_incident_metrics(args.incidents_dir, staging)
                if incidents is not None:
                    snapshots.append(incidents)
            if args.include_swarm_results:
                swarm_results = _export_swarm_child_results(
                    args.swarm_results_dir, staging
                )
                if swarm_results is not None:
                    snapshots.append(swarm_results)
            if not args.skip_run_wal:
                manifest_snapshot, primary_run_ids = _export_run_manifests(
                    args.runs_dir, staging
                )
                if manifest_snapshot is not None:
                    snapshots.append(manifest_snapshot)
                wal_snapshot = _export_run_wal(args.runs_dir, primary_run_ids, staging)
                if wal_snapshot is not None:
                    snapshots.append(wal_snapshot)
            if args.include_process_metrics:
                runtime = _export_metric_glob(
                    args.runtime_dir,
                    "runtime-*.json",
                    "dsco_runtime_metrics",
                    "runtime_process_metric",
                    staging,
                )
                child = _export_metric_glob(
                    args.runtime_dir,
                    "child-metrics-*.jsonl",
                    "dsco_child_metrics",
                    "child_process_metric",
                    staging,
                )
                snapshots.extend(item for item in (runtime, child) if item is not None)
            unknown = sorted(
                {snapshot.source_key for snapshot in snapshots} - registry_keys
            )
            if unknown:
                raise FusionError(
                    "ingested source keys are missing from the registry: "
                    + ", ".join(unknown)
                )
            for row in registry_rows:
                staging.write("source_registry", row)
            for snapshot in snapshots:
                staging.write("source_snapshot", snapshot.as_row())
        finally:
            staging.close()
        if not snapshots:
            raise FusionError("no readable sources were selected")
        candidate = temporary_root / "candidate.duckdb"
        _load_staged(
            cli,
            candidate,
            staging,
            build_id=build_id,
            built_at=built_at,
        )
        checks = _quality_checks(cli, candidate)
        _persist_checks(cli, candidate, build_id, checks)
        failed_errors = [
            check for check in checks if check.severity == "error" and not check.passed
        ]
        failed_warnings = [
            check
            for check in checks
            if check.severity == "warning" and not check.passed
        ]
        if failed_errors or (args.strict_warnings and failed_warnings):
            failures = failed_errors + (failed_warnings if args.strict_warnings else [])
            raise FusionError(
                "quality gates failed: "
                + "; ".join(f"{check.name}: {check.detail}" for check in failures)
            )
        summary = _summary(cli, candidate)
        fusion_snapshot_id = _fusion_snapshot_id(snapshots)
        cli.execute(
            candidate,
            "INSERT OR REPLACE INTO warehouse_metadata VALUES "
            f"('fusion_snapshot_id',{_sql_literal(fusion_snapshot_id)}); CHECKPOINT;",
        )
        lake_dir = args.lake_dir or (warehouse.parent / "fusion_lake")
        parquet_summary = _publish_parquet_snapshot(
            cli, candidate, lake_dir, fusion_snapshot_id, snapshots
        )
        summary.update(parquet_summary)
        os.replace(candidate, warehouse)
        _publish_current_pointer(lake_dir, parquet_summary)
    summary.update(
        {
            "warehouse": str(warehouse),
            "warehouse_sha256": _sha256_file(warehouse),
            "status": "warn" if summary.get("failed_warnings") else "pass",
        }
    )
    print(_canonical_json(summary))
    return 0


def _inspect(args: argparse.Namespace) -> int:
    warehouse = args.warehouse.expanduser().resolve()
    if not warehouse.is_file():
        raise FusionError(f"warehouse does not exist: {warehouse}")
    summary = _summary(DuckDBCLI(args.duckdb), warehouse)
    summary["warehouse"] = str(warehouse)
    summary["warehouse_sha256"] = _sha256_file(warehouse)
    print(_canonical_json(summary))
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--warehouse",
        type=Path,
        default=Path("~/.dsco/chimera_router/fusion.duckdb"),
    )
    common.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    build = subparsers.add_parser("build", parents=[common])
    build.add_argument("--registry", type=Path, default=REGISTRY_PATH)
    build.add_argument("--baseline-db", type=Path, default=Path("~/.dsco/baseline.db"))
    build.add_argument(
        "--chronicle-db",
        type=Path,
        default=Path("~/.dsco/chronicle/indexes/chronicle.sqlite"),
    )
    build.add_argument(
        "--evals-db", type=Path, default=Path("~/.dsco/openrouter_evals.db")
    )
    build.add_argument(
        "--feedback-db",
        type=Path,
        default=Path("~/.dsco/chimera_router/feedback_v3.db"),
    )
    build.add_argument(
        "--catalog",
        type=Path,
        action="append",
        default=[],
        help="pinned OpenRouter model catalog JSON; repeatable",
    )
    build.add_argument(
        "--endpoint-json",
        type=Path,
        action="append",
        default=[],
        help="captured OpenRouter endpoint API JSON; repeatable",
    )
    build.add_argument(
        "--native-model-catalog",
        type=Path,
        action="append",
        default=[],
        help="pinned oh-my-pi generated model catalog JSON; repeatable",
    )
    build.add_argument(
        "--skip-native-model-catalog",
        action="store_true",
        help="do not auto-discover or ingest the local oh-my-pi model catalog",
    )
    build.add_argument(
        "--provider-metadata-dir",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "provider_metadata/providers",
    )
    build.add_argument(
        "--skip-provider-metadata",
        action="store_true",
        help="skip reviewed local provider capability metadata",
    )
    build.add_argument(
        "--benchmark-jsonl",
        type=Path,
        action="append",
        default=[],
        help="normalized benchmark evidence without raw prompt/response fields",
    )
    build.add_argument(
        "--preference-jsonl",
        type=Path,
        action="append",
        default=[],
        help="normalized pairwise preferences without raw prompt/response fields",
    )
    build.add_argument("--runtime-dir", type=Path, default=Path("~/.dsco"))
    build.add_argument("--incidents-dir", type=Path, default=Path("~/.dsco/incidents"))
    build.add_argument(
        "--include-incident-metrics",
        action="store_true",
        help="stream redacted runtime incident metrics (large local source)",
    )
    build.add_argument(
        "--swarm-results-dir",
        type=Path,
        default=Path("~/.dsco/sessions/swarm"),
    )
    build.add_argument(
        "--include-swarm-results",
        action="store_true",
        help="ingest hashed child-agent outcome metrics for comparison groups",
    )
    build.add_argument("--sessions-dir", type=Path, default=Path("~/.dsco/sessions"))
    build.add_argument(
        "--tool-traces-dir",
        type=Path,
        default=Path("~/.dsco/workspace/memory/traces"),
    )
    build.add_argument(
        "--skip-session-metrics",
        action="store_true",
        help="skip metrics-only transcript and tool-trace JSONL sources",
    )
    build.add_argument("--runs-dir", type=Path, default=Path("~/.dsco/runs"))
    build.add_argument(
        "--skip-run-wal",
        action="store_true",
        help="skip primary Chronicle run manifests and CRC-framed WAL metrics",
    )
    build.add_argument(
        "--include-process-metrics",
        action="store_true",
        help="ingest all runtime-*.json and child-metrics-*.jsonl records",
    )
    build.add_argument("--strict-warnings", action="store_true")
    build.add_argument(
        "--lake-dir",
        type=Path,
        help="immutable Parquet lake root (default: WAREHOUSE_PARENT/fusion_lake)",
    )
    subparsers.add_parser("inspect", parents=[common])
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    if args.command == "build":
        if not args.catalog:
            pinned = Path(
                "~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json"
            ).expanduser()
            fallback = Path("~/.dsco/openrouter_models.json").expanduser()
            if pinned.is_file():
                args.catalog = [pinned]
            elif fallback.is_file():
                args.catalog = [fallback]
        if not args.native_model_catalog and not args.skip_native_model_catalog:
            native_candidates = (
                Path("~/oh-my-pi/packages/catalog/src/models.json").expanduser(),
                Path(__file__).resolve().parents[2]
                / "oh-my-pi/packages/catalog/src/models.json",
            )
            args.native_model_catalog = [
                candidate for candidate in native_candidates if candidate.is_file()
            ][:1]
        return _build(args)
    if args.command == "inspect":
        return _inspect(args)
    parser.error(f"unknown command {args.command}")
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FusionError as exc:
        print(f"fusion: {exc}", file=sys.stderr)
        raise SystemExit(2)
