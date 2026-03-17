#!/usr/bin/env python3
"""Local DSCO edge for Claude-compatible traffic and DSCO-native control APIs."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import math
import os
import re
import secrets
import sqlite3
import struct
import threading
import time
import urllib.parse
import uuid
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple


ANTHROPIC_BASE = os.environ.get("CLAUDE_UPSTREAM_BASE", "https://api.anthropic.com")
ANTHROPIC_MCP_BASE = os.environ.get(
    "CLAUDE_MCP_BASE", "https://mcp-proxy.anthropic.com"
)
CLAUDE_WEB_BASE = os.environ.get("CLAUDE_WEB_BASE", "https://claude.ai")
JINA_EMBEDDINGS_URL = os.environ.get(
    "JINA_EMBEDDINGS_URL", "https://api.jina.ai/v1/embeddings"
)
EMBEDDING_MODEL = os.environ.get("JINA_EMBEDDING_MODEL", "jina-embeddings-v4")
DEFAULT_DB = os.environ.get(
    "CLAUDE_PROXY_DB", str(Path.home() / ".dsco" / "claude_proxy.db")
)
DEFAULT_HOST = os.environ.get("CLAUDE_PROXY_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("CLAUDE_PROXY_PORT", "8031"))
DEFAULT_TIMEOUT_SECONDS = int(os.environ.get("CLAUDE_PROXY_TIMEOUT", "1800"))
DEFAULT_MAX_ARTIFACT_BYTES = int(
    os.environ.get("CLAUDE_PROXY_MAX_ARTIFACT_BYTES", str(2 * 1024 * 1024))
)
DEFAULT_MAX_ARTIFACTS = int(os.environ.get("CLAUDE_PROXY_MAX_ARTIFACTS", "256"))
DEFAULT_BATCH_SIZE = int(os.environ.get("CLAUDE_PROXY_EMBED_BATCH", "16"))
DEFAULT_WORKSPACE_ID = os.environ.get("CLAUDE_PROXY_WORKSPACE_ID", "ws_default")
PERSIST_RAW_ARTIFACTS = os.environ.get("CLAUDE_PROXY_PERSIST_RAW", "0") == "1"
INTERNAL_FIRST = os.environ.get("CLAUDE_PROXY_INTERNAL_FIRST", "0") == "1"
CONTROL_PLANE_BASE = os.environ.get("DSCO_CONTROL_PLANE_BASE", "")

INTERNAL_MODELS = [
    {
        "id": "dsco-router-1",
        "display_name": "DSCO Router",
        "type": "compat_router",
        "created_at": "2026-03-13",
    },
    {
        "id": "dsco-swarm-1",
        "display_name": "DSCO Swarm",
        "type": "dynamic_swarm",
        "created_at": "2026-03-13",
    },
    {
        "id": "dsco-research-1",
        "display_name": "DSCO Research",
        "type": "retrieval_augmented",
        "created_at": "2026-03-13",
    },
    {
        "id": "dsco-review-1",
        "display_name": "DSCO Review",
        "type": "analysis",
        "created_at": "2026-03-13",
    },
]

REDACTION_PATTERNS = (
    re.compile(
        r"(?im)\b(authorization|api[_-]?key|token|password|secret)\b\s*[:=]\s*([\"']?)([^\s,\"'}]{8,})"
    ),
    re.compile(r"(?i)\bBearer\s+[A-Za-z0-9._\-=/+]{8,}\b"),
)
TOKEN_RE = re.compile(r"[A-Za-z0-9_]+")
SKIP_PROXY_RESPONSE_HEADERS = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
}
SKIP_PROXY_REQUEST_HEADERS = {
    "connection",
    "host",
    "proxy-connection",
    "content-length",
    "accept-encoding",
}


def now_ts() -> int:
    return int(time.time())


def new_id(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex}"


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8", errors="replace")).hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pack_embedding(values: Sequence[float]) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def unpack_embedding(blob: bytes) -> Tuple[float, ...]:
    if not blob:
        return ()
    size = len(blob) // 4
    return struct.unpack(f"<{size}f", blob)


def estimate_tokens(text: str) -> int:
    return max(1, len(text) // 4)


def chunk_text(text: str, max_chars: int = 3800, overlap: int = 300) -> List[str]:
    text = text.strip()
    if not text:
        return []
    chunks: List[str] = []
    start = 0
    while start < len(text):
        end = min(len(text), start + max_chars)
        if end < len(text):
            split = text.rfind("\n\n", start, end)
            if split <= start:
                split = text.rfind("\n", start, end)
            if split <= start:
                split = text.rfind(" ", start, end)
            if split > start:
                end = split
        piece = text[start:end].strip()
        if piece:
            chunks.append(piece)
        if end >= len(text):
            break
        start = max(end - overlap, start + 1)
    return chunks


def redact_text(text: str) -> str:
    redacted = text
    redacted = REDACTION_PATTERNS[0].sub(r"\1=[REDACTED]", redacted)
    redacted = REDACTION_PATTERNS[1].sub("Bearer [REDACTED]", redacted)
    return redacted


def decode_body(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


def read_request_body(handler: BaseHTTPRequestHandler) -> bytes:
    length = handler.headers.get("Content-Length")
    if not length:
        return b""
    return handler.rfile.read(int(length))


def read_json_bytes(data: bytes) -> Dict[str, object]:
    if not data:
        return {}
    return json.loads(decode_body(data))


def to_json(data: object) -> bytes:
    return json.dumps(data, ensure_ascii=True, indent=2).encode("utf-8")


def mime_type_for_path(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix == ".json":
        return "application/json"
    if suffix in {".md", ".txt", ".log"}:
        return "text/plain"
    return "application/octet-stream"


def collect_message_text(messages: Sequence[object]) -> str:
    parts: List[str] = []
    for message in messages:
        if isinstance(message, str):
            parts.append(message)
            continue
        if not isinstance(message, dict):
            continue
        content = message.get("content")
        if isinstance(content, str):
            parts.append(content)
            continue
        if isinstance(content, list):
            for item in content:
                if isinstance(item, str):
                    parts.append(item)
                elif isinstance(item, dict) and item.get("type") == "text":
                    text = item.get("text")
                    if isinstance(text, str):
                        parts.append(text)
    return "\n".join(part for part in parts if part).strip()


def profile_keywords(query: str) -> str:
    text = query.lower()
    if any(word in text for word in ("incident", "outage", "breach", "mitigate", "rollback")):
        return "incident"
    if any(word in text for word in ("review", "audit", "risk", "regression", "findings")):
        return "review"
    if any(word in text for word in ("implement", "code", "refactor", "compile", "function")):
        return "code"
    if any(word in text for word in ("research", "analyze", "compare", "latest", "sources")):
        return "research"
    if any(word in text for word in ("write", "story", "brand", "copy", "design")):
        return "creative"
    return "general"


def build_runtime_plan(query: str, requested_topology: Optional[str] = None) -> Dict[str, object]:
    profile = profile_keywords(query)
    length_hint = len(query)
    replica_bias = 3 if length_hint > 400 else 2
    topology_name = requested_topology or f"dynamic_{profile}"
    if profile == "code":
        stages = [
            {"name": "profile", "role": "planner", "replicas": 1},
            {"name": "retrieve", "role": "artifact_retriever", "replicas": 1},
            {"name": "implement", "role": "worker", "replicas": replica_bias},
            {"name": "review", "role": "critic", "replicas": 1},
        ]
        rationale = "Code-oriented task. Bias toward implementation capacity with a review gate."
    elif profile == "research":
        stages = [
            {"name": "profile", "role": "planner", "replicas": 1},
            {"name": "retrieve", "role": "artifact_retriever", "replicas": 2},
            {"name": "synthesize", "role": "synthesizer", "replicas": replica_bias},
            {"name": "validate", "role": "validator", "replicas": 1},
        ]
        rationale = "Research-oriented task. Bias toward retrieval breadth and synthesis."
    elif profile == "review":
        stages = [
            {"name": "profile", "role": "planner", "replicas": 1},
            {"name": "retrieve", "role": "artifact_retriever", "replicas": 1},
            {"name": "inspect", "role": "reviewer", "replicas": replica_bias},
            {"name": "judge", "role": "validator", "replicas": 1},
        ]
        rationale = "Review-oriented task. Bias toward issue finding and validation."
    elif profile == "incident":
        stages = [
            {"name": "profile", "role": "incident_commander", "replicas": 1},
            {"name": "retrieve", "role": "timeline_retriever", "replicas": 2},
            {"name": "triage", "role": "responder", "replicas": replica_bias},
            {"name": "stabilize", "role": "validator", "replicas": 1},
        ]
        rationale = "Incident-oriented task. Bias toward fast retrieval and stabilization."
    elif profile == "creative":
        stages = [
            {"name": "profile", "role": "planner", "replicas": 1},
            {"name": "retrieve", "role": "reference_retriever", "replicas": 1},
            {"name": "generate", "role": "creator", "replicas": replica_bias},
            {"name": "refine", "role": "editor", "replicas": 1},
        ]
        rationale = "Creative task. Bias toward draft variation with a refinement pass."
    else:
        stages = [
            {"name": "profile", "role": "planner", "replicas": 1},
            {"name": "retrieve", "role": "artifact_retriever", "replicas": 1},
            {"name": "execute", "role": "worker", "replicas": replica_bias},
            {"name": "summarize", "role": "synthesizer", "replicas": 1},
        ]
        rationale = "General task. Balanced dynamic plan."

    return {
        "task_profile": profile,
        "topology_name": topology_name,
        "rationale": rationale,
        "feedback_edges": [
            {"from": stages[-1]["name"], "to": stages[1]["name"], "condition": "needs_more_context"}
        ],
        "stages": stages,
        "constraints": {
            "detach_on_client_disconnect": True,
            "preserve_partial_results": True,
            "max_parallel_agents": sum(stage["replicas"] for stage in stages),
        },
    }


def estimate_payload_tokens(payload: Dict[str, object]) -> int:
    texts: List[str] = []
    if isinstance(payload.get("system"), str):
        texts.append(payload["system"])
    messages = payload.get("messages")
    if isinstance(messages, list):
        texts.append(collect_message_text(messages))
    if isinstance(payload.get("input"), str):
        texts.append(payload["input"])
    if isinstance(payload.get("query"), str):
        texts.append(payload["query"])
    combined = "\n".join(text for text in texts if text)
    return estimate_tokens(combined)


def anthropic_message_payload(
    model: str,
    text: str,
    input_tokens: int,
    request_id: str,
    run_id: Optional[str] = None,
) -> Dict[str, object]:
    output_tokens = estimate_tokens(text)
    return {
        "id": new_id("msg"),
        "type": "message",
        "role": "assistant",
        "model": model,
        "content": [{"type": "text", "text": text}],
        "stop_reason": "end_turn",
        "stop_sequence": None,
        "usage": {
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
        },
        "request_id": request_id,
        "run_id": run_id,
    }


class ArtifactIndexer:
    def __init__(
        self,
        db_path: str,
        workspace_root: Optional[str] = None,
        jina_api_key: Optional[str] = None,
        max_artifact_bytes: int = DEFAULT_MAX_ARTIFACT_BYTES,
        max_artifacts: int = DEFAULT_MAX_ARTIFACTS,
    ) -> None:
        self.db_path = str(Path(db_path).expanduser())
        self.workspace_root = (
            Path(workspace_root).expanduser().resolve()
            if workspace_root
            else Path.cwd().resolve()
        )
        self.jina_api_key = jina_api_key or os.environ.get("JINA_API_KEY")
        self.max_artifact_bytes = max_artifact_bytes
        self.max_artifacts = max_artifacts
        self._lock = threading.Lock()
        Path(self.db_path).expanduser().parent.mkdir(parents=True, exist_ok=True)
        self.init_db()
        self.seed_defaults()

    @contextmanager
    def connect(self) -> Iterator[sqlite3.Connection]:
        conn = sqlite3.connect(self.db_path, timeout=30.0)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA foreign_keys=ON")
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()

    def init_db(self) -> None:
        with self.connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS workspace (
                    workspace_id TEXT PRIMARY KEY,
                    slug TEXT NOT NULL UNIQUE,
                    name TEXT NOT NULL,
                    policy_json TEXT,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS api_key (
                    key_id TEXT PRIMARY KEY,
                    workspace_id TEXT NOT NULL REFERENCES workspace(workspace_id) ON DELETE CASCADE,
                    label TEXT,
                    key_hash TEXT NOT NULL UNIQUE,
                    scope TEXT NOT NULL,
                    status TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    revoked_at INTEGER
                );

                CREATE TABLE IF NOT EXISTS artifact (
                    artifact_id TEXT PRIMARY KEY,
                    workspace_id TEXT NOT NULL REFERENCES workspace(workspace_id) ON DELETE CASCADE,
                    kind TEXT NOT NULL,
                    source TEXT NOT NULL,
                    source_path TEXT,
                    mime_type TEXT,
                    content_sha256 TEXT NOT NULL,
                    metadata_json TEXT,
                    content_text TEXT,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS artifact_chunk (
                    chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    artifact_id TEXT NOT NULL REFERENCES artifact(artifact_id) ON DELETE CASCADE,
                    chunk_index INTEGER NOT NULL,
                    chunk_text TEXT NOT NULL,
                    token_count INTEGER,
                    embedding_model TEXT NOT NULL,
                    embedding_dim INTEGER NOT NULL,
                    embedding_blob BLOB NOT NULL,
                    created_at INTEGER NOT NULL,
                    UNIQUE(artifact_id, chunk_index)
                );

                CREATE INDEX IF NOT EXISTS idx_artifact_workspace
                    ON artifact(workspace_id, source);
                CREATE INDEX IF NOT EXISTS idx_artifact_chunk_artifact
                    ON artifact_chunk(artifact_id, chunk_index);

                CREATE TABLE IF NOT EXISTS run (
                    run_id TEXT PRIMARY KEY,
                    workspace_id TEXT NOT NULL REFERENCES workspace(workspace_id) ON DELETE CASCADE,
                    kind TEXT NOT NULL,
                    status TEXT NOT NULL,
                    model TEXT,
                    topology_name TEXT,
                    task_profile TEXT,
                    request_id TEXT,
                    reason TEXT,
                    input_json TEXT,
                    result_json TEXT,
                    created_at INTEGER NOT NULL,
                    started_at INTEGER,
                    completed_at INTEGER,
                    updated_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS run_event (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    run_id TEXT NOT NULL REFERENCES run(run_id) ON DELETE CASCADE,
                    seq INTEGER NOT NULL,
                    event_type TEXT NOT NULL,
                    payload_json TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    UNIQUE(run_id, seq)
                );

                CREATE TABLE IF NOT EXISTS topology_plan (
                    plan_id TEXT PRIMARY KEY,
                    run_id TEXT REFERENCES run(run_id) ON DELETE SET NULL,
                    workspace_id TEXT NOT NULL REFERENCES workspace(workspace_id) ON DELETE CASCADE,
                    task_profile TEXT NOT NULL,
                    topology_name TEXT NOT NULL,
                    rationale TEXT,
                    plan_json TEXT NOT NULL,
                    created_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS mcp_server (
                    server_id TEXT PRIMARY KEY,
                    workspace_id TEXT NOT NULL REFERENCES workspace(workspace_id) ON DELETE CASCADE,
                    name TEXT NOT NULL,
                    base_url TEXT NOT NULL,
                    transport TEXT NOT NULL,
                    status TEXT NOT NULL,
                    metadata_json TEXT,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS proxy_access_log (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    request_ts INTEGER NOT NULL,
                    method TEXT NOT NULL,
                    path TEXT NOT NULL,
                    target_url TEXT,
                    status_code INTEGER,
                    latency_ms INTEGER,
                    request_bytes INTEGER,
                    response_bytes INTEGER,
                    metadata_json TEXT
                );

                CREATE TABLE IF NOT EXISTS event_batch (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    request_ts INTEGER NOT NULL,
                    path TEXT NOT NULL,
                    headers_json TEXT,
                    body_text TEXT
                );
                """
            )

    def seed_defaults(self) -> None:
        created = now_ts()
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO workspace (workspace_id, slug, name, policy_json, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(workspace_id) DO NOTHING
                """,
                (
                    DEFAULT_WORKSPACE_ID,
                    "default",
                    "Default Workspace",
                    json.dumps(
                        {
                            "compat": {"internal_first": INTERNAL_FIRST},
                            "artifacts": {"embedding_model": EMBEDDING_MODEL},
                            "timeouts": {"detached_runs": True},
                        },
                        ensure_ascii=True,
                    ),
                    created,
                    created,
                ),
            )

    def _iter_recent(self, paths: Iterable[Path], limit: int) -> List[Path]:
        existing = [p for p in paths if p.exists() and p.is_file()]
        existing.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        return existing[:limit]

    def _iter_project_artifacts(self) -> List[Path]:
        items: List[Path] = []
        for name in ("AGENTS.md", "CLAUDE.md", "README.md", "README"):
            candidate = self.workspace_root / name
            if candidate.exists() and candidate.is_file():
                items.append(candidate)
        return items

    def iter_critical_artifacts(self) -> List[Tuple[Path, str]]:
        home = Path.home()
        claude_root = home / ".claude"
        candidates: List[Tuple[Path, str]] = []

        direct = [
            (home / ".claude.json", "claude-config"),
            (claude_root / "CLAUDE.md", "claude-global-memory"),
            (claude_root / "debug" / "latest", "claude-debug-latest"),
        ]
        candidates.extend([(path, group) for path, group in direct if path.exists()])

        backup_paths = self._iter_recent(
            (claude_root / "backups").glob(".claude.json.backup*"), limit=12
        )
        candidates.extend((path, "claude-backup") for path in backup_paths)

        debug_logs = self._iter_recent((claude_root / "debug").glob("*.txt"), limit=24)
        candidates.extend((path, "claude-debug-log") for path in debug_logs)

        file_history = self._iter_recent((claude_root / "file-history").rglob("*"), limit=24)
        candidates.extend((path, "claude-file-history") for path in file_history)

        projects_root = claude_root / "projects"
        if projects_root.exists():
            project_files: List[Path] = []
            for path in projects_root.rglob("*.md"):
                parts = set(path.parts)
                if "memory" in parts or path.name == "CLAUDE.md":
                    project_files.append(path)
            for path in self._iter_recent(project_files, limit=64):
                candidates.append((path, "claude-project-memory"))

        for path in self._iter_project_artifacts():
            candidates.append((path, "workspace-critical"))

        seen: set[str] = set()
        deduped: List[Tuple[Path, str]] = []
        for path, group in candidates:
            resolved = str(path.resolve())
            if resolved in seen:
                continue
            seen.add(resolved)
            deduped.append((path, group))
            if len(deduped) >= self.max_artifacts:
                break
        return deduped

    def embed_texts(self, texts: Sequence[str], task: str) -> List[List[float]]:
        if not texts:
            return []
        if not self.jina_api_key:
            raise RuntimeError("JINA_API_KEY is not set")

        parsed = urllib.parse.urlparse(JINA_EMBEDDINGS_URL)
        headers = {
            "Authorization": f"Bearer {self.jina_api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        }
        payload = {
            "model": EMBEDDING_MODEL,
            "input": list(texts),
            "task": task,
            "embedding_type": "float",
            "normalized": True,
        }
        body = json.dumps(payload).encode("utf-8")
        conn = http.client.HTTPSConnection(parsed.netloc, timeout=DEFAULT_TIMEOUT_SECONDS)
        try:
            conn.request("POST", parsed.path or "/v1/embeddings", body=body, headers=headers)
            response = conn.getresponse()
            raw = response.read()
        finally:
            conn.close()
        if response.status >= 400:
            raise RuntimeError(
                f"Jina embeddings request failed: HTTP {response.status} {decode_body(raw)}"
            )
        decoded = json.loads(decode_body(raw))
        data = sorted(decoded.get("data", []), key=lambda item: item.get("index", 0))
        embeddings = [item.get("embedding") for item in data]
        if len(embeddings) != len(texts):
            raise RuntimeError(
                f"Expected {len(texts)} embeddings but received {len(embeddings)}"
            )
        return embeddings

    def _read_file(self, path: Path) -> Tuple[str, Dict[str, object]]:
        raw = path.read_bytes()
        truncated = False
        if len(raw) > self.max_artifact_bytes:
            raw = raw[: self.max_artifact_bytes]
            truncated = True
        decoded = decode_body(raw)
        metadata = {
            "size_bytes": path.stat().st_size,
            "mtime": int(path.stat().st_mtime),
            "truncated": truncated,
        }
        return decoded, metadata

    def index_path(self, path: Path, group: str, workspace_id: str = DEFAULT_WORKSPACE_ID) -> Dict[str, object]:
        resolved = path.expanduser().resolve()
        source = str(resolved)
        raw_text, base_metadata = self._read_file(resolved)
        stored_text = raw_text if PERSIST_RAW_ARTIFACTS else ""
        redacted = redact_text(raw_text)
        content_hash = sha256_text(redacted)
        artifact_id = sha256_text(f"{workspace_id}:file:{source}")
        created_at = now_ts()
        chunks = chunk_text(redacted)
        if not chunks:
            return {"artifact_id": artifact_id, "source": source, "skipped": True, "reason": "empty"}

        with self._lock:
            with self.connect() as conn:
                existing = conn.execute(
                    "SELECT content_sha256 FROM artifact WHERE artifact_id = ?",
                    (artifact_id,),
                ).fetchone()
                if existing and existing["content_sha256"] == content_hash:
                    return {"artifact_id": artifact_id, "source": source, "skipped": True, "reason": "unchanged"}

            embeddings: List[List[float]] = []
            if self.jina_api_key:
                for start in range(0, len(chunks), DEFAULT_BATCH_SIZE):
                    batch = chunks[start : start + DEFAULT_BATCH_SIZE]
                    embeddings.extend(self.embed_texts(batch, task="retrieval.passage"))
            else:
                embeddings = [[0.0] * 8 for _ in chunks]

            with self.connect() as conn:
                conn.execute("DELETE FROM artifact_chunk WHERE artifact_id = ?", (artifact_id,))
                conn.execute(
                    """
                    INSERT INTO artifact (
                        artifact_id, workspace_id, kind, source, source_path, mime_type,
                        content_sha256, metadata_json, content_text, created_at, updated_at
                    )
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(artifact_id) DO UPDATE SET
                        workspace_id = excluded.workspace_id,
                        kind = excluded.kind,
                        source = excluded.source,
                        source_path = excluded.source_path,
                        mime_type = excluded.mime_type,
                        content_sha256 = excluded.content_sha256,
                        metadata_json = excluded.metadata_json,
                        content_text = excluded.content_text,
                        updated_at = excluded.updated_at
                    """,
                    (
                        artifact_id,
                        workspace_id,
                        "file",
                        source,
                        source,
                        mime_type_for_path(resolved),
                        content_hash,
                        json.dumps(
                            {
                                "group": group,
                                "workspace_root": str(self.workspace_root),
                                **base_metadata,
                            },
                            ensure_ascii=True,
                        ),
                        stored_text,
                        created_at,
                        created_at,
                    ),
                )
                for idx, (chunk, embedding) in enumerate(zip(chunks, embeddings)):
                    conn.execute(
                        """
                        INSERT INTO artifact_chunk (
                            artifact_id, chunk_index, chunk_text, token_count,
                            embedding_model, embedding_dim, embedding_blob, created_at
                        )
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                        """,
                        (
                            artifact_id,
                            idx,
                            chunk,
                            estimate_tokens(chunk),
                            EMBEDDING_MODEL,
                            len(embedding),
                            pack_embedding(embedding),
                            created_at,
                        ),
                    )

        return {
            "artifact_id": artifact_id,
            "source": source,
            "group": group,
            "chunks": len(chunks),
            "updated": True,
        }

    def reindex(self, workspace_id: str = DEFAULT_WORKSPACE_ID) -> Dict[str, object]:
        indexed = 0
        skipped = 0
        failed: List[Dict[str, object]] = []
        artifacts = self.iter_critical_artifacts()
        for path, group in artifacts:
            try:
                result = self.index_path(path, group, workspace_id=workspace_id)
            except Exception as exc:
                failed.append({"source": str(path), "error": str(exc)})
                continue
            if result.get("skipped"):
                skipped += 1
            else:
                indexed += 1
        return {
            "indexed": indexed,
            "skipped": skipped,
            "failed": failed,
            "artifacts_seen": len(artifacts),
        }

    def lexical_search(
        self, query: str, limit: int = 8, workspace_id: str = DEFAULT_WORKSPACE_ID
    ) -> Dict[str, object]:
        terms = set(TOKEN_RE.findall(query.lower()))
        if not terms:
            return {"query": query, "results": []}
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT
                    c.chunk_id,
                    c.chunk_index,
                    c.chunk_text,
                    a.artifact_id,
                    a.source,
                    a.metadata_json
                FROM artifact_chunk c
                JOIN artifact a ON a.artifact_id = c.artifact_id
                WHERE a.workspace_id = ?
                """,
                (workspace_id,),
            ).fetchall()
        results: List[Dict[str, object]] = []
        for row in rows:
            text = row["chunk_text"]
            hay = set(TOKEN_RE.findall(text.lower()))
            overlap = len(terms & hay)
            if not overlap:
                continue
            metadata = json.loads(row["metadata_json"] or "{}")
            results.append(
                {
                    "chunk_id": row["chunk_id"],
                    "artifact_id": row["artifact_id"],
                    "source": row["source"],
                    "group": metadata.get("group"),
                    "chunk_index": row["chunk_index"],
                    "score": float(overlap),
                    "text": text[:900],
                }
            )
        results.sort(key=lambda item: item["score"], reverse=True)
        return {"query": query, "results": results[:limit]}

    def vector_search(
        self, query: str, limit: int = 8, workspace_id: str = DEFAULT_WORKSPACE_ID
    ) -> Dict[str, object]:
        query_embedding = self.embed_texts([query], task="retrieval.query")[0]
        query_norm = math.sqrt(sum(value * value for value in query_embedding)) or 1.0
        results: List[Dict[str, object]] = []
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT
                    c.chunk_id,
                    c.chunk_index,
                    c.chunk_text,
                    c.embedding_blob,
                    a.artifact_id,
                    a.source,
                    a.metadata_json
                FROM artifact_chunk c
                JOIN artifact a ON a.artifact_id = c.artifact_id
                WHERE a.workspace_id = ?
                """,
                (workspace_id,),
            ).fetchall()
        for row in rows:
            embedding = unpack_embedding(row["embedding_blob"])
            if not embedding or len(embedding) != len(query_embedding):
                continue
            score = sum(left * right for left, right in zip(query_embedding, embedding))
            score /= query_norm
            metadata = json.loads(row["metadata_json"] or "{}")
            results.append(
                {
                    "chunk_id": row["chunk_id"],
                    "artifact_id": row["artifact_id"],
                    "source": row["source"],
                    "group": metadata.get("group"),
                    "chunk_index": row["chunk_index"],
                    "score": round(score, 6),
                    "text": row["chunk_text"][:900],
                }
            )
        results.sort(key=lambda item: item["score"], reverse=True)
        return {"query": query, "results": results[:limit]}

    def search(
        self, query: str, limit: int = 8, workspace_id: str = DEFAULT_WORKSPACE_ID
    ) -> Dict[str, object]:
        if not query.strip():
            return {"query": query, "results": []}
        try:
            if self.jina_api_key:
                return self.vector_search(query, limit=limit, workspace_id=workspace_id)
        except Exception:
            pass
        return self.lexical_search(query, limit=limit, workspace_id=workspace_id)

    def list_artifacts(self, workspace_id: str = DEFAULT_WORKSPACE_ID, limit: int = 50) -> Dict[str, object]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT artifact_id, kind, source, mime_type, metadata_json, created_at, updated_at
                FROM artifact
                WHERE workspace_id = ?
                ORDER BY updated_at DESC
                LIMIT ?
                """,
                (workspace_id, limit),
            ).fetchall()
        artifacts = []
        for row in rows:
            artifacts.append(
                {
                    "artifact_id": row["artifact_id"],
                    "kind": row["kind"],
                    "source": row["source"],
                    "mime_type": row["mime_type"],
                    "metadata": json.loads(row["metadata_json"] or "{}"),
                    "created_at": row["created_at"],
                    "updated_at": row["updated_at"],
                }
            )
        return {"workspace_id": workspace_id, "data": artifacts}

    def get_artifact(self, artifact_id: str) -> Optional[Dict[str, object]]:
        with self.connect() as conn:
            row = conn.execute(
                """
                SELECT artifact_id, workspace_id, kind, source, mime_type, metadata_json, created_at, updated_at
                FROM artifact WHERE artifact_id = ?
                """,
                (artifact_id,),
            ).fetchone()
        if not row:
            return None
        return {
            "artifact_id": row["artifact_id"],
            "workspace_id": row["workspace_id"],
            "kind": row["kind"],
            "source": row["source"],
            "mime_type": row["mime_type"],
            "metadata": json.loads(row["metadata_json"] or "{}"),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }

    def get_artifact_chunks(self, artifact_id: str, limit: int = 100) -> Dict[str, object]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT chunk_id, chunk_index, chunk_text, token_count, embedding_model, embedding_dim, created_at
                FROM artifact_chunk
                WHERE artifact_id = ?
                ORDER BY chunk_index ASC
                LIMIT ?
                """,
                (artifact_id, limit),
            ).fetchall()
        return {
            "artifact_id": artifact_id,
            "data": [
                {
                    "chunk_id": row["chunk_id"],
                    "chunk_index": row["chunk_index"],
                    "chunk_text": row["chunk_text"],
                    "token_count": row["token_count"],
                    "embedding_model": row["embedding_model"],
                    "embedding_dim": row["embedding_dim"],
                    "created_at": row["created_at"],
                }
                for row in rows
            ],
        }

    def list_workspaces(self) -> Dict[str, object]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT workspace_id, slug, name, policy_json, created_at, updated_at
                FROM workspace
                ORDER BY created_at ASC
                """
            ).fetchall()
        return {
            "data": [
                {
                    "workspace_id": row["workspace_id"],
                    "slug": row["slug"],
                    "name": row["name"],
                    "policy": json.loads(row["policy_json"] or "{}"),
                    "created_at": row["created_at"],
                    "updated_at": row["updated_at"],
                }
                for row in rows
            ]
        }

    def create_workspace(self, payload: Dict[str, object]) -> Dict[str, object]:
        created = now_ts()
        workspace_id = new_id("ws")
        slug = str(payload.get("slug") or workspace_id)
        name = str(payload.get("name") or slug)
        policy = payload.get("policy") or {}
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO workspace (workspace_id, slug, name, policy_json, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    workspace_id,
                    slug,
                    name,
                    json.dumps(policy, ensure_ascii=True),
                    created,
                    created,
                ),
            )
        return {
            "workspace_id": workspace_id,
            "slug": slug,
            "name": name,
            "policy": policy,
            "created_at": created,
        }

    def get_workspace(self, workspace_id: str) -> Optional[Dict[str, object]]:
        with self.connect() as conn:
            row = conn.execute(
                """
                SELECT workspace_id, slug, name, policy_json, created_at, updated_at
                FROM workspace WHERE workspace_id = ?
                """,
                (workspace_id,),
            ).fetchone()
        if not row:
            return None
        return {
            "workspace_id": row["workspace_id"],
            "slug": row["slug"],
            "name": row["name"],
            "policy": json.loads(row["policy_json"] or "{}"),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }

    def create_api_key(self, payload: Dict[str, object]) -> Dict[str, object]:
        created = now_ts()
        key_id = new_id("key")
        workspace_id = str(payload.get("workspace_id") or DEFAULT_WORKSPACE_ID)
        label = str(payload.get("label") or "default")
        scope = str(payload.get("scope") or "full")
        cleartext = f"dsco_{secrets.token_urlsafe(24)}"
        key_hash = sha256_text(cleartext)
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO api_key (key_id, workspace_id, label, key_hash, scope, status, created_at, revoked_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, NULL)
                """,
                (key_id, workspace_id, label, key_hash, scope, "active", created),
            )
        return {
            "key_id": key_id,
            "workspace_id": workspace_id,
            "label": label,
            "scope": scope,
            "status": "active",
            "api_key": cleartext,
            "created_at": created,
        }

    def list_api_keys(self, workspace_id: Optional[str] = None) -> Dict[str, object]:
        with self.connect() as conn:
            if workspace_id:
                rows = conn.execute(
                    """
                    SELECT key_id, workspace_id, label, scope, status, created_at, revoked_at
                    FROM api_key
                    WHERE workspace_id = ?
                    ORDER BY created_at DESC
                    """,
                    (workspace_id,),
                ).fetchall()
            else:
                rows = conn.execute(
                    """
                    SELECT key_id, workspace_id, label, scope, status, created_at, revoked_at
                    FROM api_key
                    ORDER BY created_at DESC
                    """
                ).fetchall()
        return {
            "data": [
                {
                    "key_id": row["key_id"],
                    "workspace_id": row["workspace_id"],
                    "label": row["label"],
                    "scope": row["scope"],
                    "status": row["status"],
                    "created_at": row["created_at"],
                    "revoked_at": row["revoked_at"],
                }
                for row in rows
            ]
        }

    def revoke_api_key(self, key_id: str) -> Dict[str, object]:
        revoked_at = now_ts()
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE api_key
                SET status = 'revoked', revoked_at = ?
                WHERE key_id = ?
                """,
                (revoked_at, key_id),
            )
        return {"key_id": key_id, "status": "revoked", "revoked_at": revoked_at}

    def create_mcp_server(self, payload: Dict[str, object]) -> Dict[str, object]:
        created = now_ts()
        server_id = str(payload.get("server_id") or new_id("mcp"))
        workspace_id = str(payload.get("workspace_id") or DEFAULT_WORKSPACE_ID)
        name = str(payload.get("name") or server_id)
        base_url = str(payload.get("base_url") or "")
        transport = str(payload.get("transport") or "streamable_http")
        metadata = payload.get("metadata") or {}
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO mcp_server (
                    server_id, workspace_id, name, base_url, transport, status, metadata_json, created_at, updated_at
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(server_id) DO UPDATE SET
                    workspace_id = excluded.workspace_id,
                    name = excluded.name,
                    base_url = excluded.base_url,
                    transport = excluded.transport,
                    status = excluded.status,
                    metadata_json = excluded.metadata_json,
                    updated_at = excluded.updated_at
                """,
                (
                    server_id,
                    workspace_id,
                    name,
                    base_url,
                    transport,
                    str(payload.get("status") or "configured"),
                    json.dumps(metadata, ensure_ascii=True),
                    created,
                    created,
                ),
            )
        return self.get_mcp_server(server_id) or {"server_id": server_id}

    def list_mcp_servers(self, workspace_id: str = DEFAULT_WORKSPACE_ID) -> Dict[str, object]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT server_id, workspace_id, name, base_url, transport, status, metadata_json, created_at, updated_at
                FROM mcp_server
                WHERE workspace_id = ?
                ORDER BY created_at ASC
                """,
                (workspace_id,),
            ).fetchall()
        return {
            "data": [
                {
                    "server_id": row["server_id"],
                    "workspace_id": row["workspace_id"],
                    "name": row["name"],
                    "base_url": row["base_url"],
                    "transport": row["transport"],
                    "status": row["status"],
                    "metadata": json.loads(row["metadata_json"] or "{}"),
                    "created_at": row["created_at"],
                    "updated_at": row["updated_at"],
                }
                for row in rows
            ]
        }

    def get_mcp_server(self, server_id: str) -> Optional[Dict[str, object]]:
        with self.connect() as conn:
            row = conn.execute(
                """
                SELECT server_id, workspace_id, name, base_url, transport, status, metadata_json, created_at, updated_at
                FROM mcp_server WHERE server_id = ?
                """,
                (server_id,),
            ).fetchone()
        if not row:
            return None
        return {
            "server_id": row["server_id"],
            "workspace_id": row["workspace_id"],
            "name": row["name"],
            "base_url": row["base_url"],
            "transport": row["transport"],
            "status": row["status"],
            "metadata": json.loads(row["metadata_json"] or "{}"),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }

    def probe_mcp_server(self, server_id: str, timeout_seconds: int) -> Dict[str, object]:
        server = self.get_mcp_server(server_id)
        if not server:
            raise KeyError(server_id)
        parsed = urllib.parse.urlparse(server["base_url"])
        if not parsed.scheme or not parsed.netloc:
            raise RuntimeError("mcp server base_url is not valid")
        path = parsed.path or "/"
        conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
        start = time.time()
        conn = conn_cls(parsed.netloc, timeout=timeout_seconds)
        try:
            conn.request("GET", path)
            response = conn.getresponse()
            raw = response.read(2048)
        finally:
            conn.close()
        latency_ms = int((time.time() - start) * 1000)
        status = "healthy" if response.status < 500 else "degraded"
        with self.connect() as conn:
            current = now_ts()
            metadata = server["metadata"] or {}
            metadata["last_probe"] = {
                "status_code": response.status,
                "latency_ms": latency_ms,
                "body_preview": decode_body(raw[:400]),
            }
            conn.execute(
                """
                UPDATE mcp_server
                SET status = ?, metadata_json = ?, updated_at = ?
                WHERE server_id = ?
                """,
                (status, json.dumps(metadata, ensure_ascii=True), current, server_id),
            )
        return {
            "server_id": server_id,
            "status": status,
            "status_code": response.status,
            "latency_ms": latency_ms,
        }

    def get_mcp_tools(self, server_id: str) -> Dict[str, object]:
        server = self.get_mcp_server(server_id)
        if not server:
            raise KeyError(server_id)
        metadata = server.get("metadata") or {}
        tools = metadata.get("tools") or []
        return {"server_id": server_id, "data": tools}

    def create_run(
        self,
        kind: str,
        payload: Dict[str, object],
        workspace_id: str = DEFAULT_WORKSPACE_ID,
        model: Optional[str] = None,
        request_id: Optional[str] = None,
        topology_name: Optional[str] = None,
        status: str = "queued",
        reason: Optional[str] = None,
    ) -> Dict[str, object]:
        created = now_ts()
        run_id = new_id("run")
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO run (
                    run_id, workspace_id, kind, status, model, topology_name, task_profile,
                    request_id, reason, input_json, result_json, created_at, started_at,
                    completed_at, updated_at
                )
                VALUES (?, ?, ?, ?, ?, ?, NULL, ?, ?, ?, NULL, ?, NULL, NULL, ?)
                """,
                (
                    run_id,
                    workspace_id,
                    kind,
                    status,
                    model,
                    topology_name,
                    request_id,
                    reason,
                    json.dumps(payload, ensure_ascii=True),
                    created,
                    created,
                ),
            )
        return self.get_run(run_id, include_input=True) or {"run_id": run_id}

    def get_run(self, run_id: str, include_input: bool = False) -> Optional[Dict[str, object]]:
        with self.connect() as conn:
            row = conn.execute(
                """
                SELECT run_id, workspace_id, kind, status, model, topology_name, task_profile, request_id,
                       reason, input_json, result_json, created_at, started_at, completed_at, updated_at
                FROM run WHERE run_id = ?
                """,
                (run_id,),
            ).fetchone()
        if not row:
            return None
        result = {
            "run_id": row["run_id"],
            "workspace_id": row["workspace_id"],
            "kind": row["kind"],
            "status": row["status"],
            "model": row["model"],
            "topology_name": row["topology_name"],
            "task_profile": row["task_profile"],
            "request_id": row["request_id"],
            "reason": row["reason"],
            "result": json.loads(row["result_json"]) if row["result_json"] else None,
            "created_at": row["created_at"],
            "started_at": row["started_at"],
            "completed_at": row["completed_at"],
            "updated_at": row["updated_at"],
        }
        if include_input:
            result["input"] = json.loads(row["input_json"]) if row["input_json"] else {}
        return result

    def list_runs(self, workspace_id: str = DEFAULT_WORKSPACE_ID, limit: int = 50) -> Dict[str, object]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT run_id, workspace_id, kind, status, model, topology_name, task_profile,
                       request_id, reason, result_json, created_at, started_at, completed_at, updated_at
                FROM run
                WHERE workspace_id = ?
                ORDER BY created_at DESC
                LIMIT ?
                """,
                (workspace_id, limit),
            ).fetchall()
        return {
            "workspace_id": workspace_id,
            "data": [
                {
                    "run_id": row["run_id"],
                    "workspace_id": row["workspace_id"],
                    "kind": row["kind"],
                    "status": row["status"],
                    "model": row["model"],
                    "topology_name": row["topology_name"],
                    "task_profile": row["task_profile"],
                    "request_id": row["request_id"],
                    "reason": row["reason"],
                    "result": json.loads(row["result_json"]) if row["result_json"] else None,
                    "created_at": row["created_at"],
                    "started_at": row["started_at"],
                    "completed_at": row["completed_at"],
                    "updated_at": row["updated_at"],
                }
                for row in rows
            ],
        }

    def update_run(self, run_id: str, **fields: object) -> None:
        if not fields:
            return
        updates = []
        values: List[object] = []
        if "result" in fields:
            fields["result_json"] = json.dumps(fields.pop("result"), ensure_ascii=True)
        for key, value in fields.items():
            updates.append(f"{key} = ?")
            values.append(value)
        updates.append("updated_at = ?")
        values.append(now_ts())
        values.append(run_id)
        with self.connect() as conn:
            conn.execute(f"UPDATE run SET {', '.join(updates)} WHERE run_id = ?", values)

    def append_run_event(self, run_id: str, event_type: str, payload: Dict[str, object]) -> Dict[str, object]:
        with self._lock:
            with self.connect() as conn:
                seq_row = conn.execute(
                    "SELECT COALESCE(MAX(seq), 0) + 1 AS next_seq FROM run_event WHERE run_id = ?",
                    (run_id,),
                ).fetchone()
                seq = int(seq_row["next_seq"])
                created = now_ts()
                conn.execute(
                    """
                    INSERT INTO run_event (run_id, seq, event_type, payload_json, created_at)
                    VALUES (?, ?, ?, ?, ?)
                    """,
                    (run_id, seq, event_type, json.dumps(payload, ensure_ascii=True), created),
                )
        return {
            "run_id": run_id,
            "seq": seq,
            "event_type": event_type,
            "payload": payload,
            "created_at": created,
        }

    def get_run_events(self, run_id: str, limit: int = 200) -> Dict[str, object]:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT seq, event_type, payload_json, created_at
                FROM run_event
                WHERE run_id = ?
                ORDER BY seq ASC
                LIMIT ?
                """,
                (run_id, limit),
            ).fetchall()
        return {
            "run_id": run_id,
            "data": [
                {
                    "seq": row["seq"],
                    "event_type": row["event_type"],
                    "payload": json.loads(row["payload_json"]),
                    "created_at": row["created_at"],
                }
                for row in rows
            ],
        }

    def create_topology_plan(
        self,
        query: str,
        workspace_id: str = DEFAULT_WORKSPACE_ID,
        requested_topology: Optional[str] = None,
        run_id: Optional[str] = None,
    ) -> Dict[str, object]:
        plan = build_runtime_plan(query, requested_topology=requested_topology)
        created = now_ts()
        plan_id = new_id("plan")
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO topology_plan (
                    plan_id, run_id, workspace_id, task_profile, topology_name, rationale, plan_json, created_at
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    plan_id,
                    run_id,
                    workspace_id,
                    plan["task_profile"],
                    plan["topology_name"],
                    plan["rationale"],
                    json.dumps(plan, ensure_ascii=True),
                    created,
                ),
            )
        plan["plan_id"] = plan_id
        plan["created_at"] = created
        return plan

    def usage_summary(self, workspace_id: str = DEFAULT_WORKSPACE_ID) -> Dict[str, object]:
        with self.connect() as conn:
            runs = conn.execute(
                """
                SELECT result_json FROM run
                WHERE workspace_id = ?
                """,
                (workspace_id,),
            ).fetchall()
            access_count = conn.execute("SELECT COUNT(*) FROM proxy_access_log").fetchone()[0]
            event_count = conn.execute("SELECT COUNT(*) FROM event_batch").fetchone()[0]
        input_tokens = 0
        output_tokens = 0
        completed_runs = 0
        for row in runs:
            if not row["result_json"]:
                continue
            payload = json.loads(row["result_json"])
            usage = payload.get("usage") or {}
            input_tokens += int(usage.get("input_tokens") or 0)
            output_tokens += int(usage.get("output_tokens") or 0)
            completed_runs += 1
        return {
            "workspace_id": workspace_id,
            "requests": access_count,
            "event_batches": event_count,
            "completed_runs": completed_runs,
            "estimated_input_tokens": input_tokens,
            "estimated_output_tokens": output_tokens,
        }

    def cost_summary(self, workspace_id: str = DEFAULT_WORKSPACE_ID) -> Dict[str, object]:
        usage = self.usage_summary(workspace_id)
        return {
            "workspace_id": workspace_id,
            "estimated_cost_usd": 0,
            "currency": "USD",
            "note": "Costing not priced yet; token counts are tracked for later rollups.",
            **usage,
        }

    def me(self) -> Dict[str, object]:
        return {
            "service": "api.distributed.systems",
            "workspace": self.get_workspace(DEFAULT_WORKSPACE_ID),
            "models": INTERNAL_MODELS,
            "stats": self.stats(),
        }

    def log_proxy_access(
        self,
        method: str,
        path: str,
        target_url: Optional[str],
        status_code: Optional[int],
        latency_ms: int,
        request_bytes: int,
        response_bytes: int,
        metadata: Optional[Dict[str, object]] = None,
    ) -> None:
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO proxy_access_log (
                    request_ts, method, path, target_url, status_code, latency_ms,
                    request_bytes, response_bytes, metadata_json
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    now_ts(),
                    method,
                    path,
                    target_url,
                    status_code,
                    latency_ms,
                    request_bytes,
                    response_bytes,
                    json.dumps(metadata or {}, ensure_ascii=True),
                ),
            )

    def store_event_batch(
        self, path: str, headers: Dict[str, str], body: bytes
    ) -> Dict[str, object]:
        filtered_headers = {}
        for key, value in headers.items():
            if key.lower() in {"authorization", "x-api-key"}:
                filtered_headers[key] = "[REDACTED]"
            else:
                filtered_headers[key] = value
        preview = redact_text(decode_body(body[:100_000]))
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO event_batch (request_ts, path, headers_json, body_text)
                VALUES (?, ?, ?, ?)
                """,
                (
                    now_ts(),
                    path,
                    json.dumps(filtered_headers, ensure_ascii=True),
                    preview,
                ),
            )
            total = conn.execute("SELECT COUNT(*) FROM event_batch").fetchone()[0]
        return {"stored": True, "events_logged": total}

    def stats(self) -> Dict[str, object]:
        with self.connect() as conn:
            artifact_count = conn.execute("SELECT COUNT(*) FROM artifact").fetchone()[0]
            chunk_count = conn.execute("SELECT COUNT(*) FROM artifact_chunk").fetchone()[0]
            event_count = conn.execute("SELECT COUNT(*) FROM event_batch").fetchone()[0]
            access_count = conn.execute("SELECT COUNT(*) FROM proxy_access_log").fetchone()[0]
            run_count = conn.execute("SELECT COUNT(*) FROM run").fetchone()[0]
            mcp_count = conn.execute("SELECT COUNT(*) FROM mcp_server").fetchone()[0]
        return {
            "db_path": self.db_path,
            "workspace_root": str(self.workspace_root),
            "embedding_model": EMBEDDING_MODEL,
            "jina_configured": bool(self.jina_api_key),
            "artifacts": artifact_count,
            "chunks": chunk_count,
            "event_batches": event_count,
            "proxy_requests": access_count,
            "runs": run_count,
            "mcp_servers": mcp_count,
        }


