#!/usr/bin/env python3
#
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "dspy",
#   "gepa",
#   "openai",
# ]
# ///
#
"""Run a DSPy CLI with dsco as the LM backend.

dsco's web server exposes an OpenAI-compatible gateway at /v1/chat/completions
(see web/server.py). This script gives you a thin command-line wrapper around
`dspy.Predict`, plus an optional cost-aware task sampling loop, with dsco doing
provider routing behind the OpenAI-compatible gateway.

    ./dsco --ui 3141 runs the gateway -> dsco provider routing -> Anthropic / OpenAI / OpenRouter / ...

Usage:
    uv run examples/dspy_via_dsco.py "What is the capital of Australia?"
    uv run examples/dspy_via_dsco.py --prompt "What is the capital of Australia?"
    uv run examples/dspy_via_dsco.py --list-models
    echo "summarize this" | uv run examples/dspy_via_dsco.py --signature "text -> summary"
    uv run examples/dspy_via_dsco.py --agent-loop \
        --budget-usd 0.25 --max-samples 3 "Draft a launch checklist"
    uv run examples/dspy_via_dsco.py --model-role transform "Classify this log line"
    uv run examples/dspy_via_dsco.py --model-role decision \
        --signature "problem, evidence -> recommendation, risks" \
        --field evidence=@findings.txt "Choose the safest patch"
    uv run examples/dspy_via_dsco.py --stream \
        --signature "question -> answer" "How do dsco tool schemas work?"
    uv run examples/dspy_via_dsco.py --app chat --stream "Remember that dsco routes models locally."
    uv run examples/dspy_via_dsco.py --train --optimizer gepa --trainset-jsonl train.jsonl \
        --signature "question, context -> answer" --context-file docs/API_REFERENCE.md
    JINA_API_KEY=... uv run examples/dspy_via_dsco.py \
        --signature "question, context -> answer" \
        --context-file docs/API_REFERENCE.md "How do tool schemas work?"
    uv run examples/dspy_via_dsco.py --train --trainset-jsonl train.jsonl \
        --signature "question, context -> answer" --context-file docs/API_REFERENCE.md
"""

import argparse
import asyncio
import hashlib
from dataclasses import dataclass, field
import json
import math
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any
from urllib.parse import urlparse


DEFAULT_BASE_URL = "http://127.0.0.1:3141/v1"
DEFAULT_MODEL = "claude-fable-5"
DEFAULT_SIGNATURE = "question -> answer"
DEFAULT_AGENT_SIGNATURE = "task, context -> result, confidence, new_tasks"
DEFAULT_AGENT_CONTEXT = (
    "Solve the current task directly using typed DSPy outputs. If more work is genuinely useful, propose "
    "follow-up tasks. Keep result concise. Return new_tasks as a JSON array of "
    "strings; use [] when no follow-up is needed."
)
DEFAULT_JINA_API_BASE = "https://api.jina.ai/v1"
DEFAULT_JINA_EMBEDDING_MODEL = "jina-embeddings-v5-text-small"
DEFAULT_JINA_RERANKER_MODEL = "jina-reranker-v3"
DEFAULT_JINA_QUERY_TASK = "retrieval.query"
DEFAULT_JINA_DOCUMENT_TASK = "retrieval.passage"
DEFAULT_TRAIN_SAVE_PATH = "dspy_optimized_program.json"
DEFAULT_SESSION_DISTILL_SIGNATURE = "session_context -> distillation"
DEFAULT_SESSION_DISTILL_TRAINSET = "reports/dspy_session_distill_trainset.jsonl"
DEFAULT_SESSION_DISTILL_ARTIFACT = "reports/dspy_session_distill_haiku.json"
APPLICATION_PRESETS: dict[str, dict[str, Any]] = {
    "qa": {"signature": DEFAULT_SIGNATURE, "program_kind": "predict"},
    "chat": {"signature": DEFAULT_SIGNATURE, "program_kind": "predict", "chat": True},
    "rag": {"signature": "question, context -> answer", "program_kind": "cot"},
    "summarize": {"signature": "text -> summary", "program_kind": "predict"},
    "classify": {"signature": "text, labels -> label, rationale", "program_kind": "cot"},
    "extract": {"signature": "text -> extracted_json", "program_kind": "predict"},
    "rewrite": {"signature": "text, instruction -> revised_text", "program_kind": "predict"},
    "compare": {"signature": "left, right, criteria -> judgment, rationale", "program_kind": "cot"},
    "plan": {"signature": "goal -> plan, risks, next_steps", "program_kind": "cot"},
    "math": {"signature": "problem -> answer", "program_kind": "pot"},
    "code-review": {"signature": "code, criteria -> findings, risks", "program_kind": "cot"},
    "entity-extraction": {"signature": "text -> entities_json", "program_kind": "predict"},
}
OPTIMIZER_ALIASES = {
    "bootstrap-fewshot": "bootstrap",
    "bootstrap-random": "bootstrap-rs",
    "labeled-fewshot": "labeled",
}
PROMPT_CACHE_SUPPORTED_NEEDLES = ("claude", "anthropic/", "openrouter/anthropic/")
OPENAI_PROMPT_CACHE_PREFIXES = ("gpt-", "o1", "o3", "o4", "chatgpt-", "openai/gpt-", "openai/o")
DEFAULT_OPENAI_PROMPT_CACHE_RETENTION = "24h"
LOCAL_RETRIEVAL_STOPWORDS = frozenset(
    {
        "a",
        "an",
        "and",
        "are",
        "as",
        "be",
        "by",
        "do",
        "does",
        "for",
        "from",
        "how",
        "in",
        "is",
        "it",
        "of",
        "on",
        "or",
        "the",
        "to",
        "with",
        "work",
    }
)

# Latest four-layer Anthropic swarm profile used by dsco's OpenAI-compatible gateway.
# These are intentionally local metadata: DSPy only needs model/api_base/api_key,
# but the wrapper uses the profile for routing, cost estimates, safe defaults,
# and to avoid provider-side 400s on models with sampling/thinking constraints.
ANTHROPIC_MODEL_CARDS: dict[str, dict[str, Any]] = {
    "claude-fable-5": {
        "layer": "root",
        "role": "frontier",
        "label": "root / frontier delegator / ambiguous multi-day work",
        "fan_out_cap": 4,
        "efforts": ["medium", "high", "xhigh"],
        "default_effort": "high",
        "context_window": 1_000_000,
        "max_output": 128_000,
        "cache_min_tokens": 512,
        "sampling_locked": True,
        "price": (10.0, 50.0),
    },
    "claude-opus-4-8": {
        "layer": "review",
        "role": "decision",
        "label": "review / risk-laden decisions and planning",
        "fan_out_cap": 6,
        "efforts": ["low", "medium", "high", "xhigh"],
        "default_effort": "xhigh",
        "context_window": 1_000_000,
        "max_output": 128_000,
        "cache_min_tokens": 1_024,
        "sampling_locked": True,
        "price": (5.0, 25.0),
    },
    "claude-sonnet-5": {
        "layer": "orchestrate",
        "role": "synthesis",
        "label": "orchestrate / workhorse coding and synthesis",
        "fan_out_cap": 8,
        "efforts": ["low", "medium", "high", "xhigh"],
        "default_effort": "high",
        "context_window": 1_000_000,
        "max_output": 128_000,
        "cache_min_tokens": 1_024,
        "sampling_locked": True,
        # Introductory Sonnet 5 pricing from the max_swarm model card.
        "price": (2.0, 10.0),
    },
    "claude-haiku-4-5": {
        "layer": "leaf",
        "role": "transform",
        "label": "leaf / retrieval, classification, transforms",
        "fan_out_cap": 0,
        "efforts": ["low", "medium", "high", "max", "xhigh"],
        "default_effort": "low",
        "context_window": 200_000,
        "max_output": 64_000,
        "cache_min_tokens": 4_096,
        "sampling_locked": False,
        "price": (1.0, 5.0),
    },
}

SWARM_LAYERS: tuple[dict[str, str], ...] = tuple(
    {"layer": str(card["layer"]), "role": str(card["role"]), "model": model}
    for model, card in ANTHROPIC_MODEL_CARDS.items()
)

MODEL_ALIASES = {
    "root": "claude-fable-5",
    "fable": "claude-fable-5",
    "frontier": "claude-fable-5",
    "review": "claude-opus-4-8",
    "opus": "claude-opus-4-8",
    "opus-4.8": "claude-opus-4-8",
    "claude-opus-4.8": "claude-opus-4-8",
    "decision": "claude-opus-4-8",
    "orchestrate": "claude-sonnet-5",
    "sonnet": "claude-sonnet-5",
    "workhorse": "claude-sonnet-5",
    "synthesis": "claude-sonnet-5",
    "leaf": "claude-haiku-4-5",
    "haiku": "claude-haiku-4-5",
    "haiku-4.5": "claude-haiku-4-5",
    "claude-haiku-4.5": "claude-haiku-4-5",
    "transform": "claude-haiku-4-5",
}

MODEL_ROLE_TO_ID: dict[str, str] = {}
for _model, _card in ANTHROPIC_MODEL_CARDS.items():
    MODEL_ROLE_TO_ID[str(_card["role"])] = _model
    MODEL_ROLE_TO_ID[str(_card["layer"])] = _model

# Defaults from the Claude pricing table pasted into this session. Keep these
# overridable because model aliases and provider billing can drift.
MODEL_PRICE_PER_MTOK: dict[str, tuple[float, float]] = {
    **{model: tuple(card["price"]) for model, card in ANTHROPIC_MODEL_CARDS.items()},
    "claude-mythos-5": (10.0, 50.0),
    "claude-mythos-preview": (10.0, 50.0),
    "claude-opus-4.8": (5.0, 25.0),
    "claude-opus-4-7": (5.0, 25.0),
    "claude-sonnet-4-6": (3.0, 15.0),
    "claude-haiku-4.5": (1.0, 5.0),
    "claude-haiku-4-5-20251001": (1.0, 5.0),
}


@dataclass
class AgentTask:
    id: str
    task: str
    context: str = ""
    parent_id: str | None = None
    depth: int = 0
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class CostLedger:
    input_price_per_mtok: float
    output_price_per_mtok: float
    input_tokens: int = 0
    output_tokens: int = 0
    actual_spend_usd: float = 0.0

    def estimate_usd(
        self,
        input_tokens: int,
        output_tokens: int,
        prices: tuple[float, float] | None = None,
    ) -> float:
        input_price, output_price = prices or (self.input_price_per_mtok, self.output_price_per_mtok)
        return (
            (input_tokens / 1_000_000.0) * input_price
            + (output_tokens / 1_000_000.0) * output_price
        )

    def add(
        self,
        input_tokens: int,
        output_tokens: int,
        prices: tuple[float, float] | None = None,
    ) -> float:
        self.input_tokens += max(0, input_tokens)
        self.output_tokens += max(0, output_tokens)
        cost = self.estimate_usd(input_tokens, output_tokens, prices)
        self.actual_spend_usd += cost
        return cost

    @property
    def spent_usd(self) -> float:
        return self.actual_spend_usd


def import_dspy():
    try:
        import dspy
    except ModuleNotFoundError as exc:
        if exc.name == "dspy":
            raise SystemExit(
                "Missing dependency: dspy\n"
                "Run this example with `uv run examples/dspy_via_dsco.py ...` so uv installs the script dependencies."
            ) from None
        raise
    return dspy


def configure_dspy_cache(dspy) -> None:
    configure_cache = getattr(dspy, "configure_cache", None)
    if not configure_cache:
        return
    try:
        configure_cache(enable_disk_cache=True, enable_memory_cache=True, restrict_pickle=True)
    except TypeError:
        configure_cache(enable_disk_cache=True, enable_memory_cache=True)


def split_fields(field_list: str) -> list[str]:
    fields = []
    for raw in field_list.split(","):
        name = raw.strip()
        if not name:
            continue
        # Accept human-friendly fragments such as "verdict: bool"; DSPy's
        # signature parser gets the original string, this is only for IO mapping.
        name = name.split(":", 1)[0].strip()
        name = name.split(None, 1)[0].strip()
        if name:
            fields.append(name)
    return fields


def signature_io_fields(signature: str) -> tuple[list[str], list[str]]:
    if "->" not in signature:
        raise SystemExit(f"Invalid --signature {signature!r}; expected 'input -> output'.")
    inputs, outputs = signature.split("->", 1)
    input_fields = split_fields(inputs)
    output_fields = split_fields(outputs)
    if not input_fields or not output_fields:
        raise SystemExit(f"Invalid --signature {signature!r}; expected at least one input and one output field.")
    return input_fields, output_fields


def normalized_optimizer_name(name: str) -> str:
    return OPTIMIZER_ALIASES.get(name, name)


def apply_application_preset(args: argparse.Namespace) -> None:
    if args.app:
        preset = APPLICATION_PRESETS[args.app]
        if args.signature is None:
            args.signature = str(preset["signature"])
        if args.program_kind is None:
            args.program_kind = str(preset["program_kind"])
        if preset.get("chat"):
            args.chat = True
    if getattr(args, "distill_sessions", False) and args.signature is None:
        args.signature = DEFAULT_SESSION_DISTILL_SIGNATURE
    if args.signature is None:
        args.signature = DEFAULT_SIGNATURE
    args.optimizer = normalized_optimizer_name(args.optimizer)


def history_input_fields(input_fields: list[str]) -> list[str]:
    if "history" in input_fields:
        return input_fields
    return [*input_fields, "history"]


def first_non_history_field(input_fields: list[str]) -> str:
    for name in input_fields:
        if name != "history":
            return name
    raise SystemExit("Chat signatures need at least one non-history input field.") from None


