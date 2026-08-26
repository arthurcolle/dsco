#!/usr/bin/env python3
#
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = [
#   "dspy",
#   "openai",
# ]
# ///
#
"""Single-file DSPy agentic sampling harness for dsco.

The harness runs a cost-aware multi-swarm tree over dsco's OpenAI-compatible
gateway. It keeps all DSPy abstractions in this one file: signatures, modules,
LM construction, sampling policy, budget ledger, and Pareto selection.

Usage:
  uv run examples/dspy_agentic_sampling_harness.py --dry-run "Design a rollout plan"
  uv run examples/dspy_agentic_sampling_harness.py --spend deep --time-budget-seconds 180 "Solve this hard bug"
  uv run examples/dspy_agentic_sampling_harness.py --dry-run --plot-frontier frontier.svg "Map the work"
  uv run examples/dspy_agentic_sampling_harness.py --swarms 2 --resolution 1.5 --budget-usd 0.50 "Audit this repo"
  uv run examples/dspy_agentic_sampling_harness.py --json --dry-run --max-depth 3 --swarms 3 "Map the work"
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from html import escape as html_escape
import json
import math
import os
import socket
import sys
import time
import urllib.error
import urllib.request
from typing import Any
from urllib.parse import urlparse


DEFAULT_BASE_URL = "http://127.0.0.1:3141/v1"
DEFAULT_GATEWAY_KEY = "dsco"
SPEND_PRESETS = {
    "quick": {"time_budget_seconds": 20.0, "feedback_passes": 1},
    "normal": {"time_budget_seconds": 60.0, "feedback_passes": 2},
    "deep": {"time_budget_seconds": 180.0, "feedback_passes": 4},
    "exhaustive": {"time_budget_seconds": 600.0, "feedback_passes": 8},
}


@dataclass(frozen=True)
class ModelCard:
    layer: str
    role: str
    model: str
    effort: str
    fanout_cap: int
    input_price: float
    output_price: float
    context_window: int
    max_output: int
    sampling_locked: bool = True
    utility_prior: float = 1.0

    def estimate_usd(self, input_tokens: int, output_tokens: int) -> float:
        return (input_tokens / 1_000_000.0) * self.input_price + (
            output_tokens / 1_000_000.0
        ) * self.output_price


MODEL_STACK: tuple[ModelCard, ...] = (
    ModelCard(
        layer="root",
        role="frontier",
        model="claude-fable-5",
        effort="high",
        fanout_cap=4,
        input_price=10.0,
        output_price=50.0,
        context_window=1_000_000,
        max_output=128_000,
        sampling_locked=True,
        utility_prior=1.25,
    ),
    ModelCard(
        layer="review",
        role="decision",
        model="claude-opus-4-8",
        effort="xhigh",
        fanout_cap=6,
        input_price=5.0,
        output_price=25.0,
        context_window=1_000_000,
        max_output=128_000,
        sampling_locked=True,
        utility_prior=1.15,
    ),
    ModelCard(
        layer="orchestrate",
        role="synthesis",
        model="claude-sonnet-5",
        effort="high",
        fanout_cap=8,
        input_price=2.0,
        output_price=10.0,
        context_window=1_000_000,
        max_output=128_000,
        sampling_locked=True,
        utility_prior=1.0,
    ),
    ModelCard(
        layer="leaf",
        role="transform",
        model="claude-haiku-4-5",
        effort="low",
        fanout_cap=0,
        input_price=1.0,
        output_price=5.0,
        context_window=200_000,
        max_output=64_000,
        sampling_locked=False,
        utility_prior=0.85,
    ),
)

ALIASES = {
    "root": "claude-fable-5",
    "frontier": "claude-fable-5",
    "fable": "claude-fable-5",
    "review": "claude-opus-4-8",
    "decision": "claude-opus-4-8",
    "opus": "claude-opus-4-8",
    "opus-4.8": "claude-opus-4-8",
    "orchestrate": "claude-sonnet-5",
    "synthesis": "claude-sonnet-5",
    "sonnet": "claude-sonnet-5",
    "leaf": "claude-haiku-4-5",
    "transform": "claude-haiku-4-5",
    "haiku": "claude-haiku-4-5",
    "haiku-4.5": "claude-haiku-4-5",
}

CARD_BY_MODEL = {card.model: card for card in MODEL_STACK}


@dataclass
class WorkItem:
    id: str
    swarm_id: int
    layer_idx: int
    task: str
    feedback_pass: int = 1
    context: str = ""
    parent_id: str | None = None
    ancestry: list[str] = field(default_factory=list)

    @property
    def card(self) -> ModelCard:
        return MODEL_STACK[min(self.layer_idx, len(MODEL_STACK) - 1)]


@dataclass
class SampleRecord:
    id: str
    work_id: str
    swarm_id: int
    feedback_pass: int
    layer: str
    role: str
    model: str
    task: str
    result: str
    confidence: float
    followups: list[str]
    input_tokens: int
    output_tokens: int
    cost_usd: float
    projected_usd: float
    utility: float
    efficiency: float
    latency_rank: int
    error: str | None = None
    dominated: bool = False
    warning: str | None = None


@dataclass
class CostLedger:
    budget_usd: float
    spent_usd: float = 0.0
    projected_usd: float = 0.0
    input_tokens: int = 0
    output_tokens: int = 0

    @property
    def remaining_usd(self) -> float:
        if self.budget_usd <= 0:
            return float("inf")
        return max(0.0, self.budget_usd - self.spent_usd)

    def can_afford(self, projected: float, reserve_fraction: float) -> bool:
        if self.budget_usd <= 0:
            return True
        reserve = self.budget_usd * max(0.0, reserve_fraction)
        committed = max(self.spent_usd, self.projected_usd)
        return committed + projected <= max(0.0, self.budget_usd - reserve)

    def add(self, input_tokens: int, output_tokens: int, actual_cost: float, projected_cost: float) -> None:
        self.input_tokens += max(0, input_tokens)
        self.output_tokens += max(0, output_tokens)
        self.spent_usd += max(0.0, actual_cost)
        self.projected_usd += max(0.0, projected_cost)


@dataclass
class FeedbackMemory:
    max_items: int
    records: list[SampleRecord] = field(default_factory=list)

    def add(self, records: list[SampleRecord]) -> None:
        useful = [rec for rec in records if rec.result.strip() and not rec.error]
        if not useful:
            return
        by_id = {rec.id: rec for rec in self.records}
        for rec in useful:
            by_id[rec.id] = rec
        self.records = pareto_frontier(list(by_id.values()), self.max_items)

    def render(self, token_limit: int) -> str:
        if not self.records or token_limit <= 0:
            return ""
        lines = ["Selected prior outputs to feed back into the next input phase:"]
        for rec in self.records:
            task = " ".join(rec.task.split())[:220]
            result = " ".join(rec.result.split())[:700]
            line = (
                f"- {rec.id} pass={rec.feedback_pass} {rec.layer}/{rec.model} "
                f"utility={rec.utility:.4f} cost=${rec.cost_usd:.6f} conf={rec.confidence:.2f}; "
                f"task={task}; output={result}"
            )
            candidate = "\n".join(lines + [line])
            if rough_tokens(candidate) > token_limit:
                break
            lines.append(line)
        return "\n".join(lines)


def import_dspy():
    try:
        import dspy
    except ModuleNotFoundError as exc:
        if exc.name == "dspy":
            raise SystemExit(
                "Missing dependency: dspy\n"
                "Run with `uv run examples/dspy_agentic_sampling_harness.py ...`."
            ) from None
        raise
    return dspy


def canonical_model(name: str | None) -> str:
    if not name:
        return MODEL_STACK[0].model
    normalized = name.strip().lower().replace("_", "-")
    return ALIASES.get(normalized, normalized)


def card_for_model(name: str | None) -> ModelCard:
    model = canonical_model(name)
    if model not in CARD_BY_MODEL:
        raise SystemExit(f"Unknown model/layer {name!r}. Try --list-models.") from None
    return CARD_BY_MODEL[model]


def rough_tokens(text: str) -> int:
    return max(1, (len(text or "") + 3) // 4)


def parse_confidence(value: Any) -> float:
    if value is None:
        return 0.5
    text = str(value).strip().rstrip("%")
    try:
        score = float(text)
    except ValueError:
        low = text.lower()
        if "high" in low:
            return 0.85
        if "medium" in low:
            return 0.6
        if "low" in low:
            return 0.35
        return 0.5
    if score > 1:
        score /= 100.0
    return max(0.0, min(1.0, score))


def parse_followups(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    if isinstance(value, dict):
        return parse_followups(value.get("tasks") or value.get("followups"))
    text = str(value).strip()
    if not text or text.lower() in {"none", "null", "n/a", "[]"}:
        return []
    if text.startswith("```"):
        lines = text.splitlines()
        if lines:
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        text = "\n".join(lines).strip()
    try:
        parsed = json.loads(text)
        return parse_followups(parsed)
    except json.JSONDecodeError:
        pass
    out = []
    for line in text.splitlines():
        item = line.strip()
        while item[:1] in {"-", "*"}:
            item = item[1:].strip()
        if len(item) > 2 and item[0].isdigit() and item[1] in {".", ")"}:
            item = item[2:].strip()
        if item:
            out.append(item)
    return out


def prediction_to_dict(prediction: Any) -> dict[str, Any]:
    if hasattr(prediction, "toDict"):
        return dict(prediction.toDict())
    if hasattr(prediction, "items"):
        return dict(prediction.items())
    out: dict[str, Any] = {}
    for key in ("result", "confidence", "followups", "critique", "winner_id", "score", "rationale"):
        if hasattr(prediction, key):
            out[key] = getattr(prediction, key)
    return out or {"result": str(prediction)}


def extract_jsonish_string_field(text: str, key: str) -> str:
    marker = f'"{key}"'
    pos = text.find(marker)
    if pos < 0:
        return ""
    colon = text.find(":", pos + len(marker))
    if colon < 0:
        return ""
    start = text.find('"', colon + 1)
    if start < 0:
        return ""
    chars = []
    escaped = False
    for ch in text[start + 1 :]:
        if escaped:
            chars.append("\\" + ch)
            escaped = False
        elif ch == "\\":
            escaped = True
        elif ch == '"':
            break
        else:
            chars.append(ch)
    raw = "".join(chars)
    encoded = '"' + raw.replace("\n", "\\n").replace("\r", "\\r") + '"'
    try:
        return str(json.loads(encoded)).strip()
    except json.JSONDecodeError:
        return raw.replace("\\n", "\n").replace('\\"', '"').strip()


def prediction_from_adapter_error(error_text: str) -> dict[str, Any]:
    marker = "LM Response:"
    if marker not in error_text:
        return {}
    response = error_text.split(marker, 1)[1].strip()
    for stop in (
        "\n\nExpected to find output fields",
        "\nExpected to find output fields",
        "Expected to find output fields",
    ):
        idx = response.find(stop)
        if idx >= 0:
            response = response[:idx].strip()
            break
    if not response:
        return {}
    try:
        parsed = json.loads(response)
        if isinstance(parsed, dict):
            return parsed
    except json.JSONDecodeError:
        pass
    result = extract_jsonish_string_field(response, "result")
    if not result:
        return {}
    out: dict[str, Any] = {"result": result}
    confidence = extract_jsonish_string_field(response, "confidence")
    if confidence:
        out["confidence"] = confidence
    followups = extract_jsonish_string_field(response, "followups")
    if followups:
        out["followups"] = followups
    critique = extract_jsonish_string_field(response, "critique")
    if critique:
        out["critique"] = critique
    return out


def time_remaining_seconds(args: argparse.Namespace) -> float | None:
    started_at = getattr(args, "_started_at", None)
    if started_at is None or args.time_budget_seconds <= 0:
        return None
    return max(0.0, args.time_budget_seconds - (time.monotonic() - started_at))


def time_budget_expired(args: argparse.Namespace) -> bool:
    remaining = time_remaining_seconds(args)
    return remaining is not None and remaining <= 0.0


def gateway_models_url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/models"


def preflight_gateway(base_url: str, timeout: float, api_key: str) -> None:
    parsed = urlparse(base_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise SystemExit(f"Invalid --base-url {base_url!r}") from None
    request = urllib.request.Request(
        gateway_models_url(base_url),
        headers={"Authorization": f"Bearer {api_key}", "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            response.read(1024)
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            raise SystemExit("Gateway rejected the bearer token. Pass --gateway-api-key matching DSCO_WEB_TOKEN.") from None
    except (urllib.error.URLError, TimeoutError, socket.timeout, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        raise SystemExit(f"Could not reach dsco gateway at {base_url}: {reason}") from None


def make_lm(dspy, args: argparse.Namespace, card: ModelCard):
    kwargs: dict[str, Any] = {
        "model": f"openai/{card.model}",
        "api_base": args.base_url,
        "api_key": args.gateway_api_key,
        "cache": False,
        "max_tokens": min(args.max_output_tokens, card.max_output),
    }
    if args.temperature is not None and not card.sampling_locked:
        kwargs["temperature"] = args.temperature
    if args.reasoning_effort:
        kwargs["reasoning_effort"] = args.reasoning_effort
        kwargs["allowed_openai_params"] = ["reasoning_effort"]
    return dspy.LM(**kwargs)


def make_adapter(dspy, args: argparse.Namespace):
    if args.adapter == "chat":
        return dspy.ChatAdapter(use_json_adapter_fallback=not args.no_json_adapter_fallback)
    if args.adapter == "json":
        return dspy.JSONAdapter()
    if args.adapter == "xml":
        return dspy.XMLAdapter()
    raise ValueError(f"unsupported DSPy adapter {args.adapter!r}")


def build_dspy_abstractions(dspy, program_kind: str, signature_mode: str):
    class RichNodeSignature(dspy.Signature):
        """Solve one shard, estimate confidence, and propose follow-up shards only when useful."""

        objective = dspy.InputField(desc="The global user objective.")
        task = dspy.InputField(desc="This node's local shard of work.")
        context = dspy.InputField(desc="Parent context, prior results, and constraints.")
        feedback = dspy.InputField(desc="Selected outputs from prior output phases to reuse or challenge.")
        phase = dspy.InputField(desc="Current feedback pass, swarm id, layer, and sample id.")
        layer = dspy.InputField(desc="Swarm layer: root, review, orchestrate, or leaf.")
        budget_hint = dspy.InputField(desc="Remaining budget and cost policy.")
        result = dspy.OutputField(desc="The best direct answer or partial result for this shard.")
        confidence = dspy.OutputField(desc="A calibrated confidence score from 0.0 to 1.0.")
        followups = dspy.OutputField(desc="JSON array of follow-up task strings; [] if no useful children.")
        critique = dspy.OutputField(desc="Main uncertainty, risk, or reason this result may be dominated.")

    class CompactNodeSignature(dspy.Signature):
        """Solve one shard and return one concise result."""

        objective = dspy.InputField(desc="The global user objective.")
        task = dspy.InputField(desc="This node's local shard of work.")
        context = dspy.InputField(desc="Parent context, prior results, and constraints.")
        feedback = dspy.InputField(desc="Selected outputs from prior output phases to reuse or challenge.")
        phase = dspy.InputField(desc="Current feedback pass, swarm id, layer, and sample id.")
        layer = dspy.InputField(desc="Swarm layer: root, review, orchestrate, or leaf.")
        budget_hint = dspy.InputField(desc="Remaining budget and cost policy.")
        result = dspy.OutputField(desc="A concise answer for this shard.")

    class JudgeSignature(dspy.Signature):
        """Choose the best nondominated candidate from a Pareto frontier."""

        objective = dspy.InputField(desc="The global user objective.")
        candidates = dspy.InputField(desc="JSON list of nondominated candidate summaries.")
        budget_hint = dspy.InputField(desc="Remaining budget and cost policy.")
        winner_id = dspy.OutputField(desc="Candidate id that best trades off quality, cost, and risk.")
        score = dspy.OutputField(desc="Final score from 0.0 to 1.0.")
        rationale = dspy.OutputField(desc="Short reason for the choice.")

    node_signature = CompactNodeSignature if signature_mode == "compact" else RichNodeSignature
    predictor_factory = dspy.ChainOfThought if program_kind == "cot" else dspy.Predict

    class HarnessNode(dspy.Module):
        def __init__(self):
            super().__init__()
            self.predict = predictor_factory(node_signature)

        def forward(
            self,
            objective: str,
            task: str,
            context: str,
            feedback: str,
            phase: str,
            layer: str,
            budget_hint: str,
        ):
            return self.predict(
                objective=objective,
                task=task,
                context=context,
                feedback=feedback,
                phase=phase,
                layer=layer,
                budget_hint=budget_hint,
            )

    class HarnessJudge(dspy.Module):
        def __init__(self):
            super().__init__()
            self.predict = predictor_factory(JudgeSignature)

        def forward(self, objective: str, candidates: str, budget_hint: str):
            return self.predict(objective=objective, candidates=candidates, budget_hint=budget_hint)

    return HarnessNode(), HarnessJudge()


def layer_samples(args: argparse.Namespace, card: ModelCard) -> int:
    base = args.samples
    if card.layer == "leaf":
        base += 1
    if card.layer == "root":
        base = max(1, base - 1)
    return max(1, min(args.max_samples_per_node, math.ceil(base * args.resolution)))


def layer_fanout(args: argparse.Namespace, card: ModelCard) -> int:
    if card.fanout_cap <= 0:
        return 0
    base = args.branching
    if card.layer == "root":
        base = max(base, args.swarms)
    if card.layer == "orchestrate":
        base += 1
    scaled = math.ceil(base * args.resolution)
    return max(0, min(card.fanout_cap, args.max_children, scaled))


def expected_output_tokens(args: argparse.Namespace, card: ModelCard) -> int:
    factor = {"root": 1.25, "review": 1.0, "orchestrate": 0.9, "leaf": 0.55}.get(card.layer, 1.0)
    return max(64, min(card.max_output, int(args.expected_output_tokens * factor)))


def budget_hint(ledger: CostLedger, args: argparse.Namespace, card: ModelCard) -> str:
    remaining = "unbounded" if ledger.budget_usd <= 0 else f"${ledger.remaining_usd:.6f}"
    time_left = time_remaining_seconds(args)
    time_hint = "unbounded" if time_left is None else f"{time_left:.1f}s"
    return (
        f"budget={args.budget_usd:.6f}; spent=${ledger.spent_usd:.6f}; remaining={remaining}; "
        f"spend={args.spend}; time_remaining={time_hint}; feedback_passes={args.feedback_passes}; "
        f"model={card.model}; layer={card.layer}; expected_output_tokens={expected_output_tokens(args, card)}; "
        f"prefer the cheapest sufficient answer and only create followups with positive expected value"
    )


def work_input_tokens(args: argparse.Namespace, work: WorkItem, card: ModelCard, feedback_text: str) -> int:
    prompt = "\n".join(
        [
            args.objective,
            work.task,
            work.context,
            feedback_text,
            phase_label(work, 0),
            card.layer,
            card.role,
            budget_hint(CostLedger(args.budget_usd), args, card),
        ]
    )
    return rough_tokens(prompt)


def phase_label(work: WorkItem, sample_idx: int) -> str:
    sample = "preflight" if sample_idx <= 0 else f"sample={sample_idx}"
    return f"feedback_pass={work.feedback_pass}; swarm={work.swarm_id}; work={work.id}; {sample}"


def sample_utility(record: SampleRecord, card: ModelCard) -> float:
    result_tokens = max(1, record.output_tokens)
    brevity = min(1.0, 900.0 / result_tokens)
    confidence = max(0.0, min(1.0, record.confidence))
    completion = 1.0 if record.result.strip() else 0.0
    risk_penalty = 0.15 if record.error else 0.0
    return max(0.0, (0.72 * confidence + 0.18 * brevity + 0.10 * completion) * card.utility_prior - risk_penalty)


def sample_succeeded(record: SampleRecord) -> bool:
    return not record.error and bool(record.result.strip())


def is_dominated(a: SampleRecord, b: SampleRecord) -> bool:
    return (
        b.utility >= a.utility
        and b.cost_usd <= a.cost_usd
        and b.output_tokens <= a.output_tokens
        and (b.utility > a.utility or b.cost_usd < a.cost_usd or b.output_tokens < a.output_tokens)
    )


def pareto_frontier(records: list[SampleRecord], limit: int) -> list[SampleRecord]:
    records = [rec for rec in records if sample_succeeded(rec)]
    frontier = []
    for rec in records:
        rec.dominated = any(is_dominated(rec, other) for other in records if other is not rec)
        if not rec.dominated:
            frontier.append(rec)
    frontier.sort(key=lambda r: (r.efficiency, r.utility, -r.cost_usd), reverse=True)
    return frontier[:limit]


def dry_followups(work: WorkItem, fanout: int) -> list[str]:
    if fanout <= 0 or work.layer_idx >= len(MODEL_STACK) - 1:
        return []
    next_layer = MODEL_STACK[work.layer_idx + 1].layer
    return [f"{next_layer} shard {i + 1} for {work.task}" for i in range(fanout)]


def dry_sample(
    args: argparse.Namespace,
    work: WorkItem,
    sample_idx: int,
    ledger: CostLedger,
    feedback_text: str,
) -> SampleRecord:
    card = work.card
    input_tokens = work_input_tokens(args, work, card, feedback_text)
    output_tokens = max(24, rough_tokens(work.task) + 18)
    projected = card.estimate_usd(input_tokens, expected_output_tokens(args, card))
    cost = card.estimate_usd(input_tokens, output_tokens)
    followups = dry_followups(work, layer_fanout(args, card))
    confidence = min(0.95, 0.52 + 0.06 * sample_idx + 0.05 * (len(MODEL_STACK) - work.layer_idx))
    feedback_note = " with feedback" if feedback_text else ""
    rec = SampleRecord(
        id=f"{work.id}.s{sample_idx}",
        work_id=work.id,
        swarm_id=work.swarm_id,
        feedback_pass=work.feedback_pass,
        layer=card.layer,
        role=card.role,
        model=card.model,
        task=work.task,
        result=f"[dry-run] {card.layer}/{card.model} sample {sample_idx}{feedback_note}: {work.task}",
        confidence=confidence,
        followups=followups,
        input_tokens=input_tokens,
        output_tokens=output_tokens,
        cost_usd=cost,
        projected_usd=projected,
        utility=0.0,
        efficiency=0.0,
        latency_rank=sample_idx,
    )
    rec.utility = sample_utility(rec, card)
    rec.efficiency = rec.utility / max(rec.cost_usd, 0.000001)
    ledger.add(input_tokens, output_tokens, cost, projected)
    return rec


def live_sample(
    dspy,
    node_program,
    args: argparse.Namespace,
    work: WorkItem,
    sample_idx: int,
    ledger: CostLedger,
    feedback_text: str,
) -> SampleRecord:
    card = work.card
    input_tokens = work_input_tokens(args, work, card, feedback_text)
    projected = card.estimate_usd(input_tokens, expected_output_tokens(args, card))
    if not ledger.can_afford(projected, args.reserve_fraction):
        return SampleRecord(
            id=f"{work.id}.s{sample_idx}",
            work_id=work.id,
            swarm_id=work.swarm_id,
            feedback_pass=work.feedback_pass,
            layer=card.layer,
            role=card.role,
            model=card.model,
            task=work.task,
            result="",
            confidence=0.0,
            followups=[],
            input_tokens=input_tokens,
            output_tokens=0,
            cost_usd=0.0,
            projected_usd=projected,
            utility=0.0,
            efficiency=0.0,
            latency_rank=sample_idx,
            error="budget_gate",
        )

    dspy.configure(lm=make_lm(dspy, args, card), adapter=make_adapter(dspy, args))
    started = time.time()
    error = None
    warning = None
    values: dict[str, Any]
    try:
        prediction = node_program(
            objective=args.objective,
            task=work.task,
            context=work.context,
            feedback=feedback_text,
            phase=phase_label(work, sample_idx),
            layer=f"{card.layer}/{card.role}",
            budget_hint=budget_hint(ledger, args, card),
        )
        values = prediction_to_dict(prediction)
    except Exception as exc:
        error_text = f"{type(exc).__name__}: {exc}"
        values = prediction_from_adapter_error(error_text)
        if values.get("result"):
            values.setdefault("confidence", 0.35)
            values.setdefault("followups", [])
            warning = f"partial_adapter_parse: {type(exc).__name__}"
        else:
            values = {"result": "", "confidence": 0.0, "followups": []}
            error = error_text

    result = str(values.get("result") or values.get("answer") or values.get("output") or "").strip()
    if not error and not result:
        error = "empty_result"
    confidence = parse_confidence(values.get("confidence"))
    followups = parse_followups(values.get("followups"))
    output_text = json.dumps(values, ensure_ascii=False, sort_keys=True)
    output_tokens = rough_tokens(output_text)
    actual_cost = card.estimate_usd(input_tokens, output_tokens)
    rec = SampleRecord(
        id=f"{work.id}.s{sample_idx}",
        work_id=work.id,
        swarm_id=work.swarm_id,
        feedback_pass=work.feedback_pass,
        layer=card.layer,
        role=card.role,
        model=card.model,
        task=work.task,
        result=result,
        confidence=confidence,
        followups=followups,
        input_tokens=input_tokens,
        output_tokens=output_tokens,
        cost_usd=actual_cost,
        projected_usd=projected,
        utility=0.0,
        efficiency=0.0,
        latency_rank=max(1, int((time.time() - started) * 1000)),
        error=error,
        warning=warning,
    )
    rec.utility = sample_utility(rec, card)
    rec.efficiency = rec.utility / max(rec.cost_usd, 0.000001)
    ledger.add(input_tokens, output_tokens, actual_cost, projected)
    return rec


def child_context(parent: WorkItem, selected: SampleRecord) -> str:
    parts = []
    if parent.context:
        parts.append(parent.context)
    parts.append(f"parent={parent.id} layer={selected.layer} model={selected.model}")
    parts.append(f"parent_result={selected.result[:1800]}")
    return "\n".join(parts)


def seed_work_queue(args: argparse.Namespace, feedback_pass: int) -> list[WorkItem]:
    return [
        WorkItem(
            id=f"swarm{swarm_id}.pass{feedback_pass}.root",
            swarm_id=swarm_id,
            layer_idx=0,
            task=args.objective,
            feedback_pass=feedback_pass,
            context=args.context,
        )
        for swarm_id in range(1, args.swarms + 1)
    ]


def run_harness(args: argparse.Namespace) -> dict[str, Any]:
    ledger = CostLedger(args.budget_usd)
    all_samples: list[SampleRecord] = []
    work_summaries: list[dict[str, Any]] = []
    pass_summaries: list[dict[str, Any]] = []
    feedback_memory = FeedbackMemory(args.feedback_frontier_size)
    dspy = None
    node_program = None
    judge_program = None
    processed = 0
    stop_reason = "queue_empty"
    args._started_at = time.monotonic()

    if not args.dry_run:
        preflight_gateway(args.base_url, args.preflight_timeout, args.gateway_api_key)
        dspy = import_dspy()
        node_program, judge_program = build_dspy_abstractions(dspy, args.program, args.signature_mode)

    for feedback_pass in range(1, args.feedback_passes + 1):
        if processed >= args.max_nodes:
            stop_reason = "max_nodes"
            break
        if time_budget_expired(args):
            stop_reason = "time_budget"
            break

        work_queue = seed_work_queue(args, feedback_pass)
        pass_sample_start = len(all_samples)
        pass_processed_start = processed
        halted = False

        while work_queue and processed < args.max_nodes:
            if time_budget_expired(args):
                stop_reason = "time_budget"
                halted = True
                break

            work = work_queue.pop(0)
            if work.layer_idx > args.max_depth:
                continue
            card = work.card
            samples: list[SampleRecord] = []
            feedback_text = feedback_memory.render(args.feedback_token_limit) if args.feedback_enabled else ""

            for sample_idx in range(1, layer_samples(args, card) + 1):
                if time_budget_expired(args):
                    stop_reason = "time_budget"
                    halted = True
                    break
                projected = card.estimate_usd(
                    work_input_tokens(args, work, card, feedback_text),
                    expected_output_tokens(args, card),
                )
                if not ledger.can_afford(projected, args.reserve_fraction):
                    stop_reason = "budget_gate"
                    halted = True
                    break
                if args.dry_run:
                    rec = dry_sample(args, work, sample_idx, ledger, feedback_text)
                else:
                    rec = live_sample(dspy, node_program, args, work, sample_idx, ledger, feedback_text)
                samples.append(rec)
                all_samples.append(rec)

            frontier = pareto_frontier(samples, args.frontier_size)
            selected = frontier[: args.beam_width]
            if args.feedback_enabled:
                feedback_memory.add(selected or frontier)
            work_summaries.append(
                {
                    "work_id": work.id,
                    "swarm_id": work.swarm_id,
                    "feedback_pass": work.feedback_pass,
                    "layer": card.layer,
                    "role": card.role,
                    "model": card.model,
                    "task": work.task,
                    "sample_count": len(samples),
                    "feedback_input_tokens": rough_tokens(feedback_text) if feedback_text else 0,
                    "frontier": [record_summary(r) for r in frontier],
                }
            )

            if work.layer_idx < args.max_depth and not halted:
                for rec in selected:
                    fanout = min(layer_fanout(args, card), len(rec.followups))
                    next_layer = MODEL_STACK[work.layer_idx + 1].layer
                    sample_tag = rec.id.rsplit(".", 1)[-1]
                    for idx, task in enumerate(rec.followups[:fanout], start=1):
                        child_id = f"{work.id}.{sample_tag}.{next_layer}{idx}"
                        work_queue.append(
                            WorkItem(
                                id=child_id,
                                swarm_id=work.swarm_id,
                                layer_idx=work.layer_idx + 1,
                                task=task,
                                feedback_pass=work.feedback_pass,
                                context=child_context(work, rec),
                                parent_id=work.id,
                                ancestry=work.ancestry + [work.id],
                            )
                        )
            processed += 1

            if halted:
                break

        pass_records = all_samples[pass_sample_start:]
        pass_frontier = pareto_frontier(pass_records, args.frontier_size)
        pass_summaries.append(
            {
                "feedback_pass": feedback_pass,
                "work_items": processed - pass_processed_start,
                "sample_count": len(pass_records),
                "frontier": [record_summary(r) for r in pass_frontier],
                "feedback_items_after_pass": len(feedback_memory.records),
            }
        )

        if halted:
            break

    if processed >= args.max_nodes and stop_reason not in {"time_budget", "budget_gate"}:
        stop_reason = "max_nodes"

    sample_errors = sum(1 for rec in all_samples if rec.error)
    sample_warnings = sum(1 for rec in all_samples if rec.warning)
    successful_samples = [rec for rec in all_samples if sample_succeeded(rec)]
    if not successful_samples and sample_errors and stop_reason == "queue_empty":
        stop_reason = "sample_error"

    global_frontier = pareto_frontier(all_samples, args.frontier_size)
    judge = None
    if args.judge and not args.dry_run and len(global_frontier) > 1 and judge_program is not None:
        judge_card = card_for_model(args.judge_model)
        projected = judge_card.estimate_usd(
            rough_tokens(args.objective + json.dumps([record_summary(r) for r in global_frontier])),
            expected_output_tokens(args, judge_card),
        )
        if ledger.can_afford(projected, args.reserve_fraction):
            dspy.configure(lm=make_lm(dspy, args, judge_card), adapter=make_adapter(dspy, args))
            try:
                pred = judge_program(
                    objective=args.objective,
                    candidates=json.dumps([record_summary(r) for r in global_frontier], ensure_ascii=False),
                    budget_hint=budget_hint(ledger, args, judge_card),
                )
                judge = prediction_to_dict(pred)
            except Exception as exc:
                judge = {"error": f"{type(exc).__name__}: {exc}"}

    best = choose_best(global_frontier)
    return {
        "ok": bool(successful_samples),
        "mode": "dry-run" if args.dry_run else "live",
        "objective": args.objective,
        "config": {
            "spend": args.spend,
            "swarms": args.swarms,
            "resolution": args.resolution,
            "branching": args.branching,
            "samples": args.samples,
            "max_depth": args.max_depth,
            "max_nodes": args.max_nodes,
            "beam_width": args.beam_width,
            "frontier_size": args.frontier_size,
            "budget_usd": args.budget_usd,
            "time_budget_seconds": args.time_budget_seconds,
            "feedback_enabled": args.feedback_enabled,
            "feedback_passes": args.feedback_passes,
            "feedback_frontier_size": args.feedback_frontier_size,
            "feedback_token_limit": args.feedback_token_limit,
            "signature_mode": args.signature_mode,
            "adapter": args.adapter,
        },
        "runtime": {
            "elapsed_seconds": round(time.monotonic() - args._started_at, 4),
            "stop_reason": stop_reason,
            "processed_work_items": processed,
            "feedback_items": len(feedback_memory.records),
            "sample_errors": sample_errors,
            "sample_warnings": sample_warnings,
            "successful_samples": len(successful_samples),
        },
        "ledger": {
            "spent_usd": round(ledger.spent_usd, 8),
            "projected_usd": round(ledger.projected_usd, 8),
            "remaining_usd": None if ledger.budget_usd <= 0 else round(ledger.remaining_usd, 8),
            "input_tokens": ledger.input_tokens,
            "output_tokens": ledger.output_tokens,
        },
        "stack": [card.__dict__ for card in MODEL_STACK],
        "passes": pass_summaries,
        "feedback_memory": [record_summary(r) for r in feedback_memory.records],
        "work": work_summaries,
        "samples": [record_summary(r) for r in all_samples],
        "pareto_frontier": [record_summary(r) for r in global_frontier],
        "best": record_summary(best) if best else None,
        "judge": judge,
    }


def choose_best(frontier: list[SampleRecord]) -> SampleRecord | None:
    if not frontier:
        return None
    return max(frontier, key=lambda r: (r.efficiency, r.utility, -r.cost_usd))


def record_summary(record: SampleRecord) -> dict[str, Any]:
    return {
        "id": record.id,
        "work_id": record.work_id,
        "swarm_id": record.swarm_id,
        "feedback_pass": record.feedback_pass,
        "layer": record.layer,
        "role": record.role,
        "model": record.model,
        "task": record.task,
        "confidence": round(record.confidence, 4),
        "utility": round(record.utility, 6),
        "efficiency": round(record.efficiency, 6),
        "cost_usd": round(record.cost_usd, 8),
        "projected_usd": round(record.projected_usd, 8),
        "input_tokens": record.input_tokens,
        "output_tokens": record.output_tokens,
        "followup_count": len(record.followups),
        "dominated": record.dominated,
        "error": record.error,
        "warning": record.warning,
        "result": record.result,
    }


def layer_color(layer: str) -> str:
    return {
        "root": "#8b5cf6",
        "review": "#ef4444",
        "orchestrate": "#2563eb",
        "leaf": "#059669",
    }.get(layer, "#64748b")


def plot_float(value: Any, default: float = 0.0) -> float:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    if math.isnan(out) or math.isinf(out):
        return default
    return out


def plot_label(text: str, max_len: int = 96) -> str:
    text = " ".join(str(text or "").split())
    if len(text) <= max_len:
        return text
    return text[: max_len - 3] + "..."


def write_frontier_svg(result: dict[str, Any], path: str, scale: str) -> None:
    samples = list(result.get("samples") or [])
    frontier_ids = {rec.get("id") for rec in result.get("pareto_frontier") or []}
    best_id = (result.get("best") or {}).get("id")
    if not samples:
        samples = list(result.get("pareto_frontier") or [])
    mode = str(result.get("mode") or "")
    runtime = result.get("runtime") or {}
    sample_errors = int(plot_float(runtime.get("sample_errors"), 0.0))
    sample_warnings = int(plot_float(runtime.get("sample_warnings"), 0.0))

    width = 1200
    height = 760
    left = 96
    right = 48
    top = 88
    bottom = 96
    plot_w = width - left - right
    plot_h = height - top - bottom

    costs = [max(plot_float(rec.get("cost_usd")), 0.0) for rec in samples]
    utilities = [plot_float(rec.get("utility")) for rec in samples]
    positive_costs = [c for c in costs if c > 0]
    min_positive = min(positive_costs) if positive_costs else 1e-9

    def x_raw(cost: float) -> float:
        if scale == "log":
            return math.log10(max(cost, min_positive * 0.5))
        return cost

    xs = [x_raw(c) for c in costs] or [0.0]
    ys = utilities or [0.0]
    x_min = min(xs)
    x_max = max(xs)
    y_min = min(0.0, min(ys))
    y_max = max(1.0, max(ys))
    if x_min == x_max:
        x_min -= 0.5
        x_max += 0.5
    if y_min == y_max:
        y_min -= 0.5
        y_max += 0.5
    y_pad = (y_max - y_min) * 0.08
    y_min -= y_pad
    y_max += y_pad

    def sx(cost: float) -> float:
        return left + ((x_raw(cost) - x_min) / (x_max - x_min)) * plot_w

    def sy(utility: float) -> float:
        return top + (1.0 - ((utility - y_min) / (y_max - y_min))) * plot_h

    def cost_tick_label(raw: float) -> str:
        cost = 10 ** raw if scale == "log" else raw
        if cost >= 0.01:
            return f"${cost:.3f}"
        if cost >= 0.0001:
            return f"${cost:.5f}"
        return f"${cost:.1e}"

    title = "Pareto Frontier"
    if mode == "dry-run":
        title = "Pareto Frontier (dry-run simulation)"
    lines: list[str] = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f8fafc"/>',
        f'<text x="{left}" y="42" font-family="Arial, sans-serif" font-size="24" font-weight="700" fill="#0f172a">{html_escape(title)}</text>',
        f'<text x="{left}" y="68" font-family="Arial, sans-serif" font-size="13" fill="#475569">mode={html_escape(mode)} {html_escape(plot_label(result.get("objective", ""), 128))}</text>',
        f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#ffffff" stroke="#cbd5e1"/>',
    ]

    for i in range(6):
        x = left + (plot_w * i / 5)
        raw = x_min + ((x_max - x_min) * i / 5)
        lines.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#e2e8f0"/>')
        lines.append(
            f'<text x="{x:.2f}" y="{top + plot_h + 26}" text-anchor="middle" '
            f'font-family="Arial, sans-serif" font-size="11" fill="#64748b">{cost_tick_label(raw)}</text>'
        )
    for i in range(6):
        y = top + (plot_h * i / 5)
        val = y_max - ((y_max - y_min) * i / 5)
        lines.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e2e8f0"/>')
        lines.append(
            f'<text x="{left - 16}" y="{y + 4:.2f}" text-anchor="end" '
            f'font-family="Arial, sans-serif" font-size="11" fill="#64748b">{val:.2f}</text>'
        )

    lines.append(
        f'<text x="{left + plot_w / 2:.2f}" y="{height - 34}" text-anchor="middle" '
        f'font-family="Arial, sans-serif" font-size="13" fill="#334155">cost_usd ({html_escape(scale)} scale)</text>'
    )
    lines.append(
        f'<text x="28" y="{top + plot_h / 2:.2f}" text-anchor="middle" transform="rotate(-90 28 {top + plot_h / 2:.2f})" '
        f'font-family="Arial, sans-serif" font-size="13" fill="#334155">utility</text>'
    )

    for rec in samples:
        if rec.get("id") in frontier_ids:
            continue
        if rec.get("error"):
            continue
        x = sx(max(plot_float(rec.get("cost_usd")), 0.0))
        y = sy(plot_float(rec.get("utility")))
        title = html_escape(f"{rec.get('id')} {rec.get('layer')} cost=${plot_float(rec.get('cost_usd')):.8f} utility={plot_float(rec.get('utility')):.4f}")
        lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4" fill="#94a3b8" opacity="0.38"><title>{title}</title></circle>')

    for rec in samples:
        if not rec.get("error"):
            continue
        x = sx(max(plot_float(rec.get("cost_usd")), 0.0))
        y = sy(plot_float(rec.get("utility")))
        title = html_escape(
            f"{rec.get('id')} {rec.get('layer')} cost=${plot_float(rec.get('cost_usd')):.8f} "
            f"utility={plot_float(rec.get('utility')):.4f} error={rec.get('error')}"
        )
        lines.append(f'<g stroke="#dc2626" stroke-width="2" opacity="0.82"><title>{title}</title>')
        lines.append(f'<line x1="{x - 5:.2f}" y1="{y - 5:.2f}" x2="{x + 5:.2f}" y2="{y + 5:.2f}"/>')
        lines.append(f'<line x1="{x - 5:.2f}" y1="{y + 5:.2f}" x2="{x + 5:.2f}" y2="{y - 5:.2f}"/>')
        lines.append("</g>")

    frontier = sorted(
        [rec for rec in samples if rec.get("id") in frontier_ids],
        key=lambda rec: (plot_float(rec.get("cost_usd")), plot_float(rec.get("utility"))),
    )
    if len(frontier) > 1:
        points = " ".join(
            f'{sx(max(plot_float(rec.get("cost_usd")), 0.0)):.2f},{sy(plot_float(rec.get("utility"))):.2f}'
            for rec in frontier
        )
        lines.append(f'<polyline points="{points}" fill="none" stroke="#0f172a" stroke-width="2.5" opacity="0.8"/>')

    for rec in frontier:
        x = sx(max(plot_float(rec.get("cost_usd")), 0.0))
        y = sy(plot_float(rec.get("utility")))
        layer = str(rec.get("layer") or "")
        color = layer_color(layer)
        radius = 8 if rec.get("id") == best_id else 6
        stroke = "#f59e0b" if rec.get("id") == best_id else "#0f172a"
        title = html_escape(
            f"{rec.get('id')} {layer}/{rec.get('model')} cost=${plot_float(rec.get('cost_usd')):.8f} "
            f"utility={plot_float(rec.get('utility')):.4f} efficiency={plot_float(rec.get('efficiency')):.2f}"
        )
        lines.append(
            f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius}" fill="{color}" stroke="{stroke}" '
            f'stroke-width="2"><title>{title}</title></circle>'
        )
        lines.append(
            f'<text x="{x + 10:.2f}" y="{y - 8:.2f}" font-family="Arial, sans-serif" '
            f'font-size="10" fill="#334155">{html_escape(layer)}</text>'
        )

    legend_x = left + plot_w - 288
    legend_y = top + 22
    lines.append(f'<g font-family="Arial, sans-serif" font-size="12" fill="#334155">')
    lines.append(f'<rect x="{legend_x - 12}" y="{legend_y - 18}" width="280" height="132" fill="#ffffff" stroke="#cbd5e1" opacity="0.94"/>')
    lines.append(f'<text x="{legend_x}" y="{legend_y}" font-weight="700">layers</text>')
    for idx, layer in enumerate(["root", "review", "orchestrate", "leaf"]):
        y = legend_y + 22 + idx * 18
        lines.append(f'<circle cx="{legend_x + 6}" cy="{y - 4}" r="5" fill="{layer_color(layer)}"/>')
        lines.append(f'<text x="{legend_x + 18}" y="{y}">{layer}</text>')
    lines.append(f'<circle cx="{legend_x + 156}" cy="{legend_y + 18}" r="4" fill="#94a3b8" opacity="0.38"/>')
    lines.append(f'<text x="{legend_x + 168}" y="{legend_y + 22}">dominated samples</text>')
    lines.append(f'<circle cx="{legend_x + 156}" cy="{legend_y + 40}" r="7" fill="#2563eb" stroke="#f59e0b" stroke-width="2"/>')
    lines.append(f'<text x="{legend_x + 168}" y="{legend_y + 44}">selected best</text>')
    lines.append(f'<g stroke="#dc2626" stroke-width="2" opacity="0.82">')
    lines.append(f'<line x1="{legend_x + 151}" y1="{legend_y + 56}" x2="{legend_x + 161}" y2="{legend_y + 66}"/>')
    lines.append(f'<line x1="{legend_x + 151}" y1="{legend_y + 66}" x2="{legend_x + 161}" y2="{legend_y + 56}"/>')
    lines.append("</g>")
    lines.append(f'<text x="{legend_x + 168}" y="{legend_y + 66}">failed samples</text>')
    lines.append("</g>")

    ledger = result.get("ledger") or {}
    footer = (
        f"samples={len(samples)} frontier={len(frontier)} "
        f"ok={str(bool(result.get('ok'))).lower()} errors={sample_errors} warnings={sample_warnings} "
        f"stop={runtime.get('stop_reason')} spent=${plot_float(ledger.get('spent_usd')):.8f} "
        f"projected=${plot_float(ledger.get('projected_usd')):.8f}"
    )
    lines.append(
        f'<text x="{left}" y="{height - 14}" font-family="Arial, sans-serif" font-size="12" fill="#64748b">{html_escape(footer)}</text>'
    )
    lines.append("</svg>")

    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def print_text_report(result: dict[str, Any]) -> None:
    print(f"mode={result['mode']} objective={result['objective']}")
    cfg = result["config"]
    runtime = result["runtime"]
    ledger = result["ledger"]
    print(
        "config "
        f"spend={cfg['spend']} swarms={cfg['swarms']} resolution={cfg['resolution']} branching={cfg['branching']} "
        f"samples={cfg['samples']} depth={cfg['max_depth']} passes={cfg['feedback_passes']} "
        f"time={cfg['time_budget_seconds']:.1f}s budget=${cfg['budget_usd']:.4f}"
    )
    print(
        "runtime "
        f"elapsed={runtime['elapsed_seconds']:.4f}s stop={runtime['stop_reason']} "
        f"work_items={runtime['processed_work_items']} feedback_items={runtime['feedback_items']} "
        f"success={runtime.get('successful_samples', 0)} errors={runtime.get('sample_errors', 0)} "
        f"warnings={runtime.get('sample_warnings', 0)}"
    )
    print(
        "ledger "
        f"spent=${ledger['spent_usd']:.8f} projected=${ledger['projected_usd']:.8f} "
        f"tokens={ledger['input_tokens']}/{ledger['output_tokens']}"
    )
    print("stack")
    for card in result["stack"]:
        print(
            f"  {card['layer']:<12} {card['role']:<10} {card['model']:<18} "
            f"fanout_cap={card['fanout_cap']} price={card['input_price']:g}/{card['output_price']:g}"
        )
    print("pareto_frontier")
    if not result["pareto_frontier"]:
        print("  (none)")
    for rec in result["pareto_frontier"]:
        print(
            f"  {rec['id']:<28} pass={rec['feedback_pass']:<2} {rec['layer']:<12} {rec['model']:<18} "
            f"utility={rec['utility']:.4f} efficiency={rec['efficiency']:.2f} "
            f"cost=${rec['cost_usd']:.8f} conf={rec['confidence']:.2f}"
        )
    failed = [rec for rec in result.get("samples", []) if rec.get("error")]
    if failed:
        print("sample_errors")
        for rec in failed[:3]:
            error = " ".join(str(rec.get("error") or "").split())
            print(f"  {rec['id']} {rec['layer']}/{rec['model']} {error[:220]}")
        if len(failed) > 3:
            print(f"  ... {len(failed) - 3} more")
    best = result.get("best")
    if best:
        print("best")
        print(f"  {best['id']} {best['layer']}/{best['model']} cost=${best['cost_usd']:.8f}")
        print(f"  {best['result']}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="One-file DSPy multi-swarm sampling harness for dsco.")
    parser.add_argument("prompt", nargs="*", help="objective text; stdin is used when omitted")
    parser.add_argument("--context", default="", help="optional global context included in every root swarm")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help="dsco gateway base URL")
    parser.add_argument("--gateway-api-key", default=os.getenv("DSCO_WEB_TOKEN", DEFAULT_GATEWAY_KEY))
    parser.add_argument("--preflight-timeout", type=float, default=2.0)
    parser.add_argument("--dry-run", action="store_true", help="plan and cost the run without provider calls")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of text")
    parser.add_argument("--plot-frontier", default="", help="write a Pareto frontier SVG to this path")
    parser.add_argument("--plot-scale", choices=["linear", "log"], default="log", help="x-axis scale for --plot-frontier")
    parser.add_argument("--list-models", action="store_true", help="show the built-in four-layer model stack")
    parser.add_argument("--program", choices=["predict", "cot"], default="cot", help="DSPy predictor abstraction")
    parser.add_argument(
        "--signature-mode",
        choices=["compact", "rich"],
        default="rich",
        help="compact asks DSPy for only result; rich also requires confidence/followups/critique",
    )
    parser.add_argument("--adapter", choices=["chat", "json", "xml"], default="chat", help="DSPy adapter")
    parser.add_argument(
        "--no-json-adapter-fallback",
        action="store_true",
        help="for --adapter chat, disable DSPy's automatic JSONAdapter retry on parse failure",
    )
    parser.add_argument("--spend", choices=sorted(SPEND_PRESETS), default="normal", help="wall-clock/feedback preset")
    parser.add_argument(
        "--time-budget-seconds",
        type=float,
        default=None,
        help="soft wall-clock cap; 0 disables the time cap; default comes from --spend",
    )
    parser.add_argument(
        "--feedback-passes",
        type=int,
        default=None,
        help="root-to-leaf passes with previous outputs fed back into new inputs; default comes from --spend",
    )
    parser.add_argument("--no-feedback", dest="feedback_enabled", action="store_false", help="disable output-to-input feedback")
    parser.set_defaults(feedback_enabled=True)
    parser.add_argument("--feedback-frontier-size", type=int, default=8, help="max prior outputs kept as feedback memory")
    parser.add_argument("--feedback-token-limit", type=int, default=1800, help="rough token cap for feedback injected into inputs")
    parser.add_argument("--swarms", type=int, default=1, help="independent root swarms")
    parser.add_argument("--resolution", type=float, default=1.0, help="scale fanout and samples")
    parser.add_argument("--branching", type=int, default=2, help="base child tasks per non-leaf node")
    parser.add_argument("--samples", type=int, default=2, help="base samples per node")
    parser.add_argument("--max-samples-per-node", type=int, default=8)
    parser.add_argument("--max-children", type=int, default=8)
    parser.add_argument("--max-depth", type=int, default=3, help="0=root only, 3=all four layers")
    parser.add_argument("--max-nodes", type=int, default=64, help="absolute node execution cap")
    parser.add_argument("--beam-width", type=int, default=2, help="nondominated samples expanded per node")
    parser.add_argument("--frontier-size", type=int, default=12, help="Pareto frontier cap")
    parser.add_argument("--budget-usd", type=float, default=0.25)
    parser.add_argument("--reserve-fraction", type=float, default=0.12, help="budget kept unspent for synthesis/recovery")
    parser.add_argument("--expected-output-tokens", type=int, default=900)
    parser.add_argument("--max-output-tokens", type=int, default=1200)
    parser.add_argument("--temperature", type=float, default=None, help="only sent for tunable models such as Haiku")
    parser.add_argument("--reasoning-effort", choices=["low", "medium", "high", "xhigh", "max"], default=None)
    parser.add_argument("--judge", action="store_true", help="optionally ask a DSPy judge to choose among frontier items")
    parser.add_argument("--judge-model", default="opus", help="model/layer for --judge")
    return parser


def prepare_args(parser: argparse.ArgumentParser, args: argparse.Namespace, read_stdin: bool = True) -> argparse.Namespace:
    objective = " ".join(args.prompt).strip()
    if not objective and read_stdin and not sys.stdin.isatty():
        objective = sys.stdin.read().strip()
    if not objective:
        parser.error("missing objective prompt")
    args.objective = objective
    preset = SPEND_PRESETS[args.spend]
    if args.time_budget_seconds is None:
        args.time_budget_seconds = preset["time_budget_seconds"]
    if args.feedback_passes is None:
        args.feedback_passes = preset["feedback_passes"]

    if args.swarms <= 0:
        parser.error("--swarms must be positive")
    if args.resolution <= 0:
        parser.error("--resolution must be positive")
    if args.max_depth < 0:
        parser.error("--max-depth must be >= 0")
    args.max_depth = min(args.max_depth, len(MODEL_STACK) - 1)
    if args.budget_usd < 0:
        parser.error("--budget-usd must be >= 0")
    if args.time_budget_seconds < 0:
        parser.error("--time-budget-seconds must be >= 0")
    if args.feedback_passes <= 0:
        parser.error("--feedback-passes must be positive")
    if args.feedback_frontier_size <= 0:
        parser.error("--feedback-frontier-size must be positive")
    if args.feedback_token_limit < 0:
        parser.error("--feedback-token-limit must be >= 0")
    return args


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if args.list_models:
        for card in MODEL_STACK:
            print(
                f"{card.layer:<12} {card.role:<10} {card.model:<18} "
                f"effort={card.effort:<5} fanout_cap={card.fanout_cap:<2} "
                f"price={card.input_price:g}/{card.output_price:g}"
            )
        return

    args = prepare_args(parser, args)

    result = run_harness(args)
    if args.plot_frontier:
        write_frontier_svg(result, args.plot_frontier, args.plot_scale)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print_text_report(result)
    if not args.dry_run and not result.get("ok"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
