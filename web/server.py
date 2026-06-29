#!/usr/bin/env python3
"""dsco web UI — FastAPI + WebSocket + WebRTC agent server.

Multi-provider support: Anthropic (native), OpenAI, OpenRouter, Groq, DeepSeek,
Mistral, Together, xAI, Perplexity, Cerebras, Cohere — all via OpenAI-compat SDK.
"""

import argparse
import asyncio
import base64
import csv
import hashlib
import hmac
import json
import logging
import os
import sqlite3
import subprocess
import time
import traceback
import uuid
from collections import defaultdict, deque
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Optional

import anthropic
import httpx
import openai
import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.encoders import jsonable_encoder
from fastapi.responses import HTMLResponse, FileResponse, JSONResponse, PlainTextResponse

# Optional WebRTC
try:
    from aiortc import RTCPeerConnection, RTCSessionDescription
    from aiortc.contrib.media import MediaRelay
    HAS_WEBRTC = True
except ImportError:
    HAS_WEBRTC = False

log = logging.getLogger("dsco.ui")

MAX_LIST_LIMIT = 100
MAX_RESPONSE_BYTES = 256 * 1024
DEFAULT_FRESHNESS_MINUTES = 120
METRIC_HISTORY_SIZE = 240
_endpoint_metrics: dict[str, dict[str, Any]] = defaultdict(lambda: {
    "calls": 0,
    "errors": 0,
    "latencies_ms": deque(maxlen=METRIC_HISTORY_SIZE),
    "last_ms": 0.0,
})

# ── Load .env file ───────────────────────────────────────────────────────────

def _load_dotenv():
    """Load .env file from project root into os.environ (no dependency needed)."""
    for candidate in [Path.cwd() / ".env", Path(__file__).parent.parent / ".env"]:
        if candidate.exists():
            for line in candidate.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, _, val = line.partition("=")
                    key, val = key.strip(), val.strip()
                    # set from .env when unset OR when the existing value is blank
                    # (a shell-exported empty var would otherwise shadow a real key)
                    if val and not os.environ.get(key, "").strip():
                        os.environ[key] = val
            log.info(f"Loaded .env from {candidate}")
            return

_load_dotenv()

# ── Configuration ────────────────────────────────────────────────────────────

WEB_DIR = Path(__file__).parent
STATIC_DIR = WEB_DIR / "static"
DSCO_BIN = WEB_DIR.parent / "dsco"
WORK_DIR = Path(os.getenv("DSCO_WORK_DIR", str(Path.cwd())))
DSCO_VERSION_CACHE: str = "unknown"

DEFAULT_MODEL = os.getenv("DSCO_MODEL", "claude-sonnet-4-6")
DEFAULT_PORT = int(os.getenv("DSCO_UI_PORT", "3141"))
MAX_TOKENS = 16384
MAX_TOOL_OUTPUT = 64 * 1024
MAX_TURNS = 200
CONTROL_PLANE_DIR = WEB_DIR.parent / ".workspace" / "control_plane"
CONTROL_PLANE_DB = Path(os.getenv("DSCO_CONTROL_PLANE_DB", str(CONTROL_PLANE_DIR / "control_plane.db")))
CONTROL_PLANE_SCHEMA_VERSION = 1
_control_plane_ready = False

SYSTEM_PROMPT = """You are dsco, an AI software engineering agent running in a web interface.

You help users build, debug, and understand software. You have access to tools for reading/writing files, executing commands, and searching codebases.

Guidelines:
- Be concise and direct. Show code rather than explaining it when possible.
- Use tools to investigate before making assumptions.
- Execute commands to verify your changes work.
- When editing files, read them first to understand the context.
- Prefer targeted edits over full file rewrites.

Working directory: {work_dir}"""

# ── Provider Detection & Routing ─────────────────────────────────────────────

# Mirrors provider_detect() from provider.c
PROVIDER_ENDPOINTS = {
    "openai":     {"base_url": "https://api.openai.com/v1",         "env": "OPENAI_API_KEY"},
    "groq":       {"base_url": "https://api.groq.com/openai/v1",    "env": "GROQ_API_KEY"},
    "deepseek":   {"base_url": "https://api.deepseek.com/v1",       "env": "DEEPSEEK_API_KEY"},
    "together":   {"base_url": "https://api.together.xyz/v1",       "env": "TOGETHER_API_KEY"},
    "mistral":    {"base_url": "https://api.mistral.ai/v1",         "env": "MISTRAL_API_KEY"},
    "openrouter": {"base_url": "https://openrouter.ai/api/v1",      "env": "OPENROUTER_API_KEY"},
    "perplexity": {"base_url": "https://api.perplexity.ai",         "env": "PERPLEXITY_API_KEY"},
    "cerebras":   {"base_url": "https://api.cerebras.ai/v1",        "env": "CEREBRAS_API_KEY"},
    "xai":        {"base_url": "https://api.x.ai/v1",               "env": "XAI_API_KEY"},
    "cohere":     {"base_url": "https://api.cohere.com/v2",         "env": "COHERE_API_KEY"},
}


def detect_provider(model_id: str) -> str:
    """Detect provider from model ID — mirrors provider.c logic."""
    if not model_id:
        return "anthropic"
    m = model_id.lower()
    # Explicit prefix
    if m.startswith("openrouter:") or m.startswith("openrouter/"):
        return "openrouter"
    # Slash-based IDs route to OpenRouter
    if "/" in model_id:
        return "openrouter"
    # Anthropic bare IDs
    if any(k in m for k in ("claude", "opus", "sonnet", "haiku")):
        return "anthropic"
    # OpenAI bare IDs
    if "gpt" in m or m.startswith("o1") or m.startswith("o3") or m.startswith("o4") or "chatgpt" in m:
        return "openai"
    # Groq
    if any(k in m for k in ("llama", "mixtral", "gemma")):
        return "groq"
    if "deepseek" in m:
        return "deepseek"
    if any(k in m for k in ("mistral", "codestral", "pixtral")):
        return "mistral"
    if "qwen" in m or "together" in m:
        return "together"
    if "command" in m:
        return "cohere"
    if "grok" in m:
        return "xai"
    if "sonar" in m or "pplx" in m:
        return "perplexity"
    if "cerebras" in m:
        return "cerebras"
    return "anthropic"


def get_provider_key(provider: str) -> Optional[str]:
    """Get API key for a provider from environment."""
    if provider == "anthropic":
        return os.getenv("ANTHROPIC_API_KEY")
    ep = PROVIDER_ENDPOINTS.get(provider)
    if ep:
        return os.getenv(ep["env"])
    return None


# ── Dynamic Model Registry ───────────────────────────────────────────────────

MODEL_REGISTRY: list[dict] = []