def prompt_field_for_inputs(
    input_fields: list[str],
    inputs: dict[str, Any],
    explicit_field: str | None,
    *,
    skip_history: bool = False,
) -> str:
    if explicit_field:
        return explicit_field
    candidates = [name for name in input_fields if not (skip_history and name == "history")]
    for name in candidates:
        if name not in inputs:
            return name
    if candidates:
        return candidates[0]
    raise SystemExit("Signature needs at least one prompt input field.") from None


def build_history_signature(dspy, input_fields: list[str], output_fields: list[str]):
    annotations: dict[str, Any] = {}
    attrs: dict[str, Any] = {"__annotations__": annotations}
    for name in history_input_fields(input_fields):
        annotations[name] = getattr(dspy, "History") if name == "history" else str
        attrs[name] = dspy.InputField()
    for name in output_fields:
        annotations[name] = str
        attrs[name] = dspy.OutputField()
    return type("DscoHistorySignature", (dspy.Signature,), attrs)


def load_history_messages(path: str | None) -> list[dict[str, Any]]:
    if not path:
        return []
    history_path = Path(path).expanduser()
    if not history_path.exists():
        return []
    try:
        raw = history_path.read_text(encoding="utf-8").strip()
        if not raw:
            return []
        payload = json.loads(raw)
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Could not read --history-file {history_path}: {exc}") from None
    messages = payload.get("messages") if isinstance(payload, dict) else payload
    if not isinstance(messages, list) or any(not isinstance(item, dict) for item in messages):
        raise SystemExit("--history-file must contain a JSON list, or an object with a messages list.") from None
    return [dict(item) for item in messages]