class RunExecutor:
    def __init__(self, indexer: ArtifactIndexer) -> None:
        self.indexer = indexer
        self._threads: Dict[str, threading.Thread] = {}
        self._lock = threading.Lock()

    def launch(self, run_id: str) -> None:
        with self._lock:
            thread = self._threads.get(run_id)
            if thread and thread.is_alive():
                return
            worker = threading.Thread(target=self._execute, args=(run_id,), daemon=True)
            self._threads[run_id] = worker
            worker.start()

    def execute_inline(self, run_id: str) -> Dict[str, object]:
        self._execute(run_id)
        run = self.indexer.get_run(run_id)
        return run or {"run_id": run_id, "status": "unknown"}

    def _execute(self, run_id: str) -> None:
        run = self.indexer.get_run(run_id, include_input=True)
        if not run:
            return
        if run["status"] == "canceled":
            return

        payload = run.get("input") or {}
        query = str(payload.get("query") or payload.get("input") or "")
        if not query and isinstance(payload.get("messages"), list):
            query = collect_message_text(payload["messages"])
        requested_topology = payload.get("topology_name")
        self.indexer.update_run(run_id, status="running", started_at=now_ts())
        self.indexer.append_run_event(run_id, "run.started", {"kind": run["kind"]})

        plan = self.indexer.create_topology_plan(
            query=query,
            workspace_id=run["workspace_id"],
            requested_topology=str(requested_topology) if requested_topology else None,
            run_id=run_id,
        )
        self.indexer.update_run(
            run_id,
            topology_name=plan["topology_name"],
            task_profile=plan["task_profile"],
        )
        self.indexer.append_run_event(
            run_id,
            "topology.planned",
            {
                "plan_id": plan["plan_id"],
                "task_profile": plan["task_profile"],
                "topology_name": plan["topology_name"],
            },
        )

        if self.indexer.get_run(run_id).get("status") == "canceled":
            return

        self.indexer.append_run_event(run_id, "artifact.search.started", {"query": query})
        artifacts = self.indexer.search(query, limit=int(payload.get("artifact_limit") or 6))
        self.indexer.append_run_event(
            run_id,
            "artifact.search.completed",
            {"results": len(artifacts["results"])},
        )

        text = self._render_report(run, query, plan, artifacts)
        usage = {
            "input_tokens": estimate_tokens(query),
            "output_tokens": estimate_tokens(text),
        }
        result = {
            "run_id": run_id,
            "kind": run["kind"],
            "status": "completed",
            "task_profile": plan["task_profile"],
            "topology_name": plan["topology_name"],
            "plan": plan,
            "artifacts": artifacts["results"],
            "output_text": text,
            "usage": usage,
        }
        self.indexer.append_run_event(
            run_id,
            "run.completed",
            {"usage": usage, "artifacts": len(artifacts["results"])},
        )
        self.indexer.update_run(
            run_id,
            status="completed",
            completed_at=now_ts(),
            result=result,
        )

    def _render_report(
        self,
        run: Dict[str, object],
        query: str,
        plan: Dict[str, object],
        artifacts: Dict[str, object],
    ) -> str:
        lines = [
            "DSCO internal execution route selected.",
            "",
            f"Run kind: {run['kind']}",
            f"Task profile: {plan['task_profile']}",
            f"Topology: {plan['topology_name']}",
            f"Planner rationale: {plan['rationale']}",
            "",
            "Planned stages:",
        ]
        for stage in plan["stages"]:
            lines.append(
                f"- {stage['name']} :: role={stage['role']} replicas={stage['replicas']}"
            )
        lines.extend(["", "User objective:", query or "[empty query]", ""])
        if artifacts["results"]:
            lines.append("Relevant indexed artifacts:")
            for item in artifacts["results"]:
                source = item.get("source")
                score = item.get("score")
                preview = (item.get("text") or "").replace("\n", " ").strip()
                lines.append(f"- {source} (score={score}): {preview[:220]}")
            lines.append("")
        else:
            lines.extend(
                [
                    "Relevant indexed artifacts:",
                    "- No indexed matches were found. Reindexing or broader ingestion may be needed.",
                    "",
                ]
            )
        lines.extend(
            [
                "Execution notes:",
                "- Run state is durable in SQLite.",
                "- Artifact retrieval is available to future planner passes.",
                "- Client disconnects should not be used to kill this run.",
            ]
        )
        return "\n".join(lines)


