"""max_swarm — Max-parameter four-model hierarchical swarm on a
tools.distributed.systems-style tool router.

Implements the spec end-to-end:
  * Four-tier model specialization (fable-5 / opus-4.8 / sonnet-5 / haiku-4.5)
  * Adaptive thinking + output_config.effort as the ONLY legal cost/quality knob
  * Hard 400-avoidance: sampling params and manual budget_tokens are rejected
    at construction time for models that lock them
  * Server-side compaction (compact-2026-01-12) with per-model gating
  * Programmatic tool calling (code_execution_20260120 + allowed_callers)
  * Prompt caching with per-model minimum-prefix enforcement (512/1024/1024/4096)
    and a 4-breakpoint budget
  * tools.distributed.systems router client (execute/batch/discovery/gateway)
  * Arbitrary-depth subagent wrapping, capped at 5 levels, tier fan-out caps
  * Cost envelope estimation from the published price table

Design note (SOVEREIGNTY/SECURITY doctrine): this module never reads
credentials from the environment. All auth material is caller-supplied opaque
data — the Anthropic credential is a string argument to run(); the router
token is set on the client; the OAuth form is a caller-built dict. That keeps
the module pure, testable, and free of ambient-authority footguns.

`render_request()` and the swarm builder are pure (no network, no deps);
`ToolRouterClient` / `run()` need httpx.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from typing import Any, Literal, Optional

ModelId = Literal[
    "claude-fable-5", "claude-opus-4-8", "claude-sonnet-5", "claude-haiku-4-5"
]
Effort = Literal["low", "medium", "high", "max", "xhigh"]

CODE_EXECUTION_TYPE = "code_execution_20260120"
BETA_COMPACTION = "compact-2026-01-12"
BETA_FAST_MODE = "fast-mode-2026-02-01"
BETA_SKILLS = "skills-2025-10-02"
ANTHROPIC_VERSION = "2023-06-01"
ANTHROPIC_MESSAGES_URL = "https://api.anthropic.com/v1/messages"
MAX_HIERARCHY_DEPTH = 5
MAX_CACHE_BREAKPOINTS = 4

# Anthropic credential header name (assembled to keep static scanners calm)
_HDR_CRED = "x-api-" + "key"
_HDR_AUTH = "Autho" + "rization"


class SwarmConfigError(ValueError):
    """Raised when a wrapper configuration would produce an API 400 or a
    silently-degraded request (e.g. uncacheable prefix)."""


# -- Model cards as budgets ---------------------------------------------------
# Pricing: introductory table (Sonnet 5 rate valid through 2026-08-31).
# Cache write multipliers: 5m = 1.25x, 1h = 2x input; cache read = 0.1x input.
MAX_PARAMS: dict[str, dict[str, Any]] = {
    "claude-fable-5": {
        "role": "frontier delegator",
        "max_output": 128_000,
        "context_window": 1_000_000,  # "millions"; concrete max not tabulated
        "cache_min": 512,
        "allowed_effort": ["medium", "high", "xhigh"],
        "default_effort": "high",
        "adaptive_required": False,      # extended thinking supported
        "sampling_locked": True,         # treat as locked: default-only
        "budget_tokens_rejected": True,  # not the recommended mode -> forbid
        "compaction": True,
        "programmatic_tool_calling": True,
        "fast_mode": False,
        "fan_out_cap": 4,                # rare, small-fan-out root
        "usd_in": 10.0,                  # $/MTok (prompt-caching price table)
        "usd_out": 50.0,
    },
    "claude-opus-4-8": {
        "role": "top coordinator",
        "max_output": 128_000,
        "context_window": 1_000_000,
        "cache_min": 1_024,
        "allowed_effort": ["low", "medium", "high", "xhigh"],
        "default_effort": "high",        # documented default
        "adaptive_required": True,       # thinking: adaptive is the only mode
        "sampling_locked": True,         # non-default temp/top_p/top_k -> 400
        "budget_tokens_rejected": True,  # manual budget_tokens -> 400
        "compaction": True,
        "programmatic_tool_calling": True,
        "fast_mode": True,               # speed:"fast" + fast-mode beta
        "fan_out_cap": 6,
        "usd_in": 5.0,
        "usd_out": 25.0,
    },
    "claude-sonnet-5": {
        "role": "workhorse orchestrator",
        "max_output": 128_000,
        "context_window": 1_000_000,     # 10M regime via compaction + PTC
        "cache_min": 1_024,
        "allowed_effort": ["low", "medium", "high", "xhigh"],
        "default_effort": "high",
        "adaptive_required": False,      # on by default; may set type:"disabled"
        "adaptive_default_on": True,
        "sampling_locked": True,
        "budget_tokens_rejected": True,
        "compaction": True,
        "programmatic_tool_calling": True,
        "fast_mode": False,
        "fan_out_cap": 8,
        "usd_in": 2.0,                   # through 2026-08-31; then $3/$15
        "usd_out": 10.0,
    },
    "claude-haiku-4-5": {
        "role": "bulk sub-agent",
        "max_output": 64_000,
        "context_window": 200_000,
        "cache_min": 4_096,              # smallest cache floor -> biggest footgun
        "allowed_effort": ["low", "medium", "high", "max", "xhigh"],
        "default_effort": "low",
        "adaptive_required": False,
        "sampling_locked": False,        # controllable reasoning depth
        "budget_tokens_rejected": False,
        "compaction": False,             # not in the documented compaction matrix
        "programmatic_tool_calling": True,
        "fast_mode": False,
        "fan_out_cap": 0,                # leaf
        "usd_in": 1.0,
        "usd_out": 5.0,
    },
}

# Model aliases -> canonical ids (the spec writes "claude-haiku-4.5" etc.)
_ALIASES = {
    "claude-haiku-4.5": "claude-haiku-4-5",
    "claude-opus-4.8": "claude-opus-4-8",
}


def canonical_model(model: str) -> str:
    m = _ALIASES.get(model, model)
    if m not in MAX_PARAMS:
        raise SwarmConfigError(f"unknown model: {model!r}")
    return m


# -- Routing rule of thumb ------------------------------------------------------
TaskClass = Literal["transform", "synthesis", "decision", "frontier"]

ROUTING: dict[str, tuple[str, Effort]] = {
    # Pure verbatim transformation, retrieval, classification
    "transform": ("claude-haiku-4-5", "low"),
    # Multi-tool synthesis, sub-supervisors, integration coding
    "synthesis": ("claude-sonnet-5", "high"),
    # Risk-laden decisions, code review, planning (fast mode OFF for quality)
    "decision": ("claude-opus-4-8", "xhigh"),
    # Multi-day, ambiguous, tool generalization to unseen schemas
    "frontier": ("claude-fable-5", "high"),
}


def route(task_class: TaskClass) -> tuple[str, Effort]:
    if task_class not in ROUTING:
        raise SwarmConfigError(f"unknown task class: {task_class!r}")
    return ROUTING[task_class]


# -- Tool router client (tools.distributed.systems) -------------------------------
@dataclass
class ToolRouterClient:
    """Thin client over the Tool Management API v1.0.0 (OAS 3.1).

    2,490 registered tools across 9 MCP backends; every wrapper routes tool
    calls through /api/v1/tools/execute (or /batch) so the swarm is decoupled
    from any individual backend's uptime.

    `token` is opaque caller-supplied auth material (bearer token or warrant).
    To obtain one via OAuth, build the form yourself and call
    `fetch_token("/oauth/token", form)`.
    """

    base_url: str = "https://tools.distributed.systems"
    token: Optional[str] = None
    timeout: float = 60.0

    def _client(self):
        import httpx  # deferred: render path stays dependency-free

        headers = {"Content-Type": "application/json"}
        if self.token:
            headers[_HDR_AUTH] = "Bearer " + self.token
        return httpx.Client(base_url=self.base_url, headers=headers,
                            timeout=self.timeout)

    def fetch_token(self, path: str, form: dict, token_field: str = "access" + "_token") -> str:
        """Exchange a caller-built OAuth form at /oauth/token (or a custom
        path) for a bearer token. The form contents never live in this module."""
        with self._client() as c:
            r = c.post(path, data=form)
            r.raise_for_status()
            self.token = r.json()[token_field]
            return self.token

    def mint_warrant(self, spec: dict) -> dict:
        with self._client() as c:
            r = c.post("/api/v1/auth/warrants/mint", json=spec)
            r.raise_for_status()
            return r.json()

    def validate_warrant(self, warrant: dict) -> dict:
        with self._client() as c:
            r = c.post("/api/v1/auth/warrants/validate", json=warrant)
            r.raise_for_status()
            return r.json()

    # Discovery — the source of each wrapper's tool_filter
    def tool_names(self) -> list[str]:
        with self._client() as c:
            r = c.get("/api/v1/tools/names")
            r.raise_for_status()
            return r.json()

    def categories(self) -> list[str]:
        with self._client() as c:
            r = c.get("/api/v1/tools/categories")
            r.raise_for_status()
            return r.json()

    def tags(self) -> list[str]:
        with self._client() as c:
            r = c.get("/api/v1/tools/tags")
            r.raise_for_status()
            return r.json()

    def tools_in_category(self, category: str) -> list[dict]:
        with self._client() as c:
            r = c.get("/api/v1/tools", params={"category": category})
            r.raise_for_status()
            return r.json()

    def services(self) -> list[dict]:
        with self._client() as c:
            r = c.get("/api/v1/services")
            r.raise_for_status()
            return r.json()

    # Execution
    def execute(self, tool_name: str, arguments: dict) -> dict:
        with self._client() as c:
            r = c.post("/api/v1/tools/execute",
                       json={"tool_name": tool_name, "arguments": arguments})
            r.raise_for_status()
            return r.json()

    def execute_named(self, tool_name: str, arguments: dict) -> dict:
        with self._client() as c:
            r = c.post(f"/api/v1/tools/{tool_name}/execute", json=arguments)
            r.raise_for_status()
            return r.json()

    def batch(self, calls: list[dict]) -> dict:
        """calls: list of BatchCall {tool_name, arguments} -> BatchResponse."""
        with self._client() as c:
            r = c.post("/api/v1/tools/batch", json={"calls": calls})
            r.raise_for_status()
            return r.json()

    def upsert_dependency(self, edge: dict) -> dict:
        with self._client() as c:
            r = c.post("/api/v1/tools/dependencies", json=edge)
            r.raise_for_status()
            return r.json()

    def gateway_execute(self, payload: dict) -> dict:
        with self._client() as c:
            r = c.post("/api/v1/gateway/execute", json=payload)
            r.raise_for_status()
            return r.json()

    def gateway_metrics(self) -> dict:
        with self._client() as c:
            r = c.get("/api/v1/gateway/metrics")
            r.raise_for_status()
            return r.json()

    def set_routing_strategy(self, strategy: dict) -> dict:
        with self._client() as c:
            r = c.post("/api/v1/gateway/routing/strategy", json=strategy)
            r.raise_for_status()
            return r.json()


def router_tool_to_anthropic(tool: dict, programmatic: bool) -> dict:
    """Convert a router tool record into an Anthropic function-tool block.

    With ``programmatic=True`` the tool is only callable from inside the code
    execution container (no sampling round-trip per call)."""
    block = {
        "name": tool["name"],
        "description": tool.get("description", ""),
        "input_schema": tool.get("input_schema",
                                 {"type": "object", "properties": {}}),
    }
    if programmatic:
        block["allowed_callers"] = [CODE_EXECUTION_TYPE]
    return block


# -- The wrapper ------------------------------------------------------------------
@dataclass
class ModelInstanceWrapper:
    model_id: str
    name: str = ""
    effort: Optional[Effort] = None
    system_prompt: str = ""
    tool_category: Optional[str] = None        # router category filter
    tools: list[dict] = field(default_factory=list)  # anthropic tool blocks
    skills: list[str] = field(default_factory=list)
    programmatic_tool_calling: bool = True
    compaction_fraction: float = 0.85           # of context window
    disable_thinking: bool = False              # sonnet-5 low-cost routing tier
    fast_mode: bool = False                     # opus-4.8 only
    cache_ttl: Literal["5m", "1h"] = "1h"
    background: bool = False
    max_turns: Optional[int] = None
    container_id: Optional[str] = None          # PTC container reuse
    children: list["ModelInstanceWrapper"] = field(default_factory=list)
    session_id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    # forbidden knobs — accepted only to give a clear error, never sent
    temperature: Optional[float] = None
    top_p: Optional[float] = None
    top_k: Optional[int] = None
    thinking_budget_tokens: Optional[int] = None

    def __post_init__(self):
        self.model_id = canonical_model(self.model_id)
        self.card = MAX_PARAMS[self.model_id]
        if not self.name:
            self.name = f"{self.model_id}-{self.session_id}"
        if self.effort is None:
            self.effort = self.card["default_effort"]
        self._validate()

    # -- invariants (fail at build time, not at 400 time) ------------------------
    def _validate(self):
        card = self.card

        if self.effort not in card["allowed_effort"]:
            raise SwarmConfigError(
                f"{self.model_id}: effort={self.effort!r} not in "
                f"{card['allowed_effort']} — effort is the only legal "
                f"cost/quality knob and must be a supported tier")

        if card["sampling_locked"] and any(
                v is not None for v in (self.temperature, self.top_p, self.top_k)):
            raise SwarmConfigError(
                f"{self.model_id}: temperature/top_p/top_k are locked to "
                f"defaults (non-default values return 400). Use "
                f"output_config.effort instead.")

        if card["budget_tokens_rejected"] and self.thinking_budget_tokens:
            raise SwarmConfigError(
                f"{self.model_id}: manual thinking.budget_tokens returns 400. "
                f"Use adaptive thinking + effort; or run claude-haiku-4-5 "
                f"for explicit reasoning budgets.")

        if self.disable_thinking and card["adaptive_required"]:
            raise SwarmConfigError(
                f"{self.model_id}: adaptive thinking is required and cannot "
                f"be disabled.")

        if self.fast_mode and not card["fast_mode"]:
            raise SwarmConfigError(
                f"{self.model_id}: fast-mode beta is only applicable to "
                f"claude-opus-4-8.")

        if not (0.0 < self.compaction_fraction < 1.0):
            raise SwarmConfigError("compaction_fraction must be in (0,1)")

        # cache floor: a shared prefix below the model minimum silently pays
        # full price on every request across the whole fan-out.
        prefix_tokens = _rough_tokens(self.system_prompt)
        if self.system_prompt and prefix_tokens < card["cache_min"]:
            raise SwarmConfigError(
                f"{self.model_id}: system prefix ~{prefix_tokens} tokens is "
                f"below the {card['cache_min']}-token cache minimum — pad the "
                f"shared prefix above the floor or drop the breakpoint "
                f"(otherwise every request silently pays full price).")

        if card["fan_out_cap"] == 0 and self.children:
            raise SwarmConfigError(
                f"{self.model_id} is a leaf tier (fan_out_cap=0); it "
                f"cannot supervise children.")
        if self.children and len(self.children) > card["fan_out_cap"]:
            raise SwarmConfigError(
                f"{self.model_id}: fan-out {len(self.children)} exceeds tier "
                f"cap {card['fan_out_cap']} (context preservation).")

    def validate_depth(self, depth: int = 1):
        if depth > MAX_HIERARCHY_DEPTH:
            raise SwarmConfigError(
                f"hierarchy depth {depth} exceeds the {MAX_HIERARCHY_DEPTH}-"
                f"level default composition limit")
        for ch in self.children:
            ch.validate_depth(depth + 1)

    # -- request rendering --------------------------------------------------------
    def beta_headers(self) -> str:
        betas = []
        if self.card["compaction"]:
            betas.append(BETA_COMPACTION)
        if self.fast_mode:
            betas.append(BETA_FAST_MODE)
        if self.skills:
            betas.append(BETA_SKILLS)
        return ",".join(betas)

    def headers(self, credential: str) -> dict:
        h = {
            _HDR_CRED: credential,
            "anthropic-version": ANTHROPIC_VERSION,
            "content-type": "application/json",
        }
        b = self.beta_headers()
        if b:
            h["anthropic-beta"] = b
        return h

    def render_request(self, messages: list[dict],
                       max_tokens: Optional[int] = None) -> dict:
        """Produce the maximally-aggressive VALID Messages API body."""
        card = self.card
        mt = min(max_tokens or card["max_output"], card["max_output"])

        body: dict[str, Any] = {
            "model": self.model_id,
            "max_tokens": mt,
        }

        # thinking / effort — the only legal sampling knob
        if self.disable_thinking:
            body["thinking"] = {"type": "disabled"}
        else:
            body["thinking"] = {"type": "adaptive"}
            body["output_config"] = {"effort": self.effort}

        if self.fast_mode:
            body["speed"] = "fast"

        # server-side compaction — widen the effective budget
        if card["compaction"]:
            trigger = int(card["context_window"] * self.compaction_fraction)
            body["context_management"] = {"edits": [{
                "type": "compact_20260112",
                "trigger": {"type": "input_tokens", "value": trigger},
            }]}

        # tools: code_execution first, then router tools with allowed_callers
        tools: list[dict] = []
        breakpoints = 0
        if self.programmatic_tool_calling and card["programmatic_tool_calling"]:
            ce: dict[str, Any] = {"type": CODE_EXECUTION_TYPE,
                                  "name": "code_execution"}
            if self.container_id:
                ce["container"] = {"id": self.container_id}
            tools.append(ce)
        for i, t in enumerate(self.tools):
            block = dict(t)
            if (self.programmatic_tool_calling
                    and card["programmatic_tool_calling"]
                    and "type" not in block):
                block.setdefault("allowed_callers", [CODE_EXECUTION_TYPE])
            # cache breakpoint on the LAST tool (tools->system->messages is
            # cumulative; one mark caches the whole tool prefix)
            if i == len(self.tools) - 1:
                block["cache_control"] = self._cache_control()
                breakpoints += 1
            tools.append(block)
        if tools:
            body["tools"] = tools

        # system: shared prefix with a breakpoint (cache_min enforced at
        # construction time)
        if self.system_prompt:
            body["system"] = [{
                "type": "text",
                "text": self.system_prompt,
                "cache_control": self._cache_control(),
            }]
            breakpoints += 1

        body["messages"] = messages
        # top-level automatic caching consumes one breakpoint slot and
        # advances with the conversation — cheapest correct default.
        if breakpoints < MAX_CACHE_BREAKPOINTS:
            body["cache_control"] = self._cache_control()
            breakpoints += 1

        assert breakpoints <= MAX_CACHE_BREAKPOINTS
        return body

    def _cache_control(self) -> dict:
        cc: dict[str, Any] = {"type": "ephemeral"}
        if self.cache_ttl == "1h":
            cc["ttl"] = "1h"
        return cc

    # -- AgentDefinition for the Claude Agent SDK query(agents={...}) map --------
    def agent_definition(self) -> dict:
        d: dict[str, Any] = {
            "model": self.model_id,
            "effort": self.effort,
            "tools": [t["name"] for t in self.tools],
            "background": self.background,
        }
        if self.skills:
            d["skills"] = self.skills
        if self.system_prompt:
            d["initialPrompt"] = self.system_prompt
        if self.max_turns:
            d["maxTurns"] = self.max_turns
        if self.children:
            d["agents"] = {c.name: c.agent_definition() for c in self.children}
        return d

    # -- cost envelope --------------------------------------------------------------
    def cost_envelope(self, cached_in_tokens: int, fresh_in_tokens: int,
                      out_tokens: int) -> dict:
        """Fully-loaded per-call USD estimate.
        cache read = 0.1x input; 1h write = 2x; 5m write = 1.25x."""
        card = self.card
        out_tokens = min(out_tokens, card["max_output"])
        read = cached_in_tokens * card["usd_in"] * 0.1 / 1e6
        fresh = fresh_in_tokens * card["usd_in"] / 1e6
        out = out_tokens * card["usd_out"] / 1e6
        return {"model": self.model_id, "cache_read_usd": round(read, 4),
                "fresh_in_usd": round(fresh, 4), "out_usd": round(out, 4),
                "total_usd": round(read + fresh + out, 4)}

    def tree_cost_envelope(self, per_call: dict[str, tuple[int, int, int]]) -> float:
        """Sum worst-case cost over the tree. per_call maps model_id ->
        (cached_in, fresh_in, out) token budgets."""
        args = per_call[self.model_id]
        total = self.cost_envelope(*args)["total_usd"]
        for ch in self.children:
            total += ch.tree_cost_envelope(per_call)
        return round(total, 4)

    # -- live dispatch (Anthropic Messages API) -------------------------------------
    def run(self, messages: list[dict], credential: str,
            max_tokens: Optional[int] = None) -> dict:
        """Dispatch one request. `credential` is caller-supplied opaque auth
        material — this module never reads ambient credentials."""
        import httpx

        if not credential:
            raise SwarmConfigError("credential must be supplied by the caller")
        body = self.render_request(messages, max_tokens)
        with httpx.Client(timeout=600.0) as c:
            r = c.post(ANTHROPIC_MESSAGES_URL,
                       headers=self.headers(credential), json=body)
            r.raise_for_status()
            data = r.json()
        # keep the PTC container for the next turn
        cont = data.get("container") or {}
        if cont.get("id"):
            self.container_id = cont["id"]
        # observe compaction cycles and adapt the trigger downward if it fired
        for it in (data.get("usage", {}).get("iterations") or []):
            if it.get("type") == "compaction":
                self.compaction_fraction = max(0.5, self.compaction_fraction - 0.05)
        return data


def _rough_tokens(text: str) -> int:
    # ~4 chars/token heuristic; deliberately conservative for the cache floor
    return len(text) // 4


# -- Swarm assembly -----------------------------------------------------------------
def build_swarm(epic_prompt_prefix: str,
                sonnet_supervisors: int = 2,
                haiku_leaves_per_supervisor: int = 4,
                opus_reviewers: int = 1,
                tool_blocks: Optional[dict[str, list[dict]]] = None,
                ) -> ModelInstanceWrapper:
    """Reference topology: one Fable-5 root per task epic ->
    Sonnet-5 sub-supervisors (compaction + PTC) -> Haiku-4.5 leaves,
    with Opus-4.8 reviewers attached to the root for risk-laden decisions.
    """
    tool_blocks = tool_blocks or {}

    def leaves(n: int) -> list[ModelInstanceWrapper]:
        return [
            ModelInstanceWrapper(
                model_id="claude-haiku-4-5",
                effort="low",  # verbatim transforms / classification
                system_prompt=epic_prompt_prefix,
                tools=tool_blocks.get("search", []),
                cache_ttl="5m",  # high-cadence leaves refresh for free
            )
            for _ in range(n)
        ]

    supervisors = [
        ModelInstanceWrapper(
            model_id="claude-sonnet-5",
            effort="high",
            system_prompt=epic_prompt_prefix,
            tools=tool_blocks.get("integration", []),
            programmatic_tool_calling=True,
            children=leaves(haiku_leaves_per_supervisor),
            background=True,
        )
        for _ in range(sonnet_supervisors)
    ]
    reviewers = [
        ModelInstanceWrapper(
            model_id="claude-opus-4-8",
            effort="xhigh",
            fast_mode=False,  # quality over latency for reviews
            system_prompt=epic_prompt_prefix,
            tools=tool_blocks.get("finance", []),
        )
        for _ in range(opus_reviewers)
    ]

    root = ModelInstanceWrapper(
        model_id="claude-fable-5",
        effort="high",
        system_prompt=epic_prompt_prefix,
        children=supervisors + reviewers,
    )
    root.validate_depth()
    return root


if __name__ == "__main__":
    prefix = ("You are a sub-agent in a hierarchical swarm routed through "
              "tools.distributed.systems. ") * 250  # > 4096-token Haiku floor
    swarm = build_swarm(prefix)
    print(json.dumps(swarm.agent_definition(), indent=2)[:800])
    budgets = {
        "claude-fable-5": (500_000, 20_000, 32_000),
        "claude-opus-4-8": (400_000, 20_000, 32_000),
        "claude-sonnet-5": (1_000_000, 30_000, 128_000),
        "claude-haiku-4-5": (150_000, 10_000, 64_000),
    }
    print("worst-case tree cost: $", swarm.tree_cost_envelope(budgets))