def save_history_messages(path: str | None, messages: list[dict[str, Any]]) -> None:
    if not path:
        return
    history_path = Path(path).expanduser()
    history_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        history_path.write_text(json.dumps({"messages": messages}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except OSError as exc:
        raise SystemExit(f"Could not write --history-file {history_path}: {exc}") from None


def chat_prompts_from_cli(args: argparse.Namespace) -> list[str]:
    if args.prompt_text is not None:
        prompt = args.prompt_text.strip()
        return [prompt] if prompt else []
    prompt = " ".join(args.prompt).strip() if args.prompt else ""
    if prompt:
        return [prompt]
    if not sys.stdin.isatty():
        return [line.strip() for line in sys.stdin if line.strip()]
    return []


def canonical_model(model: str | None, role: str | None = None) -> str:
    """Resolve convenience aliases and DSPy task roles to gateway model ids."""
    if role:
        if role not in MODEL_ROLE_TO_ID:
            raise SystemExit(f"Unknown --model-role {role!r}; choose one of: {', '.join(sorted(MODEL_ROLE_TO_ID))}")
        return MODEL_ROLE_TO_ID[role]
    raw = model or DEFAULT_MODEL
    normalized = raw.lower().replace("_", "-")
    return MODEL_ALIASES.get(normalized, normalized)


def model_card(model: str) -> dict[str, Any] | None:
    return ANTHROPIC_MODEL_CARDS.get(canonical_model(model))


def model_supports_provider_prompt_cache(model: str) -> bool:
    normalized = canonical_model(model).lower().replace("_", "-")
    return any(needle in normalized for needle in PROMPT_CACHE_SUPPORTED_NEEDLES)


def model_supports_openai_prompt_cache_hints(model: str) -> bool:
    normalized = canonical_model(model).lower().replace("_", "-")
    if normalized.startswith("openai/"):
        normalized = normalized[len("openai/") :]
    return normalized.startswith(OPENAI_PROMPT_CACHE_PREFIXES)


def prompt_cache_key(args: argparse.Namespace, model: str) -> str:
    signature = getattr(args, "agent_signature", None) if getattr(args, "agent_loop", False) else getattr(args, "signature", "")
    material = json.dumps(
        {
            "base_url": getattr(args, "base_url", DEFAULT_BASE_URL),
            "model": canonical_model(model),
            "program_kind": getattr(args, "program_kind", None)
            or ("cot" if getattr(args, "train", False) else "predict"),
            "signature": signature,
        },
        ensure_ascii=False,
        sort_keys=True,
    )
    return "dsco-dspy-" + hashlib.sha256(material.encode("utf-8")).hexdigest()[:24]


def prompt_cache_control(args: argparse.Namespace, model: str) -> dict[str, str] | None:
    if not model_supports_provider_prompt_cache(model):
        return None
    control = {"type": "ephemeral"}
    if getattr(args, "prompt_cache_ttl", "5m") == "1h":
        control["ttl"] = "1h"
    return control


def prompt_cache_extra_body(args: argparse.Namespace, model: str) -> dict[str, Any]:
    cache_key = prompt_cache_key(args, model)
    extra: dict[str, Any] = {
        "dsco_prompt_cache": True,
        "dsco_prompt_cache_key": cache_key,
    }
    cache_control = prompt_cache_control(args, model)
    if cache_control:
        extra["cache_control"] = cache_control
        extra["dsco_prompt_cache_target"] = getattr(args, "prompt_cache_target", "system")
    if model_supports_openai_prompt_cache_hints(model):
        extra["prompt_cache_key"] = cache_key
        extra["prompt_cache_retention"] = getattr(
            args,
            "prompt_cache_retention",
            DEFAULT_OPENAI_PROMPT_CACHE_RETENTION,
        )
    return extra


def prompt_cache_plan(args: argparse.Namespace, model: str) -> dict[str, Any]:
    control = prompt_cache_control(args, model)
    openai_hints = model_supports_openai_prompt_cache_hints(model)
    if control and openai_hints:
        note = "DSPy response cache is on; provider cache_control and OpenAI cache-affinity hints are sent through extra_body."
    elif control:
        note = "DSPy response cache is on; provider cache_control is sent through extra_body."
    elif openai_hints:
        note = "DSPy response cache is on; OpenAI prompt caching is automatic, with cache-affinity hints sent through extra_body."
    else:
        note = "DSPy response cache is on; provider prompt caching is automatic only when the routed model supports it."
    return {
        "enabled": True,
        "dspy_response_cache": True,
        "provider_prompt_cache": {
            "cache_control": bool(control),
            "openai_hints": bool(openai_hints),
            "target": getattr(args, "prompt_cache_target", "system") if control else None,
            "ttl": getattr(args, "prompt_cache_ttl", "5m") if control else None,
            "retention": getattr(args, "prompt_cache_retention", DEFAULT_OPENAI_PROMPT_CACHE_RETENTION)
            if openai_hints
            else None,
            "cache_key": prompt_cache_key(args, model),
            "transport": "dsco-gateway-extra-body",
        },
        "note": note,
    }


def swarm_layer_for_depth(depth: int) -> dict[str, str]:
    idx = max(0, min(depth, len(SWARM_LAYERS) - 1))
    return SWARM_LAYERS[idx]


def agent_loop_model_for_task(args: argparse.Namespace, task: AgentTask) -> str:
    if getattr(args, "swarm_layers", False):
        return swarm_layer_for_depth(task.depth)["model"]
    return canonical_model(args.model, getattr(args, "model_role", None))


def read_value(value: str) -> str:
    """Support DSPy's field-rich style from CLI: --field context=@file.md."""
    if value.startswith("@@"):
        return value[1:]
    if value.startswith("@"):
        path = Path(value[1:]).expanduser()
        try:
            return path.read_text(encoding="utf-8")
        except OSError as exc:
            raise SystemExit(f"Could not read field file {path}: {exc}") from None
    return value


def parse_field_assignments(assignments: list[str]) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for item in assignments:
        name, sep, value = item.partition("=")
        name = name.strip()
        if not sep or not name:
            raise SystemExit(f"Invalid --field {item!r}; expected NAME=VALUE.")
        values[name] = read_value(value)
    return values


def env_or_arg(value: str | None, env_name: str) -> str | None:
    if value:
        return value
    env_value = os.getenv(env_name)
    return env_value if env_value else None


def resolve_jina_api_key(args: argparse.Namespace, required: bool = False) -> str | None:
    api_key = env_or_arg(getattr(args, "jina_api_key", None), "JINA_API_KEY")
    if required and not api_key:
        raise SystemExit(
            "Jina retrieval requested but no API key was provided.\n"
            "Set JINA_API_KEY or pass --jina-api-key. The key is used for both /v1/embeddings and /v1/rerank."
        ) from None
    return api_key


def jina_endpoint(args: argparse.Namespace, path: str) -> str:
    base = getattr(args, "jina_api_base", DEFAULT_JINA_API_BASE).rstrip("/")
    return f"{base}/{path.lstrip('/')}"


def jina_json_post_curl(endpoint: str, api_key: str, payload: dict[str, Any], timeout: float) -> dict[str, Any] | None:
    if not shutil.which("curl"):
        return None

    body = json.dumps(payload, ensure_ascii=False)
    marker = "\nHTTP_STATUS="
    attempts = 4
    last_response = ""
    last_status = 0

    for attempt in range(1, attempts + 1):
        cmd = [
            "curl",
            "-sS",
            "--max-time",
            str(timeout),
            "-X",
            "POST",
            endpoint,
            "-H",
            f"Authorization: Bearer {api_key}",
            "-H",
            "Content-Type: application/json",
            "-H",
            "Accept: application/json",
            "--data-binary",
            "@-",
            "-w",
            marker + "%{http_code}",
        ]
        try:
            result = subprocess.run(cmd, input=body, text=True, capture_output=True, timeout=timeout + 2)
        except (OSError, subprocess.TimeoutExpired) as exc:
            if attempt < attempts:
                time.sleep(min(8.0, 0.75 * (2 ** (attempt - 1))))
                continue
            raise SystemExit(f"Could not reach Jina API with curl transport: {exc}") from None
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()[:2000]
            if attempt < attempts:
                time.sleep(min(8.0, 0.75 * (2 ** (attempt - 1))))
                continue
            raise SystemExit(f"Jina API curl transport failed: {detail}") from None

        response_text, sep, status_text = result.stdout.rpartition(marker)
        if not sep:
            raise SystemExit("Jina API curl transport did not report an HTTP status.") from None
        try:
            status = int(status_text.strip())
        except ValueError:
            raise SystemExit(f"Jina API curl transport returned invalid HTTP status: {status_text!r}") from None
        last_response = response_text
        last_status = status
        if status in {429, 503} and attempt < attempts:
            time.sleep(min(8.0, 0.75 * (2 ** (attempt - 1))))
            continue
        if status < 200 or status >= 300:
            raise SystemExit(f"Jina API request failed with HTTP {status}: {response_text[:2000]}") from None
        try:
            decoded = json.loads(response_text)
        except json.JSONDecodeError as exc:
            raise SystemExit(f"Jina API returned invalid JSON: {exc}") from None
        if not isinstance(decoded, dict):
            raise SystemExit(f"Jina API returned an unexpected payload: {type(decoded).__name__}")
        return decoded

    raise SystemExit(f"Jina API request failed with HTTP {last_status}: {last_response[:2000]}") from None


def jina_json_post(args: argparse.Namespace, path: str, payload: dict[str, Any]) -> dict[str, Any]:
    api_key = resolve_jina_api_key(args, required=True)
    endpoint = jina_endpoint(args, path)
    timeout = getattr(args, "jina_timeout", 30.0)
    curl_response = jina_json_post_curl(endpoint, api_key, payload, timeout)
    if curl_response is not None:
        return curl_response

    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "dsco-dspy-via-dsco/1.0",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        try:
            detail = exc.read().decode("utf-8", errors="replace")[:2000]
        except Exception:
            detail = str(exc)
        raise SystemExit(f"Jina API request to {path} failed with HTTP {exc.code}: {detail}") from None
    except (urllib.error.URLError, TimeoutError, socket.timeout, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        raise SystemExit(f"Could not reach Jina API endpoint {path}: {reason}") from None

    try:
        decoded = json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Jina API endpoint {path} returned invalid JSON: {exc}") from None
    if not isinstance(decoded, dict):
        raise SystemExit(f"Jina API endpoint {path} returned an unexpected payload: {type(decoded).__name__}")
    return decoded


def clean_optional_task(task: str | None) -> str | None:
    if not task:
        return None
    task = task.strip()
    if not task or task.lower() in {"none", "null", "off"}:
        return None
    return task


def parse_embedding_items(response: dict[str, Any], expected: int) -> list[list[float]]:
    data = response.get("data")
    if not isinstance(data, list):
        raise SystemExit("Jina embeddings response did not include a data list.")
    try:
        ordered = sorted(data, key=lambda item: int(item.get("index", 0)) if isinstance(item, dict) else 0)
        embeddings = []
        for item in ordered:
            if not isinstance(item, dict) or "embedding" not in item:
                raise ValueError("missing embedding")
            embeddings.append([float(x) for x in item["embedding"]])
    except (TypeError, ValueError) as exc:
        raise SystemExit(f"Jina embeddings response had an unexpected shape: {exc}") from None
    if len(embeddings) != expected:
        raise SystemExit(f"Jina embeddings returned {len(embeddings)} vectors for {expected} inputs.")
    return embeddings


def jina_embed_texts(args: argparse.Namespace, texts: list[str], task: str | None) -> list[list[float]]:
    if not texts:
        return []
    batch_size = max(1, int(getattr(args, "jina_batch_size", 32)))
    all_embeddings: list[list[float]] = []
    for start in range(0, len(texts), batch_size):
        batch = texts[start : start + batch_size]
        payload: dict[str, Any] = {
            "model": args.jina_embedding_model,
            "input": batch,
            "embedding_type": "float",
            "normalized": bool(args.jina_normalized),
        }
        task_value = clean_optional_task(task)
        if task_value:
            payload["task"] = task_value
        if args.jina_dimensions:
            payload["dimensions"] = args.jina_dimensions
        response = jina_json_post(args, "/embeddings", payload)
        all_embeddings.extend(parse_embedding_items(response, len(batch)))
    return all_embeddings


def cosine_similarity(left: list[float], right: list[float]) -> float:
    if not left or not right or len(left) != len(right):
        return 0.0
    dot = sum(a * b for a, b in zip(left, right))
    left_norm = math.sqrt(sum(a * a for a in left))
    right_norm = math.sqrt(sum(b * b for b in right))
    if left_norm == 0.0 or right_norm == 0.0:
        return 0.0
    return dot / (left_norm * right_norm)


def jina_rerank_documents(
    args: argparse.Namespace,
    query: str,
    documents: list[dict[str, Any]],
    top_n: int,
) -> list[dict[str, Any]]:
    if not documents or top_n <= 0:
        return []
    payload = {
        "model": args.jina_reranker_model,
        "query": query,
        "documents": [str(doc["text"]) for doc in documents],
        "top_n": min(top_n, len(documents)),
        "return_documents": True,
    }
    response = jina_json_post(args, "/rerank", payload)
    results = response.get("results") or response.get("data")
    if not isinstance(results, list):
        raise SystemExit("Jina rerank response did not include a results list.")

    reranked: list[dict[str, Any]] = []
    for result in results:
        if not isinstance(result, dict):
            continue
        try:
            index = int(result.get("index", len(reranked)))
        except (TypeError, ValueError):
            continue
        if not 0 <= index < len(documents):
            continue
        doc = dict(documents[index])
        score = result.get("relevance_score", result.get("score", result.get("score_float", 0.0)))
        try:
            doc["rerank_score"] = float(score)
        except (TypeError, ValueError):
            doc["rerank_score"] = 0.0
        reranked.append(doc)
    return reranked[:top_n] or documents[:top_n]


def local_retrieval_terms(text: str) -> list[str]:
    return [
        term
        for term in re.findall(r"[a-z0-9_]+", text.lower())
        if len(term) > 1 and term not in LOCAL_RETRIEVAL_STOPWORDS
    ]


def local_context_score(query_terms: set[str], query_phrases: set[str], text: str) -> float:
    terms = local_retrieval_terms(text)
    if not terms:
        return 0.0

    counts: dict[str, int] = {}
    for term in terms:
        counts[term] = counts.get(term, 0) + 1

    unique_overlap = sum(1 for term in query_terms if term in counts)
    frequency = sum(min(counts.get(term, 0), 6) for term in query_terms)
    density = frequency / max(1, len(terms))
    lower_text = text.lower()
    phrase_bonus = sum(1 for phrase in query_phrases if phrase in lower_text)
    return unique_overlap * 8.0 + frequency * 1.5 + density * 20.0 + phrase_bonus * 5.0


def retrieve_local_context(
    args: argparse.Namespace,
    query: str,
    documents: list[dict[str, Any]],
    fallback_reason: str | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    if not documents:
        return [], {"enabled": False, "strategy": "local_lexical", "reason": "no_context_documents"}

    terms = local_retrieval_terms(query)
    query_terms = set(terms)
    query_phrases = {" ".join(pair) for pair in zip(terms, terms[1:])}
    top_n = max(1, min(int(getattr(args, "jina_rerank_top_n", 5)), len(documents)))
    scored: list[tuple[float, int, dict[str, Any]]] = []
    for idx, doc in enumerate(documents):
        enriched = dict(doc)
        score = local_context_score(query_terms, query_phrases, str(doc.get("text", "")))
        enriched["local_score"] = score
        scored.append((score, idx, enriched))

    if query_terms and any(score > 0.0 for score, _, _ in scored):
        selected = [doc for _, _, doc in sorted(scored, key=lambda item: (-item[0], item[1]))[:top_n]]
    else:
        selected = []
        for _, _, doc in scored[:top_n]:
            doc["local_score"] = 0.0
            selected.append(doc)

    metadata: dict[str, Any] = {
        "enabled": True,
        "strategy": "local_lexical",
        "retrieval_mode": getattr(args, "retrieval", "auto"),
        "candidate_chunks": len(documents),
        "selected": [
            {
                "source": doc.get("source"),
                "chunk": doc.get("chunk"),
                "local_score": round(float(doc.get("local_score", 0.0)), 6),
            }
            for doc in selected
        ],
    }
    if fallback_reason:
        metadata["fallback_from"] = "jina"
        metadata["fallback_reason"] = fallback_reason[:1000]
    return selected, metadata


def chunk_text(text: str, max_chars: int, overlap: int) -> list[str]:
    text = text.strip()
    if not text:
        return []
    if max_chars <= 0 or len(text) <= max_chars:
        return [text]
    overlap = max(0, min(overlap, max_chars - 1))
    step = max(1, max_chars - overlap)
    chunks: list[str] = []
    for start in range(0, len(text), step):
        chunk = text[start : start + max_chars].strip()
        if chunk:
            chunks.append(chunk)
        if start + max_chars >= len(text):
            break
    return chunks


def add_context_chunks(
    documents: list[dict[str, Any]],
    source: str,
    text: str,
    max_chars: int,
    overlap: int,
) -> None:
    for idx, chunk in enumerate(chunk_text(text, max_chars, overlap), start=1):
        documents.append({"source": source, "chunk": idx, "text": chunk})


def collect_context_documents(args: argparse.Namespace) -> list[dict[str, Any]]:
    documents: list[dict[str, Any]] = []
    max_chars = int(getattr(args, "chunk_chars", 2400))
    overlap = int(getattr(args, "chunk_overlap", 240))

    for idx, value in enumerate(getattr(args, "context_text", []) or [], start=1):
        add_context_chunks(documents, f"--context-text[{idx}]", read_value(value), max_chars, overlap)

    for raw_path in getattr(args, "context_file", []) or []:
        path = Path(raw_path).expanduser()
        if not path.exists():
            raise SystemExit(f"Context path does not exist: {path}") from None
        paths = [path]
        if path.is_dir():
            paths = [p for p in sorted(path.rglob("*")) if p.is_file()]
        for file_path in paths:
            try:
                text = file_path.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise SystemExit(f"Could not read context file {file_path}: {exc}") from None
            add_context_chunks(documents, str(file_path), text, max_chars, overlap)
    return documents


def select_query_text(
    args: argparse.Namespace,
    inputs: dict[str, Any],
    input_fields: list[str],
    prompt_field: str | None = None,
) -> tuple[str, str]:
    if getattr(args, "jina_query", None):
        return str(args.jina_query), "--jina-query"
    query_field = getattr(args, "query_field", None)
    candidates = []
    if query_field:
        candidates.append(query_field)
    if prompt_field:
        candidates.append(prompt_field)
    candidates.extend(name for name in input_fields if name != getattr(args, "context_field", "context"))
    for name in candidates:
        value = inputs.get(name)
        if value is None:
            continue
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
        text = text.strip()
        if text:
            return text, name
    raise SystemExit(
        "Jina retrieval needs a query. Pass a prompt, set --query-field to an existing input, or pass --jina-query."
    ) from None


def format_context_documents(documents: list[dict[str, Any]]) -> list[str]:
    formatted = []
    for rank, doc in enumerate(documents, start=1):
        scores = []
        if "embedding_score" in doc:
            scores.append(f"embedding={float(doc['embedding_score']):.4f}")
        if "rerank_score" in doc:
            scores.append(f"rerank={float(doc['rerank_score']):.4f}")
        if "local_score" in doc:
            scores.append(f"local={float(doc['local_score']):.4f}")
        suffix = f" {' '.join(scores)}" if scores else ""
        formatted.append(f"[{rank}] source={doc.get('source')} chunk={doc.get('chunk')}{suffix}\n{doc['text']}")
    return formatted


def retrieve_jina_context(
    args: argparse.Namespace,
    query: str,
    documents: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    if not documents:
        return [], {"enabled": False, "reason": "no_context_documents"}

    retrieval_mode = getattr(args, "retrieval", "auto")
    if retrieval_mode == "local":
        return retrieve_local_context(args, query, documents)

    api_key = resolve_jina_api_key(args, required=retrieval_mode == "jina")
    if not api_key:
        return retrieve_local_context(args, query, documents, "JINA_API_KEY was not set")

    try:
        query_embedding = jina_embed_texts(args, [query], args.jina_query_task)[0]
        document_embeddings = jina_embed_texts(args, [str(doc["text"]) for doc in documents], args.jina_document_task)
    except SystemExit as exc:
        if retrieval_mode == "jina":
            raise
        return retrieve_local_context(args, query, documents, str(exc))

    scored = []
    for doc, embedding in zip(documents, document_embeddings):
        enriched = dict(doc)
        enriched["embedding_score"] = cosine_similarity(query_embedding, embedding)
        scored.append(enriched)

    embed_top_k = max(1, min(int(args.jina_embed_top_k), len(scored)))
    candidates = sorted(scored, key=lambda doc: float(doc["embedding_score"]), reverse=True)[:embed_top_k]
    final_top_n = max(1, min(int(args.jina_rerank_top_n), len(candidates)))
    try:
        if args.jina_rerank:
            selected = jina_rerank_documents(args, query, candidates, final_top_n)
            strategy = "embedding_then_rerank"
        else:
            selected = candidates[:final_top_n]
            strategy = "embedding_only"
    except SystemExit as exc:
        if retrieval_mode == "jina":
            raise
        return retrieve_local_context(args, query, documents, str(exc))

    metadata = {
        "enabled": True,
        "strategy": strategy,
        "retrieval_mode": retrieval_mode,
        "api_key_source": "JINA_API_KEY" if not getattr(args, "jina_api_key", None) else "--jina-api-key",
        "embedding_model": args.jina_embedding_model,
        "reranker_model": args.jina_reranker_model if args.jina_rerank else None,
        "query_task": clean_optional_task(args.jina_query_task),
        "document_task": clean_optional_task(args.jina_document_task),
        "candidate_chunks": len(documents),
        "embedded_top_k": embed_top_k,
        "selected": [
            {
                "source": doc.get("source"),
                "chunk": doc.get("chunk"),
                "embedding_score": round(float(doc.get("embedding_score", 0.0)), 6),
                "rerank_score": round(float(doc["rerank_score"]), 6) if "rerank_score" in doc else None,
            }
            for doc in selected
        ],
    }
    return selected, metadata


def enrich_inputs_with_jina_context(
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    input_fields: list[str],
    inputs: dict[str, Any],
    prompt_field: str | None,
) -> dict[str, Any] | None:
    documents = collect_context_documents(args)
    if not documents:
        return None
    context_field = args.context_field
    if context_field not in input_fields:
        parser.error(
            f"--context-file/--context-text needs signature input {context_field!r}; "
            "change --context-field or include it in --signature"
        )
    query, query_source = select_query_text(args, inputs, input_fields, prompt_field)
    selected, metadata = retrieve_jina_context(args, query, documents)
    context_values = format_context_documents(selected)
    existing = inputs.get(context_field)
    if existing:
        if isinstance(existing, list):
            context_values = [str(item) for item in existing] + context_values
        else:
            context_values = [str(existing)] + context_values
    inputs[context_field] = context_values
    metadata["context_field"] = context_field
    metadata["query_source"] = query_source
    return metadata


def read_prompt(parts: list[str], prompt_text: str | None = None) -> str:
    if prompt_text is not None:
        return prompt_text.strip()
    if parts:
        return " ".join(parts).strip()
    if not sys.stdin.isatty():
        return sys.stdin.read().strip()
    return ""


def gateway_models_url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/models"


def preflight_gateway(base_url: str, timeout: float, api_key: str) -> None:
    """Fail early with a dsco-specific hint when the local gateway is offline."""
    parsed = urlparse(base_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise SystemExit(f"Invalid --base-url: {base_url!r}")

    request = urllib.request.Request(
        gateway_models_url(base_url),
        headers={"Authorization": f"Bearer {api_key}", "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            # Drain a small response so connection errors surface before DSPy runs.
            response.read(1024)
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            raise SystemExit(
                f"Could not authenticate to the dsco OpenAI gateway at {base_url}.\n"
                "Start the gateway with a known token, for example:\n"
                "  DSCO_WEB_TOKEN=dsco ./dsco --ui 3141\n"
                "Then retry with:\n"
                "  --gateway-api-key dsco\n"
                "If you use a different DSCO_WEB_TOKEN, pass the same value via --gateway-api-key."
            ) from None
        # A live OpenAI-compatible endpoint may reject /models but still accept
        # /chat/completions. The important preflight is "server is reachable".
        return
    except (urllib.error.URLError, TimeoutError, socket.timeout, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        raise SystemExit(
            f"Could not reach the dsco OpenAI gateway at {base_url} ({reason}).\n"
            "Start it from the repo root with:\n"
            "  ./dsco --ui 3141\n"
            f"Then retry:\n"
            f"  uv run examples/dspy_via_dsco.py --base-url {DEFAULT_BASE_URL} \"your prompt\""
        ) from None


def prediction_to_dict(prediction, output_fields: list[str]) -> dict[str, object]:
    values = {}
    for name in output_fields:
        if hasattr(prediction, name):
            values[name] = getattr(prediction, name)
    if values:
        return values
    if hasattr(prediction, "toDict"):
        return dict(prediction.toDict())
    if hasattr(prediction, "items"):
        return dict(prediction.items())
    return {"output": str(prediction)}


def stream_listeners_for_outputs(dspy, output_fields: list[str]):
    streaming = getattr(dspy, "streaming", None)
    listener_cls = getattr(streaming, "StreamListener", None) if streaming else None
    if listener_cls is None:
        raise SystemExit(
            "This DSPy install does not expose dspy.streaming.StreamListener; "
            "upgrade DSPy or run without --stream."
        ) from None
    return [listener_cls(signature_field_name=name) for name in output_fields]


def stream_response_parts(dspy, value: object) -> tuple[str, str] | None:
    streaming = getattr(dspy, "streaming", None)
    response_cls = getattr(streaming, "StreamResponse", None) if streaming else None
    if response_cls is not None and isinstance(value, response_cls):
        return str(value.signature_field_name), str(value.chunk)
    if hasattr(value, "signature_field_name") and hasattr(value, "chunk"):
        return str(getattr(value, "signature_field_name")), str(getattr(value, "chunk"))
    return None


def stream_status_text(dspy, value: object) -> str | None:
    streaming = getattr(dspy, "streaming", None)
    status_cls = getattr(streaming, "StatusMessage", None) if streaming else None
    if status_cls is not None and isinstance(value, status_cls):
        text = getattr(value, "message", None) or getattr(value, "status", None)
        return str(text if text is not None else value)
    return None


def is_dspy_prediction(dspy, value: object) -> bool:
    prediction_cls = getattr(dspy, "Prediction", None)
    return prediction_cls is not None and isinstance(value, prediction_cls)


async def consume_streamified_prediction(
    dspy,
    stream_program,
    inputs: dict[str, Any],
    output_fields: list[str],
    json_mode: bool,
    retrieval_metadata: dict[str, Any] | None,
) -> dict[str, object]:
    final_prediction = None
    chunks_seen = False
    current_field = None
    streamed_values: dict[str, str] = {}

    async for value in stream_program(**inputs):
        parts = stream_response_parts(dspy, value)
        if parts is not None:
            field_name, chunk = parts
            chunks_seen = True
            streamed_values[field_name] = streamed_values.get(field_name, "") + chunk
            if json_mode:
                print(
                    json.dumps({"type": "chunk", "field": field_name, "chunk": chunk}, ensure_ascii=False),
                    flush=True,
                )
            elif len(output_fields) == 1:
                print(chunk, end="", flush=True)
            else:
                if current_field != field_name:
                    if current_field is not None:
                        print()
                    print(f"[{field_name}] ", end="", flush=True)
                    current_field = field_name
                print(chunk, end="", flush=True)
            continue

        if is_dspy_prediction(dspy, value):
            final_prediction = value
            continue

        status = stream_status_text(dspy, value)
        if status:
            if json_mode:
                print(json.dumps({"type": "status", "message": status}, ensure_ascii=False), flush=True)
            else:
                print(status, file=sys.stderr, flush=True)

    if final_prediction is None and not streamed_values:
        raise SystemExit("DSPy stream ended without a final Prediction.") from None

    values = (
        prediction_to_dict(final_prediction, output_fields)
        if final_prediction is not None
        else {name: streamed_values[name] for name in output_fields if name in streamed_values}
    )
    if json_mode:
        payload: dict[str, Any] = {"type": "final", "prediction": values}
        if retrieval_metadata:
            payload["retrieval"] = retrieval_metadata
        print(json.dumps(payload, ensure_ascii=False), flush=True)
    elif not chunks_seen:
        if len(values) != 1:
            print(json.dumps(values, ensure_ascii=False, indent=2))
        else:
            print(next(iter(values.values())))
    else:
        print()
    return values


def run_streamified_prediction(
    dspy,
    program,
    inputs: dict[str, Any],
    output_fields: list[str],
    args: argparse.Namespace,
    retrieval_metadata: dict[str, Any] | None,
) -> dict[str, object]:
    streamify = getattr(dspy, "streamify", None)
    if streamify is None:
        raise SystemExit(
            "This DSPy install does not expose dspy.streamify; upgrade DSPy or run without --stream."
        ) from None
    listeners = stream_listeners_for_outputs(dspy, output_fields)
    try:
        stream_program = streamify(
            program,
            stream_listeners=listeners,
            include_final_prediction_in_output_stream=True,
        )
    except TypeError:
        stream_program = streamify(program, stream_listeners=listeners)
    return asyncio.run(
        consume_streamified_prediction(
            dspy,
            stream_program,
            inputs,
            output_fields,
            args.json,
            retrieval_metadata,
        )
    )


def estimate_tokens(text: str) -> int:
    # Cheap, provider-independent heuristic. Real billing should come from the
    # gateway/provider ledger; this is for pre-call budget gating.
    return max(1, (len(text or "") + 3) // 4)


def model_price_for(model: str) -> tuple[float, float]:
    normalized = canonical_model(model).lower().replace("_", "-")
    if normalized.startswith("openai/"):
        normalized = normalized[len("openai/") :]
    for key, price in MODEL_PRICE_PER_MTOK.items():
        if key in normalized:
            return price
    return (0.0, 0.0)


def parse_json_payload(text: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Invalid JSON task payload: {exc}") from None


def normalize_task_payload(payload: Any) -> list[dict[str, Any]]:
    if isinstance(payload, dict) and "tasks" in payload:
        payload = payload["tasks"]
    if isinstance(payload, str):
        payload = [payload]
    if not isinstance(payload, list):
        raise SystemExit("Agent tasks JSON must be a list, a string, or an object with a 'tasks' list.")

    tasks: list[dict[str, Any]] = []
    for item in payload:
        if isinstance(item, str):
            text = item.strip()
            if text:
                tasks.append({"task": text})
            continue
        if isinstance(item, dict):
            text = str(item.get("task") or item.get("prompt") or item.get("description") or "").strip()
            if not text:
                raise SystemExit(f"Agent task object is missing task/prompt/description: {item!r}")
            task_obj = dict(item)
            task_obj["task"] = text
            tasks.append(task_obj)
            continue
        raise SystemExit(f"Unsupported agent task item: {item!r}")
    return tasks


def read_agent_tasks(args: argparse.Namespace) -> list[AgentTask]:
    raw_items: list[dict[str, Any]] = []

    if args.tasks_json:
        if args.tasks_json == "-":
            text = sys.stdin.read()
        else:
            with open(args.tasks_json, "r", encoding="utf-8") as f:
                text = f.read()
        raw_items.extend(normalize_task_payload(parse_json_payload(text)))

    for task_text in args.task:
        raw_items.append({"task": task_text})

    prompt = read_prompt(args.prompt, args.prompt_text)
    if prompt:
        stripped = prompt.strip()
        if not raw_items and stripped[:1] in "[{":
            raw_items.extend(normalize_task_payload(parse_json_payload(stripped)))
        else:
            raw_items.append({"task": stripped})

    tasks: list[AgentTask] = []
    for idx, item in enumerate(raw_items, start=1):
        metadata = {k: v for k, v in item.items() if k not in {"id", "task", "context", "parent_id", "depth"}}
        tasks.append(
            AgentTask(
                id=str(item.get("id") or f"t{idx}"),
                task=str(item["task"]),
                context=str(item.get("context") or ""),
                parent_id=str(item["parent_id"]) if item.get("parent_id") is not None else None,
                depth=int(item.get("depth") or 0),
                metadata=metadata,
            )
        )
    return tasks


def strip_code_fence(text: str) -> str:
    s = text.strip()
    if s.startswith("```"):
        lines = s.splitlines()
        if lines:
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        s = "\n".join(lines).strip()
    return s


def parse_new_tasks(value: object) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    if isinstance(value, dict):
        return normalize_new_task_objects(value.get("tasks") or value.get("new_tasks") or [])

    text = strip_code_fence(str(value))
    if not text or text.lower() in {"none", "n/a", "null"}:
        return []
    try:
        parsed = json.loads(text)
        return normalize_new_task_objects(parsed)
    except json.JSONDecodeError:
        pass

    tasks: list[str] = []
    for line in text.splitlines():
        cleaned = line.strip()
        while cleaned[:1] in {"-", "*", "•"}:
            cleaned = cleaned[1:].strip()
        if len(cleaned) > 2 and cleaned[0].isdigit() and cleaned[1] in {".", ")"}:
            cleaned = cleaned[2:].strip()
        if cleaned:
            tasks.append(cleaned)
    return tasks


def normalize_new_task_objects(value: object) -> list[str]:
    if isinstance(value, str):
        return parse_new_tasks(value)
    if isinstance(value, list):
        out = []
        for item in value:
            if isinstance(item, dict):
                text = str(item.get("task") or item.get("prompt") or item.get("description") or "").strip()
            else:
                text = str(item).strip()
            if text:
                out.append(text)
        return out
    return []


def confidence_score(value: object) -> float:
    if value is None:
        return 0.0
    text = str(value).strip().rstrip("%")
    try:
        score = float(text)
    except ValueError:
        return 0.0
    if score > 1.0:
        score /= 100.0
    return max(0.0, min(1.0, score))


def primary_result(values: dict[str, object]) -> str:
    for key in ("result", "answer", "output", "summary"):
        if key in values and str(values[key]).strip():
            return str(values[key]).strip()
    for key, value in values.items():
        if key not in {"confidence", "new_tasks"} and str(value).strip():
            return str(value).strip()
    return ""


def build_agent_context(args: argparse.Namespace, task: AgentTask, ledger: CostLedger) -> str:
    parts = [
        args.agent_context,
        f"Task id: {task.id}",
        f"Task depth: {task.depth}",
        f"Estimated spend so far: ${ledger.spent_usd:.6f}",
    ]
    if args.budget_usd is not None and args.budget_usd > 0:
        parts.append(f"Budget cap: ${args.budget_usd:.6f}")
    if task.context:
        parts.append(f"Task context: {task.context}")
    if task.metadata:
        parts.append(f"Task metadata: {json.dumps(task.metadata, ensure_ascii=False, sort_keys=True)}")
    return "\n".join(parts)


def agent_inputs_for_signature(input_fields: list[str], task: AgentTask, context: str) -> dict[str, str]:
    if not input_fields:
        return {}
    inputs: dict[str, str] = {}
    if "task" in input_fields:
        inputs["task"] = task.task
    else:
        inputs[input_fields[0]] = task.task
    if "context" in input_fields:
        inputs["context"] = context
    elif len(input_fields) > 1:
        inputs[input_fields[1]] = context
    for field_name in input_fields:
        inputs.setdefault(field_name, "")
    return inputs


def sample_sort_key(sample: dict[str, Any]) -> tuple[float, int, int]:
    result = str(sample.get("result") or "")
    return (float(sample.get("confidence") or 0.0), 1 if result else 0, -len(result))


def make_lm(
    dspy,
    args: argparse.Namespace,
    model_override: str | None = None,
    temperature_override: float | None = None,
    max_tokens_override: int | None = None,
):
    model = canonical_model(
        model_override or args.model,
        None if model_override else getattr(args, "model_role", None),
    )
    card = ANTHROPIC_MODEL_CARDS.get(model)
    lm_kwargs: dict[str, Any] = {
        "model": f"openai/{model}",
        "api_base": args.base_url,
        "api_key": args.gateway_api_key,
        "cache": True,
        "extra_body": prompt_cache_extra_body(args, model),
    }
    max_tokens = (
        max_tokens_override
        if max_tokens_override is not None
        else args.max_output_tokens if getattr(args, "agent_loop", False) else getattr(args, "max_tokens", None)
    )
    if max_tokens:
        if card:
            max_tokens = min(max_tokens, int(card["max_output"]))
        lm_kwargs["max_tokens"] = max_tokens

    # The newest Fable/Opus/Sonnet cards are sampling-locked in the native
    # Anthropic surface. Through dsco's OpenAI-compatible gateway, omit
    # temperature unless the model profile says sampling is safe. Haiku remains
    # tunable for bulk sampling and agent-loop candidate generation.
    temperature = temperature_override if temperature_override is not None else getattr(args, "temperature", None)
    if temperature is not None and not (card and card.get("sampling_locked")):
        lm_kwargs["temperature"] = temperature

    effort = getattr(args, "reasoning_effort", None)
    if effort and card and effort not in card["efforts"]:
        raise SystemExit(f"{model} does not allow reasoning effort {effort!r}; choose {card['efforts']}")
    if effort:
        lm_kwargs["reasoning_effort"] = effort
        lm_kwargs["allowed_openai_params"] = ["reasoning_effort"]
    return dspy.LM(**lm_kwargs)


def build_dspy_program(dspy, signature: Any, kind: str | None):
    program_kind = (kind or "predict").replace("_", "-")
    if program_kind == "predict":
        return dspy.Predict(signature)
    if program_kind in {"cot", "chain-of-thought"}:
        return dspy.ChainOfThought(signature)
    if program_kind in {"pot", "program-of-thought"}:
        program_of_thought = getattr(dspy, "ProgramOfThought", None)
        if program_of_thought is None:
            raise SystemExit("This DSPy install does not expose dspy.ProgramOfThought.") from None
        return program_of_thought(signature)
    raise SystemExit(f"Unsupported --program-kind {program_kind!r}") from None


def load_jsonl_records(path: str, limit: int | None = None) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    try:
        f = open(Path(path).expanduser(), "r", encoding="utf-8")
    except OSError as exc:
        raise SystemExit(f"Could not read JSONL file {path}: {exc}") from None
    with f:
        for line_num, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{line_num}: invalid JSONL row: {exc}") from None
            if not isinstance(item, dict):
                raise SystemExit(f"{path}:{line_num}: each JSONL row must be an object") from None
            records.append(dict(item))
            if limit is not None and limit > 0 and len(records) >= limit:
                break
    return records


def write_jsonl_records(path: str, records: list[dict[str, Any]]) -> None:
    output_path = Path(path).expanduser()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output_path.open("w", encoding="utf-8") as f:
            for record in records:
                f.write(json.dumps(record, ensure_ascii=False) + "\n")
    except OSError as exc:
        raise SystemExit(f"Could not write JSONL file {output_path}: {exc}") from None


def redact_sensitive_text(text: str) -> str:
    patterns = [
        (r"(?i)(authorization\s*:\s*bearer\s+)[^\s'\"]+", r"\1[REDACTED]"),
        (r"(?i)((?:api[_-]?key|token|secret|password)\s*[:=]\s*)[^\s'\",}]+", r"\1[REDACTED]"),
        (r"sk-[A-Za-z0-9_\-]{20,}", "[REDACTED_API_KEY]"),
        (r"sk-ant-[A-Za-z0-9_\-]{20,}", "[REDACTED_API_KEY]"),
    ]
    redacted = text
    for pattern, replacement in patterns:
        redacted = re.sub(pattern, replacement, redacted)
    return redacted


def truncate_middle(text: str, limit: int) -> str:
    if limit <= 0 or len(text) <= limit:
        return text
    head = max(1, limit // 2)
    tail = max(1, limit - head)
    return text[:head].rstrip() + "\n\n[... session content truncated ...]\n\n" + text[-tail:].lstrip()


def read_optional_text(path: Path, limit: int = 80_000) -> str:
    if not path.exists() or not path.is_file():
        return ""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return truncate_middle(text, limit)


def read_optional_json(path: Path) -> Any:
    text = read_optional_text(path)
    if not text.strip():
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def session_run_dirs(args: argparse.Namespace) -> list[Path]:
    explicit = [Path(item).expanduser() for item in getattr(args, "session_path", [])]
    dirs = [path for path in explicit if path.exists()]
    root = Path(getattr(args, "session_root", ".swarm/runs")).expanduser()
    if root.exists():
        dirs.extend(path for path in root.iterdir() if path.is_dir())
    unique: dict[str, Path] = {}
    for path in dirs:
        unique[str(path.resolve())] = path
    return sorted(unique.values(), key=lambda path: path.stat().st_mtime, reverse=True)


def has_substantive_session_output(manifest: dict[str, Any], coordinator: str, transcript: str) -> bool:
    if int(manifest.get("completed_workers") or 0) > 0 or int(manifest.get("failed_workers") or 0) > 0:
        return True
    if coordinator.strip():
        return True
    for match in re.finditer(r"```(?:text)?\s*(.*?)```", transcript, flags=re.DOTALL | re.IGNORECASE):
        if match.group(1).strip():
            return True
    return False


def collect_session_distill_records(args: argparse.Namespace, input_field: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    limit = max(1, args.session_limit)
    for run_dir in session_run_dirs(args):
        manifest = read_optional_json(run_dir / "manifest.json") or {}
        metrics = read_optional_json(run_dir / "metrics.json")
        claims = read_optional_json(run_dir / "claims.json")
        coordinator = read_optional_text(run_dir / "coordinator.md")
        transcript = read_optional_text(run_dir / "transcript.md")
        run_id = str(manifest.get("run_id") or run_dir.name)
        status = str(manifest.get("status") or "")
        user_prompt = str(manifest.get("user_prompt") or manifest.get("name") or run_dir.name)
        if not has_substantive_session_output(manifest, coordinator, transcript):
            continue

        body_parts = [
            f"Run: {run_id}",
            f"Status: {status}",
            f"User prompt: {user_prompt}",
            "Distillation task: Review this dsco session and extract compact behavior that a Haiku 4.5 DSPy program can reuse. Focus on operational decisions, failure modes, guardrails, commands, and reusable judgment. Ignore empty worker scaffolding.",
        ]
        if manifest:
            body_parts.append("Manifest:\n" + json.dumps(manifest, ensure_ascii=False, indent=2))
        if metrics:
            body_parts.append("Metrics:\n" + json.dumps(metrics, ensure_ascii=False, indent=2))
        if claims:
            body_parts.append("Claims:\n" + json.dumps(claims, ensure_ascii=False, indent=2))
        if coordinator.strip():
            body_parts.append("Coordinator:\n" + coordinator)
        if transcript.strip():
            body_parts.append("Transcript:\n" + transcript)

        context = redact_sensitive_text("\n\n".join(body_parts).strip())
        if len(context) < args.session_min_chars:
            continue
        record = {
            input_field: truncate_middle(context, args.session_char_limit),
            "session_id": run_id,
            "session_path": str(run_dir),
            "session_status": status,
            "session_prompt": user_prompt,
        }
        records.append(record)
        if len(records) >= limit:
            break
    return records


def build_teacher_program(dspy, args: argparse.Namespace, signature: Any, kind: str | None):
    teacher_model = getattr(args, "teacher_model", None)
    if not teacher_model:
        return None
    teacher_lm = make_lm(dspy, args, model_override=teacher_model)
    teacher_program = build_dspy_program(dspy, signature, kind)
    if hasattr(teacher_program, "set_lm"):
        teacher_program.set_lm(teacher_lm)
    return teacher_program


def label_records_with_teacher(
    dspy,
    args: argparse.Namespace,
    teacher_program,
    records: list[dict[str, Any]],
    input_fields: list[str],
    output_fields: list[str],
) -> list[dict[str, Any]]:
    labeled: list[dict[str, Any]] = []
    for idx, record in enumerate(records, start=1):
        inputs = {field_name: record[field_name] for field_name in input_fields if field_name in record}
        missing = [field_name for field_name in input_fields if field_name not in inputs]
        if missing:
            raise SystemExit(f"session distill record {idx} missing input field(s): {', '.join(missing)}") from None
        try:
            prediction = teacher_program(**inputs)
        except Exception as exc:
            if args.debug:
                raise
            raise SystemExit(
                f"Fable teacher failed while labeling session record {idx}.\n"
                f"{type(exc).__name__}: {exc}\n"
                "Use --debug for the full DSPy/LiteLLM traceback."
            ) from None
        values = prediction_to_dict(prediction, output_fields)
        missing_outputs = [field_name for field_name in output_fields if field_name not in values]
        if missing_outputs:
            raise SystemExit(
                f"Fable teacher output for session record {idx} missed field(s): {', '.join(missing_outputs)}"
            ) from None
        labeled_record = dict(record)
        for field_name in output_fields:
            labeled_record[field_name] = values[field_name]
        labeled.append(labeled_record)
    return labeled


def prediction_field(obj: object, name: str) -> Any:
    if hasattr(obj, name):
        return getattr(obj, name)
    if isinstance(obj, dict):
        return obj.get(name)
    if hasattr(obj, "get"):
        try:
            return obj.get(name)
        except Exception:
            return None
    return None


def normalized_label(value: Any) -> str:
    text = "" if value is None else str(value)
    return " ".join(text.strip().lower().split())


def validate_training_records(
    records: list[dict[str, Any]],
    input_fields: list[str],
    output_fields: list[str],
    split_name: str,
) -> None:
    if not records:
        raise SystemExit(f"{split_name} is empty.") from None
    needed = set(input_fields) | set(output_fields)
    for idx, record in enumerate(records, start=1):
        missing = [field_name for field_name in needed if field_name not in record]
        if missing:
            raise SystemExit(
                f"{split_name} row {idx} is missing field(s): {', '.join(missing)}. "
                "JSONL keys must match --signature fields."
            ) from None


def records_to_examples(dspy, records: list[dict[str, Any]], input_fields: list[str]):
    return [dspy.Example(**record).with_inputs(*input_fields) for record in records]


def enrich_records_with_jina_context(
    args: argparse.Namespace,
    input_fields: list[str],
    records: list[dict[str, Any]],
    documents: list[dict[str, Any]],
    split_name: str,
) -> dict[str, Any] | None:
    if not documents:
        return None
    context_field = args.context_field
    if context_field not in input_fields:
        raise SystemExit(
            f"Training context enrichment needs signature input {context_field!r}; "
            "change --context-field or include it in --signature."
        ) from None

    first_selection: list[dict[str, Any]] = []
    first_query_source = ""
    for idx, record in enumerate(records, start=1):
        inputs = {field_name: record[field_name] for field_name in input_fields if field_name in record}
        try:
            query, query_source = select_query_text(args, inputs, input_fields, None)
        except SystemExit as exc:
            raise SystemExit(f"{split_name} row {idx}: {exc}") from None
        selected, metadata = retrieve_jina_context(args, query, documents)
        record[context_field] = format_context_documents(selected)
        if idx == 1:
            first_query_source = query_source
            first_selection = metadata.get("selected", [])

    return {
        "enabled": True,
        "split": split_name,
        "context_field": context_field,
        "records_enriched": len(records),
        "candidate_chunks": len(documents),
        "first_query_source": first_query_source,
        "first_selection": first_selection,
        "embedding_model": args.jina_embedding_model,
        "reranker_model": args.jina_reranker_model if args.jina_rerank else None,
    }


def make_training_metric(dspy, args: argparse.Namespace, output_fields: list[str]):
    label_field = args.label_field or output_fields[0]
    prediction_name = args.metric_field or label_field

    if args.metric == "answer-exact-match":
        metric = getattr(getattr(dspy, "evaluate", object()), "answer_exact_match", None)
        if metric is None:
            raise SystemExit("This DSPy install does not expose dspy.evaluate.answer_exact_match.") from None
        return metric
    if args.metric == "semantic-f1":
        semantic_metric = dspy.SemanticF1()

        def semantic_f1_metric(example, pred, trace=None):
            try:
                return semantic_metric(example, pred, trace=trace)
            except TypeError:
                return semantic_metric(example, pred)

        return semantic_f1_metric

    def exact_match_metric(example, pred, trace=None):
        expected = prediction_field(example, label_field)
        actual = prediction_field(pred, prediction_name)
        return normalized_label(expected) == normalized_label(actual)

    return exact_match_metric


def score_to_float(score: Any) -> float:
    if isinstance(score, bool):
        return 1.0 if score else 0.0
    if isinstance(score, dict) and "score" in score:
        return score_to_float(score["score"])
    try:
        return float(score)
    except (TypeError, ValueError):
        return 1.0 if score else 0.0


def make_gepa_feedback_metric(base_metric, args: argparse.Namespace, output_fields: list[str]):
    label_field = args.label_field or output_fields[0]
    prediction_name = args.metric_field or label_field
    try:
        from dspy.teleprompt.gepa.gepa_utils import ScoreWithFeedback
    except Exception:
        ScoreWithFeedback = None

    def gepa_metric(gold, pred, trace=None, pred_name=None, pred_trace=None):
        try:
            score = base_metric(gold, pred, trace)
        except TypeError:
            score = base_metric(gold, pred)
        if hasattr(score, "score") and hasattr(score, "feedback"):
            return score
        if isinstance(score, dict) and "feedback" in score:
            numeric_score = max(0.0, min(1.0, score_to_float(score)))
            if ScoreWithFeedback is not None:
                return ScoreWithFeedback(score=numeric_score, feedback=str(score["feedback"]))
            return {"score": numeric_score, "feedback": str(score["feedback"])}
        numeric_score = max(0.0, min(1.0, score_to_float(score)))
        expected = prediction_field(gold, label_field)
        actual = prediction_field(pred, prediction_name)
        target = f" for predictor {pred_name}" if pred_name else ""
        if normalized_label(expected) == normalized_label(actual):
            feedback = f"score={numeric_score:.3f}{target}; prediction matched {label_field}."
        else:
            feedback = (
                f"score={numeric_score:.3f}{target}; expected {label_field}={expected!r}, "
                f"got {prediction_name}={actual!r}."
            )
        if ScoreWithFeedback is not None:
            return ScoreWithFeedback(score=numeric_score, feedback=feedback)
        return {"score": numeric_score, "feedback": feedback}

    return gepa_metric


def gepa_budget_args(args: argparse.Namespace) -> dict[str, Any]:
    if args.gepa_max_metric_calls is not None and args.gepa_max_full_evals is not None:
        raise SystemExit("Choose only one GEPA budget: --gepa-max-metric-calls or --gepa-max-full-evals.") from None
    if args.gepa_max_metric_calls is not None:
        return {"max_metric_calls": args.gepa_max_metric_calls}
    if args.gepa_max_full_evals is not None:
        return {"max_full_evals": args.gepa_max_full_evals}
    return {"auto": args.gepa_auto}


def compile_with_optimizer(
    dspy,
    args: argparse.Namespace,
    program,
    trainset,
    devset,
    metric,
    output_fields: list[str],
    teacher=None,
):
    optimizer_name = normalized_optimizer_name(args.optimizer)
    if optimizer_name == "labeled":
        optimizer = dspy.LabeledFewShot(k=args.max_labeled_demos)
        return optimizer.compile(student=program, trainset=trainset)

    if optimizer_name == "bootstrap":
        optimizer = dspy.BootstrapFewShot(
            metric=metric,
            metric_threshold=args.metric_threshold,
            max_bootstrapped_demos=args.max_bootstrapped_demos,
            max_labeled_demos=args.max_labeled_demos,
            max_rounds=args.max_rounds,
            max_errors=args.max_errors,
        )
        return optimizer.compile(student=program, teacher=teacher, trainset=trainset)

    if optimizer_name == "bootstrap-rs":
        optimizer = dspy.BootstrapFewShotWithRandomSearch(
            metric=metric,
            metric_threshold=args.metric_threshold,
            max_bootstrapped_demos=args.max_bootstrapped_demos,
            max_labeled_demos=args.max_labeled_demos,
            num_candidate_programs=args.num_candidate_programs,
            num_threads=args.num_threads,
            max_errors=args.max_errors,
        )
        return optimizer.compile(student=program, teacher=teacher, trainset=trainset, valset=devset or trainset)

    if optimizer_name == "mipro":
        optimizer = dspy.MIPROv2(
            metric=metric,
            auto=args.optimizer_auto,
            num_threads=args.num_threads,
            max_errors=args.max_errors,
            max_bootstrapped_demos=args.max_bootstrapped_demos,
            max_labeled_demos=args.max_labeled_demos,
        )
        try:
            return optimizer.compile(student=program, trainset=trainset, valset=devset)
        except TypeError:
            return optimizer.compile(program, trainset=trainset, valset=devset)

    if optimizer_name == "gepa":
        gepa_cls = getattr(dspy, "GEPA", None)
        if gepa_cls is None:
            raise SystemExit(
                "This DSPy install does not expose dspy.GEPA. Run with the script dependency resolver "
                "(`uv run examples/dspy_via_dsco.py ...`) so the gepa package is installed."
            ) from None
        reflection_model = canonical_model(args.gepa_reflection_model or args.model)
        reflection_lm = make_lm(
            dspy,
            args,
            model_override=reflection_model,
            temperature_override=args.gepa_reflection_temperature,
            max_tokens_override=args.gepa_reflection_max_tokens,
        )
        optimizer = gepa_cls(
            metric=make_gepa_feedback_metric(metric, args, output_fields),
            reflection_lm=reflection_lm,
            num_threads=args.num_threads,
            log_dir=args.gepa_log_dir,
            track_stats=args.gepa_track_stats,
            **gepa_budget_args(args),
        )
        return optimizer.compile(student=program, trainset=trainset, valset=devset or trainset)

    raise SystemExit(f"Unsupported --optimizer {args.optimizer!r}") from None


def run_training(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if not args.trainset_jsonl:
        parser.error("--train requires --trainset-jsonl")

    input_fields, output_fields = signature_io_fields(args.signature)
    train_records = load_jsonl_records(args.trainset_jsonl, args.train_limit)
    dev_records = load_jsonl_records(args.devset_jsonl, args.dev_limit) if args.devset_jsonl else []
    context_documents = collect_context_documents(args)
    jina_plan = {
        "enabled": bool(context_documents),
        "retrieval": args.retrieval,
        "context_field": args.context_field,
        "candidate_chunks": len(context_documents),
        "embedding_model": args.jina_embedding_model,
        "reranker_model": args.jina_reranker_model if args.jina_rerank else None,
    }

    if args.dry_run:
        output = {
            "ok": True,
            "dry_run": True,
            "mode": "train",
            "app": args.app,
            "signature": args.signature,
            "program_kind": args.program_kind or "cot",
            "optimizer": args.optimizer,
            "optimizer_auto": args.optimizer_auto if args.optimizer == "mipro" else None,
            "gepa_budget": gepa_budget_args(args) if args.optimizer == "gepa" else None,
            "gepa_reflection_model": canonical_model(args.gepa_reflection_model or args.model) if args.optimizer == "gepa" else None,
            "metric": args.metric,
            "metric_threshold": args.metric_threshold,
            "input_fields": input_fields,
            "output_fields": output_fields,
            "train_rows": len(train_records),
            "dev_rows": len(dev_records),
            "jina": jina_plan,
            "prompt_cache": prompt_cache_plan(args, args.model),
            "save_optimized": args.save_optimized,
            "model": args.model,
            "teacher_model": canonical_model(args.teacher_model) if args.teacher_model else None,
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return

    train_jina = enrich_records_with_jina_context(args, input_fields, train_records, context_documents, "trainset")
    dev_jina = (
        enrich_records_with_jina_context(args, input_fields, dev_records, context_documents, "devset")
        if dev_records
        else None
    )
    validate_training_records(train_records, input_fields, output_fields, "trainset")
    if dev_records:
        validate_training_records(dev_records, input_fields, output_fields, "devset")

    preflight_gateway(args.base_url, args.preflight_timeout, args.gateway_api_key)
    dspy = import_dspy()
    configure_dspy_cache(dspy)
    student_lm = make_lm(dspy, args)
    dspy.configure(lm=student_lm)
    program = build_dspy_program(dspy, args.signature, args.program_kind or "cot")
    if hasattr(program, "set_lm"):
        program.set_lm(student_lm)
    teacher = build_teacher_program(dspy, args, args.signature, args.program_kind or "cot")
    trainset = records_to_examples(dspy, train_records, input_fields)
    devset = records_to_examples(dspy, dev_records, input_fields) if dev_records else None
    metric = make_training_metric(dspy, args, output_fields)
    optimized = compile_with_optimizer(dspy, args, program, trainset, devset, metric, output_fields, teacher=teacher)

    save_path = Path(args.save_optimized).expanduser()
    save_path.parent.mkdir(parents=True, exist_ok=True)
    optimized.save(str(save_path))

    output = {
        "ok": True,
        "mode": "train",
        "signature": args.signature,
        "program_kind": args.program_kind or "cot",
        "optimizer": args.optimizer,
        "metric": args.metric,
        "train_rows": len(train_records),
        "dev_rows": len(dev_records),
        "jina": {
            "enabled": bool(context_documents),
            "trainset": train_jina,
            "devset": dev_jina,
        },
        "saved": str(save_path),
        "model": args.model,
        "teacher_model": canonical_model(args.teacher_model) if args.teacher_model else None,
    }
    if args.json:
        print(json.dumps(output, ensure_ascii=False, indent=2))
    else:
        print(f"optimized_program={save_path}")
        print(f"optimizer={args.optimizer} program_kind={args.program_kind or 'cot'} train_rows={len(train_records)}")


def emit_chat_values(
    values: dict[str, object],
    output_fields: list[str],
    args: argparse.Namespace,
    turn_num: int,
    retrieval_metadata: dict[str, Any] | None,
) -> None:
    if args.json:
        payload: dict[str, Any] = {"type": "turn", "turn": turn_num, "prediction": values}
        if retrieval_metadata:
            payload["retrieval"] = retrieval_metadata
        print(json.dumps(payload, ensure_ascii=False), flush=True)
    elif len(values) != 1:
        print(json.dumps(values, ensure_ascii=False, indent=2), flush=True)
    else:
        print(next(iter(values.values())), flush=True)


def run_chat_turn(
    dspy,
    program,
    args: argparse.Namespace,
    parser: argparse.ArgumentParser,
    base_input_fields: list[str],
    output_fields: list[str],
    base_inputs: dict[str, Any],
    prompt_field: str,
    question: str,
    history_messages: list[dict[str, Any]],
    turn_num: int,
) -> None:
    inputs = dict(base_inputs)
    inputs[prompt_field] = question
    retrieval_metadata = enrich_inputs_with_jina_context(args, parser, base_input_fields, inputs, prompt_field)
    inputs["history"] = dspy.History(messages=history_messages)

    runtime_inputs = history_input_fields(base_input_fields)
    missing = [name for name in runtime_inputs if name not in inputs]
    if missing:
        parser.error(f"missing chat input field(s): {', '.join(missing)}; pass --field NAME=VALUE")

    try:
        if args.stream:
            values = run_streamified_prediction(dspy, program, inputs, output_fields, args, retrieval_metadata)
        else:
            prediction = program(**inputs)
            values = prediction_to_dict(prediction, output_fields)
    except Exception as exc:
        if args.debug:
            raise
        raise SystemExit(
            "DSPy chat turn failed through the dsco gateway.\n"
            f"{type(exc).__name__}: {exc}\n\n"
            "Check that the gateway process has provider credentials for this model. "
            "For gateway routing errors, inspect the `./dsco --ui 3141` terminal output. "
            "Use `--debug` to show the full DSPy/LiteLLM traceback."
        ) from None

    turn = {name: inputs[name] for name in base_input_fields if name != "history" and name in inputs}
    turn.update(values)
    history_messages.append(turn)
    save_history_messages(args.history_file, history_messages)

    if args.stream:
        if args.json:
            print(
                json.dumps({"type": "history", "turn": turn_num, "history_len": len(history_messages)}, ensure_ascii=False),
                flush=True,
            )
        return
    emit_chat_values(values, output_fields, args, turn_num, retrieval_metadata)


def run_chat(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    base_input_fields, output_fields = signature_io_fields(args.signature)
    base_inputs = parse_field_assignments(args.field)
    prompt_field = prompt_field_for_inputs(base_input_fields, base_inputs, args.input_field, skip_history=True)
    if "history" in base_inputs:
        parser.error("Pass chat history with --history-file; do not set the history field directly.")

    context_documents = collect_context_documents(args)
    prompts = chat_prompts_from_cli(args)
    history_messages = load_history_messages(args.history_file)

    if args.dry_run:
        missing = [
            name for name in base_input_fields
            if name != "history"
            and name != prompt_field
            and name not in base_inputs
            and not (name == args.context_field and context_documents)
        ]
        if missing:
            parser.error(f"missing chat input field(s): {', '.join(missing)}; pass --field NAME=VALUE")
        output = {
            "ok": True,
            "dry_run": True,
            "mode": "chat",
            "app": args.app,
            "signature": args.signature,
            "program_kind": args.program_kind or "predict",
            "stream": bool(args.stream),
            "prompt_field": prompt_field,
            "input_fields": history_input_fields(base_input_fields),
            "output_fields": output_fields,
            "scripted_turns": len(prompts),
            "history_len": len(history_messages),
            "history_file": args.history_file,
            "jina": {
                "enabled": bool(context_documents),
                "retrieval": args.retrieval,
                "context_field": args.context_field,
                "candidate_chunks": len(context_documents),
            },
            "prompt_cache": prompt_cache_plan(args, args.model),
            "model": args.model,
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return

    preflight_gateway(args.base_url, args.preflight_timeout, args.gateway_api_key)
    dspy = import_dspy()
    if not hasattr(dspy, "History"):
        raise SystemExit("This DSPy install does not expose dspy.History; upgrade DSPy or run without --chat.") from None
    configure_dspy_cache(dspy)
    dspy.configure(lm=make_lm(dspy, args))
    program_signature = build_history_signature(dspy, base_input_fields, output_fields)
    program = build_dspy_program(dspy, program_signature, args.program_kind or "predict")

    if prompts:
        for idx, question in enumerate(prompts, start=1):
            if args.max_chat_turns and idx > args.max_chat_turns:
                break
            run_chat_turn(
                dspy,
                program,
                args,
                parser,
                base_input_fields,
                output_fields,
                base_inputs,
                prompt_field,
                question,
                history_messages,
                idx,
            )
        return

    turn_num = 0
    while not args.max_chat_turns or turn_num < args.max_chat_turns:
        try:
            question = input("you> ").strip()
        except EOFError:
            break
        if not question:
            continue
        if question.lower() in {"exit", "quit", ":q"}:
            break
        turn_num += 1
        run_chat_turn(
            dspy,
            program,
            args,
            parser,
            base_input_fields,
            output_fields,
            base_inputs,
            prompt_field,
            question,
            history_messages,
            turn_num,
        )


def run_session_distillation(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    input_fields, output_fields = signature_io_fields(args.signature)
    if len(input_fields) != 1:
        parser.error("--distill-sessions expects a one-input signature, e.g. 'session_context -> distillation'")
    if not args.teacher_model:
        args.teacher_model = "claude-fable-5"
    if args.save_optimized == DEFAULT_TRAIN_SAVE_PATH:
        args.save_optimized = DEFAULT_SESSION_DISTILL_ARTIFACT
    if args.distill_trainset_jsonl is None:
        args.distill_trainset_jsonl = DEFAULT_SESSION_DISTILL_TRAINSET
    if args.optimizer == "mipro":
        args.optimizer = "labeled"

    input_field = input_fields[0]
    session_records = collect_session_distill_records(args, input_field)
    if not session_records:
        raise SystemExit(
            f"No non-empty session records found under {args.session_root!r}. "
            "Pass --session-path for specific run directories or lower --session-min-chars."
        ) from None

    if args.dry_run:
        output = {
            "ok": True,
            "dry_run": True,
            "mode": "distill-sessions",
            "signature": args.signature,
            "program_kind": args.program_kind or "cot",
            "optimizer": args.optimizer,
            "student_model": args.model,
            "teacher_model": canonical_model(args.teacher_model),
            "session_records": len(session_records),
            "session_ids": [record.get("session_id") for record in session_records],
            "session_char_limit": args.session_char_limit,
            "distill_trainset_jsonl": args.distill_trainset_jsonl,
            "save_optimized": args.save_optimized,
            "student_prompt_cache": prompt_cache_plan(args, args.model),
            "teacher_prompt_cache": prompt_cache_plan(args, canonical_model(args.teacher_model)),
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return

    preflight_gateway(args.base_url, args.preflight_timeout, args.gateway_api_key)
    dspy = import_dspy()
    configure_dspy_cache(dspy)

    teacher_signature_kind = args.distill_teacher_program_kind or args.program_kind or "cot"
    teacher_program = build_teacher_program(dspy, args, args.signature, teacher_signature_kind)
    if teacher_program is None:
        raise SystemExit("--distill-sessions requires --teacher-model; default should have supplied claude-fable-5.") from None
    labeled_records = label_records_with_teacher(
        dspy,
        args,
        teacher_program,
        session_records,
        input_fields,
        output_fields,
    )
    write_jsonl_records(args.distill_trainset_jsonl, labeled_records)

    student_lm = make_lm(dspy, args)
    dspy.configure(lm=student_lm)
    student_program = build_dspy_program(dspy, args.signature, args.program_kind or "predict")
    if hasattr(student_program, "set_lm"):
        student_program.set_lm(student_lm)
    teacher_for_bootstrap = teacher_program if args.optimizer in {"bootstrap", "bootstrap-rs"} else None
    trainset = records_to_examples(dspy, labeled_records, input_fields)
    metric = make_training_metric(dspy, args, output_fields)
    optimized = compile_with_optimizer(
        dspy,
        args,
        student_program,
        trainset,
        None,
        metric,
        output_fields,
        teacher=teacher_for_bootstrap,
    )

    save_path = Path(args.save_optimized).expanduser()
    save_path.parent.mkdir(parents=True, exist_ok=True)
    optimized.save(str(save_path))

    output = {
        "ok": True,
        "mode": "distill-sessions",
        "signature": args.signature,
        "program_kind": args.program_kind or "predict",
        "optimizer": args.optimizer,
        "student_model": args.model,
        "teacher_model": canonical_model(args.teacher_model),
        "session_records": len(labeled_records),
        "session_ids": [record.get("session_id") for record in labeled_records],
        "distill_trainset_jsonl": args.distill_trainset_jsonl,
        "saved": str(save_path),
    }
    if args.json:
        print(json.dumps(output, ensure_ascii=False, indent=2))
    else:
        print(f"distill_trainset={args.distill_trainset_jsonl}")
        print(f"optimized_program={save_path}")
        print(f"teacher={canonical_model(args.teacher_model)} student={args.model} optimizer={args.optimizer}")


def print_models() -> None:
    print("layer        role        model              default_effort  fanout  context   max_out  sampling  price $/MTok in/out")
    for layer in SWARM_LAYERS:
        model = layer["model"]
        card = ANTHROPIC_MODEL_CARDS[model]
        print(
            f"{card['layer']:<12} {card['role']:<11} {model:<18} "
            f"{card['default_effort']:<14} {card['fan_out_cap']:<7} "
            f"{card['context_window']:<9} {card['max_output']:<8} "
            f"{'locked' if card['sampling_locked'] else 'tunable':<8} "
            f"{card['price'][0]:g}/{card['price'][1]:g}  # {card['label']}"
        )


def print_quickstart() -> None:
    print("dsco DSPy CLI")
    print()
    print("Run a one-shot DSPy prediction through the local dsco gateway:")
    print('  uv run examples/dspy_via_dsco.py "What is the capital of Australia?"')
    print('  uv run examples/dspy_via_dsco.py --prompt "Classify this log line" --model-role transform')
    print()
    print("Useful entrypoints:")
    print("  --list-models          show built-in model roles")
    print("  --app chat --stream    run multi-turn streaming chat")
    print("  --agent-loop           run the cost-aware task sampler")
    print("  --dry-run              validate inputs without calling the gateway")
    print("  --help                 show the full option reference")
    print()
    print("Built-in model roles:")
    print_models()


def run_agent_loop(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    tasks = read_agent_tasks(args)
    if not tasks:
        parser.error("agent loop needs at least one task via prompt, stdin, --task, or --tasks-json")
    if args.max_agent_steps <= 0:
        parser.error("--max-agent-steps must be positive")
    if args.max_samples <= 0:
        parser.error("--max-samples must be positive")

    input_fields, output_fields = signature_io_fields(args.agent_signature)
    default_in_price, default_out_price = model_price_for(args.model)
    input_price = args.input_price_per_mtok if args.input_price_per_mtok is not None else default_in_price
    output_price = args.output_price_per_mtok if args.output_price_per_mtok is not None else default_out_price
    ledger = CostLedger(input_price_per_mtok=input_price, output_price_per_mtok=output_price)

    dspy = None
    program = None
    if not args.dry_run:
        preflight_gateway(args.base_url, args.preflight_timeout, args.gateway_api_key)
        dspy = import_dspy()
        configure_dspy_cache(dspy)
        dspy.configure(lm=make_lm(dspy, args))
        program = dspy.Predict(args.agent_signature)

    queue = list(tasks)
    next_task_num = len(queue) + 1
    results: list[dict[str, Any]] = []
    generated: list[dict[str, Any]] = []
    budget_exhausted = False

    while queue and len(results) < args.max_agent_steps:
        task = queue.pop(0)
        task_model = agent_loop_model_for_task(args, task)
        task_card = ANTHROPIC_MODEL_CARDS.get(task_model)
        task_prices = model_price_for(task_model)
        context = build_agent_context(args, task, ledger)
        samples: list[dict[str, Any]] = []
        budget_stop_reason = None

        for sample_idx in range(1, args.max_samples + 1):
            call_inputs = agent_inputs_for_signature(input_fields, task, context)
            prompt_estimate = json.dumps(call_inputs, ensure_ascii=False, sort_keys=True)
            input_tokens = estimate_tokens(args.agent_signature + "\n" + prompt_estimate)
            projected = ledger.estimate_usd(input_tokens, args.expected_output_tokens, task_prices)
            if args.budget_usd is not None and args.budget_usd > 0 and ledger.spent_usd + projected > args.budget_usd:
                budget_exhausted = True
                budget_stop_reason = (
                    f"projected sample cost ${projected:.6f} would exceed budget "
                    f"${args.budget_usd:.6f}"
                )
                break

            if args.dry_run:
                values = {
                    "result": f"[dry-run] planned sample {sample_idx} for task: {task.task}",
                    "confidence": 0.5,
                    "new_tasks": [],
                }
            else:
                try:
                    dspy.configure(lm=make_lm(dspy, args, model_override=task_model))
                    prediction = program(**call_inputs)
                    values = prediction_to_dict(prediction, output_fields)
                except Exception as exc:
                    if args.debug:
                        raise
                    values = {
                        "result": "",
                        "confidence": 0.0,
                        "new_tasks": [],
                        "error": f"{type(exc).__name__}: {exc}",
                    }

            output_text = json.dumps(values, ensure_ascii=False, sort_keys=True)
            output_tokens = estimate_tokens(output_text)
            sample_cost = ledger.add(input_tokens, output_tokens, task_prices)
            sample = {
                "sample": sample_idx,
                "model": task_model,
                "swarm_layer": task_card.get("layer") if task_card else None,
                "swarm_role": task_card.get("role") if task_card else None,
                "result": primary_result(values),
                "confidence": confidence_score(values.get("confidence")),
                "new_tasks": parse_new_tasks(values.get("new_tasks")),
                "estimated_input_tokens": input_tokens,
                "estimated_output_tokens": output_tokens,
                "estimated_cost_usd": round(sample_cost, 8),
            }
            if "error" in values:
                sample["error"] = values["error"]
            samples.append(sample)

        best = max(samples, key=sample_sort_key) if samples else None
        new_task_texts = list(best.get("new_tasks") or []) if best else []
        enqueued: list[dict[str, Any]] = []
        fan_out_cap = int(task_card.get("fan_out_cap", args.max_new_tasks)) if task_card else args.max_new_tasks
        child_limit = min(args.max_new_tasks, max(0, fan_out_cap))
        if best and task.depth < args.max_depth and child_limit > 0:
            for new_text in new_task_texts[:child_limit]:
                child = AgentTask(
                    id=f"t{next_task_num}",
                    task=new_text,
                    parent_id=task.id,
                    depth=task.depth + 1,
                )
                next_task_num += 1
                queue.append(child)
                child_json = {
                    "id": child.id,
                    "task": child.task,
                    "parent_id": child.parent_id,
                    "depth": child.depth,
                    "swarm_layer": swarm_layer_for_depth(child.depth)["layer"] if args.swarm_layers else None,
                    "model": swarm_layer_for_depth(child.depth)["model"] if args.swarm_layers else args.model,
                }
                enqueued.append(child_json)
                generated.append(child_json)

        results.append(
            {
                "task_id": task.id,
                "task": task.task,
                "parent_id": task.parent_id,
                "depth": task.depth,
                "swarm_layer": task_card.get("layer") if task_card else None,
                "swarm_role": task_card.get("role") if task_card else None,
                "model": task_model,
                "result": best.get("result") if best else "",
                "confidence": best.get("confidence") if best else 0.0,
                "new_tasks": enqueued,
                "samples": samples,
                "budget_stop_reason": budget_stop_reason,
            }
        )

        if budget_exhausted:
            break

    had_errors = any(any("error" in sample for sample in item["samples"]) for item in results)
    output = {
        "ok": not budget_exhausted and not had_errors,
        "model": args.model,
        "swarm_layers": args.swarm_layers,
        "swarm": SWARM_LAYERS if args.swarm_layers else [],
        "prompt_cache": prompt_cache_plan(args, args.model),
        "price_per_mtok": {
            "input": input_price,
            "output": output_price,
            "source": "default_table" if (input_price, output_price) == model_price_for(args.model) else "override",
        },
        "budget_usd": args.budget_usd,
        "estimated_spend_usd": round(ledger.spent_usd, 8),
        "estimated_tokens": {
            "input": ledger.input_tokens,
            "output": ledger.output_tokens,
        },
        "results": results,
        "generated_tasks": generated,
        "remaining_queue": [
            {"id": t.id, "task": t.task, "parent_id": t.parent_id, "depth": t.depth} for t in queue
        ],
        "dry_run": args.dry_run,
    }

    if args.json or args.dry_run:
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return

    for item in results:
        layer = f"{item['swarm_layer']}/{item['model']}" if item.get("swarm_layer") else item["model"]
        print(f"[{item['task_id']}] {layer} :: {item['task']}")
        print(item["result"])
        if item["new_tasks"]:
            print("new_tasks:")
            for child in item["new_tasks"]:
                print(f"  - {child['id']}: {child['task']}")
        print()
    print(f"estimated_spend_usd={output['estimated_spend_usd']:.8f}")


def main() -> None:
    raw_argv = sys.argv[1:]
    parser = argparse.ArgumentParser(description="Run DSPy through the dsco OpenAI gateway.")
    parser.add_argument("prompt", nargs="*", help="prompt text; stdin or --prompt is used when omitted")
    parser.add_argument("--list-models", action="store_true", help="show the four built-in latest Anthropic model profiles and exit")
    parser.add_argument("--prompt", dest="prompt_text", help="prompt text; equivalent to positional prompt")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help="dsco gateway base URL")
    parser.add_argument(
        "--gateway-api-key",
        default=os.getenv("DSCO_WEB_TOKEN", "dsco"),
        help="Bearer token for the dsco web gateway; must match DSCO_WEB_TOKEN when token auth is enabled",
    )
    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
        help=(
            "Any model id dsco can route. Defaults to root claude-fable-5. "
            "Aliases/layers: root/fable/frontier, review/opus/decision, "
            "orchestrate/sonnet/synthesis, leaf/haiku/transform."
        ),
    )
    parser.add_argument(
        "--model-role",
        choices=sorted(MODEL_ROLE_TO_ID),
        help="route by swarm layer or task role: root/frontier, review/decision, orchestrate/synthesis, leaf/transform",
    )
    parser.add_argument("--app", choices=sorted(APPLICATION_PRESETS), help="application preset that fills default signature/program kind")
    parser.add_argument("--signature", default=None, help="DSPy signature, e.g. 'question -> answer'")
    parser.add_argument("--input-field", default=None, help="field that receives the positional prompt; defaults to the first signature input")
    parser.add_argument("--field", action="append", default=[], metavar="NAME=VALUE", help="set an additional DSPy input field; use NAME=@path to feed a file")
    parser.add_argument(
        "--program-kind",
        choices=["predict", "cot", "chain-of-thought", "pot", "program-of-thought"],
        default=None,
        help="DSPy module kind; defaults to Predict, or ChainOfThought in --train mode",
    )
    parser.add_argument("--chat", "--multi-turn", dest="chat", action="store_true", help="run a multi-turn DSPy History chat")
    parser.add_argument("--history-file", help="JSON file for persisted dspy.History messages")
    parser.add_argument("--max-chat-turns", type=int, default=0, help="maximum chat turns; 0 means until stdin/exit")
    parser.add_argument("--load-optimized", help="load a saved DSPy program artifact for prediction")
    parser.add_argument("--allow-pickle-load", action="store_true", help="allow pickle when loading a trusted optimized DSPy artifact")
    parser.add_argument("--context-file", action="append", default=[], help="file or directory to chunk, retrieve, and feed into --context-field")
    parser.add_argument("--context-text", action="append", default=[], help="literal context text, or @path, to chunk, retrieve, and feed into --context-field")
    parser.add_argument("--context-field", default="context", help="DSPy input field that receives selected retrieval chunks")
    parser.add_argument("--query-field", default=None, help="DSPy input field used as the context retrieval query")
    parser.add_argument("--retrieval", choices=["auto", "jina", "local"], default="auto", help="context retrieval backend; auto uses Jina when available and falls back to local lexical ranking")
    parser.add_argument("--jina-query", default=None, help="explicit retrieval query instead of deriving it from DSPy inputs")
    parser.add_argument("--jina-api-key", default=None, help="Jina API key; defaults to JINA_API_KEY")
    parser.add_argument("--jina-api-base", default=DEFAULT_JINA_API_BASE, help="Jina API base URL")
    parser.add_argument("--jina-embedding-model", default=DEFAULT_JINA_EMBEDDING_MODEL, help="Jina embedding model for context candidate retrieval")
    parser.add_argument("--jina-reranker-model", default=DEFAULT_JINA_RERANKER_MODEL, help="Jina reranker model for final context ordering")
    parser.add_argument("--jina-query-task", default=DEFAULT_JINA_QUERY_TASK, help="Jina embeddings task for the query; pass 'none' to omit")
    parser.add_argument("--jina-document-task", default=DEFAULT_JINA_DOCUMENT_TASK, help="Jina embeddings task for documents; pass 'none' to omit")
    parser.add_argument("--jina-dimensions", type=int, default=None, help="optional Matryoshka embedding dimension")
    parser.add_argument("--jina-normalized", action=argparse.BooleanOptionalAction, default=True, help="request normalized Jina embeddings")
    parser.add_argument("--jina-batch-size", type=int, default=32, help="texts per Jina embeddings request")
    parser.add_argument("--jina-embed-top-k", type=int, default=24, help="embedding candidates to send to reranker")
    parser.add_argument("--jina-rerank-top-n", type=int, default=5, help="reranked chunks to feed into the DSPy context field")
    parser.add_argument("--jina-rerank", action=argparse.BooleanOptionalAction, default=True, help="use Jina reranker after embedding retrieval")
    parser.add_argument("--jina-timeout", type=float, default=30.0, help="seconds to wait for each Jina API request")
    parser.add_argument("--chunk-chars", type=int, default=2400, help="maximum characters per context chunk")
    parser.add_argument("--chunk-overlap", type=int, default=240, help="overlap characters between context chunks")
    parser.add_argument("--json", action="store_true", help="emit all prediction fields as JSON")
    parser.add_argument(
        "--stream",
        action="store_true",
        help="stream one-shot prediction output with dspy.streamify; with --json, emit JSONL stream events",
    )
    parser.add_argument("--preflight-timeout", type=float, default=2.0, help="seconds to wait for the gateway preflight")
    parser.add_argument("--debug", action="store_true", help="show the full DSPy/LiteLLM traceback on request failures")
    parser.add_argument("--reasoning-effort", choices=["low", "medium", "high", "max", "xhigh"], help="provider reasoning effort hint when supported by dsco/provider")
    parser.add_argument("--max-tokens", type=int, default=None, help="DSPy LM max_tokens for a single Predict call")
    parser.add_argument("--prompt-cache-ttl", choices=["5m", "1h"], default="5m", help="Anthropic prompt cache TTL")
    parser.add_argument(
        "--prompt-cache-target",
        choices=["system", "automatic"],
        default="system",
        help="cache the stable DSPy system prompt, or ask Anthropic to choose the automatic breakpoint",
    )
    parser.add_argument(
        "--prompt-cache-retention",
        choices=["24h", "in_memory"],
        default=DEFAULT_OPENAI_PROMPT_CACHE_RETENTION,
        help="OpenAI prompt cache retention hint for routed OpenAI models",
    )
    parser.add_argument("--train", action="store_true", help="compile/optimize a DSPy program from JSONL examples")
    parser.add_argument("--distill-sessions", action="store_true", help="review .swarm session data with a Fable teacher and compile a Haiku-runnable DSPy artifact")
    parser.add_argument("--session-root", default=".swarm/runs", help="directory containing dsco swarm run directories for --distill-sessions")
    parser.add_argument("--session-path", action="append", default=[], help="specific swarm run directory to include in --distill-sessions")
    parser.add_argument("--session-limit", type=int, default=4, help="maximum session runs to distill")
    parser.add_argument("--session-min-chars", type=int, default=300, help="skip session contexts shorter than this")
    parser.add_argument("--session-char-limit", type=int, default=8000, help="maximum characters per session context sent to the teacher")
    parser.add_argument("--distill-trainset-jsonl", default=None, help="where --distill-sessions writes Fable-labeled JSONL")
    parser.add_argument(
        "--distill-teacher-program-kind",
        choices=["predict", "cot", "chain-of-thought", "pot", "program-of-thought"],
        default="cot",
        help="teacher module kind for Fable session labeling",
    )
    parser.add_argument("--trainset-jsonl", help="JSONL training set whose keys match --signature fields")
    parser.add_argument("--devset-jsonl", help="optional JSONL development set whose keys match --signature fields")
    parser.add_argument("--train-limit", type=int, default=None, help="optional cap on training rows")
    parser.add_argument("--dev-limit", type=int, default=None, help="optional cap on dev rows")
    parser.add_argument(
        "--optimizer",
        choices=[
            "mipro",
            "gepa",
            "bootstrap-rs",
            "bootstrap-random",
            "bootstrap",
            "bootstrap-fewshot",
            "labeled",
            "labeled-fewshot",
        ],
        default="mipro",
        help="DSPy optimizer for --train",
    )
    parser.add_argument("--optimizer-auto", choices=["light", "medium", "heavy"], default="light", help="MIPROv2 optimization budget")
    parser.add_argument("--gepa-auto", choices=["light", "medium", "heavy"], default="light", help="GEPA auto budget when no explicit GEPA budget is set")
    parser.add_argument("--gepa-max-metric-calls", type=int, default=None, help="GEPA budget cap in metric calls")
    parser.add_argument("--gepa-max-full-evals", type=int, default=None, help="GEPA budget cap in full evaluations")
    parser.add_argument("--gepa-reflection-model", default=None, help="model used by GEPA for reflective prompt mutation")
    parser.add_argument("--gepa-reflection-temperature", type=float, default=1.0, help="GEPA reflection LM temperature when the provider permits sampling")
    parser.add_argument("--gepa-reflection-max-tokens", type=int, default=32000, help="GEPA reflection LM max_tokens")
    parser.add_argument("--gepa-log-dir", default=None, help="optional GEPA log/checkpoint directory")
    parser.add_argument("--gepa-track-stats", action="store_true", help="store GEPA detailed_results on the optimized program")
    parser.add_argument("--metric", choices=["exact-match", "answer-exact-match", "semantic-f1"], default="exact-match", help="metric used by DSPy optimizers")
    parser.add_argument("--metric-threshold", type=float, default=None, help="optional BootstrapFewShot metric threshold")
    parser.add_argument("--metric-field", default=None, help="prediction field to score; defaults to --label-field or first output")
    parser.add_argument("--label-field", default=None, help="example label field to score; defaults to first output")
    parser.add_argument("--max-labeled-demos", type=int, default=8, help="labeled demos available to DSPy optimizers")
    parser.add_argument("--max-bootstrapped-demos", type=int, default=4, help="bootstrapped demos generated by DSPy optimizers")
    parser.add_argument("--max-rounds", type=int, default=1, help="BootstrapFewShot max_rounds")
    parser.add_argument("--max-errors", type=int, default=10, help="maximum optimizer errors before aborting")
    parser.add_argument("--num-candidate-programs", type=int, default=8, help="BootstrapFewShotWithRandomSearch candidates")
    parser.add_argument("--num-threads", type=int, default=4, help="optimizer worker threads")
    parser.add_argument("--save-optimized", default=DEFAULT_TRAIN_SAVE_PATH, help="path for optimized DSPy program artifact")
    parser.add_argument("--teacher-model", default=None, help="optional teacher model for BootstrapFewShot distillation; --distill-sessions defaults this to Fable 5")
    parser.add_argument("--agent-loop", action="store_true", help="run a cost-aware queued task sampling loop")
    parser.add_argument("--task", action="append", default=[], help="add one task to the agent-loop queue")
    parser.add_argument("--tasks-json", help="read agent-loop tasks from JSON file, or '-' for stdin")
    parser.add_argument("--agent-signature", default=DEFAULT_AGENT_SIGNATURE, help="DSPy signature for agent-loop samples")
    parser.add_argument("--agent-context", default=DEFAULT_AGENT_CONTEXT, help="global instruction/context for agent-loop samples")
    parser.add_argument("--max-agent-steps", type=int, default=8, help="maximum tasks to process in --agent-loop")
    parser.add_argument("--max-samples", type=int, default=2, help="candidate samples per task in --agent-loop")
    parser.add_argument("--max-depth", type=int, default=3, help="maximum generated-task depth in --agent-loop")
    parser.add_argument("--max-new-tasks", type=int, default=6, help="maximum child tasks to enqueue per processed task")
    parser.add_argument("--budget-usd", type=float, default=0.25, help="estimated spend cap for --agent-loop; <=0 disables")
    parser.add_argument("--input-price-per-mtok", type=float, help="override input token price per million tokens")
    parser.add_argument("--output-price-per-mtok", type=float, help="override output token price per million tokens")
    parser.add_argument("--expected-output-tokens", type=int, default=900, help="pre-call output token estimate for budget gating")
    parser.add_argument("--max-output-tokens", type=int, default=1200, help="DSPy LM max_tokens for --agent-loop")
    parser.add_argument("--temperature", type=float, default=0.7, help="sampling temperature for --agent-loop")
    parser.add_argument(
        "--swarm-layers",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="in --agent-loop, route depth 0/1/2/3 through Fable/Opus/Sonnet/Haiku; use --no-swarm-layers to pin --model",
    )
    parser.add_argument("--dry-run", action="store_true", help="plan prediction, Jina enrichment, --train, or --agent-loop without external model calls")
    args = parser.parse_args()

    if args.list_models:
        print_models()
        return

    if args.prompt_text is not None and args.prompt:
        parser.error("pass prompt either positionally or with --prompt, not both")

    apply_application_preset(args)
    if args.distill_sessions and args.model == DEFAULT_MODEL and not args.model_role:
        args.model = "claude-haiku-4-5"
    args.model = canonical_model(args.model, args.model_role)

    if sum(bool(flag) for flag in (args.train, args.distill_sessions, args.agent_loop, args.chat)) > 1:
        parser.error("--train, --distill-sessions, --agent-loop, and --chat/--multi-turn are mutually exclusive")
    if args.app and (args.agent_loop or args.distill_sessions):
        parser.error("--app cannot be combined with --agent-loop or --distill-sessions")
    if args.load_optimized and (args.train or args.distill_sessions or args.agent_loop or args.chat):
        parser.error("--load-optimized is for one-shot prediction, not --train/--distill-sessions/--agent-loop/--chat")
    if args.stream and (args.train or args.distill_sessions or args.agent_loop):
        parser.error("--stream supports one-shot predict and --chat, not --train, --distill-sessions, or --agent-loop")
    if args.distill_sessions:
        run_session_distillation(args, parser)
        return
    if args.train:
        run_training(args, parser)
        return

    if args.agent_loop:
        run_agent_loop(args, parser)
        return

    if args.chat:
        run_chat(args, parser)
        return

    input_fields, output_fields = signature_io_fields(args.signature)
    prompt = read_prompt(args.prompt, args.prompt_text)
    inputs = parse_field_assignments(args.field)
    if not raw_argv and not prompt and not inputs:
        print_quickstart()
        return
    prompt_field = prompt_field_for_inputs(input_fields, inputs, args.input_field)
    if prompt:
        inputs[prompt_field] = prompt

    if args.dry_run:
        context_documents = collect_context_documents(args)
        missing = [
            name for name in input_fields
            if name not in inputs and not (name == args.context_field and context_documents)
        ]
        if missing:
            parser.error(f"missing input field(s): {', '.join(missing)}; pass a prompt or --field NAME=VALUE")
        output = {
            "ok": True,
            "dry_run": True,
            "mode": "predict",
            "app": args.app,
            "signature": args.signature,
            "program_kind": args.program_kind or "predict",
            "load_optimized": args.load_optimized,
            "stream": bool(args.stream),
            "input_fields": input_fields,
            "output_fields": output_fields,
            "inputs": sorted(inputs),
            "jina": {
                "enabled": bool(context_documents),
                "retrieval": args.retrieval,
                "context_field": args.context_field,
                "candidate_chunks": len(context_documents),
                "embedding_model": args.jina_embedding_model,
                "reranker_model": args.jina_reranker_model if args.jina_rerank else None,
                "api_key_source": "JINA_API_KEY" if os.getenv("JINA_API_KEY") and not args.jina_api_key else (
                    "--jina-api-key" if args.jina_api_key else None
                ),
            },
            "prompt_cache": prompt_cache_plan(args, args.model),
            "model": args.model,
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return

    retrieval_metadata = enrich_inputs_with_jina_context(args, parser, input_fields, inputs, prompt_field)
    missing = [name for name in input_fields if name not in inputs]
    if missing:
        parser.error(f"missing input field(s): {', '.join(missing)}; pass a prompt or --field NAME=VALUE")

    preflight_gateway(args.base_url, args.preflight_timeout, args.gateway_api_key)
    dspy = import_dspy()
    configure_dspy_cache(dspy)

    lm = make_lm(dspy, args)
    dspy.configure(lm=lm)
    if args.load_optimized:
        program = build_dspy_program(dspy, args.signature, args.program_kind or "predict")
        try:
            program.load(str(Path(args.load_optimized).expanduser()), allow_pickle=args.allow_pickle_load)
        except Exception as exc:
            raise SystemExit(f"Could not load optimized DSPy program {args.load_optimized}: {exc}") from None
        if hasattr(program, "set_lm"):
            program.set_lm(lm)
    else:
        program = build_dspy_program(dspy, args.signature, args.program_kind or "predict")

    try:
        if args.stream:
            run_streamified_prediction(dspy, program, inputs, output_fields, args, retrieval_metadata)
            return
        prediction = program(**inputs)
    except Exception as exc:
        if args.debug:
            raise
        raise SystemExit(
            "DSPy request failed through the dsco gateway.\n"
            f"{type(exc).__name__}: {exc}\n\n"
            "Check that the gateway process has provider credentials for this model. "
            "For gateway routing errors, inspect the `./dsco --ui 3141` terminal output. "
            "Use `--debug` to show the full DSPy/LiteLLM traceback."
        ) from None

    values = prediction_to_dict(prediction, output_fields)
    if args.json:
        if retrieval_metadata:
            print(json.dumps({"prediction": values, "retrieval": retrieval_metadata}, ensure_ascii=False, indent=2))
        else:
            print(json.dumps(values, ensure_ascii=False, indent=2))
    elif len(values) != 1:
        print(json.dumps(values, ensure_ascii=False, indent=2))
    else:
        print(next(iter(values.values())))


if __name__ == "__main__":
    main()