class ProxyState:
    def __init__(
        self,
        indexer: ArtifactIndexer,
        executor: RunExecutor,
        timeout_seconds: int,
        control_plane_base: str,
    ) -> None:
        self.indexer = indexer
        self.executor = executor
        self.timeout_seconds = timeout_seconds
        self.control_plane_base = control_plane_base.rstrip("/")


class ClaudeProxyHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "dsco-claude-proxy/0.2"

    @property
    def state(self) -> ProxyState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, format: str, *args: object) -> None:
        return

    def do_GET(self) -> None:
        self.handle_request()

    def do_POST(self) -> None:
        self.handle_request()

    def do_PUT(self) -> None:
        self.handle_request()

    def do_PATCH(self) -> None:
        self.handle_request()

    def do_DELETE(self) -> None:
        self.handle_request()

    def send_json(
        self, status: int, payload: Dict[str, object], request_id: Optional[str] = None
    ) -> None:
        body = to_json(payload)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if request_id:
            self.send_header("X-Request-Id", request_id)
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True

    def send_sse_event(self, event: str, data: Dict[str, object]) -> None:
        payload = f"event: {event}\ndata: {json.dumps(data, ensure_ascii=True)}\n\n".encode(
            "utf-8"
        )
        self.wfile.write(payload)
        self.wfile.flush()

    def forward_control_plane(
        self,
        mapped_path: str,
        request_body: bytes,
        request_id: str,
    ) -> Tuple[int, int, str]:
        if not self.state.control_plane_base:
            raise RuntimeError("control plane base is not configured")
        return self.proxy_to_base(
            self.state.control_plane_base,
            mapped_path,
            request_body,
            request_id,
        )

    def post_control_plane_sink(
        self,
        mapped_path: str,
        request_body: bytes,
        request_id: str,
    ) -> bool:
        if not self.state.control_plane_base:
            return False
        parsed = urllib.parse.urlparse(self.state.control_plane_base)
        conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
        conn = conn_cls(parsed.netloc, timeout=self.state.timeout_seconds)
        path_prefix = parsed.path.rstrip("/")
        target_path = f"{path_prefix}{mapped_path}" if path_prefix else mapped_path
        if not target_path.startswith("/"):
            target_path = "/" + target_path
        try:
            headers = self.add_internal_proxy_headers(
                self.build_upstream_headers(request_id),
                target_path,
                request_body,
                request_id,
            )
            conn.request(
                "POST",
                target_path,
                body=request_body if request_body else None,
                headers=headers,
            )
            response = conn.getresponse()
            response.read()
            return 200 <= response.status < 300
        except Exception:
            return False
        finally:
            conn.close()

    def handle_request(self) -> None:
        started = time.time()
        request_id = new_id("req")
        request_body = read_request_body(self)
        path = self.path
        parsed = urllib.parse.urlparse(path)
        status_code: Optional[int] = None
        response_bytes = 0
        target_url: Optional[str] = None

        try:
            if self.handle_local_routes(parsed, request_body, request_id):
                status_code = 200
                target_url = "local"
                return

            if parsed.path.startswith("/v1/mcp/") or parsed.path.startswith("/mcp/"):
                status_code, response_bytes, target_url = self.handle_mcp_proxy(
                    parsed, request_body, request_id
                )
                return

            target = determine_target_base(parsed.path)
            if not target:
                self.send_json(
                    404,
                    {"error": "unknown_route", "path": parsed.path, "request_id": request_id},
                    request_id=request_id,
                )
                status_code = 404
                return

            status_code, response_bytes, target_url = self.forward_request(
                target_base=target,
                upstream_path=path,
                request_body=request_body,
                request_id=request_id,
            )
        except Exception as exc:
            payload = {
                "error": "proxy_failure",
                "detail": str(exc),
                "path": parsed.path,
                "request_id": request_id,
            }
            self.send_json(502, payload, request_id=request_id)
            status_code = 502
        finally:
            self.state.indexer.log_proxy_access(
                self.command,
                path,
                target_url,
                status_code,
                int((time.time() - started) * 1000),
                len(request_body),
                response_bytes,
                {"request_id": request_id},
            )

    def handle_local_routes(
        self, parsed: urllib.parse.ParseResult, request_body: bytes, request_id: str
    ) -> bool:
        path = parsed.path

        if path == "/health":
            self.send_json(200, {"ok": True, **self.state.indexer.stats()}, request_id=request_id)
            return True

        if path == "/artifacts":
            self.send_json(200, self.state.indexer.stats(), request_id=request_id)
            return True

        if path == "/artifacts/search":
            params = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
            query = params.get("q", [""])[0]
            limit = int(params.get("limit", ["8"])[0])
            self.send_json(
                200,
                self.state.indexer.search(query, limit=limit),
                request_id=request_id,
            )
            return True

        if path == "/artifacts/reindex":
            if self.command not in {"POST", "PUT"}:
                self.send_json(405, {"error": "method_not_allowed"}, request_id=request_id)
                return True
            self.send_json(200, self.state.indexer.reindex(), request_id=request_id)
            return True

        if path == "/api/event_logging/v2/batch":
            if not self.post_control_plane_sink(
                "/v1/distributed-systems/telemetry/events",
                request_body,
                request_id,
            ):
                headers = {key: value for key, value in self.headers.items()}
                self.state.indexer.store_event_batch(path, headers, request_body)
            self.send_response(204)
            self.send_header("Connection", "close")
            self.send_header("X-Request-Id", request_id)
            self.end_headers()
            self.close_connection = True
            return True

        if path == "/v1/models":
            if self.state.control_plane_base:
                self.forward_control_plane(
                    "/v1/distributed-systems/compat/models",
                    request_body,
                    request_id,
                )
                return True
            self.send_json(200, {"data": INTERNAL_MODELS}, request_id=request_id)
            return True

        if path == "/v1/messages/count_tokens":
            if self.state.control_plane_base:
                self.forward_control_plane(
                    "/v1/distributed-systems/compat/messages/count_tokens",
                    request_body,
                    request_id,
                )
                return True
            payload = read_json_bytes(request_body)
            self.send_json(
                200,
                {"input_tokens": estimate_payload_tokens(payload), "request_id": request_id},
                request_id=request_id,
            )
            return True

        if path == "/v1/mcp_servers":
            if self.state.control_plane_base:
                mapped = "/v1/distributed-systems/mcp/servers"
                if parsed.query:
                    mapped = f"{mapped}?{parsed.query}"
                self.forward_control_plane(
                    mapped,
                    request_body,
                    request_id,
                )
                return True
            self.send_json(
                200,
                {
                    "data": self.state.indexer.list_mcp_servers()["data"],
                    "has_more": False,
                    "request_id": request_id,
                },
                request_id=request_id,
            )
            return True

        if path == "/v1/messages":
            payload = read_json_bytes(request_body)
            if self.state.control_plane_base and self.should_handle_message_internally(payload):
                self.forward_control_plane(
                    "/v1/distributed-systems/compat/messages",
                    request_body,
                    request_id,
                )
                return True
            if self.should_handle_message_internally(payload):
                self.handle_internal_message(payload, request_id)
                return True
            return False

        if path.startswith("/ds/v1/"):
            if self.state.control_plane_base:
                mapped = "/v1/distributed-systems" + path[len("/ds/v1") :]
                if parsed.query:
                    mapped = f"{mapped}?{parsed.query}"
                self.forward_control_plane(mapped, request_body, request_id)
                return True
            self.handle_dsco_api(path, parsed, request_body, request_id)
            return True

        return False

    def should_handle_message_internally(self, payload: Dict[str, object]) -> bool:
        model = str(payload.get("model") or "")
        route_header = self.headers.get("X-Dsco-Route", "").lower()
        metadata = payload.get("metadata") if isinstance(payload.get("metadata"), dict) else {}
        route_value = str(metadata.get("route") or "").lower()
        return (
            INTERNAL_FIRST
            or model.startswith("dsco-")
            or route_header in {"internal", "dsco"}
            or route_value in {"internal", "dsco"}
        )

    def handle_internal_message(self, payload: Dict[str, object], request_id: str) -> None:
        run = self.state.indexer.create_run(
            kind="message",
            payload=payload,
            model=str(payload.get("model") or "dsco-router-1"),
            request_id=request_id,
        )
        result = self.state.executor.execute_inline(run["run_id"])
        output_text = ((result.get("result") or {}).get("output_text")) or ""
        model = str(payload.get("model") or "dsco-router-1")
        response = anthropic_message_payload(
            model=model,
            text=output_text,
            input_tokens=estimate_payload_tokens(payload),
            request_id=request_id,
            run_id=run["run_id"],
        )
        if payload.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("X-Request-Id", request_id)
            self.end_headers()
            start_message = dict(response)
            start_message["content"] = []
            self.send_sse_event("message_start", {"type": "message_start", "message": start_message})
            self.send_sse_event(
                "content_block_start",
                {
                    "type": "content_block_start",
                    "index": 0,
                    "content_block": {"type": "text", "text": ""},
                },
            )
            text = output_text
            for idx in range(0, len(text), 220):
                chunk = text[idx : idx + 220]
                self.send_sse_event(
                    "content_block_delta",
                    {
                        "type": "content_block_delta",
                        "index": 0,
                        "delta": {"type": "text_delta", "text": chunk},
                    },
                )
            self.send_sse_event("content_block_stop", {"type": "content_block_stop", "index": 0})
            self.send_sse_event(
                "message_delta",
                {
                    "type": "message_delta",
                    "delta": {"stop_reason": "end_turn", "stop_sequence": None},
                    "usage": response["usage"],
                },
            )
            self.send_sse_event("message_stop", {"type": "message_stop"})
            self.close_connection = True
            return
        self.send_json(200, response, request_id=request_id)

    def handle_dsco_api(
        self,
        path: str,
        parsed: urllib.parse.ParseResult,
        request_body: bytes,
        request_id: str,
    ) -> None:
        payload = read_json_bytes(request_body)
        params = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)

        if path == "/ds/v1/me":
            self.send_json(200, self.state.indexer.me(), request_id=request_id)
            return

        if path == "/ds/v1/workspaces" and self.command == "GET":
            self.send_json(200, self.state.indexer.list_workspaces(), request_id=request_id)
            return

        if path == "/ds/v1/workspaces" and self.command == "POST":
            self.send_json(201, self.state.indexer.create_workspace(payload), request_id=request_id)
            return

        if path.startswith("/ds/v1/workspaces/") and self.command == "GET":
            workspace_id = path.split("/")[-1]
            workspace = self.state.indexer.get_workspace(workspace_id)
            if not workspace:
                self.send_json(404, {"error": "workspace_not_found"}, request_id=request_id)
                return
            self.send_json(200, workspace, request_id=request_id)
            return

        if path == "/ds/v1/api_keys" and self.command == "GET":
            workspace_id = params.get("workspace_id", [None])[0]
            self.send_json(200, self.state.indexer.list_api_keys(workspace_id), request_id=request_id)
            return

        if path == "/ds/v1/api_keys" and self.command == "POST":
            self.send_json(201, self.state.indexer.create_api_key(payload), request_id=request_id)
            return

        if path.startswith("/ds/v1/api_keys/") and self.command == "DELETE":
            key_id = path.split("/")[-1]
            self.send_json(200, self.state.indexer.revoke_api_key(key_id), request_id=request_id)
            return

        if path in {"/ds/v1/artifacts", "/ds/v1/artifacts/"} and self.command == "GET":
            limit = int(params.get("limit", ["50"])[0])
            self.send_json(200, self.state.indexer.list_artifacts(limit=limit), request_id=request_id)
            return

        if path == "/ds/v1/artifacts/search" and self.command == "GET":
            query = params.get("q", [""])[0]
            limit = int(params.get("limit", ["8"])[0])
            self.send_json(
                200,
                self.state.indexer.search(query, limit=limit),
                request_id=request_id,
            )
            return

        if path == "/ds/v1/artifacts/reindex" and self.command in {"POST", "PUT"}:
            self.send_json(200, self.state.indexer.reindex(), request_id=request_id)
            return

        if path.startswith("/ds/v1/artifacts/") and path.endswith("/chunks") and self.command == "GET":
            artifact_id = path.split("/")[-2]
            self.send_json(
                200,
                self.state.indexer.get_artifact_chunks(artifact_id),
                request_id=request_id,
            )
            return

        if path.startswith("/ds/v1/artifacts/") and self.command == "GET":
            artifact_id = path.split("/")[-1]
            artifact = self.state.indexer.get_artifact(artifact_id)
            if not artifact:
                self.send_json(404, {"error": "artifact_not_found"}, request_id=request_id)
                return
            self.send_json(200, artifact, request_id=request_id)
            return

        if path == "/ds/v1/topologies/plan" and self.command == "POST":
            query = str(payload.get("query") or payload.get("input") or "")
            if not query and isinstance(payload.get("messages"), list):
                query = collect_message_text(payload["messages"])
            plan = self.state.indexer.create_topology_plan(
                query=query,
                requested_topology=str(payload.get("topology_name") or "") or None,
            )
            self.send_json(200, plan, request_id=request_id)
            return

        if path in {"/ds/v1/runs", "/ds/v1/topologies/run"} and self.command == "POST":
            kind = "topology" if path.endswith("/topologies/run") else str(payload.get("kind") or "run")
            run_payload = dict(payload)
            if path.endswith("/topologies/run"):
                run_payload["kind"] = "topology"
            run = self.state.indexer.create_run(
                kind=kind,
                payload=run_payload,
                model=str(payload.get("model") or "dsco-swarm-1"),
                request_id=request_id,
                topology_name=str(payload.get("topology_name") or "") or None,
            )
            if bool(payload.get("wait")):
                run = self.state.executor.execute_inline(run["run_id"])
                self.send_json(200, run, request_id=request_id)
                return
            self.state.executor.launch(run["run_id"])
            self.send_json(202, run, request_id=request_id)
            return

        if path == "/ds/v1/runs" and self.command == "GET":
            limit = int(params.get("limit", ["50"])[0])
            self.send_json(200, self.state.indexer.list_runs(limit=limit), request_id=request_id)
            return

        if path.startswith("/ds/v1/runs/") and path.endswith("/events") and self.command == "GET":
            run_id = path.split("/")[-2]
            limit = int(params.get("limit", ["200"])[0])
            self.send_json(
                200,
                self.state.indexer.get_run_events(run_id, limit=limit),
                request_id=request_id,
            )
            return

        if path.startswith("/ds/v1/runs/") and path.endswith("/results") and self.command == "GET":
            run_id = path.split("/")[-2]
            run = self.state.indexer.get_run(run_id)
            if not run:
                self.send_json(404, {"error": "run_not_found"}, request_id=request_id)
                return
            self.send_json(
                200,
                {"run_id": run_id, "result": run.get("result"), "status": run["status"]},
                request_id=request_id,
            )
            return

        if path.startswith("/ds/v1/runs/") and path.endswith("/cancel") and self.command == "POST":
            run_id = path.split("/")[-2]
            self.state.indexer.update_run(run_id, status="canceled", reason="canceled_by_user")
            self.state.indexer.append_run_event(
                run_id,
                "run.canceled",
                {"request_id": request_id},
            )
            self.send_json(200, {"run_id": run_id, "status": "canceled"}, request_id=request_id)
            return

        if path.startswith("/ds/v1/runs/") and path.endswith("/resume") and self.command == "POST":
            run_id = path.split("/")[-2]
            run = self.state.indexer.get_run(run_id)
            if not run:
                self.send_json(404, {"error": "run_not_found"}, request_id=request_id)
                return
            self.state.indexer.update_run(run_id, status="queued", reason="resumed")
            self.state.indexer.append_run_event(
                run_id,
                "run.resumed",
                {"request_id": request_id},
            )
            self.state.executor.launch(run_id)
            self.send_json(202, self.state.indexer.get_run(run_id), request_id=request_id)
            return

        if path.startswith("/ds/v1/runs/") and self.command == "GET":
            run_id = path.split("/")[-1]
            run = self.state.indexer.get_run(run_id)
            if not run:
                self.send_json(404, {"error": "run_not_found"}, request_id=request_id)
                return
            self.send_json(200, run, request_id=request_id)
            return

        if path == "/ds/v1/mcp/servers" and self.command == "GET":
            self.send_json(200, self.state.indexer.list_mcp_servers(), request_id=request_id)
            return

        if path == "/ds/v1/mcp/servers" and self.command == "POST":
            self.send_json(201, self.state.indexer.create_mcp_server(payload), request_id=request_id)
            return

        if path.startswith("/ds/v1/mcp/servers/") and path.endswith("/probe") and self.command == "POST":
            server_id = path.split("/")[-2]
            try:
                result = self.state.indexer.probe_mcp_server(
                    server_id, timeout_seconds=self.state.timeout_seconds
                )
            except KeyError:
                self.send_json(404, {"error": "mcp_server_not_found"}, request_id=request_id)
                return
            self.send_json(200, result, request_id=request_id)
            return

        if path.startswith("/ds/v1/mcp/servers/") and path.endswith("/tools") and self.command == "GET":
            server_id = path.split("/")[-2]
            try:
                result = self.state.indexer.get_mcp_tools(server_id)
            except KeyError:
                self.send_json(404, {"error": "mcp_server_not_found"}, request_id=request_id)
                return
            self.send_json(200, result, request_id=request_id)
            return

        if path.startswith("/ds/v1/mcp/servers/") and self.command == "GET":
            server_id = path.split("/")[-1]
            server = self.state.indexer.get_mcp_server(server_id)
            if not server:
                self.send_json(404, {"error": "mcp_server_not_found"}, request_id=request_id)
                return
            self.send_json(200, server, request_id=request_id)
            return

        if path == "/ds/v1/usage" and self.command == "GET":
            self.send_json(200, self.state.indexer.usage_summary(), request_id=request_id)
            return

        if path == "/ds/v1/costs" and self.command == "GET":
            self.send_json(200, self.state.indexer.cost_summary(), request_id=request_id)
            return

        if path == "/ds/v1/telemetry/events" and self.command == "POST":
            self.state.indexer.store_event_batch(path, {k: v for k, v in self.headers.items()}, request_body)
            self.send_json(202, {"accepted": True}, request_id=request_id)
            return

        if path == "/ds/v1/telemetry/spans" and self.command == "POST":
            self.state.indexer.store_event_batch(path, {k: v for k, v in self.headers.items()}, request_body)
            self.send_json(202, {"accepted": True}, request_id=request_id)
            return

        self.send_json(404, {"error": "unknown_dsco_route", "path": path}, request_id=request_id)

    def build_upstream_headers(self, request_id: str) -> Dict[str, str]:
        headers: Dict[str, str] = {}
        for key, value in self.headers.items():
            if key.lower() in SKIP_PROXY_REQUEST_HEADERS:
                continue
            headers[key] = value
        headers["X-Dsco-Proxy"] = "1"
        headers["X-Request-Id"] = request_id
        return headers

    def add_internal_proxy_headers(
        self,
        headers: Dict[str, str],
        target_path: str,
        request_body: bytes,
        request_id: str,
    ) -> Dict[str, str]:
        shared_secret = os.environ.get("DSCO_SHARED_SECRET", "").strip()
        if not shared_secret:
            return headers
        import hashlib as _hashlib
        import hmac as _hmac

        timestamp = str(int(time.time()))
        body_hash = _hashlib.sha256(request_body or b"").hexdigest()
        canonical = "\n".join(
            [
                self.command,
                target_path,
                request_id,
                timestamp,
                body_hash,
            ]
        )
        signature = _hmac.new(
            shared_secret.encode("utf-8"),
            canonical.encode("utf-8"),
            _hashlib.sha256,
        ).hexdigest()
        headers["X-Dsco-Proxy-Timestamp"] = timestamp
        headers["X-Dsco-Proxy-Path"] = target_path
        headers["X-Dsco-Body-Sha256"] = body_hash
        headers["X-Dsco-Proxy-Signature"] = signature
        return headers

    def proxy_to_base(
        self,
        target_base: str,
        upstream_path: str,
        request_body: bytes,
        request_id: str,
    ) -> Tuple[int, int, str]:
        parsed = urllib.parse.urlparse(target_base)
        conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
        conn = conn_cls(parsed.netloc, timeout=self.state.timeout_seconds)
        path_prefix = parsed.path.rstrip("/")
        target_path = f"{path_prefix}{upstream_path}" if path_prefix else upstream_path
        if not target_path.startswith("/"):
            target_path = "/" + target_path
        target_url = f"{parsed.scheme}://{parsed.netloc}{target_path}"
        try:
            headers = self.build_upstream_headers(request_id)
            if self.state.control_plane_base and target_base.rstrip("/") == self.state.control_plane_base:
                headers = self.add_internal_proxy_headers(
                    headers,
                    target_path,
                    request_body,
                    request_id,
                )
            conn.request(
                self.command,
                target_path,
                body=request_body if request_body else None,
                headers=headers,
            )
            response = conn.getresponse()
            self.send_response(response.status, response.reason)
            self.send_header("Connection", "close")
            self.send_header("X-Request-Id", request_id)
            for key, value in response.getheaders():
                if key.lower() in SKIP_PROXY_RESPONSE_HEADERS:
                    continue
                self.send_header(key, value)
            self.end_headers()
            total = 0
            while True:
                chunk = response.read(64 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                self.wfile.write(chunk)
                self.wfile.flush()
            self.close_connection = True
            return response.status, total, target_url
        finally:
            conn.close()

    def forward_request(
        self,
        target_base: str,
        upstream_path: str,
        request_body: bytes,
        request_id: str,
    ) -> Tuple[int, int, str]:
        return self.proxy_to_base(target_base, upstream_path, request_body, request_id)

    def handle_mcp_proxy(
        self,
        parsed: urllib.parse.ParseResult,
        request_body: bytes,
        request_id: str,
    ) -> Tuple[int, int, str]:
        path = parsed.path
        if path.startswith("/v1/mcp/"):
            prefix = "/v1/mcp/"
        else:
            prefix = "/mcp/"
        rest = path[len(prefix) :]
        parts = rest.split("/", 1)
        server_id = parts[0]
        suffix = "/" + parts[1] if len(parts) > 1 else "/"
        if parsed.query:
            suffix = f"{suffix}?{parsed.query}"
        server = self.state.indexer.get_mcp_server(server_id)
        if server and server.get("base_url"):
            return self.proxy_to_base(server["base_url"], suffix, request_body, request_id)
        return self.proxy_to_base(ANTHROPIC_MCP_BASE, self.path, request_body, request_id)


def determine_target_base(path: str) -> Optional[str]:
    if path.startswith("/v1/"):
        return ANTHROPIC_BASE
    if path.startswith("/api/"):
        return CLAUDE_WEB_BASE
    return None


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run a local Claude-compatible DSCO edge with SQLite-backed state."
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--db", default=DEFAULT_DB)
    parser.add_argument("--workspace-root", default=str(Path.cwd()))
    parser.add_argument("--timeout-seconds", type=int, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--control-plane-base", default=CONTROL_PLANE_BASE)
    parser.add_argument("--max-artifact-bytes", type=int, default=DEFAULT_MAX_ARTIFACT_BYTES)
    parser.add_argument("--max-artifacts", type=int, default=DEFAULT_MAX_ARTIFACTS)
    parser.add_argument(
        "--skip-startup-reindex",
        action="store_true",
        help="Do not scan and embed critical artifacts at startup.",
    )
    return parser


def main() -> None:
    args = build_parser().parse_args()
    indexer = ArtifactIndexer(
        db_path=args.db,
        workspace_root=args.workspace_root,
        max_artifact_bytes=args.max_artifact_bytes,
        max_artifacts=args.max_artifacts,
    )
    if not args.skip_startup_reindex:
        indexer.reindex()
    executor = RunExecutor(indexer)
    state = ProxyState(
        indexer=indexer,
        executor=executor,
        timeout_seconds=args.timeout_seconds,
        control_plane_base=args.control_plane_base,
    )
    server = ThreadingHTTPServer((args.host, args.port), ClaudeProxyHandler)
    server.state = state  # type: ignore[attr-defined]

    print(
        json.dumps(
            {
                "listening": f"http://{args.host}:{args.port}",
                "db": args.db,
                "workspace_root": str(Path(args.workspace_root).resolve()),
                "embedding_model": EMBEDDING_MODEL,
                "anthropic_base": ANTHROPIC_BASE,
                "anthropic_mcp_base": ANTHROPIC_MCP_BASE,
                "claude_web_base": CLAUDE_WEB_BASE,
                "control_plane_base": args.control_plane_base,
                "internal_first": INTERNAL_FIRST,
                "startup_reindex": not args.skip_startup_reindex,
            },
            ensure_ascii=True,
        )
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