def load_model_registry():
    """Load models from `dsco --models-json` if available."""
    global MODEL_REGISTRY
    try:
        result = subprocess.run(
            [str(DSCO_BIN), "--models-json"],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0 and result.stdout.strip():
            MODEL_REGISTRY = json.loads(result.stdout)
            log.info(f"Loaded {len(MODEL_REGISTRY)} models from dsco binary")
            return
    except Exception as e:
        log.warning(f"Could not load models from dsco: {e}")

    # Fallback: minimal set
    MODEL_REGISTRY = [
        {"alias": "opus", "model_id": "claude-opus-4-7", "context_window": 200000,
         "max_output": 32000, "input_price": 15.0, "output_price": 75.0,
         "cache_read_price": 1.5, "cache_write_price": 18.75, "supports_thinking": 1},
        {"alias": "sonnet", "model_id": "claude-sonnet-4-6", "context_window": 200000,
         "max_output": 16000, "input_price": 3.0, "output_price": 15.0,
         "cache_read_price": 0.3, "cache_write_price": 3.75, "supports_thinking": 1},
        {"alias": "haiku", "model_id": "claude-haiku-4-5-20251001", "context_window": 200000,
         "max_output": 8192, "input_price": 0.8, "output_price": 4.0,
         "cache_read_price": 0.08, "cache_write_price": 1.0, "supports_thinking": 0},
    ]


def model_info(model_id_or_alias: str) -> Optional[dict]:
    """Look up model info by alias or model_id."""
    for m in MODEL_REGISTRY:
        if m["alias"] == model_id_or_alias or m["model_id"] == model_id_or_alias:
            return m
    return None


def resolve_model(name: str) -> str:
    """Resolve alias to full model_id, passthrough if unknown."""
    m = model_info(name)
    return m["model_id"] if m else name


def _clamp_int(value: Any, default: int, minimum: int = 1, maximum: int = MAX_LIST_LIMIT) -> int:
    try:
        return max(minimum, min(maximum, int(value)))
    except Exception:
        return default


def _pctile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(values)
    idx = (len(ordered) - 1) * pct
    lo = int(idx)
    hi = min(lo + 1, len(ordered) - 1)
    frac = idx - lo
    return float(ordered[lo] * (1 - frac) + ordered[hi] * frac)


def _record_metric(name: str, elapsed_ms: float, ok: bool = True) -> None:
    metric = _endpoint_metrics[name]
    metric["calls"] += 1
    metric["last_ms"] = float(elapsed_ms)
    metric["latencies_ms"].append(float(elapsed_ms))
    if not ok:
        metric["errors"] += 1


def _metrics_snapshot() -> dict[str, Any]:
    endpoints = {}
    total_calls = 0
    total_errors = 0
    for name, metric in sorted(_endpoint_metrics.items()):
        latencies = list(metric["latencies_ms"])
        endpoints[name] = {
            "calls": metric["calls"],
            "errors": metric["errors"],
            "last_ms": round(metric["last_ms"], 2),
            "p95_ms": round(_pctile(latencies, 0.95), 2),
            "max_ms": round(max(latencies) if latencies else 0.0, 2),
        }
        total_calls += metric["calls"]
        total_errors += metric["errors"]
    return {
        "generated_at": time.time(),
        "totals": {
            "calls": total_calls,
            "errors": total_errors,
            "error_rate": round(total_errors / total_calls, 4) if total_calls else 0.0,
        },
        "endpoints": endpoints,
    }


# ── Control Plane Store ──────────────────────────────────────────────────────

def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _bool_flag(value: Any) -> int:
    if isinstance(value, str):
        return 0 if value.strip().lower() in ("", "0", "false", "off", "no") else 1
    return 1 if value else 0


def _json_text(value: Any) -> str:
    return json.dumps(value if value is not None else {}, sort_keys=True, separators=(",", ":"))


def _json_value(value: str, default: Any) -> Any:
    try:
        return json.loads(value) if value else default
    except Exception:
        return default


def _provider_env_name(provider: str) -> str:
    if provider == "anthropic":
        return "ANTHROPIC_API_KEY"
    ep = PROVIDER_ENDPOINTS.get(provider)
    return ep["env"] if ep else ""


def _provider_base_url(provider: str) -> str:
    ep = PROVIDER_ENDPOINTS.get(provider)
    return ep["base_url"] if ep else ""


def _mask_secret(secret: str) -> str:
    secret = (secret or "").strip()
    if not secret:
        return ""
    if len(secret) <= 8:
        return f"{secret[:2]}…{secret[-2:]}"
    return f"{secret[:4]}…{secret[-4:]}"


def _secret_fingerprint(secret: str) -> str:
    secret = (secret or "").strip()
    if not secret:
        return ""
    return hashlib.sha256(secret.encode("utf-8")).hexdigest()[:12]


def _estimate_request_cost(model_id: str, input_tokens: int, output_tokens: int, cache_read_tokens: int = 0) -> float:
    info = model_info(model_id)
    if not info:
        return 0.0
    input_price = float(info.get("input_price") or 0.0)
    output_price = float(info.get("output_price") or 0.0)
    cache_price = float(info.get("cache_read_price") or 0.0)
    cost = (
        (max(0, input_tokens) / 1_000_000.0) * input_price +
        (max(0, output_tokens) / 1_000_000.0) * output_price +
        (max(0, cache_read_tokens) / 1_000_000.0) * cache_price
    )
    return round(cost, 6)


def _seed_control_plane(conn: sqlite3.Connection) -> None:
    now = _now_iso()
    if conn.execute("SELECT COUNT(*) FROM plans").fetchone()[0] == 0:
        conn.executemany(
            """
            INSERT INTO plans (
                id, name, price_monthly, included_input_tokens, included_output_tokens,
                overage_rate_input, overage_rate_output, seats, byok_allowed,
                managed_allowed, hosted_models_allowed, is_active, notes, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    "starter-byok", "Starter BYOK", 0.0, 750_000, 250_000,
                    0.0, 0.0, 1, 1, 0, 0, 1,
                    "Request-scoped BYOK lane for local tenants that want DSCO routing without shared provider spend.",
                    now, now,
                ),
                (
                    "team-managed", "Team Managed", 49.0, 5_000_000, 2_000_000,
                    3.0, 15.0, 5, 0, 1, 1, 1,
                    "Managed credentials, shared budgets, and DSCO-routed hosted backends when available.",
                    now, now,
                ),
                (
                    "hybrid-scale", "Hybrid Scale", 249.0, 30_000_000, 12_000_000,
                    2.5, 12.0, 25, 1, 1, 1, 1,
                    "Hybrid tenant lane with BYOK fallback, managed routing, and DSCO-hosted model eligibility.",
                    now, now,
                ),
            ],
        )

    if conn.execute("SELECT COUNT(*) FROM engine_backends").fetchone()[0] == 0:
        modal_env = os.getenv("DSCO_MODAL_API_ENV", "DSCO_MODAL_API_KEY")
        modal_base = os.getenv("DSCO_MODAL_BASE_URL", "https://modal.distributed.systems/openai/v1")
        conn.executemany(
            """
            INSERT INTO engine_backends (
                id, name, provider, transport, base_url, api_key_env, hosted_by_dsco,
                status, enabled, priority, notes, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    "managed-anthropic", "Managed Anthropic", "anthropic", "anthropic", "",
                    "ANTHROPIC_API_KEY", 0, "live" if os.getenv("ANTHROPIC_API_KEY") else "planned",
                    1, 40, "Direct DSCO-managed Anthropic credentials.", now, now,
                ),
                (
                    "managed-openai", "Managed OpenAI", "openai", "openai_compat",
                    _provider_base_url("openai"), "OPENAI_API_KEY", 0,
                    "live" if os.getenv("OPENAI_API_KEY") else "planned",
                    1, 50, "Direct DSCO-managed OpenAI credentials.", now, now,
                ),
                (
                    "managed-openrouter", "Managed OpenRouter", "openrouter", "openai_compat",
                    _provider_base_url("openrouter"), "OPENROUTER_API_KEY", 0,
                    "live" if os.getenv("OPENROUTER_API_KEY") else "planned",
                    1, 60, "Cross-provider DSCO routing lane backed by OpenRouter.", now, now,
                ),
                (
                    "modal-openai", "DSCO Modal Gateway", "openai", "openai_compat",
                    modal_base, modal_env, 1,
                    "live" if os.getenv(modal_env) else "planned",
                    1, 10, "Reserved DSCO-hosted inference gateway for managed plans and private serving.", now, now,
                ),
            ],
        )

    if conn.execute("SELECT COUNT(*) FROM people").fetchone()[0] == 0:
        conn.executemany(
            """
            INSERT INTO people (
                id, name, email, organization, plan_id, status, auth_policy,
                monthly_spend_limit, metadata_json, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    "founder-operator", "Founder Operator", "operator@distributed.systems", "Distributed Systems",
                    "hybrid-scale", "active", "managed_or_byok", 500.0,
                    _json_text({"seed": True, "role": "operator"}), now, now,
                ),
                (
                    "ops-team", "Operations Team", "ops@alpha.local", "Alpha Labs",
                    "team-managed", "active", "managed_only", 150.0,
                    _json_text({"seed": True, "role": "ops"}), now, now,
                ),
                (
                    "builder-studio", "Builder Studio", "builder@studio.local", "Studio Zero",
                    "starter-byok", "active", "byok_only", 75.0,
                    _json_text({"seed": True, "role": "builder"}), now, now,
                ),
            ],
        )


def _ensure_control_plane() -> None:
    global _control_plane_ready
    if _control_plane_ready and CONTROL_PLANE_DB.exists():
        return
    CONTROL_PLANE_DIR.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(CONTROL_PLANE_DB, timeout=30)
    try:
        conn.executescript(
            """
            PRAGMA journal_mode=WAL;
            PRAGMA foreign_keys=ON;

            CREATE TABLE IF NOT EXISTS cp_meta (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS plans (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL UNIQUE,
                price_monthly REAL NOT NULL DEFAULT 0,
                included_input_tokens INTEGER NOT NULL DEFAULT 0,
                included_output_tokens INTEGER NOT NULL DEFAULT 0,
                overage_rate_input REAL NOT NULL DEFAULT 0,
                overage_rate_output REAL NOT NULL DEFAULT 0,
                seats INTEGER NOT NULL DEFAULT 1,
                byok_allowed INTEGER NOT NULL DEFAULT 1,
                managed_allowed INTEGER NOT NULL DEFAULT 1,
                hosted_models_allowed INTEGER NOT NULL DEFAULT 0,
                is_active INTEGER NOT NULL DEFAULT 1,
                notes TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS people (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                email TEXT NOT NULL UNIQUE,
                organization TEXT NOT NULL DEFAULT '',
                plan_id TEXT NOT NULL REFERENCES plans(id),
                status TEXT NOT NULL DEFAULT 'active',
                auth_policy TEXT NOT NULL DEFAULT 'managed_or_byok',
                monthly_spend_limit REAL NOT NULL DEFAULT 0,
                metadata_json TEXT NOT NULL DEFAULT '{}',
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS engine_backends (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL UNIQUE,
                provider TEXT NOT NULL,
                transport TEXT NOT NULL,
                base_url TEXT NOT NULL DEFAULT '',
                api_key_env TEXT NOT NULL DEFAULT '',
                hosted_by_dsco INTEGER NOT NULL DEFAULT 0,
                status TEXT NOT NULL DEFAULT 'planned',
                enabled INTEGER NOT NULL DEFAULT 1,
                priority INTEGER NOT NULL DEFAULT 100,
                notes TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS request_logs (
                id TEXT PRIMARY KEY,
                person_id TEXT,
                person_email TEXT NOT NULL DEFAULT '',
                person_name TEXT NOT NULL DEFAULT '',
                organization TEXT NOT NULL DEFAULT '',
                plan_id TEXT NOT NULL DEFAULT '',
                model TEXT NOT NULL,
                provider TEXT NOT NULL,
                backend_id TEXT NOT NULL DEFAULT '',
                auth_mode TEXT NOT NULL,
                route_source TEXT NOT NULL,
                status TEXT NOT NULL,
                input_tokens INTEGER NOT NULL DEFAULT 0,
                output_tokens INTEGER NOT NULL DEFAULT 0,
                cache_read_tokens INTEGER NOT NULL DEFAULT 0,
                latency_ms REAL NOT NULL DEFAULT 0,
                estimated_cost_usd REAL NOT NULL DEFAULT 0,
                error TEXT NOT NULL DEFAULT '',
                metadata_json TEXT NOT NULL DEFAULT '{}',
                created_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS credential_bindings (
                id TEXT PRIMARY KEY,
                person_id TEXT NOT NULL DEFAULT '',
                provider TEXT NOT NULL,
                label TEXT NOT NULL DEFAULT '',
                source TEXT NOT NULL,
                secret_ref TEXT NOT NULL DEFAULT '',
                masked_key TEXT NOT NULL DEFAULT '',
                fingerprint TEXT NOT NULL DEFAULT '',
                last_used_at TEXT,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                UNIQUE (person_id, provider, source, secret_ref, fingerprint)
            );

            CREATE INDEX IF NOT EXISTS idx_people_plan_id ON people(plan_id);
            CREATE INDEX IF NOT EXISTS idx_request_logs_person_created ON request_logs(person_id, created_at DESC);
            CREATE INDEX IF NOT EXISTS idx_request_logs_status_created ON request_logs(status, created_at DESC);
            CREATE INDEX IF NOT EXISTS idx_request_logs_backend_created ON request_logs(backend_id, created_at DESC);
            CREATE INDEX IF NOT EXISTS idx_credentials_person_provider ON credential_bindings(person_id, provider);
            """
        )
        conn.execute(
            "INSERT INTO cp_meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            ("schema_version", str(CONTROL_PLANE_SCHEMA_VERSION)),
        )
        _seed_control_plane(conn)
        conn.commit()
        _control_plane_ready = True
    finally:
        conn.close()


def _control_plane_conn() -> sqlite3.Connection:
    _ensure_control_plane()
    conn = sqlite3.connect(CONTROL_PLANE_DB, timeout=30)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def _serialize_plan(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["id"],
        "name": row["name"],
        "price_monthly": float(row["price_monthly"] or 0.0),
        "included_input_tokens": int(row["included_input_tokens"] or 0),
        "included_output_tokens": int(row["included_output_tokens"] or 0),
        "overage_rate_input": float(row["overage_rate_input"] or 0.0),
        "overage_rate_output": float(row["overage_rate_output"] or 0.0),
        "seats": int(row["seats"] or 0),
        "byok_allowed": bool(row["byok_allowed"]),
        "managed_allowed": bool(row["managed_allowed"]),
        "hosted_models_allowed": bool(row["hosted_models_allowed"]),
        "is_active": bool(row["is_active"]),
        "notes": row["notes"] or "",
        "member_count": int(row["member_count"] or 0) if "member_count" in row.keys() else 0,
        "request_count": int(row["request_count"] or 0) if "request_count" in row.keys() else 0,
        "estimated_cost_usd": round(float(row["estimated_cost_usd"] or 0.0), 6) if "estimated_cost_usd" in row.keys() else 0.0,
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _serialize_person(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["id"],
        "name": row["name"],
        "email": row["email"],
        "organization": row["organization"] or "",
        "plan_id": row["plan_id"],
        "plan_name": row["plan_name"] if "plan_name" in row.keys() else row["plan_id"],
        "status": row["status"],
        "auth_policy": row["auth_policy"],
        "monthly_spend_limit": float(row["monthly_spend_limit"] or 0.0),
        "metadata": _json_value(row["metadata_json"], {}),
        "request_count": int(row["request_count"] or 0) if "request_count" in row.keys() else 0,
        "input_tokens": int(row["input_tokens"] or 0) if "input_tokens" in row.keys() else 0,
        "output_tokens": int(row["output_tokens"] or 0) if "output_tokens" in row.keys() else 0,
        "cache_read_tokens": int(row["cache_read_tokens"] or 0) if "cache_read_tokens" in row.keys() else 0,
        "estimated_cost_usd": round(float(row["estimated_cost_usd"] or 0.0), 6) if "estimated_cost_usd" in row.keys() else 0.0,
        "last_seen_at": row["last_seen_at"] if "last_seen_at" in row.keys() else None,
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _serialize_backend(row: sqlite3.Row) -> dict[str, Any]:
    env_name = (row["api_key_env"] or "").strip()
    has_key = bool(env_name and os.getenv(env_name))
    ready = bool(row["enabled"]) and (not env_name or has_key) and row["status"] != "disabled"
    return {
        "id": row["id"],
        "name": row["name"],
        "provider": row["provider"],
        "transport": row["transport"],
        "base_url": row["base_url"] or _provider_base_url(row["provider"]),
        "api_key_env": env_name,
        "hosted_by_dsco": bool(row["hosted_by_dsco"]),
        "status": row["status"],
        "enabled": bool(row["enabled"]),
        "priority": int(row["priority"] or 0),
        "notes": row["notes"] or "",
        "has_managed_key": has_key,
        "ready": ready,
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _serialize_credential(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["id"],
        "person_id": row["person_id"] or "",
        "person_name": row["person_name"] if "person_name" in row.keys() else "",
        "provider": row["provider"],
        "label": row["label"] or "",
        "source": row["source"],
        "secret_ref": row["secret_ref"] or "",
        "masked_key": row["masked_key"] or "",
        "fingerprint": row["fingerprint"] or "",
        "last_used_at": row["last_used_at"],
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _serialize_request(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["id"],
        "person_id": row["person_id"] or "",
        "person_email": row["person_email"] or "",
        "person_name": row["person_name"] or "",
        "organization": row["organization"] or "",
        "plan_id": row["plan_id"] or "",
        "model": row["model"],
        "provider": row["provider"],
        "backend_id": row["backend_id"] or "",
        "auth_mode": row["auth_mode"],
        "route_source": row["route_source"],
        "status": row["status"],
        "input_tokens": int(row["input_tokens"] or 0),
        "output_tokens": int(row["output_tokens"] or 0),
        "cache_read_tokens": int(row["cache_read_tokens"] or 0),
        "latency_ms": round(float(row["latency_ms"] or 0.0), 2),
        "estimated_cost_usd": round(float(row["estimated_cost_usd"] or 0.0), 6),
        "error": row["error"] or "",
        "metadata": _json_value(row["metadata_json"], {}),
        "created_at": row["created_at"],
    }


def _lookup_person(conn: sqlite3.Connection, person_id: str = "", email: str = "") -> Optional[sqlite3.Row]:
    pid = (person_id or "").strip()
    em = (email or "").strip().lower()
    if pid:
        return conn.execute("SELECT * FROM people WHERE id = ?", (pid,)).fetchone()
    if em:
        return conn.execute("SELECT * FROM people WHERE lower(email) = ?", (em,)).fetchone()
    return None


def _lookup_plan(conn: sqlite3.Connection, plan_id: str) -> Optional[sqlite3.Row]:
    return conn.execute("SELECT * FROM plans WHERE id = ?", (plan_id,)).fetchone()


def _lookup_backend(conn: sqlite3.Connection, backend_id: str) -> Optional[sqlite3.Row]:
    return conn.execute("SELECT * FROM engine_backends WHERE id = ?", (backend_id,)).fetchone()


def _backend_candidates(conn: sqlite3.Connection, provider: str) -> list[sqlite3.Row]:
    return conn.execute(
        """
        SELECT * FROM engine_backends
        WHERE provider = ? AND enabled = 1
        ORDER BY priority ASC, created_at ASC
        """,
        (provider,),
    ).fetchall()


def _month_start_iso() -> str:
    now = datetime.now(timezone.utc)
    return now.replace(day=1, hour=0, minute=0, second=0, microsecond=0).isoformat(timespec="seconds")


def _person_month_usage(conn: sqlite3.Connection, person_id: str) -> dict[str, Any]:
    row = conn.execute(
        """
        SELECT
            COUNT(*) AS request_count,
            COALESCE(SUM(estimated_cost_usd), 0) AS spend_usd,
            COALESCE(SUM(input_tokens), 0) AS input_tokens,
            COALESCE(SUM(output_tokens), 0) AS output_tokens
        FROM request_logs
        WHERE person_id = ? AND created_at >= ?
        """,
        (person_id, _month_start_iso()),
    ).fetchone()
    return {
        "request_count": int(row["request_count"] or 0),
        "spend_usd": float(row["spend_usd"] or 0.0),
        "input_tokens": int(row["input_tokens"] or 0),
        "output_tokens": int(row["output_tokens"] or 0),
    }


def _auth_policy_error(person: sqlite3.Row, plan: sqlite3.Row, wants_byok: bool, backend: Optional[sqlite3.Row]) -> Optional[str]:
    if person["status"] != "active":
        return f"person status is {person['status']}"
    policy = person["auth_policy"]
    if wants_byok:
        if policy == "managed_only":
            return "person policy requires managed routing"
        if not bool(plan["byok_allowed"]):
            return "plan does not allow BYOK credentials"
    else:
        if policy == "byok_only":
            return "person policy requires BYOK credentials"
        if not bool(plan["managed_allowed"]):
            return "plan does not allow managed credentials"
    if backend and bool(backend["hosted_by_dsco"]) and not bool(plan["hosted_models_allowed"]):
        return "plan is not allowed to use DSCO-hosted backends"
    return None


def _select_backend(conn: sqlite3.Connection, provider: str, wants_byok: bool, backend_id: str = "") -> Optional[sqlite3.Row]:
    if backend_id:
        return _lookup_backend(conn, backend_id)
    for row in _backend_candidates(conn, provider):
        env_name = (row["api_key_env"] or "").strip()
        if wants_byok or not env_name or os.getenv(env_name):
            return row
    return None


def _implicit_backend(provider: str) -> dict[str, Any]:
    return {
        "id": f"implicit-{provider}",
        "name": f"Implicit {provider}",
        "provider": provider,
        "transport": "anthropic" if provider == "anthropic" else "openai_compat",
        "base_url": _provider_base_url(provider),
        "api_key_env": _provider_env_name(provider),
        "hosted_by_dsco": False,
        "status": "live" if get_provider_key(provider) else "planned",
        "enabled": True,
        "priority": 999,
        "notes": "Implicit provider route derived from DSCO provider registry.",
        "has_managed_key": bool(get_provider_key(provider)),
        "ready": bool(get_provider_key(provider)),
        "created_at": "",
        "updated_at": "",
    }


def _record_request_log(conn: sqlite3.Connection, payload: dict[str, Any]) -> None:
    conn.execute(
        """
        INSERT INTO request_logs (
            id, person_id, person_email, person_name, organization, plan_id, model, provider,
            backend_id, auth_mode, route_source, status, input_tokens, output_tokens,
            cache_read_tokens, latency_ms, estimated_cost_usd, error, metadata_json, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            payload["id"], payload.get("person_id", ""), payload.get("person_email", ""),
            payload.get("person_name", ""), payload.get("organization", ""), payload.get("plan_id", ""),
            payload["model"], payload["provider"], payload.get("backend_id", ""), payload["auth_mode"],
            payload["route_source"], payload["status"], int(payload.get("input_tokens", 0) or 0),
            int(payload.get("output_tokens", 0) or 0), int(payload.get("cache_read_tokens", 0) or 0),
            float(payload.get("latency_ms", 0.0) or 0.0), float(payload.get("estimated_cost_usd", 0.0) or 0.0),
            payload.get("error", ""), _json_text(payload.get("metadata", {})), payload["created_at"],
        ),
    )


def _upsert_credential_binding(
    conn: sqlite3.Connection,
    person_id: str,
    person_name: str,
    provider: str,
    source: str,
    secret_ref: str = "",
    api_key: str = "",
    label: str = "",
) -> None:
    now = _now_iso()
    person_key = person_id or ""
    secret_ref = secret_ref or ""
    fingerprint = _secret_fingerprint(api_key)
    masked = _mask_secret(api_key)
    row = conn.execute(
        """
        SELECT id FROM credential_bindings
        WHERE person_id = ? AND provider = ? AND source = ? AND secret_ref = ? AND fingerprint = ?
        """,
        (person_key, provider, source, secret_ref, fingerprint),
    ).fetchone()
    if row:
        conn.execute(
            """
            UPDATE credential_bindings
            SET label = ?, masked_key = ?, last_used_at = ?, updated_at = ?
            WHERE id = ?
            """,
            (label or person_name or "", masked, now, now, row["id"]),
        )
        return
    conn.execute(
        """
        INSERT INTO credential_bindings (
            id, person_id, provider, label, source, secret_ref, masked_key,
            fingerprint, last_used_at, created_at, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            str(uuid.uuid4())[:12], person_key, provider, label or person_name or "", source,
            secret_ref, masked, fingerprint, now, now, now,
        ),
    )


def _normalize_engine_messages(body: dict[str, Any]) -> tuple[str, list[dict[str, str]], list[dict[str, str]]]:
    raw_messages = body.get("messages")
    if not isinstance(raw_messages, list) or not raw_messages:
        prompt = str(body.get("prompt", "")).strip()
        if not prompt:
            raise ValueError("prompt or messages is required")
        raw_messages = [{"role": "user", "content": prompt}]

    system_parts: list[str] = []
    openai_messages: list[dict[str, str]] = []
    anthropic_messages: list[dict[str, str]] = []
    for msg in raw_messages:
        if not isinstance(msg, dict):
            continue
        role = str(msg.get("role", "user")).strip().lower() or "user"
        content = msg.get("content", "")
        if isinstance(content, (dict, list)):
            content = json.dumps(content, ensure_ascii=False)
        content = str(content).strip()
        if not content:
            continue
        if role == "system":
            system_parts.append(content)
            continue
        if role not in ("user", "assistant"):
            role = "user"
        openai_messages.append({"role": role, "content": content})
        anthropic_messages.append({"role": role, "content": content})

    if not openai_messages:
        raise ValueError("at least one user or assistant message is required")

    system_text = "\n\n".join(system_parts).strip()
    if system_text:
        openai_messages = [{"role": "system", "content": system_text}] + openai_messages
    return system_text, openai_messages, anthropic_messages


def _openai_message_text(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for item in content:
            if isinstance(item, dict) and item.get("type") == "text":
                parts.append(str(item.get("text", "")))
            elif hasattr(item, "text"):
                parts.append(str(getattr(item, "text", "")))
        return "\n".join(part for part in parts if part)
    return str(content or "")


def _json_response_size(payload: Any) -> int:
    try:
        return len(json.dumps(payload, default=str).encode("utf-8"))
    except Exception:
        return 0


def _attach_common_lineage(city_key: str, city: tuple, row: dict[str, Any]) -> dict[str, Any]:
    stats = row.get("stats", {}) or {}
    current_time = stats.get("current_time")
    age_minutes = None
    stale = False
    freshness = "unknown"
    if current_time:
        if hasattr(current_time, "tzinfo") and current_time.tzinfo is None:
            current_time = current_time.replace(tzinfo=timezone.utc)
        try:
            age_minutes = max(0.0, (datetime.now(timezone.utc) - current_time).total_seconds() / 60.0)
            stale = age_minutes > DEFAULT_FRESHNESS_MINUTES
            freshness = "stale" if stale else "fresh"
        except Exception:
            stale = False
    models = row.get("models", {}) or {}
    model_sources = [
        {
            "model": model,
            "high_f": models.get(model),
            "available": models.get(model) is not None,
        }
        for model in ("hrrr", "nam", "gfs")
    ]
    row["freshness"] = {
        "status": freshness,
        "age_minutes": round(age_minutes, 1) if age_minutes is not None else None,
        "stale": stale,
        "threshold_minutes": DEFAULT_FRESHNESS_MINUTES,
    }
    row["source_lineage"] = {
        "settlement_station": city[1],
        "bufkit_station": city[2],
        "cli_id": city[7],
        "wfo": city[8],
        "observation_time": current_time.isoformat() if hasattr(current_time, "isoformat") else None,
        "observation_available": bool(stats.get("current") is not None),
        "forecast_sources": model_sources,
    }
    row["badges"] = {
        "freshness": row["freshness"]["status"],
        "models": sum(1 for item in model_sources if item["available"]),
        "observation": "stale" if stale else "live" if current_time else "unknown",
    }
    row["why"] = {
        "fallback_reason": "stale-observation" if stale else ("multi-model" if len([m for m in model_sources if m["available"]]) > 1 else "single-model"),
        "forecast_mode": "calibrated" if row.get("calib") else "heuristic",
    }
    return row


def _csv_bytes(rows: list[dict[str, Any]], fieldnames: list[str]) -> str:
    buf = StringIO()
    writer = csv.DictWriter(buf, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    for row in rows:
        writer.writerow(row)
    return buf.getvalue()


def _weather_dashboard_rows(rt_module: Any) -> list[dict[str, Any]]:
    dashboard_fn = getattr(rt_module, "dashboard", None)
    if callable(dashboard_fn):
        return dashboard_fn(verbose=False)
    run_dashboard_fn = getattr(rt_module, "run_dashboard", None)
    if callable(run_dashboard_fn):
        sink = StringIO()
        with redirect_stdout(sink), redirect_stderr(sink):
            rows = run_dashboard_fn()
        return rows if isinstance(rows, list) else []
    return []


# ── Tool Definitions ─────────────────────────────────────────────────────────

# Native Python implementations for the 6 core tools (used as fast path).
# All other tools are proxied through the dsco binary via --tool-exec.
_NATIVE_TOOLS = {"bash", "read_file", "write_file", "edit_file", "glob", "grep"}

_TOOLS_ANTHROPIC_FALLBACK = [
    {
        "name": "bash",
        "description": "Execute a shell command. Returns stdout+stderr.",
        "input_schema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to execute"},
                "timeout": {"type": "integer", "description": "Timeout in seconds (default 120)"},
            },
            "required": ["command"],
        },
    },
    {
        "name": "read_file",
        "description": "Read the contents of a file. Returns numbered lines.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path (absolute or relative to working dir)"},
                "offset": {"type": "integer", "description": "Start line (1-based)"},
                "limit": {"type": "integer", "description": "Max lines to read"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "write_file",
        "description": "Create or overwrite a file with the given content.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path"},
                "content": {"type": "string", "description": "File content to write"},
            },
            "required": ["path", "content"],
        },
    },
    {
        "name": "edit_file",
        "description": "Make a targeted edit to a file by replacing an exact string match.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path"},
                "old_string": {"type": "string", "description": "Exact text to find (must be unique in file)"},
                "new_string": {"type": "string", "description": "Replacement text"},
            },
            "required": ["path", "old_string", "new_string"],
        },
    },
    {
        "name": "glob",
        "description": "Find files matching a glob pattern. Returns matching file paths.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "Glob pattern (e.g. '**/*.py', 'src/*.c')"},
                "path": {"type": "string", "description": "Directory to search in (default: working dir)"},
            },
            "required": ["pattern"],
        },
    },
    {
        "name": "grep",
        "description": "Search file contents using regex. Returns matching lines with context.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "Regex pattern to search for"},
                "path": {"type": "string", "description": "File or directory to search (default: working dir)"},
                "include": {"type": "string", "description": "File glob filter (e.g. '*.py')"},
            },
            "required": ["pattern"],
        },
    },
]

TOOLS_ANTHROPIC: list[dict] = []


def load_tool_registry():
    """Load all tool definitions from `dsco --tools-json`. Falls back to 6 built-ins."""
    global TOOLS_ANTHROPIC
    if DSCO_BIN.exists():
        try:
            result = subprocess.run(
                [str(DSCO_BIN), "--tools-json"],
                capture_output=True, text=True, timeout=10,
            )
            if result.returncode == 0 and result.stdout.strip():
                raw = json.loads(result.stdout)
                TOOLS_ANTHROPIC = [
                    {
                        "name": t["name"],
                        "description": t.get("description", ""),
                        "input_schema": t.get("input_schema",
                                              {"type": "object", "properties": {}}),
                    }
                    for t in raw
                    if t.get("name")
                ]
                log.info(f"Loaded {len(TOOLS_ANTHROPIC)} tools from dsco binary")
                return
        except Exception as e:
            log.warning(f"Could not load tools from dsco: {e}")

    TOOLS_ANTHROPIC = _TOOLS_ANTHROPIC_FALLBACK
    log.info(f"Using {len(TOOLS_ANTHROPIC)} fallback tools")


# OpenAI-compatible format (function calling) — rebuilt after load_tool_registry()
def _build_tools_openai() -> list[dict]:
    return [
        {
            "type": "function",
            "function": {
                "name": t["name"],
                "description": t["description"],
                "parameters": t["input_schema"],
            },
        }
        for t in TOOLS_ANTHROPIC
    ]


def get_tools_openai() -> list[dict]:
    return _build_tools_openai()


# Kept for backwards compat — will be populated after load_tool_registry() in main()
TOOLS_OPENAI: list[dict] = []

# ── Tool Execution ───────────────────────────────────────────────────────────

def _resolve_path(p: str) -> Path:
    path = Path(p)
    if not path.is_absolute():
        path = WORK_DIR / path
    return path.resolve()


async def tool_bash(command: str, timeout: int = 120) -> str:
    proc = None
    try:
        proc = await asyncio.create_subprocess_shell(
            command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            cwd=str(WORK_DIR),
            env={**os.environ, "TERM": "dumb", "NO_COLOR": "1"},
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
        out = stdout.decode("utf-8", errors="replace")
        if len(out) > MAX_TOOL_OUTPUT:
            out = out[:MAX_TOOL_OUTPUT] + f"\n... (truncated, {len(out)} bytes total)"
        if proc.returncode != 0:
            out += f"\n[exit code: {proc.returncode}]"
        return out or "(no output)"
    except asyncio.TimeoutError:
        if proc:
            proc.kill()
        return f"[timed out after {timeout}s]"
    except Exception as e:
        return f"[error: {e}]"


def tool_read_file(path: str, offset: Optional[int] = None, limit: Optional[int] = None) -> str:
    fp = _resolve_path(path)
    if not fp.exists():
        return f"[error: file not found: {fp}]"
    if fp.is_dir():
        return f"[error: {fp} is a directory, not a file]"
    try:
        lines = fp.read_text(errors="replace").splitlines(keepends=True)
        start = max(0, (offset or 1) - 1)
        end = start + limit if limit else len(lines)
        numbered = []
        for i, line in enumerate(lines[start:end], start=start + 1):
            numbered.append(f"{i:>6}\t{line.rstrip()}")
        result = "\n".join(numbered)
        if len(result) > MAX_TOOL_OUTPUT:
            result = result[:MAX_TOOL_OUTPUT] + "\n... (truncated)"
        return result or "(empty file)"
    except Exception as e:
        return f"[error reading {fp}: {e}]"


def tool_write_file(path: str, content: str) -> str:
    fp = _resolve_path(path)
    try:
        fp.parent.mkdir(parents=True, exist_ok=True)
        fp.write_text(content)
        return f"Wrote {len(content)} bytes to {fp}"
    except Exception as e:
        return f"[error writing {fp}: {e}]"


def tool_edit_file(path: str, old_string: str, new_string: str) -> str:
    fp = _resolve_path(path)
    if not fp.exists():
        return f"[error: file not found: {fp}]"
    try:
        text = fp.read_text(errors="replace")
        count = text.count(old_string)
        if count == 0:
            return "[error: old_string not found in file]"
        if count > 1:
            return f"[error: old_string found {count} times — must be unique]"
        fp.write_text(text.replace(old_string, new_string, 1))
        return f"Edited {fp} (replaced 1 occurrence)"
    except Exception as e:
        return f"[error editing {fp}: {e}]"


def tool_glob(pattern: str, path: Optional[str] = None) -> str:
    base = _resolve_path(path) if path else WORK_DIR
    try:
        matches = sorted(base.glob(pattern))[:200]
        if not matches:
            return "(no matches)"
        return "\n".join(str(m.relative_to(WORK_DIR)) for m in matches)
    except Exception as e:
        return f"[error: {e}]"


async def tool_grep(pattern: str, path: Optional[str] = None, include: Optional[str] = None) -> str:
    search_path = _resolve_path(path) if path else WORK_DIR
    cmd = ["grep", "-rn", "--color=never"]
    if include:
        cmd += [f"--include={include}"]
    cmd += [pattern, str(search_path)]
    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, cwd=str(WORK_DIR),
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=30)
        out = stdout.decode("utf-8", errors="replace")
        if len(out) > MAX_TOOL_OUTPUT:
            out = out[:MAX_TOOL_OUTPUT] + "\n... (truncated)"
        return out or "(no matches)"
    except asyncio.TimeoutError:
        return "[grep timed out]"
    except Exception as e:
        return f"[error: {e}]"


async def tool_dsco_exec(name: str, input_data: dict) -> str:
    """Proxy any tool through `dsco --tool-exec <name> <json>`."""
    if not DSCO_BIN.exists():
        return f"[dsco binary not found — cannot execute tool: {name}]"
    input_json = json.dumps(input_data)
    try:
        proc = await asyncio.create_subprocess_exec(
            str(DSCO_BIN), "--tool-exec", name, input_json,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=str(WORK_DIR),
            env={**os.environ},
        )
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=60)
        out = stdout.decode("utf-8", errors="replace").strip()
        if not out:
            err = stderr.decode("utf-8", errors="replace").strip()
            return f"[tool {name} produced no output{': ' + err if err else ''}]"
        # dsco emits {"ok":bool,"result":"..."} — unwrap it
        try:
            payload = json.loads(out)
            return payload.get("result", out)
        except json.JSONDecodeError:
            return out
    except asyncio.TimeoutError:
        return f"[tool {name} timed out after 60s]"
    except Exception as e:
        return f"[error executing tool {name}: {e}]"


async def execute_tool(name: str, input_data: dict) -> str:
    # Fast path: native Python implementations for core tools
    if name == "bash":
        return await tool_bash(input_data["command"], input_data.get("timeout", 120))
    elif name == "read_file":
        return tool_read_file(input_data["path"], input_data.get("offset"), input_data.get("limit"))
    elif name == "write_file":
        return tool_write_file(input_data["path"], input_data["content"])
    elif name == "edit_file":
        return tool_edit_file(input_data["path"], input_data["old_string"], input_data["new_string"])
    elif name == "glob":
        return tool_glob(input_data["pattern"], input_data.get("path"))
    elif name == "grep":
        return await tool_grep(input_data["pattern"], input_data.get("path"), input_data.get("include"))
    # Proxy all other tools through the dsco binary
    return await tool_dsco_exec(name, input_data)


# ── Session ──────────────────────────────────────────────────────────────────

class Session:
    def __init__(self, model: str = DEFAULT_MODEL):
        self.id = str(uuid.uuid4())[:8]
        self.model = resolve_model(model)
        self.messages: list[dict] = []
        self.total_input = 0
        self.total_output = 0
        self.total_cache_read = 0
        self.turns = 0
        self.cancelled = False
        self.active_profile: Optional[str] = None  # agent profile name

    def system_prompt(self) -> str:
        base = SYSTEM_PROMPT.format(work_dir=WORK_DIR)
        profile = self._active_profile_data()
        if profile and profile.get("prompt_prefix"):
            base = profile["prompt_prefix"] + "\n\n" + base
        return base

    def _active_profile_data(self) -> Optional[dict]:
        if not self.active_profile:
            return None
        data = _load_profiles_file()
        for p in data.get("profiles", []):
            if p.get("name") == self.active_profile:
                return p
        return None

    def get_tools_anthropic(self) -> list[dict]:
        """Return TOOLS_ANTHROPIC filtered by active agent profile."""
        profile = self._active_profile_data()
        if not profile:
            return TOOLS_ANTHROPIC
        return _filter_tools(TOOLS_ANTHROPIC, profile)

    def get_tools_openai(self) -> list[dict]:
        profile = self._active_profile_data()
        if not profile:
            return get_tools_openai()
        filtered = _filter_tools(TOOLS_ANTHROPIC, profile)
        return [
            {"type": "function", "function": {
                "name": t["name"], "description": t["description"],
                "parameters": t["input_schema"],
            }} for t in filtered
        ]


def normalize_user_content(raw_content: Any) -> Optional[Any]:
    """Accept plain text or mixed text/image blocks from the web client."""
    if isinstance(raw_content, str):
        text = raw_content.strip()
        return text or None

    if not isinstance(raw_content, list):
        return None

    blocks: list[dict[str, Any]] = []
    image_count = 0
    for block in raw_content:
        if not isinstance(block, dict):
            continue
        block_type = str(block.get("type", "")).strip().lower()
        if block_type == "text":
            text = str(block.get("text", "")).strip()
            if text:
                blocks.append({"type": "text", "text": text})
        elif block_type == "image":
            if image_count >= 8:
                break
            media_type = str(block.get("media_type", "image/png")).strip() or "image/png"
            data = block.get("data", "")
            if not isinstance(data, str):
                continue
            data = data.strip()
            if not data or len(data) > 10 * 1024 * 1024:
                continue
            blocks.append({
                "type": "image",
                "source": {
                    "type": "base64",
                    "media_type": media_type,
                    "data": data,
                },
            })
            image_count += 1

    return blocks or None


def user_content_to_openai(content: Any) -> Any:
    """Convert dsco user content into OpenAI chat-completions content."""
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""

    text_parts: list[str] = []
    image_parts: list[dict[str, Any]] = []
    for block in content:
        if not isinstance(block, dict):
            continue
        block_type = block.get("type")
        if block_type == "text" and block.get("text"):
            text_parts.append(str(block["text"]))
        elif block_type == "image":
            source = block.get("source") or {}
            source_type = source.get("type")
            if source_type == "base64" and source.get("data"):
                media_type = source.get("media_type") or "image/png"
                image_parts.append({
                    "type": "image_url",
                    "image_url": {"url": f"data:{media_type};base64,{source['data']}"},
                })
            elif source_type == "url" and source.get("url"):
                image_parts.append({
                    "type": "image_url",
                    "image_url": {"url": source["url"]},
                })

    if not image_parts:
        return "\n".join(text_parts)

    parts: list[dict[str, Any]] = []
    if text_parts:
        parts.append({"type": "text", "text": "\n".join(text_parts)})
    parts.extend(image_parts)
    return parts


def to_openai_messages(session: Session, msgs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Convert dsco conversation history into OpenAI-compatible messages."""
    out: list[dict[str, Any]] = [{"role": "system", "content": session.system_prompt()}]
    for m in msgs:
        role = m.get("role")
        content = m.get("content")
        if role == "user":
            if isinstance(content, list):
                # Tool results are promoted to tool messages.
                tool_results = [tr for tr in content if isinstance(tr, dict) and tr.get("type") == "tool_result"]
                if tool_results:
                    for tr in tool_results:
                        out.append({
                            "role": "tool",
                            "tool_call_id": tr.get("tool_use_id", "call_0"),
                            "content": tr.get("content", ""),
                        })
                else:
                    out.append({"role": "user", "content": user_content_to_openai(content)})
            elif isinstance(content, str):
                out.append({"role": "user", "content": content})
        elif role == "assistant":
            if isinstance(content, str):
                out.append({"role": "assistant", "content": content})
            elif isinstance(content, list):
                text_parts = []
                tool_calls = []
                for b in content:
                    if b["type"] == "text":
                        text_parts.append(b["text"])
                    elif b["type"] == "tool_use":
                        tool_calls.append({
                            "id": b["id"], "type": "function",
                            "function": {"name": b["name"], "arguments": json.dumps(b["input"])},
                        })
                msg: dict[str, Any] = {"role": "assistant"}
                if text_parts:
                    msg["content"] = "\n".join(text_parts)
                if tool_calls:
                    msg["tool_calls"] = tool_calls
                out.append(msg)
    return out


def assistant_content_for_replay(content_blocks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Keep UI-only thinking blocks out of follow-up Anthropic requests."""
    return [block for block in content_blocks if block.get("type") != "thinking"]


# ── Agent Loop: Anthropic Provider ───────────────────────────────────────────

async def agent_loop_anthropic(ws: WebSocket, session: Session):
    """Agentic tool loop using Anthropic native SDK."""
    api_key = get_provider_key("anthropic")
    if not api_key:
        await ws.send_json({"type": "error",
                            "message": "No API key for anthropic — set ANTHROPIC_API_KEY env var or add it to .env"})
        return
    client = anthropic.AsyncAnthropic(api_key=api_key)
    turn = 0
    mi = model_info(session.model) or model_info(detect_provider(session.model))
    supports_thinking = mi and mi.get("supports_thinking")

    while turn < MAX_TURNS and not session.cancelled:
        turn += 1
        session.turns = turn

        kwargs: dict[str, Any] = {
            "model": session.model,
            "max_tokens": MAX_TOKENS,
            "messages": session.messages,
            "tools": session.get_tools_anthropic(),
            "system": session.system_prompt(),
            "stream": True,
        }

        if supports_thinking:
            kwargs["thinking"] = {"type": "enabled", "budget_tokens": 10000}
            kwargs["max_tokens"] = max(kwargs["max_tokens"], 16000)

        content_blocks: list[dict] = []
        current_text = ""
        current_tool_json = ""
        current_tool: Optional[dict] = None
        current_thinking = ""
        current_thinking_signature = ""
        stop_reason = None
        input_tokens = 0
        output_tokens = 0
        cache_read = 0

        stream_start = time.monotonic()
        ttft_sent = False
        try:
            stream = await client.messages.create(**kwargs)
            async for event in stream:
                if session.cancelled:
                    break
                if event.type == "message_start":
                    u = event.message.usage
                    input_tokens = getattr(u, "input_tokens", 0)
                    cache_read = getattr(u, "cache_read_input_tokens", 0) or 0
                elif event.type == "content_block_start":
                    b = event.content_block
                    if b.type == "text":
                        current_text = ""
                    elif b.type == "tool_use":
                        current_tool = {"id": b.id, "name": b.name}
                        current_tool_json = ""
                        await ws.send_json({"type": "tool_start", "tool_id": b.id, "name": b.name})
                    elif b.type == "thinking":
                        current_thinking = ""
                        await ws.send_json({"type": "thinking_start"})
                elif event.type == "content_block_delta":
                    d = event.delta
                    if d.type == "text_delta":
                        if not ttft_sent:
                            ttft_ms = round((time.monotonic() - stream_start) * 1000)
                            await ws.send_json({"type": "stream_metrics", "ttft_ms": ttft_ms})
                            ttft_sent = True
                        current_text += d.text
                        await ws.send_json({"type": "text_delta", "content": d.text})
                    elif d.type == "input_json_delta":
                        current_tool_json += d.partial_json
                    elif d.type == "thinking_delta":
                        if not ttft_sent:
                            ttft_ms = round((time.monotonic() - stream_start) * 1000)
                            await ws.send_json({"type": "stream_metrics", "ttft_ms": ttft_ms})
                            ttft_sent = True
                        current_thinking += d.thinking
                        await ws.send_json({"type": "thinking_delta", "content": d.thinking})
                    elif d.type == "signature_delta":
                        current_thinking_signature = getattr(d, "signature", "")
                elif event.type == "content_block_stop":
                    if current_thinking:
                        block = {"type": "thinking", "thinking": current_thinking}
                        if current_thinking_signature:
                            block["signature"] = current_thinking_signature
                        content_blocks.append(block)
                        current_thinking = ""
                        current_thinking_signature = ""
                        await ws.send_json({"type": "thinking_end"})
                    elif current_tool:
                        inp = json.loads(current_tool_json) if current_tool_json else {}
                        content_blocks.append({"type": "tool_use", "id": current_tool["id"],
                                               "name": current_tool["name"], "input": inp})
                        await ws.send_json({"type": "tool_input", "tool_id": current_tool["id"],
                                            "name": current_tool["name"], "input": inp})
                        current_tool = None
                        current_tool_json = ""
                    elif current_text:
                        content_blocks.append({"type": "text", "text": current_text})
                        current_text = ""
                elif event.type == "message_delta":
                    stop_reason = event.delta.stop_reason
                    output_tokens = getattr(event.usage, "output_tokens", 0)

        except Exception as e:
            log.error(f"Anthropic stream error: {e}\n{traceback.format_exc()}")
            await ws.send_json({"type": "error", "message": f"API error: {e}"})
            return

        stream_dur = time.monotonic() - stream_start
        tps = round(output_tokens / max(stream_dur, 0.001), 1)

        session.total_input += input_tokens
        session.total_output += output_tokens
        session.total_cache_read += cache_read
        replay_blocks = assistant_content_for_replay(content_blocks)
        if replay_blocks:
            session.messages.append({"role": "assistant", "content": replay_blocks})

        # Estimate context usage
        est_ctx = session.total_input + session.total_output
        mi_cur = model_info(session.model)
        ctx_max = mi_cur["context_window"] if mi_cur else 200000
        ctx_pct = round(min(est_ctx / max(ctx_max, 1) * 100, 100), 1)

        await ws.send_json({"type": "turn_end", "turn": turn,
                            "usage": {"input": input_tokens, "output": output_tokens, "cache_read": cache_read},
                            "timing": {"tps": tps, "total_ms": round(stream_dur * 1000)},
                            "context": {"used": est_ctx, "max": ctx_max, "pct": ctx_pct},
                            "stop_reason": stop_reason})

        if session.cancelled:
            break

        tool_uses = [b for b in replay_blocks if b["type"] == "tool_use"]
        if not tool_uses:
            break

        tool_results = []
        for tu in tool_uses:
            t0 = time.monotonic()
            result = await execute_tool(tu["name"], tu["input"])
            dur_ms = round((time.monotonic() - t0) * 1000)
            tool_results.append({"type": "tool_result", "tool_use_id": tu["id"], "content": result})
            await ws.send_json({"type": "tool_result", "tool_id": tu["id"],
                                "output": result, "duration_ms": dur_ms})
        session.messages.append({"role": "user", "content": tool_results})

    await ws.send_json({"type": "agent_done", "total_turns": turn,
                        "total_input": session.total_input, "total_output": session.total_output})


# ── Agent Loop: OpenAI-Compatible Provider ───────────────────────────────────

async def agent_loop_openai(ws: WebSocket, session: Session):
    """Agentic tool loop using OpenAI-compatible SDK (OpenRouter, OpenAI, Groq, etc)."""
    provider = detect_provider(session.model)
    ep = PROVIDER_ENDPOINTS.get(provider)
    if not ep:
        await ws.send_json({"type": "error", "message": f"Unknown provider: {provider}"})
        return
    api_key = get_provider_key(provider)
    if not api_key:
        await ws.send_json({"type": "error",
                            "message": f"No API key for {provider} — set {ep['env']} env var"})
        return

    client = openai.AsyncOpenAI(api_key=api_key, base_url=ep["base_url"])
    # For OpenRouter, strip the provider prefix if model_id has one
    model_id = session.model
    turn = 0

    while turn < MAX_TURNS and not session.cancelled:
        turn += 1
        session.turns = turn

        oai_msgs = to_openai_messages(session, session.messages)

        content_blocks: list[dict] = []
        current_text = ""
        tool_call_map: dict[int, dict] = {}  # index -> {id, name, args_json}
        stop_reason = None
        input_tokens = 0
        output_tokens = 0

        try:
            stream = await client.chat.completions.create(
                model=model_id,
                messages=oai_msgs,
                tools=session.get_tools_openai(),
                max_tokens=MAX_TOKENS,
                stream=True,
            )
            async for chunk in stream:
                if session.cancelled:
                    break
                if not chunk.choices:
                    # Usage chunk
                    if chunk.usage:
                        input_tokens = chunk.usage.prompt_tokens or 0
                        output_tokens = chunk.usage.completion_tokens or 0
                    continue
                delta = chunk.choices[0].delta
                finish = chunk.choices[0].finish_reason

                # Text delta
                if delta and delta.content:
                    current_text += delta.content
                    await ws.send_json({"type": "text_delta", "content": delta.content})

                # Tool call deltas
                if delta and delta.tool_calls:
                    for tc in delta.tool_calls:
                        idx = tc.index
                        if idx not in tool_call_map:
                            tool_call_map[idx] = {"id": tc.id or f"call_{idx}", "name": "", "args": ""}
                            if tc.function and tc.function.name:
                                tool_call_map[idx]["name"] = tc.function.name
                                await ws.send_json({"type": "tool_start",
                                                    "tool_id": tool_call_map[idx]["id"],
                                                    "name": tc.function.name})
                        if tc.function and tc.function.arguments:
                            tool_call_map[idx]["args"] += tc.function.arguments

                if finish:
                    stop_reason = finish

        except Exception as e:
            log.error(f"OpenAI-compat stream error ({provider}): {e}\n{traceback.format_exc()}")
            await ws.send_json({"type": "error", "message": f"{provider} API error: {e}"})
            return

        # Finalize text
        if current_text:
            content_blocks.append({"type": "text", "text": current_text})

        # Finalize tool calls
        for idx in sorted(tool_call_map):
            tc = tool_call_map[idx]
            try:
                inp = json.loads(tc["args"]) if tc["args"] else {}
            except json.JSONDecodeError:
                inp = {}
            content_blocks.append({"type": "tool_use", "id": tc["id"], "name": tc["name"], "input": inp})
            await ws.send_json({"type": "tool_input", "tool_id": tc["id"], "name": tc["name"], "input": inp})

        session.total_input += input_tokens
        session.total_output += output_tokens
        session.messages.append({"role": "assistant", "content": content_blocks})
        await ws.send_json({"type": "turn_end", "turn": turn,
                            "usage": {"input": input_tokens, "output": output_tokens, "cache_read": 0},
                            "stop_reason": stop_reason})

        if session.cancelled:
            break

        tool_uses = [b for b in content_blocks if b["type"] == "tool_use"]
        if not tool_uses:
            break

        tool_results = []
        for tu in tool_uses:
            result = await execute_tool(tu["name"], tu["input"])
            tool_results.append({"type": "tool_result", "tool_use_id": tu["id"], "content": result})
            await ws.send_json({"type": "tool_result", "tool_id": tu["id"], "output": result})
        session.messages.append({"role": "user", "content": tool_results})

    await ws.send_json({"type": "agent_done", "total_turns": turn,
                        "total_input": session.total_input, "total_output": session.total_output})


# ── Agent Loop Dispatcher ────────────────────────────────────────────────────

async def agent_loop(ws: WebSocket, session: Session):
    """Route to correct provider loop."""
    provider = detect_provider(session.model)
    log.info(f"agent_loop: model={session.model} provider={provider}")
    if provider == "anthropic":
        await agent_loop_anthropic(ws, session)
    else:
        await agent_loop_openai(ws, session)


# ── FastAPI App ──────────────────────────────────────────────────────────────

app = FastAPI(title="dsco", docs_url=None, redoc_url=None)
sessions: dict[str, Session] = {}
pcs: set = set()
relay = MediaRelay() if HAS_WEBRTC else None


@app.middleware("http")
async def record_http_metrics(request: Request, call_next):
    start = time.perf_counter()
    ok = True
    key = request.url.path
    try:
        response = await call_next(request)
        ok = getattr(response, "status_code", 200) < 400
    except Exception:
        ok = False
        raise
    finally:
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        route = request.scope.get("route")
        key = getattr(route, "path", None) or request.url.path
        _record_metric(key, elapsed_ms, ok=ok)
    return response


@app.get("/", response_class=HTMLResponse)
async def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/admin", response_class=HTMLResponse)
async def admin_panel():
    return FileResponse(STATIC_DIR / "admin.html")


@app.get("/management", response_class=HTMLResponse)
async def management_panel():
    return FileResponse(STATIC_DIR / "management.html")


@app.get("/engine", response_class=HTMLResponse)
async def engine_panel():
    return FileResponse(STATIC_DIR / "engine.html")


def _ontology_data_dir() -> Path:
    return WEB_DIR.parent / "data" / "consumer_profile_ontology"


def _ontology_registry_path() -> Path:
    data_dir = _ontology_data_dir()
    candidates = [
        data_dir / "facet_definitions_extended_50k_with_boundaries.jsonl",
        data_dir / "facet_definitions_base_with_boundaries.jsonl",
        data_dir / "facet_definitions_extended_50k.jsonl",
        data_dir / "facet_definitions.jsonl",
    ]
    for path in candidates:
        if path.exists():
            return path
    return candidates[-1]


def _ontology_summary_path() -> Path:
    data_dir = _ontology_data_dir()
    candidates = [
        data_dir / "summary_extended_50k_with_boundaries.json",
        data_dir / "summary_base_with_boundaries.json",
        data_dir / "summary_extended_50k.json",
        data_dir / "summary.json",
    ]
    for path in candidates:
        if path.exists():
            return path
    return candidates[-1]


@app.get("/api/ontology/summary")
async def ontology_summary():
    path = _ontology_summary_path()
    if not path.exists():
        return JSONResponse({"error": "ontology summary not found", "path": str(path)}, status_code=404)
    data = json.loads(path.read_text())
    data["registry_path"] = str(_ontology_registry_path())
    data["summary_path"] = str(path)
    return data


@app.get("/api/ontology/facets")
async def ontology_facets(
    q: str = "",
    domain: str = "",
    sensitivity: str = "",
    boundary: int = 0,
    limit: int = 100,
    offset: int = 0,
):
    path = _ontology_registry_path()
    if not path.exists():
        return JSONResponse({"error": "ontology registry not found", "path": str(path)}, status_code=404)
    limit = max(1, min(int(limit or 100), 5000))
    offset = max(0, int(offset or 0))
    q_l = (q or "").strip().lower()
    domain_l = (domain or "").strip().lower()
    sensitivity_l = (sensitivity or "").strip().lower()
    rows = []
    matched = 0
    with path.open() as f:
        for line in f:
            try:
                facet = json.loads(line)
            except json.JSONDecodeError:
                continue
            if boundary and not str(facet.get("sensitivity_class", "")).startswith("S4"):
                continue
            if domain_l and str(facet.get("domain", "")).lower() != domain_l:
                continue
            if sensitivity_l and str(facet.get("sensitivity_class", "")).lower() != sensitivity_l:
                continue
            if q_l:
                haystack = " ".join(str(facet.get(k, "")) for k in (
                    "facet_id", "display_name", "description", "domain", "subdomain", "topic",
                    "context", "boundary_class", "relationship_type", "facet_kind", "sensitivity_class",
                )).lower()
                if q_l not in haystack:
                    continue
            if matched >= offset and len(rows) < limit:
                rows.append(facet)
            matched += 1
    return {"facets": rows, "total_matched": matched, "limit": limit, "offset": offset, "registry_path": str(path)}


@app.get("/health")
async def health():
    return {"status": "ok", "work_dir": str(WORK_DIR), "model": DEFAULT_MODEL, "webrtc": HAS_WEBRTC}


@app.get("/api/control/overview")
async def control_overview():
    with _control_plane_conn() as conn:
        window_24h = (datetime.now(timezone.utc) - timedelta(hours=24)).isoformat(timespec="seconds")
        window_30d = (datetime.now(timezone.utc) - timedelta(days=30)).isoformat(timespec="seconds")
        people_total = conn.execute("SELECT COUNT(*) FROM people").fetchone()[0]
        active_people = conn.execute("SELECT COUNT(*) FROM people WHERE status = 'active'").fetchone()[0]
        plans_total = conn.execute("SELECT COUNT(*) FROM plans WHERE is_active = 1").fetchone()[0]
        requests_24h = conn.execute("SELECT COUNT(*) FROM request_logs WHERE created_at >= ?", (window_24h,)).fetchone()[0]
        usage_30d = conn.execute(
            """
            SELECT
                COUNT(*) AS request_count,
                COALESCE(SUM(input_tokens), 0) AS input_tokens,
                COALESCE(SUM(output_tokens), 0) AS output_tokens,
                COALESCE(SUM(estimated_cost_usd), 0) AS spend_usd
            FROM request_logs
            WHERE created_at >= ?
            """,
            (window_30d,),
        ).fetchone()
        auth_mix = conn.execute(
            """
            SELECT auth_mode, COUNT(*) AS n
            FROM request_logs
            WHERE created_at >= ?
            GROUP BY auth_mode
            ORDER BY n DESC
            """,
            (window_30d,),
        ).fetchall()
        top_models = conn.execute(
            """
            SELECT model, COUNT(*) AS n
            FROM request_logs
            WHERE created_at >= ?
            GROUP BY model
            ORDER BY n DESC, model ASC
            LIMIT 5
            """,
            (window_30d,),
        ).fetchall()
        recent_errors = conn.execute(
            """
            SELECT * FROM request_logs
            WHERE status != 'ok'
            ORDER BY created_at DESC
            LIMIT 8
            """
        ).fetchall()
        backend_rows = conn.execute("SELECT * FROM engine_backends ORDER BY priority ASC, name ASC").fetchall()
    backends = [_serialize_backend(row) for row in backend_rows]
    live_backends = [row for row in backends if row["ready"]]
    return {
        "generated_at": _now_iso(),
        "db_path": str(CONTROL_PLANE_DB),
        "people": {"total": int(people_total or 0), "active": int(active_people or 0)},
        "plans": {"active": int(plans_total or 0)},
        "traffic": {
            "requests_24h": int(requests_24h or 0),
            "requests_30d": int(usage_30d["request_count"] or 0),
            "input_tokens_30d": int(usage_30d["input_tokens"] or 0),
            "output_tokens_30d": int(usage_30d["output_tokens"] or 0),
            "estimated_spend_30d": round(float(usage_30d["spend_usd"] or 0.0), 6),
        },
        "routing": {
            "ready_backends": len(live_backends),
            "configured_backends": len(backends),
            "auth_mix_30d": [{"auth_mode": row["auth_mode"], "count": int(row["n"] or 0)} for row in auth_mix],
            "top_models_30d": [{"model": row["model"], "count": int(row["n"] or 0)} for row in top_models],
        },
        "recent_errors": [_serialize_request(row) for row in recent_errors],
    }


@app.get("/api/control/plans")
async def control_plans():
    with _control_plane_conn() as conn:
        rows = conn.execute(
            """
            SELECT
                pl.*,
                COALESCE((SELECT COUNT(*) FROM people p WHERE p.plan_id = pl.id), 0) AS member_count,
                COALESCE((SELECT COUNT(*) FROM request_logs r WHERE r.plan_id = pl.id), 0) AS request_count,
                COALESCE((SELECT SUM(r.estimated_cost_usd) FROM request_logs r WHERE r.plan_id = pl.id), 0) AS estimated_cost_usd
            FROM plans pl
            ORDER BY pl.price_monthly ASC, pl.name ASC
            """
        ).fetchall()
    return {"plans": [_serialize_plan(row) for row in rows]}


@app.post("/api/control/plans")
async def control_save_plan(request: Request):
    body = await request.json()
    name = str(body.get("name", "")).strip()
    if not name:
        return JSONResponse({"error": "name is required"}, status_code=400)
    plan_id = str(body.get("id", "")).strip() or name.lower().replace(" ", "-")
    now = _now_iso()
    with _control_plane_conn() as conn:
        conn.execute(
            """
            INSERT INTO plans (
                id, name, price_monthly, included_input_tokens, included_output_tokens,
                overage_rate_input, overage_rate_output, seats, byok_allowed,
                managed_allowed, hosted_models_allowed, is_active, notes, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET
                name = excluded.name,
                price_monthly = excluded.price_monthly,
                included_input_tokens = excluded.included_input_tokens,
                included_output_tokens = excluded.included_output_tokens,
                overage_rate_input = excluded.overage_rate_input,
                overage_rate_output = excluded.overage_rate_output,
                seats = excluded.seats,
                byok_allowed = excluded.byok_allowed,
                managed_allowed = excluded.managed_allowed,
                hosted_models_allowed = excluded.hosted_models_allowed,
                is_active = excluded.is_active,
                notes = excluded.notes,
                updated_at = excluded.updated_at
            """,
            (
                plan_id,
                name,
                float(body.get("price_monthly", 0.0) or 0.0),
                int(body.get("included_input_tokens", 0) or 0),
                int(body.get("included_output_tokens", 0) or 0),
                float(body.get("overage_rate_input", 0.0) or 0.0),
                float(body.get("overage_rate_output", 0.0) or 0.0),
                int(body.get("seats", 1) or 1),
                _bool_flag(body.get("byok_allowed", True)),
                _bool_flag(body.get("managed_allowed", True)),
                _bool_flag(body.get("hosted_models_allowed", False)),
                _bool_flag(body.get("is_active", True)),
                str(body.get("notes", "")).strip(),
                now,
                now,
            ),
        )
    return {"ok": True, "id": plan_id}


@app.get("/api/control/people")
async def control_people():
    with _control_plane_conn() as conn:
        rows = conn.execute(
            """
            SELECT
                p.*,
                pl.name AS plan_name,
                COALESCE(COUNT(r.id), 0) AS request_count,
                COALESCE(SUM(r.input_tokens), 0) AS input_tokens,
                COALESCE(SUM(r.output_tokens), 0) AS output_tokens,
                COALESCE(SUM(r.cache_read_tokens), 0) AS cache_read_tokens,
                COALESCE(SUM(r.estimated_cost_usd), 0) AS estimated_cost_usd,
                MAX(r.created_at) AS last_seen_at
            FROM people p
            LEFT JOIN plans pl ON pl.id = p.plan_id
            LEFT JOIN request_logs r ON r.person_id = p.id
            GROUP BY p.id
            ORDER BY (MAX(r.created_at) IS NULL) ASC, MAX(r.created_at) DESC, p.created_at ASC
            """
        ).fetchall()
    return {"people": [_serialize_person(row) for row in rows]}


@app.post("/api/control/people")
async def control_save_person(request: Request):
    body = await request.json()
    name = str(body.get("name", "")).strip()
    email = str(body.get("email", "")).strip().lower()
    if not name or not email:
        return JSONResponse({"error": "name and email are required"}, status_code=400)
    with _control_plane_conn() as conn:
        plan_id = str(body.get("plan_id", "")).strip()
        if not plan_id:
            row = conn.execute("SELECT id FROM plans WHERE is_active = 1 ORDER BY price_monthly ASC, name ASC LIMIT 1").fetchone()
            plan_id = row["id"] if row else ""
        if not plan_id or not _lookup_plan(conn, plan_id):
            return JSONResponse({"error": "valid plan_id is required"}, status_code=400)
        person_id = str(body.get("id", "")).strip() or email.split("@", 1)[0].replace(".", "-")
        now = _now_iso()
        conn.execute(
            """
            INSERT INTO people (
                id, name, email, organization, plan_id, status, auth_policy,
                monthly_spend_limit, metadata_json, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET
                name = excluded.name,
                email = excluded.email,
                organization = excluded.organization,
                plan_id = excluded.plan_id,
                status = excluded.status,
                auth_policy = excluded.auth_policy,
                monthly_spend_limit = excluded.monthly_spend_limit,
                metadata_json = excluded.metadata_json,
                updated_at = excluded.updated_at
            """,
            (
                person_id,
                name,
                email,
                str(body.get("organization", "")).strip(),
                plan_id,
                str(body.get("status", "active")).strip() or "active",
                str(body.get("auth_policy", "managed_or_byok")).strip() or "managed_or_byok",
                float(body.get("monthly_spend_limit", 0.0) or 0.0),
                _json_text(body.get("metadata", {})),
                now,
                now,
            ),
        )
    return {"ok": True, "id": person_id}


@app.get("/api/control/backends")
async def control_backends():
    with _control_plane_conn() as conn:
        rows = conn.execute(
            "SELECT * FROM engine_backends ORDER BY priority ASC, hosted_by_dsco DESC, name ASC"
        ).fetchall()
    return {"backends": [_serialize_backend(row) for row in rows]}


@app.post("/api/control/backends")
async def control_save_backend(request: Request):
    body = await request.json()
    name = str(body.get("name", "")).strip()
    provider = str(body.get("provider", "")).strip().lower()
    transport = str(body.get("transport", "")).strip().lower()
    if not name or not provider or not transport:
        return JSONResponse({"error": "name, provider, and transport are required"}, status_code=400)
    backend_id = str(body.get("id", "")).strip() or name.lower().replace(" ", "-")
    now = _now_iso()
    with _control_plane_conn() as conn:
        conn.execute(
            """
            INSERT INTO engine_backends (
                id, name, provider, transport, base_url, api_key_env, hosted_by_dsco,
                status, enabled, priority, notes, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET
                name = excluded.name,
                provider = excluded.provider,
                transport = excluded.transport,
                base_url = excluded.base_url,
                api_key_env = excluded.api_key_env,
                hosted_by_dsco = excluded.hosted_by_dsco,
                status = excluded.status,
                enabled = excluded.enabled,
                priority = excluded.priority,
                notes = excluded.notes,
                updated_at = excluded.updated_at
            """,
            (
                backend_id,
                name,
                provider,
                transport,
                str(body.get("base_url", "")).strip() or _provider_base_url(provider),
                str(body.get("api_key_env", "")).strip() or _provider_env_name(provider),
                _bool_flag(body.get("hosted_by_dsco", False)),
                str(body.get("status", "planned")).strip() or "planned",
                _bool_flag(body.get("enabled", True)),
                int(body.get("priority", 100) or 100),
                str(body.get("notes", "")).strip(),
                now,
                now,
            ),
        )
    return {"ok": True, "id": backend_id}


@app.get("/api/control/credentials")
async def control_credentials(limit: int = 100):
    safe_limit = _clamp_int(limit, 50, 1, 500)
    with _control_plane_conn() as conn:
        rows = conn.execute(
            """
            SELECT c.*, p.name AS person_name
            FROM credential_bindings c
            LEFT JOIN people p ON p.id = c.person_id
            ORDER BY (c.last_used_at IS NULL) ASC, c.last_used_at DESC, c.created_at DESC
            LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
    return {"credentials": [_serialize_credential(row) for row in rows], "limit": safe_limit}


@app.get("/api/control/requests")
async def control_requests(limit: int = 100, person_id: str = "", status: str = ""):
    safe_limit = _clamp_int(limit, 100, 1, 500)
    clauses = []
    params: list[Any] = []
    if person_id:
        clauses.append("person_id = ?")
        params.append(person_id)
    if status:
        clauses.append("status = ?")
        params.append(status)
    where = f"WHERE {' AND '.join(clauses)}" if clauses else ""
    query = f"""
        SELECT * FROM request_logs
        {where}
        ORDER BY created_at DESC
        LIMIT ?
    """
    params.append(safe_limit)
    with _control_plane_conn() as conn:
        rows = conn.execute(query, params).fetchall()
    return {"requests": [_serialize_request(row) for row in rows], "limit": safe_limit}


@app.get("/api/engine/catalog")
async def engine_catalog():
    with _control_plane_conn() as conn:
        people_rows = conn.execute("SELECT p.*, pl.name AS plan_name FROM people p LEFT JOIN plans pl ON pl.id = p.plan_id ORDER BY p.name ASC").fetchall()
        backend_rows = conn.execute("SELECT * FROM engine_backends WHERE enabled = 1 ORDER BY priority ASC, name ASC").fetchall()
        plan_rows = conn.execute("SELECT * FROM plans WHERE is_active = 1 ORDER BY price_monthly ASC, name ASC").fetchall()
    models = []
    for model in MODEL_REGISTRY:
        provider = detect_provider(model["model_id"])
        models.append({
            "alias": model["alias"],
            "model_id": model["model_id"],
            "provider": provider,
            "context_window": model["context_window"],
            "max_output": model["max_output"],
            "supports_thinking": bool(model.get("supports_thinking")),
        })
    return {
        "default_model": DEFAULT_MODEL,
        "people": [_serialize_person(row) for row in people_rows],
        "plans": [_serialize_plan(row) for row in plan_rows],
        "backends": [_serialize_backend(row) for row in backend_rows],
        "models": models,
    }


@app.get("/api/engine/health")
async def engine_health():
    provider_rows = [{"provider": "anthropic", "env": "ANTHROPIC_API_KEY", "has_key": bool(os.getenv("ANTHROPIC_API_KEY"))}]
    for provider, meta in PROVIDER_ENDPOINTS.items():
        provider_rows.append({"provider": provider, "env": meta["env"], "has_key": bool(os.getenv(meta["env"]))})
    with _control_plane_conn() as conn:
        backend_rows = conn.execute("SELECT * FROM engine_backends ORDER BY priority ASC, name ASC").fetchall()
    backends = [_serialize_backend(row) for row in backend_rows]
    return {
        "status": "ok",
        "default_model": DEFAULT_MODEL,
        "providers": provider_rows,
        "backends": backends,
        "db_path": str(CONTROL_PLANE_DB),
    }


@app.post("/api/engine/chat")
async def engine_chat(request: Request):
    started = time.perf_counter()
    body = await request.json()
    request_id = str(body.get("request_id", "")).strip() or str(uuid.uuid4())[:12]
    person_id = str(body.get("person_id", "")).strip()
    email = str(body.get("email", "")).strip().lower()
    if not person_id and not email:
        return JSONResponse({"error": "person_id or email is required"}, status_code=400)

    model_id = resolve_model(str(body.get("model", DEFAULT_MODEL)).strip() or DEFAULT_MODEL)
    provider = str(body.get("provider", "")).strip().lower() or detect_provider(model_id)
    requested_backend_id = str(body.get("backend_id", "")).strip()
    provided_key = str(body.get("api_key", "")).strip()
    wants_byok = bool(provided_key)
    dry_run = _bool_flag(body.get("dry_run", False)) == 1

    try:
        system_text, openai_messages, anthropic_messages = _normalize_engine_messages(body)
    except ValueError as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)

    with _control_plane_conn() as conn:
        person = _lookup_person(conn, person_id=person_id, email=email)
        if not person:
            return JSONResponse({"error": "person not found"}, status_code=404)
        plan = _lookup_plan(conn, person["plan_id"])
        if not plan:
            return JSONResponse({"error": "plan not found for person"}, status_code=500)
        backend_row = _select_backend(conn, provider, wants_byok, backend_id=requested_backend_id)
        if requested_backend_id and not backend_row:
            return JSONResponse({"error": "backend not found"}, status_code=404)
        policy_error = _auth_policy_error(person, plan, wants_byok, backend_row)
        if policy_error:
            return JSONResponse({"error": policy_error}, status_code=403)
        month_usage = _person_month_usage(conn, person["id"])

    spend_limit = float(person["monthly_spend_limit"] or 0.0)
    if spend_limit > 0 and month_usage["spend_usd"] >= spend_limit:
        return JSONResponse(
            {
                "error": "monthly spend limit reached",
                "limit_usd": spend_limit,
                "current_spend_usd": round(month_usage["spend_usd"], 6),
            },
            status_code=402,
        )

    backend = _serialize_backend(backend_row) if backend_row else _implicit_backend(provider)
    route_source = "backend_override" if requested_backend_id else ("backend_catalog" if backend_row else "implicit_provider")
    auth_mode = "byok_request" if wants_byok else "managed_env"
    api_key = provided_key
    if not api_key:
        env_name = backend.get("api_key_env", "") or _provider_env_name(provider)
        api_key = os.getenv(env_name, "").strip() if env_name else ""
    if not api_key and not dry_run:
        return JSONResponse(
            {
                "error": "no routed credential available",
                "provider": provider,
                "backend_id": backend["id"],
                "expected_env": backend.get("api_key_env") or _provider_env_name(provider),
            },
            status_code=400,
        )

    route = {
        "provider": provider,
        "model": model_id,
        "backend_id": backend["id"],
        "backend_name": backend["name"],
        "transport": backend["transport"],
        "base_url": backend.get("base_url") or _provider_base_url(provider),
        "auth_mode": auth_mode,
        "route_source": route_source,
        "hosted_by_dsco": bool(backend.get("hosted_by_dsco")),
    }

    input_tokens = 0
    output_tokens = 0
    cache_read_tokens = 0
    response_text = ""
    status = "dry_run" if dry_run else "ok"
    error = ""

    if not dry_run:
        max_tokens = int(body.get("max_tokens", 4096) or 4096)
        info = model_info(model_id)
        if info and info.get("max_output"):
            max_tokens = max(1, min(max_tokens, int(info["max_output"])))
        try:
            if backend["transport"] == "anthropic":
                client = anthropic.AsyncAnthropic(api_key=api_key)
                kwargs: dict[str, Any] = {
                    "model": model_id,
                    "max_tokens": max_tokens,
                    "messages": anthropic_messages,
                }
                if system_text:
                    kwargs["system"] = system_text
                response = await client.messages.create(**kwargs)
                response_text = "\n".join(
                    block.text for block in response.content
                    if getattr(block, "type", "") == "text" and getattr(block, "text", "")
                ).strip()
                usage = getattr(response, "usage", None)
                input_tokens = int(getattr(usage, "input_tokens", 0) or 0)
                output_tokens = int(getattr(usage, "output_tokens", 0) or 0)
                cache_read_tokens = int(getattr(usage, "cache_read_input_tokens", 0) or 0)
            else:
                client = openai.AsyncOpenAI(api_key=api_key, base_url=route["base_url"])
                response = await client.chat.completions.create(
                    model=model_id,
                    messages=openai_messages,
                    max_tokens=max_tokens,
                    stream=False,
                )
                message = response.choices[0].message if response.choices else None
                response_text = _openai_message_text(message.content if message else "")
                usage = getattr(response, "usage", None)
                input_tokens = int(getattr(usage, "prompt_tokens", 0) or 0)
                output_tokens = int(getattr(usage, "completion_tokens", 0) or 0)
        except Exception as exc:
            log.error(f"engine chat error: {exc}\n{traceback.format_exc()}")
            status = "error"
            error = str(exc)

    latency_ms = round((time.perf_counter() - started) * 1000.0, 2)
    estimated_cost = _estimate_request_cost(model_id, input_tokens, output_tokens, cache_read_tokens)
    record = {
        "id": request_id,
        "person_id": person["id"],
        "person_email": person["email"],
        "person_name": person["name"],
        "organization": person["organization"],
        "plan_id": person["plan_id"],
        "model": model_id,
        "provider": provider,
        "backend_id": backend["id"],
        "auth_mode": auth_mode,
        "route_source": route_source,
        "status": status,
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "cache_read_tokens": cache_read_tokens,
        "latency_ms": latency_ms,
        "estimated_cost_usd": estimated_cost,
        "error": error,
        "metadata": body.get("metadata", {}),
        "created_at": _now_iso(),
    }
    with _control_plane_conn() as conn:
        _record_request_log(conn, record)
        if wants_byok and provided_key:
            _upsert_credential_binding(
                conn,
                person_id=person["id"],
                person_name=person["name"],
                provider=provider,
                source="byok_request",
                api_key=provided_key,
                label=str(body.get("credential_label", "")).strip(),
            )
        elif api_key:
            _upsert_credential_binding(
                conn,
                person_id=person["id"],
                person_name=person["name"],
                provider=provider,
                source="managed_env",
                secret_ref=backend.get("api_key_env", "") or _provider_env_name(provider),
                api_key=api_key,
                label=backend["name"],
            )

    payload = {
        "request_id": request_id,
        "person": {
            "id": person["id"],
            "name": person["name"],
            "email": person["email"],
            "organization": person["organization"],
            "plan_id": person["plan_id"],
        },
        "route": route,
        "status": status,
        "dry_run": dry_run,
        "usage": {
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
            "cache_read_tokens": cache_read_tokens,
            "estimated_cost_usd": estimated_cost,
        },
        "latency_ms": latency_ms,
        "content": response_text,
        "error": error,
    }
    if status == "error":
        return JSONResponse(payload, status_code=502)
    return payload


@app.get("/api/dashboard/meta")
async def dashboard_meta():
    """Return metadata used by the UI for badges, limits, and exports."""
    return {
        "generated_at": time.time(),
        "limits": {
            "list": MAX_LIST_LIMIT,
            "response_bytes": MAX_RESPONSE_BYTES,
            "freshness_minutes": DEFAULT_FRESHNESS_MINUTES,
        },
        "exports": ["json", "csv"],
        "views": {
            "weather": "/api/weather/dashboard",
            "trading": "/api/trading/portfolio",
        },
        "runbooks": [
            {"id": "weather-stale", "title": "Weather freshness", "summary": "Check stale METAR or model payloads before trading."},
            {"id": "kalshi-outage", "title": "Trading data outage", "summary": "Inspect Kalshi/Polymarket connectivity and dry-run mode."},
            {"id": "ui-regression", "title": "UI regression", "summary": "Use the dashboard metadata and metrics endpoint to spot broken panels."},
        ],
    }


@app.get("/api/metrics")
async def endpoint_metrics():
    return _metrics_snapshot()


@app.get("/api/docs/runbooks")
async def runbook_index():
    return {
        "runbooks": [
            {
                "id": "weather-stale",
                "title": "Weather freshness",
                "file": "docs/RUNBOOK_INDEX.md",
                "summary": "Age, stale-state, and source-lineage checks for weather ingest/dashboard views.",
            },
            {
                "id": "kalshi-outage",
                "title": "Trading data outage",
                "file": "docs/RUNBOOK_INDEX.md",
                "summary": "Fallback behavior when external market APIs return empty or stale data.",
            },
            {
                "id": "ui-regression",
                "title": "UI regression",
                "file": "docs/RUNBOOK_INDEX.md",
                "summary": "Smoke checks for dashboard badges, loading states, and export hooks.",
            },
        ]
    }


@app.get("/api/models")
async def list_models():
    """Return full model registry with provider detection and key availability."""
    models = []
    for m in MODEL_REGISTRY:
        provider = detect_provider(m["model_id"])
        has_key = get_provider_key(provider) is not None
        models.append({
            "alias": m["alias"],
            "model_id": m["model_id"],
            "provider": provider,
            "context_window": m["context_window"],
            "max_output": m["max_output"],
            "input_price": m["input_price"],
            "output_price": m["output_price"],
            "supports_thinking": bool(m.get("supports_thinking")),
            "has_key": has_key,
        })
    return {"models": models, "default": DEFAULT_MODEL}


@app.get("/api/files")
async def list_files(path: str = ".", limit: int = MAX_LIST_LIMIT, offset: int = 0):
    """Return directory listing for file explorer."""
    target = (WORK_DIR / path).resolve()
    if not str(target).startswith(str(WORK_DIR)):
        return JSONResponse({"error": "access denied"}, status_code=403)
    if not target.exists():
        return JSONResponse({"error": "not found"}, status_code=404)
    entries = []
    try:
        for item in sorted(target.iterdir(), key=lambda x: (not x.is_dir(), x.name.lower())):
            if item.name.startswith(".") and item.name not in (".env", ".gitignore"):
                continue
            if item.name in ("node_modules", "__pycache__", "build", ".git"):
                continue
            entries.append({
                "name": item.name,
                "path": str(item.relative_to(WORK_DIR)),
                "is_dir": item.is_dir(),
                "size": item.stat().st_size if item.is_file() else None,
            })
    except PermissionError:
        pass
    safe_limit = _clamp_int(limit, MAX_LIST_LIMIT, 1, MAX_LIST_LIMIT)
    safe_offset = max(0, int(offset or 0))
    window = entries[safe_offset:safe_offset + safe_limit]
    return {
        "path": str(target.relative_to(WORK_DIR)),
        "entries": window,
        "total": len(entries),
        "limit": safe_limit,
        "offset": safe_offset,
        "truncated": len(window) < len(entries),
    }


@app.get("/api/file")
async def get_file(path: str):
    """Return file content for file viewer."""
    target = (WORK_DIR / path).resolve()
    if not str(target).startswith(str(WORK_DIR)):
        return JSONResponse({"error": "access denied"}, status_code=403)
    if not target.is_file():
        return JSONResponse({"error": "not a file"}, status_code=404)
    try:
        content = target.read_text(errors="replace")
        if len(content) > 500_000:
            content = content[:500_000] + "\n... (truncated)"
        return {"path": path, "content": content, "size": target.stat().st_size}
    except Exception as e:
        return JSONResponse({"error": str(e)}, status_code=500)


@app.get("/api/trading/status")
async def trading_status():
    """Check trading platform connection status."""
    platforms = {}
    # Kalshi
    kalshi_key = os.getenv("KALSHI_API_KEY")
    kalshi_pk = os.getenv("KALSHI_RSA_PRIVATE_KEY_PATH")
    platforms["kalshi"] = {
        "connected": bool(kalshi_key and kalshi_pk),
        "has_key": bool(kalshi_key),
        "has_pk": bool(kalshi_pk),
    }
    # Polymarket
    poly_addr = os.getenv("POLYMARKET_ADDRESS")
    poly_key = os.getenv("POLYMARKET_API_KEY")
    poly_secret = os.getenv("POLYMARKET_API_SECRET")
    poly_pk = os.getenv("POLYMARKET_PRIVATE_KEY")
    platforms["polymarket"] = {
        "connected": bool(poly_addr and poly_key and poly_secret and poly_pk),
        "has_address": bool(poly_addr),
        "has_api_key": bool(poly_key),
        "has_secret": bool(poly_secret),
        "has_private_key": bool(poly_pk),
        "address": poly_addr[:10] + "..." if poly_addr and len(poly_addr) > 10 else None,
    }
    market_state = {
        "no_market_data": not any(info.get("connected") for info in platforms.values()),
        "stale": not platforms["kalshi"]["connected"] or not platforms["polymarket"]["connected"],
        "summary": "offline" if not any(info.get("connected") for info in platforms.values()) else "degraded" if any(not info.get("connected") for info in platforms.values()) else "live",
    }
    return {"platforms": platforms, "market_state": market_state}


# ── Trading API Infrastructure ────────────────────────────────────────────────

KALSHI_BASE = "https://api.elections.kalshi.com/trade-api/v2"
POLY_CLOB_BASE = "https://clob.polymarket.com"
POLY_GAMMA_BASE = "https://gamma-api.polymarket.com"

# Risk state (mirrors C risk_limits_t)
_risk = {
    "max_position_usd": float(os.getenv("DSCO_TRADING_MAX_POSITION", "500")),
    "max_total_exposure_usd": float(os.getenv("DSCO_TRADING_MAX_EXPOSURE", "2000")),
    "max_order_usd": float(os.getenv("DSCO_TRADING_MAX_ORDER", "100")),
    "min_arb_spread": float(os.getenv("DSCO_TRADING_MIN_ARB_SPREAD", "0.03")),
    "max_open_orders": int(os.getenv("DSCO_TRADING_MAX_OPEN_ORDERS", "20")),
    "dry_run": os.getenv("DSCO_TRADING_DRY_RUN", "true").lower() not in ("0", "false", "off", "no"),
}


def _kalshi_auth_headers(method: str, path: str, body: str = "") -> dict:
    """Generate Kalshi RSA-PSS auth headers."""
    api_key = os.getenv("KALSHI_API_KEY", "")
    pk_path = os.getenv("KALSHI_RSA_PRIVATE_KEY_PATH", "")
    if not api_key or not pk_path:
        return {}
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding
        ts = str(int(time.time() * 1000))
        message = f"{ts}{method}{path}"
        if body:
            message += body
        pk_data = Path(pk_path).read_bytes()
        private_key = serialization.load_pem_private_key(pk_data, password=None)
        sig = private_key.sign(
            message.encode(),
            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.MAX_LENGTH),
            hashes.SHA256()
        )
        return {
            "KALSHI-ACCESS-KEY": api_key,
            "KALSHI-ACCESS-SIGNATURE": base64.b64encode(sig).decode(),
            "KALSHI-ACCESS-TIMESTAMP": ts,
            "Content-Type": "application/json",
            "Accept": "application/json",
        }
    except Exception as e:
        log.warning(f"Kalshi auth failed: {e}")
        return {}


def _poly_clob_headers(method: str, path: str, body: str = "") -> dict:
    """Generate Polymarket CLOB HMAC auth headers."""
    api_key = os.getenv("POLYMARKET_API_KEY", "")
    secret = os.getenv("POLYMARKET_API_SECRET", "")
    passphrase = os.getenv("POLYMARKET_PASSPHRASE", "")
    address = os.getenv("POLYMARKET_ADDRESS", "")
    if not all([api_key, secret, passphrase]):
        return {}
    try:
        ts = str(int(time.time()))
        nonce = str(int(time.time() * 1000))
        message = f"{ts}{method}{path}"
        if body:
            message += body
        sig = hmac.new(
            base64.b64decode(secret), message.encode(), hashlib.sha256
        ).hexdigest()
        return {
            "POLY-ADDRESS": address,
            "POLY-SIGNATURE": sig,
            "POLY-TIMESTAMP": ts,
            "POLY-NONCE": nonce,
            "POLY-API-KEY": api_key,
            "POLY-PASSPHRASE": passphrase,
            "Content-Type": "application/json",
            "Accept": "application/json",
        }
    except Exception as e:
        log.warning(f"Polymarket auth failed: {e}")
        return {}


async def _kalshi_get(path: str) -> dict:
    """Make authenticated GET to Kalshi API."""
    headers = _kalshi_auth_headers("GET", f"/trade-api/v2{path}")
    if not headers:
        return {"error": "Kalshi not configured"}
    async with httpx.AsyncClient(timeout=15) as client:
        r = await client.get(f"{KALSHI_BASE}{path}", headers=headers)
        return r.json() if r.status_code == 200 else {"error": f"HTTP {r.status_code}", "body": r.text[:500]}


async def _kalshi_post(path: str, body: dict) -> dict:
    """Make authenticated POST to Kalshi API."""
    body_str = json.dumps(body)
    headers = _kalshi_auth_headers("POST", f"/trade-api/v2{path}", body_str)
    if not headers:
        return {"error": "Kalshi not configured"}
    async with httpx.AsyncClient(timeout=15) as client:
        r = await client.post(f"{KALSHI_BASE}{path}", headers=headers, content=body_str)
        return r.json() if r.status_code in (200, 201) else {"error": f"HTTP {r.status_code}", "body": r.text[:500]}


async def _kalshi_delete(path: str) -> dict:
    """Make authenticated DELETE to Kalshi API."""
    headers = _kalshi_auth_headers("DELETE", f"/trade-api/v2{path}")
    if not headers:
        return {"error": "Kalshi not configured"}
    async with httpx.AsyncClient(timeout=15) as client:
        r = await client.delete(f"{KALSHI_BASE}{path}", headers=headers)
        return r.json() if r.status_code in (200, 204) else {"error": f"HTTP {r.status_code}", "body": r.text[:500]}


async def _poly_get(path: str, auth: bool = False) -> dict:
    """Make GET to Polymarket API (CLOB or Gamma)."""
    if auth:
        headers = _poly_clob_headers("GET", path)
        if not headers:
            return {"error": "Polymarket not configured"}
        url = f"{POLY_CLOB_BASE}{path}"
    else:
        headers = {"Accept": "application/json"}
        url = f"{POLY_GAMMA_BASE}{path}"
    async with httpx.AsyncClient(timeout=15) as client:
        r = await client.get(url, headers=headers)
        try:
            return r.json() if r.status_code == 200 else {"error": f"HTTP {r.status_code}"}
        except Exception:
            return {"error": f"HTTP {r.status_code}", "body": r.text[:500]}


async def _poly_post(path: str, body: dict) -> dict:
    """Make authenticated POST to Polymarket CLOB."""
    body_str = json.dumps(body)
    headers = _poly_clob_headers("POST", path, body_str)
    if not headers:
        return {"error": "Polymarket not configured"}
    async with httpx.AsyncClient(timeout=15) as client:
        r = await client.post(f"{POLY_CLOB_BASE}{path}", headers=headers, content=body_str)
        try:
            return r.json() if r.status_code in (200, 201) else {"error": f"HTTP {r.status_code}"}
        except Exception:
            return {"error": f"HTTP {r.status_code}", "body": r.text[:500]}


# ── Kalshi Trading Endpoints ──────────────────────────────────────────────────

@app.get("/api/trading/kalshi/balance")
async def kalshi_balance():
    data = await _kalshi_get("/portfolio/balance")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    balance = data.get("balance", 0) / 100.0
    portfolio_value = data.get("portfolio_value", 0) / 100.0
    return {"platform": "kalshi", "balance_usd": balance, "portfolio_value_usd": portfolio_value}


@app.get("/api/trading/kalshi/positions")
async def kalshi_positions(limit: int = 100):
    safe_limit = _clamp_int(limit, 100, 1, 100)
    data = await _kalshi_get(f"/portfolio/positions?count_filter=position&limit={safe_limit}")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    positions = data.get("market_positions", [])[:safe_limit]
    return {"platform": "kalshi", "positions": positions, "limit": safe_limit, "truncated": len(positions) < len(data.get("market_positions", []))}


@app.get("/api/trading/kalshi/orders")
async def kalshi_orders(limit: int = 100):
    safe_limit = _clamp_int(limit, 100, 1, 100)
    data = await _kalshi_get(f"/portfolio/orders?status=resting&limit={safe_limit}")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    orders = data.get("orders", [])[:safe_limit]
    return {"platform": "kalshi", "orders": orders, "limit": safe_limit, "truncated": len(orders) < len(data.get("orders", []))}


@app.get("/api/trading/kalshi/fills")
async def kalshi_fills(ticker: str = "", limit: int = 50):
    safe_limit = _clamp_int(limit, 50, 1, 500)
    path = f"/portfolio/fills?limit={safe_limit}"
    if ticker:
        path += f"&ticker={ticker}"
    data = await _kalshi_get(path)
    if "error" in data:
        return JSONResponse(data, status_code=502)
    fills = data.get("fills", [])[:safe_limit]
    return {"platform": "kalshi", "fills": fills, "limit": safe_limit, "truncated": len(fills) < len(data.get("fills", []))}


@app.get("/api/trading/kalshi/markets")
async def kalshi_markets(ticker: str = "", event_ticker: str = "", limit: int = 20):
    safe_limit = _clamp_int(limit, 20, 1, 100)
    if ticker:
        data = await _kalshi_get(f"/markets/{ticker}")
    elif event_ticker:
        data = await _kalshi_get(f"/events/{event_ticker}")
    else:
        data = await _kalshi_get(f"/markets?limit={safe_limit}&status=open")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    if "markets" in data and isinstance(data.get("markets"), list):
        original_markets = list(data["markets"])
        markets = original_markets[:safe_limit]
        data["markets"] = markets
        data["limit"] = safe_limit
        data["truncated"] = len(markets) < len(original_markets)
    return data


@app.get("/api/trading/kalshi/search")
async def kalshi_search(q: str = "", limit: int = 20):
    if not q:
        return {"events": []}
    safe_limit = _clamp_int(limit, 20, 1, 100)
    data = await _kalshi_get(f"/events?status=open&with_nested_markets=true&series_ticker=&limit={safe_limit}")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    data["limit"] = safe_limit
    return data


@app.get("/api/trading/kalshi/orderbook")
async def kalshi_orderbook(ticker: str):
    data = await _kalshi_get(f"/markets/{ticker}/orderbook")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    return {"platform": "kalshi", "ticker": ticker, "orderbook": data}


@app.post("/api/trading/kalshi/order")
async def kalshi_create_order(request: Request):
    body = await request.json()
    if _risk["dry_run"]:
        return {"dry_run": True, "would_place": body, "message": "Dry run mode — order not placed"}
    amount = body.get("count", 1) * body.get("yes_price", body.get("no_price", 50)) / 100.0
    if amount > _risk["max_order_usd"]:
        return JSONResponse({"error": f"Order ${amount:.2f} exceeds max ${_risk['max_order_usd']:.2f}"}, status_code=400)
    data = await _kalshi_post("/portfolio/orders", body)
    if "error" in data:
        return JSONResponse(data, status_code=502)
    return data


@app.delete("/api/trading/kalshi/order/{order_id}")
async def kalshi_cancel_order(order_id: str):
    data = await _kalshi_delete(f"/portfolio/orders/{order_id}")
    if "error" in data:
        return JSONResponse(data, status_code=502)
    return {"cancelled": order_id}


# ── Polymarket Trading Endpoints ──────────────────────────────────────────────

@app.get("/api/trading/poly/markets")
async def poly_markets(q: str = "", limit: int = 20, tag: str = ""):
    safe_limit = _clamp_int(limit, 20, 1, 100)
    params = f"?limit={safe_limit}&active=true"
    if q:
        params = f"?_q={q}&limit={safe_limit}&active=true"
    if tag:
        params += f"&tag={tag}"
    data = await _poly_get(f"/markets{params}")
    if isinstance(data, dict) and "error" in data:
        return JSONResponse(data, status_code=502)
    original_markets = data if isinstance(data, list) else [data]
    markets = original_markets[:safe_limit]
    return {"platform": "polymarket", "markets": markets, "limit": safe_limit, "truncated": len(markets) < len(original_markets)}


@app.get("/api/trading/poly/events")
async def poly_events(limit: int = 20):
    safe_limit = _clamp_int(limit, 20, 1, 100)
    data = await _poly_get(f"/events?limit={safe_limit}&active=true")
    if isinstance(data, dict) and "error" in data:
        return JSONResponse(data, status_code=502)
    original_events = data if isinstance(data, list) else [data]
    events = original_events[:safe_limit]
    return {"platform": "polymarket", "events": events, "limit": safe_limit, "truncated": len(events) < len(original_events)}


@app.get("/api/trading/poly/book")
async def poly_book(token_id: str):
    data = await _poly_get(f"/book?token_id={token_id}", auth=True)
    if isinstance(data, dict) and "error" in data:
        return JSONResponse(data, status_code=502)
    return {"platform": "polymarket", "token_id": token_id, "book": data}


@app.get("/api/trading/poly/prices")
async def poly_prices(token_id: str):
    data = await _poly_get(f"/price?token_id={token_id}", auth=True)
    if isinstance(data, dict) and "error" in data:
        return JSONResponse(data, status_code=502)
    return {"platform": "polymarket", "token_id": token_id, "price": data}


@app.get("/api/trading/poly/positions")
async def poly_positions():
    address = os.getenv("POLYMARKET_ADDRESS", "")
    if not address:
        return JSONResponse({"error": "POLYMARKET_ADDRESS not set"}, status_code=400)
    data = await _poly_get(f"/positions?user={address}", auth=True)
    if isinstance(data, dict) and "error" in data:
        return JSONResponse(data, status_code=502)
    return {"platform": "polymarket", "positions": data if isinstance(data, list) else []}


@app.get("/api/trading/poly/trades")
async def poly_trades(limit: int = 50):
    address = os.getenv("POLYMARKET_ADDRESS", "")
    safe_limit = _clamp_int(limit, 50, 1, 500)
    data = await _poly_get(f"/trades?limit={safe_limit}" + (f"&maker={address}" if address else ""), auth=True)
    if isinstance(data, dict) and "error" in data:
        return JSONResponse(data, status_code=502)
    original_trades = data if isinstance(data, list) else []
    trades = original_trades[:safe_limit]
    return {"platform": "polymarket", "trades": trades, "limit": safe_limit, "truncated": len(trades) < len(original_trades)}


# ── Cross-Platform Endpoints ─────────────────────────────────────────────────

@app.get("/api/trading/portfolio")
async def cross_portfolio():
    """Unified portfolio view across both platforms."""
    kalshi_bal, kalshi_pos, poly_pos = await asyncio.gather(
        _kalshi_get("/portfolio/balance"),
        _kalshi_get("/portfolio/positions?count_filter=position&limit=100"),
        _poly_get(f"/positions?user={os.getenv('POLYMARKET_ADDRESS', '')}", auth=True),
        return_exceptions=True,
    )

    result = {"kalshi": {}, "polymarket": {}, "total_usd": 0.0}

    if isinstance(kalshi_bal, dict) and "error" not in kalshi_bal:
        bal = kalshi_bal.get("balance", 0) / 100.0
        pv = kalshi_bal.get("portfolio_value", 0) / 100.0
        result["kalshi"]["balance_usd"] = bal
        result["kalshi"]["portfolio_value_usd"] = pv
        result["total_usd"] += bal + pv
    else:
        result["kalshi"]["error"] = str(kalshi_bal) if isinstance(kalshi_bal, Exception) else (kalshi_bal.get("error") if isinstance(kalshi_bal, dict) else "unavailable")

    if isinstance(kalshi_pos, dict) and "error" not in kalshi_pos:
        result["kalshi"]["positions"] = kalshi_pos.get("market_positions", [])
    else:
        result["kalshi"]["positions"] = []

    if isinstance(poly_pos, (list, dict)) and not (isinstance(poly_pos, dict) and "error" in poly_pos):
        positions = poly_pos if isinstance(poly_pos, list) else []
        result["polymarket"]["positions"] = positions
    else:
        result["polymarket"]["error"] = poly_pos.get("error") if isinstance(poly_pos, dict) else "unavailable"
        result["polymarket"]["positions"] = []

    return result


@app.get("/api/trading/arb/scan")
async def arb_scan(min_spread: float = 0.03):
    """Scan for arbitrage opportunities across platforms."""
    kalshi_data, poly_data = await asyncio.gather(
        _kalshi_get("/events?status=open&with_nested_markets=true&limit=50"),
        _poly_get("/markets?limit=50&active=true"),
        return_exceptions=True,
    )

    opportunities = []

    # Within-market arbs on Kalshi (YES + NO < 1.00)
    if isinstance(kalshi_data, dict) and "events" in kalshi_data:
        for event in kalshi_data.get("events", []):
            for market in event.get("markets", []):
                yes_bid = market.get("yes_bid", 0) / 100.0 if market.get("yes_bid") else 0
                no_bid = market.get("no_bid", 0) / 100.0 if market.get("no_bid") else 0
                yes_ask = market.get("yes_ask", 0) / 100.0 if market.get("yes_ask") else 0
                no_ask = market.get("no_ask", 0) / 100.0 if market.get("no_ask") else 0
                if yes_ask > 0 and no_ask > 0:
                    total = yes_ask + no_ask
                    if total < 1.0 - min_spread:
                        opportunities.append({
                            "type": "within_market",
                            "platform": "kalshi",
                            "ticker": market.get("ticker", ""),
                            "title": market.get("title", event.get("title", "")),
                            "yes_ask": yes_ask,
                            "no_ask": no_ask,
                            "total_cost": total,
                            "guaranteed_profit": round(1.0 - total, 4),
                            "spread_pct": round((1.0 - total) * 100, 2),
                        })

    return {
        "opportunities": sorted(opportunities, key=lambda x: x.get("guaranteed_profit", 0), reverse=True),
        "scanned_at": time.time(),
        "min_spread": min_spread,
    }


# ── Risk Management Endpoints ────────────────────────────────────────────────

@app.get("/api/trading/risk")
async def get_risk():
    return _risk


@app.post("/api/trading/risk")
async def update_risk(request: Request):
    body = await request.json()
    for key in ("max_position_usd", "max_total_exposure_usd", "max_order_usd", "min_arb_spread"):
        if key in body:
            _risk[key] = float(body[key])
    if "max_open_orders" in body:
        _risk["max_open_orders"] = int(body["max_open_orders"])
    if "dry_run" in body:
        _risk["dry_run"] = bool(body["dry_run"])
    return _risk


# ═══════════════════════════════════════════════════════════════════
#  WEATHER / NWP API
# ═══════════════════════════════════════════════════════════════════

def _lazy_import_weather():
    """Import weather modules lazily to avoid startup penalty."""
    import sys, importlib
    parent = str(Path(__file__).resolve().parent.parent)
    if parent not in sys.path:
        sys.path.insert(0, parent)
    bufkit = importlib.import_module("bufkit")
    nwp = importlib.import_module("nwp_pipeline")
    rt = importlib.import_module("realtime")
    return bufkit, nwp, rt


@app.get("/api/weather/cities")
async def weather_cities():
    _, nwp, _ = _lazy_import_weather()
    return {k: {"name": v[0], "settle_icao": v[1], "bufkit_icao": v[2],
                "lat": v[3], "lon": v[4], "series_high": v[5], "series_low": v[6],
                "cli_id": v[7], "wfo": v[8]}
            for k, v in nwp.KALSHI_CITIES.items()}


@app.get("/api/weather/dashboard")
async def weather_dashboard(limit: int = 20):
    """Real-time 20-city dashboard data."""
    _, _, rt = _lazy_import_weather()
    safe_limit = _clamp_int(limit, 20, 1, MAX_LIST_LIMIT)
    rows = _weather_dashboard_rows(rt)[:safe_limit]
    enriched = []
    stale_count = 0
    for row in rows:
        city_key = row.get("ck")
        city = rt.KALSHI_CITIES.get(city_key) if hasattr(rt, "KALSHI_CITIES") else None
        if city:
            stats = row.get("stats", {}) or {}
            models = row.get("models", {}) or {}
            row["city"] = city_key
            row["name"] = row.get("city_name") or city[0]
            row["current_f"] = row.get("current_f", stats.get("current"))
            row["obs_max_f"] = row.get("obs_max_f", stats.get("obs_max"))
            row["hrrr_high_f"] = row.get("hrrr_high_f", models.get("hrrr"))
            row["nam_high_f"] = row.get("nam_high_f", models.get("nam"))
            row["gfs_high_f"] = row.get("gfs_high_f", models.get("gfs"))
            row["est_high_f"] = row.get("est_high_f", row.get("est_high"))
            row["trend_3h"] = row.get("trend_3h", stats.get("trend_3h"))
            enriched_row = _attach_common_lineage(city_key, city, row)
            if enriched_row["freshness"]["stale"]:
                stale_count += 1
            enriched.append(enriched_row)
        else:
            enriched.append(row)
    payload = {
        "updated": time.time(),
        "limit": safe_limit,
        "count": len(enriched),
        "stale_count": stale_count,
        "cities": enriched,
    }
    if _json_response_size(payload) > MAX_RESPONSE_BYTES:
        payload["truncated"] = True
        payload["cities"] = enriched[:max(1, safe_limit // 2)]
    return payload


@app.get("/api/weather/sounding/{city_key}/{model}")
async def weather_sounding(city_key: str, model: str, cycle: int = -1):
    """Full sounding data for Skew-T rendering."""
    bufkit_mod, nwp, _ = _lazy_import_weather()
    from datetime import datetime as dt, timezone as tz

    if city_key not in nwp.KALSHI_CITIES:
        return JSONResponse({"error": f"Unknown city: {city_key}"}, 400)

    now = dt.now(tz.utc)
    if cycle < 0:
        cycle = now.hour

    # Try a few recent cycles
    for offset in range(4):
        c = (cycle - offset) % 24
        forecasts_raw = nwp.fetch_bufkit(model, city_key, c, now)
        if forecasts_raw:
            break
    else:
        return JSONResponse({"error": f"No data for {model}/{city_key}"}, 404)

    # Re-fetch raw bytes and parse with full bufkit parser
    city = nwp.KALSHI_CITIES[city_key]
    bufkit_stn = city[2].lower()
    cfg = nwp.MODELS.get(model, {})
    prefix = cfg.get("bufkit_prefix", model)
    psu_dir = {"hrrr": "HRRR", "rap": "RAP", "nam": "NAM",
               "nam3": "NAMNEST", "gfs": "GFS", "sref": "SREF"}.get(model, model.upper())

    url = f"http://www.meteo.psu.edu/bufkit/data/{psu_dir}/{c:02d}/{prefix}_{bufkit_stn}.buf"
    import urllib.request
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "dsco-nwp/2.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read()
    except:
        return JSONResponse({"error": "Failed to fetch BUFKIT data"}, 502)

    bf = bufkit_mod.parse(raw, model)

    # Build JSON response with all sounding data
    soundings = []
    for s in bf.soundings[:48]:  # limit to 48 hours
        levels = [
            {"pres": l.pres, "tmpc": l.tmpc, "dwpc": l.dwpc, "tmwc": l.tmwc,
             "thte": l.thte, "drct": l.drct, "sknt": l.sknt, "omeg": l.omeg,
             "hght": l.hght, "cfrl": l.cfrl, "tmpf": l.tmpf, "dwpf": l.dwpf}
            for l in s.levels
        ]
        soundings.append({
            "fhr": s.fhr, "valid_utc": s.valid_utc.isoformat(),
            "member": s.member,
            "sfc_temp_f": s.sfc_temp_f, "sfc_dp_f": s.sfc_dewpoint_f,
            "sfc_pres": s.sfc_pres, "sfc_wind": list(s.sfc_wind),
            "cape": s.surface.cape, "cin": s.surface.cins,
            "pwat": s.surface.pwat, "lift": s.surface.lift,
            "kinx": s.surface.kinx, "show": s.surface.show,
            "swet": s.surface.swet, "totl": s.surface.totl,
            "brch": s.surface.brch,
            "freezing_level_m": s.freezing_level(),
            "shear_0_6km": s.wind_shear_0_6km(),
            "convective_risk": s.surface.convective_risk,
            "levels": levels,
        })

    hi_f, hi_t = bf.forecast_high()
    lo_f, lo_t = bf.forecast_low()

    return {
        "model": model, "station": bf.station, "city": city_key,
        "city_name": city[0], "cycle": c,
        "n_hours": bf.n_hours, "n_members": bf.n_members,
        "forecast_high_f": hi_f, "forecast_high_time": hi_t.isoformat(),
        "forecast_low_f": lo_f, "forecast_low_time": lo_t.isoformat(),
        "snparm": bf.snparm_fields, "stnprm": bf.stnprm_fields,
        "soundings": soundings,
    }


@app.get("/api/weather/cross-section/{city_key}/{model}")
async def weather_cross_section(city_key: str, model: str, field: str = "tmpc"):
    """Time-height cross-section data for contour plotting."""
    bufkit_mod, nwp, _ = _lazy_import_weather()
    from datetime import datetime as dt, timezone as tz

    if city_key not in nwp.KALSHI_CITIES:
        return JSONResponse({"error": f"Unknown city: {city_key}"}, 400)

    city = nwp.KALSHI_CITIES[city_key]
    bufkit_stn = city[2].lower()
    cfg = nwp.MODELS.get(model, {})
    prefix = cfg.get("bufkit_prefix", model)
    psu_dir = {"hrrr": "HRRR", "rap": "RAP", "nam": "NAM",
               "nam3": "NAMNEST", "gfs": "GFS", "sref": "SREF"}.get(model, model.upper())

    now = dt.now(tz.utc)
    # Find latest available cycle
    for offset in range(6):
        c = (now.hour - offset) % 24
        if c in cfg.get("cycles", []):
            break
    else:
        c = 0

    url = f"http://www.meteo.psu.edu/bufkit/data/{psu_dir}/{c:02d}/{prefix}_{bufkit_stn}.buf"
    import urllib.request
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "dsco-nwp/2.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read()
    except:
        return JSONResponse({"error": "Failed to fetch"}, 502)

    bf = bufkit_mod.parse(raw, model)

    # Build 2D grid: time (fhr) × pressure
    valid_fields = {"tmpc", "dwpc", "tmwc", "thte", "sknt", "drct", "omeg", "cfrl", "hght"}
    if field not in valid_fields:
        field = "tmpc"

    fhrs = []
    valid_times = []
    pressures = set()
    grid_data = {}

    for s in bf.soundings:
        if s.member != 0:
            continue
        fhrs.append(s.fhr)
        valid_times.append(s.valid_utc.isoformat())
        for lev in s.levels:
            pressures.add(round(lev.pres, 1))
            grid_data[(s.fhr, round(lev.pres, 1))] = getattr(lev, field, None)

    pressures = sorted(pressures, reverse=True)  # high pressure (surface) at bottom

    # Build 2D array [pressure_idx][fhr_idx]
    z = []
    for p in pressures:
        row = []
        for fhr in fhrs:
            val = grid_data.get((fhr, p))
            row.append(val if val is not None and val > -9990 else None)
        z.append(row)

    return {
        "model": model, "city": city_key, "field": field, "cycle": c,
        "fhrs": fhrs, "valid_times": valid_times,
        "pressures": pressures, "z": z,
        "city_name": city[0], "station": bf.station,
    }


@app.get("/api/weather/ensemble/{city_key}")
async def weather_ensemble(city_key: str):
    """SREF ensemble spread data."""
    bufkit_mod, nwp, _ = _lazy_import_weather()
    from datetime import datetime as dt, timezone as tz

    city = nwp.KALSHI_CITIES.get(city_key)
    if not city:
        return JSONResponse({"error": f"Unknown city: {city_key}"}, 400)

    bufkit_stn = city[2].lower()
    now = dt.now(tz.utc)
    sref_cfg = nwp.MODELS["sref"]

    for offset in range(24):
        c = (now.hour - offset) % 24
        if c in sref_cfg["cycles"]:
            break

    url = f"http://www.meteo.psu.edu/bufkit/data/SREF/{c:02d}/sref_{bufkit_stn}.buf"
    import urllib.request
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "dsco-nwp/2.0"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read()
    except:
        return JSONResponse({"error": "Failed to fetch SREF"}, 502)

    bf = bufkit_mod.parse(raw, "sref")

    # Compute spread statistics per forecast hour
    spreads = []
    for fhr in bf.forecast_hours[:48]:
        sp = bf.ensemble_spread(fhr)
        if sp:
            sp["fhr"] = fhr
            # Find valid time
            for s in bf.soundings:
                if s.fhr == fhr:
                    sp["valid_utc"] = s.valid_utc.isoformat()
                    break
            spreads.append(sp)

    return {
        "model": "sref", "city": city_key, "city_name": city[0],
        "cycle": c, "n_members": bf.n_members,
        "spreads": spreads,
    }


@app.get("/api/weather/calibrate/{city_key}")
async def weather_calibrate(city_key: str):
    """Calibrated temperature distribution and trading signals for a city."""
    _, nwp, rt = _lazy_import_weather()
    from datetime import datetime as dt, timezone as tz

    city = nwp.KALSHI_CITIES.get(city_key)
    if not city:
        return JSONResponse({"error": f"Unknown city: {city_key}"}, 400)

    now = dt.now(tz.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)

    # Fetch model highs
    model_highs = rt.fetch_model_highs(city_key, today)
    if not model_highs:
        return JSONResponse({"error": "No model data available"}, 404)

    try:
        from calibration import calibrate_city as _calib
        result = _calib(city_key, model_highs, today)
        result["source_lineage"] = {
            "city": city_key,
            "settlement_station": city[1],
            "series": city[5],
            "model_highs": [{"model": k, "value_f": v} for k, v in sorted(model_highs.items())],
        }
        result["freshness"] = {
            "observation": "fresh" if model_highs else "unknown",
            "model_count": len(model_highs),
        }

        # Fetch Kalshi buckets for edge calculation
        buckets = rt.fetch_kalshi_buckets(city[5], today)
        if buckets:
            from calibration import EdgeCalculator
            calc = EdgeCalculator()
            dist = result.get("distribution")
            if dist:
                edges = calc.compute_edges(dist, buckets)
                portfolio = calc.expected_value(edges)
                result["edges"] = edges
                result["portfolio"] = portfolio

        # Remove non-serializable distribution object
        result.pop("distribution", None)
        return result

    except ImportError:
        return JSONResponse({"error": "Calibration engine not available"}, 500)
    except Exception as e:
        return JSONResponse({"error": str(e)}, 500)


@app.get("/api/weather/calibrate")
async def weather_calibrate_all():
    """Calibrated distributions for all 20 cities."""
    _, nwp, rt = _lazy_import_weather()
    from datetime import datetime as dt, timezone as tz

    now = dt.now(tz.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)

    results = []
    for city_key in sorted(nwp.KALSHI_CITIES.keys()):
        model_highs = rt.fetch_model_highs(city_key, today)
        if not model_highs:
            results.append({"city": city_key, "error": "no data"})
            continue
        try:
            from calibration import calibrate_city as _calib
            r = _calib(city_key, model_highs, today)
            r["source_lineage"] = {
                "city": city_key,
                "settlement_station": city[1],
                "series": city[5],
                "model_highs": [{"model": k, "value_f": v} for k, v in sorted(model_highs.items())],
            }
            r.pop("distribution", None)
            results.append(r)
        except Exception as e:
            results.append({"city": city_key, "error": str(e)})

    return {"updated": time.time(), "cities": results}


@app.get("/api/weather/dashboard/export")
async def weather_dashboard_export(format: str = "json", limit: int = 20):
    _, _, rt = _lazy_import_weather()
    payload = await weather_dashboard(limit=limit)
    cities = payload.get("cities", [])
    fmt = format.lower()
    if fmt == "csv":
        rows = []
        for row in cities:
            stats = row.get("stats", {}) or {}
            freshness = row.get("freshness", {}) or {}
            rows.append({
                "city": row.get("city"),
                "name": row.get("name"),
                "current_f": stats.get("current"),
                "obs_max_f": stats.get("obs_max"),
                "est_high_f": row.get("est_high"),
                "freshness": freshness.get("status"),
                "stale": freshness.get("stale"),
                "settlement_station": row.get("source_lineage", {}).get("settlement_station"),
                "bufkit_station": row.get("source_lineage", {}).get("bufkit_station"),
            })
        csv_text = _csv_bytes(rows, [
            "city", "name", "current_f", "obs_max_f", "est_high_f",
            "freshness", "stale", "settlement_station", "bufkit_station",
        ])
        return PlainTextResponse(csv_text, media_type="text/csv")
    return JSONResponse(content=jsonable_encoder(payload))


@app.get("/api/trading/portfolio/export")
async def trading_portfolio_export(format: str = "json"):
    payload = await cross_portfolio()
    fmt = format.lower()
    if fmt == "csv":
        rows = []
        for platform, data in payload.items():
            if platform == "total_usd":
                continue
            rows.append({
                "platform": platform,
                "balance_usd": data.get("balance_usd"),
                "portfolio_value_usd": data.get("portfolio_value_usd"),
                "positions": len(data.get("positions", []) if isinstance(data.get("positions", []), list) else []),
                "error": data.get("error"),
            })
        csv_text = _csv_bytes(rows, ["platform", "balance_usd", "portfolio_value_usd", "positions", "error"])
        return PlainTextResponse(csv_text, media_type="text/csv")
    return JSONResponse(content=jsonable_encoder(payload))


@app.get("/api/system/status")
async def system_status():
    """Return system subsystem status indicators."""
    return {
        "ooda": {"phase": "idle", "action_space": ["execute", "delegate", "wait", "rest", "escalate"]},
        "governance": {
            "trust_tier": "operator",
            "tiers": ["founder", "operator", "agent", "user"],
            "hardcoded_rules": {"must_always": 7, "must_never": 7},
        },
        "memory": {
            "working": {"halflife_s": 60, "description": "Current task context"},
            "episodic": {"halflife_s": 3600, "description": "Recent interactions"},
            "semantic": {"halflife_s": 0, "description": "Learned facts (no decay)"},
        },
        "pheromone": {
            "signal_types": ["progress", "attraction", "warning", "success", "help_needed", "capacity"],
            "decay_functions": ["exponential", "linear", "step", "logarithmic", "sigmoid"],
            "default_lambda": 0.01,
            "max_signals": 1024,
        },
        "swarm": {
            "max_children": 64,
            "max_groups": 16,
            "max_depth": 6,
            "executors": ["dsco", "claude", "codex"],
        },
        "killswitch": {"armed": True, "status": "nominal"},
    }


@app.get("/api/sessions")
async def list_sessions_endpoint(limit: int = MAX_LIST_LIMIT):
    """List all active sessions."""
    safe_limit = _clamp_int(limit, MAX_LIST_LIMIT, 1, MAX_LIST_LIMIT)
    rows = [
        {
            "id": s.id,
            "model": s.model,
            "turns": s.turns,
            "messages": len(s.messages),
            "total_input": s.total_input,
            "total_output": s.total_output,
        }
        for s in sessions.values()
    ][:safe_limit]
    return {
        "sessions": rows,
        "limit": safe_limit,
        "truncated": len(rows) < len(sessions),
    }


@app.get("/api/topologies")
async def list_topologies(limit: int = MAX_LIST_LIMIT):
    """Return topology list from dsco binary."""
    safe_limit = _clamp_int(limit, MAX_LIST_LIMIT, 1, MAX_LIST_LIMIT)
    try:
        result = subprocess.run(
            [str(DSCO_BIN), "--topology-list"],
            capture_output=True, text=True, timeout=5,
        )
        lines = result.stdout.strip().split("\n")[1:]  # skip header
        topos = []
        for line in lines:
            parts = line.split()
            if len(parts) >= 5:
                topos.append({
                    "id": parts[0], "name": parts[1], "category": parts[2],
                    "agents": parts[3].replace("agents=", ""),
                    "latency": parts[4].replace("latency=", ""),
                })
        trimmed = topos[:safe_limit]
        return {"topologies": trimmed, "limit": safe_limit, "truncated": len(trimmed) < len(topos)}
    except Exception:
        return {"topologies": [], "limit": safe_limit, "truncated": False}


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    session = Session()
    sessions[session.id] = session

    await ws.send_json({
        "type": "connected",
        "session_id": session.id,
        "model": session.model,
        "work_dir": str(WORK_DIR),
        "webrtc": HAS_WEBRTC,
        "model_count": len(MODEL_REGISTRY),
        "dsco_version": DSCO_VERSION_CACHE,
    })

    try:
        while True:
            data = await ws.receive_json()
            msg_type = data.get("type", "")

            if msg_type == "chat":
                content = normalize_user_content(data.get("content", ""))
                if not content:
                    continue
                session.cancelled = False
                session.messages.append({"role": "user", "content": content})
                await ws.send_json({"type": "agent_start"})
                try:
                    await agent_loop(ws, session)
                except Exception as e:
                    log.error(f"agent_loop crash: {e}\n{traceback.format_exc()}")
                    try:
                        await ws.send_json({"type": "error", "message": str(e)})
                        await ws.send_json({"type": "agent_done", "total_turns": 0,
                                            "total_input": session.total_input,
                                            "total_output": session.total_output})
                    except Exception:
                        pass

            elif msg_type == "cancel":
                session.cancelled = True
                await ws.send_json({"type": "cancelled"})

            elif msg_type == "set_model":
                raw = data.get("model", DEFAULT_MODEL)
                session.model = resolve_model(raw)
                await ws.send_json({"type": "model_changed", "model": session.model})

            elif msg_type == "clear":
                session.messages = []
                session.total_input = 0
                session.total_output = 0
                session.total_cache_read = 0
                session.turns = 0
                await ws.send_json({"type": "cleared"})

            elif msg_type == "set_agent_profile":
                profile_name = data.get("profile", "")
                if profile_name:
                    profiles_data = _load_profiles_file()
                    if any(p.get("name") == profile_name for p in profiles_data.get("profiles", [])):
                        session.active_profile = profile_name
                        profile = next(p for p in profiles_data["profiles"] if p["name"] == profile_name)
                        tool_count = len(session.get_tools_anthropic())
                        await ws.send_json({"type": "agent_profile_changed",
                                            "profile": profile_name,
                                            "tool_count": tool_count,
                                            "description": profile.get("description", "")})
                    else:
                        await ws.send_json({"type": "error", "message": f"profile not found: {profile_name}"})
                else:
                    session.active_profile = None
                    await ws.send_json({"type": "agent_profile_changed", "profile": "",
                                        "tool_count": len(TOOLS_ANTHROPIC)})

    except WebSocketDisconnect:
        pass
    except Exception as e:
        log.error(f"ws error: {e}")
        try:
            await ws.send_json({"type": "error", "message": str(e)})
        except Exception:
            pass
    finally:
        sessions.pop(session.id, None)


# ── WebRTC Signaling ─────────────────────────────────────────────────────────

@app.post("/rtc/offer")
async def rtc_offer(request: Request):
    if not HAS_WEBRTC:
        return JSONResponse({"error": "WebRTC not available — pip install aiortc"}, status_code=501)
    body = await request.json()
    offer = RTCSessionDescription(sdp=body["sdp"], type=body["type"])
    pc = RTCPeerConnection()
    pcs.add(pc)

    @pc.on("connectionstatechange")
    async def on_state():
        if pc.connectionState in ("failed", "closed"):
            await pc.close()
            pcs.discard(pc)

    @pc.on("track")
    def on_track(track):
        @track.on("ended")
        async def on_ended():
            pass

    await pc.setRemoteDescription(offer)
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    return JSONResponse({"sdp": pc.localDescription.sdp, "type": pc.localDescription.type})


@app.on_event("shutdown")
async def on_shutdown():
    await asyncio.gather(*(pc.close() for pc in pcs))
    pcs.clear()


# ── Agent Profiles ────────────────────────────────────────────────────────────

AGENT_PROFILES_FILE = Path.home() / ".dsco" / "agent_profiles.json"

ALL_TOOL_GROUPS = [
    "file_io", "git", "network", "shell", "code", "crypto",
    "swarm", "ast", "pipeline", "math", "search", "general",
    "market", "prediction", "memory",
]


def _load_profiles_file() -> dict:
    """Load agent_profiles.json. Returns {profiles: [...], active: str}."""
    if not AGENT_PROFILES_FILE.exists():
        return {"profiles": [], "active": ""}
    try:
        return json.loads(AGENT_PROFILES_FILE.read_text())
    except Exception:
        return {"profiles": [], "active": ""}


def _save_profiles_file(data: dict) -> None:
    AGENT_PROFILES_FILE.parent.mkdir(parents=True, exist_ok=True)
    AGENT_PROFILES_FILE.write_text(json.dumps(data, indent=2))


def _filter_tools(tools: list[dict], profile: dict) -> list[dict]:
    """Filter tool list by an agent profile's groups and tool whitelist."""
    tool_names = set(profile.get("tools", []))
    group_names = set(profile.get("groups", []))
    if not tool_names and not group_names:
        return tools  # no filter
    # Build group membership from tool name heuristics (mirrors assign_group in tools.c)
    def tool_group(name: str) -> str:
        n = name.lower()
        if any(x in n for x in ("file", "read", "write", "edit", "append", "mkdir", "tree", "wc", "head", "tail", "symlink", "page", "list_dir", "find_file", "grep_file", "chmod", "move_file", "copy_file", "delete_file", "file_info")):
            return "file_io"
        if "git" in n: return "git"
        if any(x in n for x in ("http", "curl", "dns", "ping", "port", "net", "cert", "whois", "download", "upload", "websocket", "traceroute", "fetch", "socket")):
            return "network"
        if any(x in n for x in ("bash", "exec", "compile", "run_", "shell")):
            return "shell"
        if any(x in n for x in ("code", "python", "snippet", "eval", "ast", "parse")):
            return "code"
        if any(x in n for x in ("sha", "md5", "base64", "hmac", "hex", "crypt", "hash")):
            return "crypto"
        if any(x in n for x in ("agent", "swarm", "spawn", "legion", "kill")):
            return "swarm"
        if any(x in n for x in ("self_", "inspect", "call_graph", "depend", "ast")):
            return "ast"
        if any(x in n for x in ("pipeline", "stage", "dag")):
            return "pipeline"
        if any(x in n for x in ("math", "calc", "stat", "numeric")):
            return "math"
        if any(x in n for x in ("search", "web_search", "query", "semantic")):
            return "search"
        if any(x in n for x in ("kalshi", "polymarket", "trade", "market", "order", "bet")):
            return "market"
        if any(x in n for x in ("predict", "forecast", "arb")):
            return "prediction"
        if any(x in n for x in ("memory", "remember", "recall", "semantic_mem")):
            return "memory"
        return "general"

    filtered = []
    for t in tools:
        name = t.get("name", "")
        if name in tool_names:
            filtered.append(t)
            continue
        if group_names and tool_group(name) in group_names:
            filtered.append(t)
    return filtered


@app.get("/api/agent-profiles")
async def list_agent_profiles():
    return _load_profiles_file()


@app.post("/api/agent-profiles")
async def save_agent_profile(request: Request):
    data = await request.json()
    name = data.get("name", "").strip()
    if not name:
        return JSONResponse({"error": "name required"}, status_code=400)
    profiles_data = _load_profiles_file()
    profiles = profiles_data.get("profiles", [])
    # Update existing or append
    for i, p in enumerate(profiles):
        if p.get("name") == name:
            profiles[i] = data
            _save_profiles_file({**profiles_data, "profiles": profiles})
            return {"ok": True, "action": "updated"}
    profiles.append(data)
    _save_profiles_file({**profiles_data, "profiles": profiles})
    return {"ok": True, "action": "created"}


@app.delete("/api/agent-profiles/{name}")
async def delete_agent_profile(name: str):
    profiles_data = _load_profiles_file()
    profiles = [p for p in profiles_data.get("profiles", []) if p.get("name") != name]
    active = profiles_data.get("active", "")
    if active == name:
        active = ""
    _save_profiles_file({"profiles": profiles, "active": active})
    return {"ok": True}


@app.post("/api/agent-profiles/{name}/activate")
async def activate_agent_profile(name: str):
    profiles_data = _load_profiles_file()
    if not any(p.get("name") == name for p in profiles_data.get("profiles", [])):
        return JSONResponse({"error": "profile not found"}, status_code=404)
    _save_profiles_file({**profiles_data, "active": name})
    return {"ok": True, "active": name}


@app.post("/api/agent-profiles/deactivate")
async def deactivate_agent_profile():
    profiles_data = _load_profiles_file()
    _save_profiles_file({**profiles_data, "active": ""})
    return {"ok": True}


@app.get("/api/agent-profiles/groups")
async def list_tool_groups():
    return {"groups": ALL_TOOL_GROUPS}


# ── Fleet / Bridge API ───────────────────────────────────────────────────────

BRIDGE_DIR = Path.home() / "bridge"


def _bridge_nodes() -> list[dict]:
    """Scan ~/bridge for .host registry files and the fleet directory."""
    nodes = []
    # fleet/<hostname>.host style
    fleet_dir = BRIDGE_DIR / "fleet"
    if fleet_dir.is_dir():
        for f in sorted(fleet_dir.glob("*.host")):
            lines = f.read_text(errors="ignore").splitlines()
            meta: dict[str, str] = {}
            for ln in lines:
                if "=" in ln:
                    k, _, v = ln.partition("=")
                    meta[k.strip()] = v.strip()
            meta.setdefault("name", f.stem)
            meta.setdefault("host_file", str(f))
            nodes.append(meta)
    # single-file .host at bridge root
    for f in sorted(BRIDGE_DIR.glob("*.host")):
        lines = f.read_text(errors="ignore").splitlines()
        meta: dict[str, str] = {}
        for ln in lines:
            if "=" in ln:
                k, _, v = ln.partition("=")
                meta[k.strip()] = v.strip()
        meta.setdefault("name", f.stem)
        meta.setdefault("host_file", str(f))
        nodes.append(meta)
    return nodes


def _inbox_messages(node_name: str | None = None, limit: int = 50) -> list[dict]:
    """Read messages from ~/bridge/inbox (or node-specific inbox)."""
    msgs = []
    if node_name:
        inbox = BRIDGE_DIR / node_name / "inbox"
    else:
        inbox = BRIDGE_DIR / "inbox"
    if not inbox.is_dir():
        return msgs
    files = sorted(inbox.iterdir(), reverse=True)[:limit]
    for f in files:
        try:
            msgs.append({
                "file": f.name,
                "mtime": f.stat().st_mtime,
                "content": f.read_text(errors="ignore")[:2048],
            })
        except Exception:
            pass
    return msgs


def _outbox_messages(node_name: str | None = None, limit: int = 50) -> list[dict]:
    msgs = []
    if node_name:
        outbox = BRIDGE_DIR / node_name / "outbox"
    else:
        outbox = BRIDGE_DIR / "outbox"
    if not outbox.is_dir():
        return msgs
    files = sorted(outbox.iterdir(), reverse=True)[:limit]
    for f in files:
        try:
            msgs.append({
                "file": f.name,
                "mtime": f.stat().st_mtime,
                "content": f.read_text(errors="ignore")[:2048],
            })
        except Exception:
            pass
    return msgs


def _audit_tail(n: int = 100) -> list[dict]:
    """Read the dsco binary audit log (text format) from ~/.dsco/audit.log.
    Returns list of {seq, ts, tag, msg} dicts.  The binary format is parsed
    lightly — falls back to raw lines if unrecognised."""
    path = Path.home() / ".dsco" / "audit.log"
    if not path.exists():
        return []
    # binary audit log from audit_log.c — skip, just return size info
    size = path.stat().st_size
    return [{"seq": -1, "ts": int(path.stat().st_mtime),
              "tag": "meta", "msg": f"audit log {size} bytes (binary)"}]


def _watchdog_ping_age() -> float | None:
    """Seconds since last watchdog ping; None if file absent."""
    p = Path.home() / ".dsco" / "watchdog.ping"
    if not p.exists():
        return None
    try:
        ts = int(p.read_text().strip())
        return time.time() - ts
    except Exception:
        return None


@app.get("/api/fleet/nodes")
async def fleet_nodes():
    nodes = _bridge_nodes()
    ping_age = _watchdog_ping_age()
    return {
        "nodes": nodes,
        "bridge_dir": str(BRIDGE_DIR),
        "bridge_exists": BRIDGE_DIR.is_dir(),
        "watchdog_ping_age_s": ping_age,
    }


@app.get("/api/fleet/inbox")
async def fleet_inbox(node: Optional[str] = None, limit: int = 50):
    return {"messages": _inbox_messages(node, limit)}


@app.get("/api/fleet/outbox")
async def fleet_outbox(node: Optional[str] = None, limit: int = 50):
    return {"messages": _outbox_messages(node, limit)}


@app.post("/api/fleet/send")
async def fleet_send(request: Request):
    """Write a message to ~/bridge/outbox/<timestamp>_<uuid>.msg"""
    body = await request.json()
    content = body.get("content", "")
    node = body.get("node")
    if node:
        outbox = BRIDGE_DIR / node / "outbox"
    else:
        outbox = BRIDGE_DIR / "outbox"
    outbox.mkdir(parents=True, exist_ok=True)
    ts = int(time.time() * 1000)
    fname = outbox / f"{ts}_{uuid.uuid4().hex[:8]}.msg"
    fname.write_text(content)
    return {"ok": True, "file": str(fname)}


@app.get("/api/fleet/audit")
async def fleet_audit(n: int = 100):
    return {"entries": _audit_tail(n)}


@app.get("/api/fleet/status")
async def fleet_status():
    nodes = _bridge_nodes()
    ping_age = _watchdog_ping_age()
    daemon_log = Path.home() / ".dsco" / "daemon.log"
    recent_log = ""
    if daemon_log.exists():
        try:
            text = daemon_log.read_text(errors="ignore")
            recent_log = text[-4096:] if len(text) > 4096 else text
        except Exception:
            pass
    return {
        "node_count": len(nodes),
        "bridge_exists": BRIDGE_DIR.is_dir(),
        "watchdog_ping_age_s": ping_age,
        "watchdog_ok": ping_age is not None and ping_age < 120,
        "daemon_log_tail": recent_log,
        "nodes": nodes,
    }


# ── Static Files ─────────────────────────────────────────────────────────────

if STATIC_DIR.exists():
    from starlette.staticfiles import StaticFiles
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="dsco web UI")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--dir", type=str, default=None)
    parser.add_argument("--model", type=str, default=None)
    parser.add_argument("--open", action="store_true")
    args = parser.parse_args()

    global WORK_DIR, DEFAULT_MODEL, DSCO_BIN
    if args.dir:
        WORK_DIR = Path(args.dir).resolve()
        # dsco binary is likely next to web/ dir
        candidate = WORK_DIR / "dsco"
        if candidate.exists():
            DSCO_BIN = candidate
    if args.model:
        DEFAULT_MODEL = args.model

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")

    # Load model and tool registries from dsco binary
    load_model_registry()
    load_tool_registry()
    _ensure_control_plane()
    global TOOLS_OPENAI, DSCO_VERSION_CACHE
    TOOLS_OPENAI = get_tools_openai()
    if DSCO_BIN.exists():
        try:
            DSCO_VERSION_CACHE = subprocess.run(
                [str(DSCO_BIN), "--version"], capture_output=True, text=True, timeout=5
            ).stdout.strip() or "unknown"
        except (subprocess.TimeoutExpired, OSError):
            DSCO_VERSION_CACHE = "unknown"

    port = args.port
    url = f"http://{args.host}:{port}"

    # Show available providers
    available = []
    for prov, ep in PROVIDER_ENDPOINTS.items():
        if os.getenv(ep["env"]):
            available.append(prov)
    if os.getenv("ANTHROPIC_API_KEY"):
        available.insert(0, "anthropic")

    print(f"\033[36m")
    print(f"  ┌──────────────────────────────────────────┐")
    print(f"  │  dsco web UI                              │")
    print(f"  │                                           │")
    print(f"  │  {url:<41s}│")
    print(f"  │                                           │")
    print(f"  │  dir:      {str(WORK_DIR)[:30]:<30s} │")
    print(f"  │  model:    {DEFAULT_MODEL[:30]:<30s} │")
    print(f"  │  models:   {len(MODEL_REGISTRY):<30d} │")
    print(f"  │  providers: {', '.join(available) if available else '(none!)':<29s}│")
    print(f"  │  webrtc:   {'yes' if HAS_WEBRTC else 'no':<30s} │")
    print(f"  └──────────────────────────────────────────┘")
    print(f"\033[0m")

    if not available:
        print("\033[33m  warning: no API keys found — set ANTHROPIC_API_KEY or OPENROUTER_API_KEY\033[0m\n")

    if args.open:
        import webbrowser
        webbrowser.open(url)

    uvicorn.run(app, host=args.host, port=port, log_level="warning",
                ws_ping_interval=30, ws_ping_timeout=120)


if __name__ == "__main__":
    main()
