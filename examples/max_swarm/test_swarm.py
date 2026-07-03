"""Invariant tests for max_swarm — every constraint from the spec is
enforced at build time, not at 400 time. Run: python3 test_swarm.py"""

import json
import sys

from swarm import (
    MAX_PARAMS, MAX_CACHE_BREAKPOINTS, MAX_HIERARCHY_DEPTH,
    ModelInstanceWrapper, SwarmConfigError, build_swarm, canonical_model,
    route, router_tool_to_anthropic, CODE_EXECUTION_TYPE,
    BETA_COMPACTION, BETA_FAST_MODE,
)

PASS = 0
def ok(cond, msg):
    global PASS
    assert cond, msg
    PASS += 1

PREFIX_LONG = ("hierarchical swarm shared system prefix padding block " * 400)  # ~22k chars
MSGS = [{"role": "user", "content": "go"}]


def expect_config_error(fn, needle, msg):
    try:
        fn()
    except SwarmConfigError as e:
        ok(needle in str(e), f"{msg}: wrong error: {e}")
        return
    raise AssertionError(f"{msg}: expected SwarmConfigError")


# ── model aliases & routing ────────────────────────────────────────────────
ok(canonical_model("claude-haiku-4.5") == "claude-haiku-4-5", "alias haiku")
ok(canonical_model("claude-opus-4.8") == "claude-opus-4-8", "alias opus")
expect_config_error(lambda: canonical_model("gpt-5"), "unknown model", "reject foreign model")
ok(route("transform") == ("claude-haiku-4-5", "low"), "route transform")
ok(route("synthesis") == ("claude-sonnet-5", "high"), "route synthesis")
ok(route("decision") == ("claude-opus-4-8", "xhigh"), "route decision")
ok(route("frontier") == ("claude-fable-5", "high"), "route frontier")

# ── 400-avoidance: sampling locked on opus/sonnet/fable ───────────────────
for m in ("claude-opus-4-8", "claude-sonnet-5", "claude-fable-5"):
    expect_config_error(
        lambda m=m: ModelInstanceWrapper(model_id=m, temperature=0.7),
        "locked to", f"{m} rejects temperature")
# haiku allows sampling knobs
ModelInstanceWrapper(model_id="claude-haiku-4-5", temperature=0.2)
PASS += 1

# ── 400-avoidance: manual budget_tokens rejected on opus/sonnet ───────────
for m in ("claude-opus-4-8", "claude-sonnet-5"):
    expect_config_error(
        lambda m=m: ModelInstanceWrapper(model_id=m, thinking_budget_tokens=8000),
        "budget_tokens", f"{m} rejects manual budget")
# haiku allows explicit budgets (and effort: max)
ModelInstanceWrapper(model_id="claude-haiku-4-5", thinking_budget_tokens=8000,
                     effort="max")
PASS += 1

# ── effort tiers per model ─────────────────────────────────────────────────
expect_config_error(
    lambda: ModelInstanceWrapper(model_id="claude-fable-5", effort="low"),
    "effort", "fable-5 rejects effort=low")
expect_config_error(
    lambda: ModelInstanceWrapper(model_id="claude-sonnet-5", effort="max"),
    "effort", "sonnet-5 rejects effort=max (haiku-only tier)")
w = ModelInstanceWrapper(model_id="claude-opus-4-8")
ok(w.effort == "high", "opus default effort=high")
w = ModelInstanceWrapper(model_id="claude-haiku-4-5")
ok(w.effort == "low", "haiku default effort=low")

# ── adaptive thinking required on opus; disableable on sonnet ──────────────
expect_config_error(
    lambda: ModelInstanceWrapper(model_id="claude-opus-4-8", disable_thinking=True),
    "adaptive", "opus cannot disable thinking")
w = ModelInstanceWrapper(model_id="claude-sonnet-5", disable_thinking=True)
body = w.render_request(MSGS)
ok(body["thinking"] == {"type": "disabled"}, "sonnet thinking disabled emits type:disabled")
ok("output_config" not in body, "disabled thinking omits output_config")

# ── fast mode only on opus ──────────────────────────────────────────────────
expect_config_error(
    lambda: ModelInstanceWrapper(model_id="claude-sonnet-5", fast_mode=True),
    "fast-mode", "sonnet rejects fast mode")
w = ModelInstanceWrapper(model_id="claude-opus-4-8", fast_mode=True)
body = w.render_request(MSGS)
ok(body["speed"] == "fast", "opus fast mode emits speed:fast")
ok(BETA_FAST_MODE in w.beta_headers(), "fast-mode beta header present")

# ── cache floor enforcement (the Haiku 4,096 footgun) ──────────────────────
short = "short prefix " * 20  # ~65 tokens
expect_config_error(
    lambda: ModelInstanceWrapper(model_id="claude-haiku-4-5", system_prompt=short),
    "cache minimum", "haiku rejects sub-4096 prefix")
# same prefix is fine on fable (512 floor) if above 512 tokens
mid = "x" * 3000  # ~750 tokens
ModelInstanceWrapper(model_id="claude-fable-5", system_prompt=mid)
PASS += 1
expect_config_error(
    lambda: ModelInstanceWrapper(model_id="claude-sonnet-5", system_prompt=mid),
    "cache minimum", "sonnet rejects sub-1024 prefix")

# ── compaction gating ──────────────────────────────────────────────────────
w = ModelInstanceWrapper(model_id="claude-sonnet-5")
body = w.render_request(MSGS)
edits = body["context_management"]["edits"]
ok(edits[0]["type"] == "compact_20260112", "sonnet compaction edit type")
ok(edits[0]["trigger"] == {"type": "input_tokens", "value": 850_000},
   "sonnet compaction trigger at 0.85 of 1M")
ok(BETA_COMPACTION in w.beta_headers(), "compaction beta header on sonnet")
w = ModelInstanceWrapper(model_id="claude-haiku-4-5")
body = w.render_request(MSGS)
ok("context_management" not in body, "haiku (not in compaction matrix) omits edits")
ok(BETA_COMPACTION not in w.beta_headers(), "haiku omits compaction beta")

# ── programmatic tool calling ──────────────────────────────────────────────
tool = router_tool_to_anthropic(
    {"name": "search_docs", "description": "d",
     "input_schema": {"type": "object", "properties": {}}}, programmatic=True)
ok(tool["allowed_callers"] == [CODE_EXECUTION_TYPE], "allowed_callers set")
w = ModelInstanceWrapper(model_id="claude-sonnet-5", tools=[tool])
body = w.render_request(MSGS)
ok(body["tools"][0] == {"type": CODE_EXECUTION_TYPE, "name": "code_execution"},
   "code_execution tool first")
ok(body["tools"][1]["allowed_callers"] == [CODE_EXECUTION_TYPE],
   "router tool restricted to container")
w.container_id = "cont_abc"
body = w.render_request(MSGS)
ok(body["tools"][0]["container"] == {"id": "cont_abc"}, "container id passed back")

# ── cache breakpoint budget ────────────────────────────────────────────────
w = ModelInstanceWrapper(model_id="claude-sonnet-5",
                         system_prompt=PREFIX_LONG, tools=[dict(tool)])
body = w.render_request(MSGS)
marks = json.dumps(body).count('"cache_control"')
ok(marks <= MAX_CACHE_BREAKPOINTS, f"breakpoints {marks} within budget")
ok(body["system"][0]["cache_control"]["ttl"] == "1h", "1h ttl on system prefix")
ok(body["tools"][-1].get("cache_control") is not None, "mark on last tool")
ok(body["cache_control"]["type"] == "ephemeral", "top-level automatic caching present")
w5 = ModelInstanceWrapper(model_id="claude-sonnet-5", cache_ttl="5m")
ok("ttl" not in w5._cache_control(), "5m ttl emits no ttl field")

# ── max_tokens clamped to model card ───────────────────────────────────────
w = ModelInstanceWrapper(model_id="claude-haiku-4-5")
ok(w.render_request(MSGS, max_tokens=999_999)["max_tokens"] == 64_000,
   "haiku output clamped to 64k")
w = ModelInstanceWrapper(model_id="claude-fable-5")
ok(w.render_request(MSGS)["max_tokens"] == 128_000, "fable defaults to 128k out")

# ── hierarchy: depth cap, fan-out caps, leaf tier ──────────────────────────
expect_config_error(
    lambda: ModelInstanceWrapper(
        model_id="claude-haiku-4-5",
        children=[ModelInstanceWrapper(model_id="claude-haiku-4-5")]),
    "leaf tier", "haiku cannot supervise")
expect_config_error(
    lambda: ModelInstanceWrapper(
        model_id="claude-fable-5",
        children=[ModelInstanceWrapper(model_id="claude-sonnet-5")
                  for _ in range(5)]),
    "fan-out", "fable fan-out cap of 4 enforced")

deep = ModelInstanceWrapper(model_id="claude-sonnet-5")
for _ in range(5):
    deep = ModelInstanceWrapper(model_id="claude-sonnet-5", children=[deep])
try:
    deep.validate_depth()
    raise AssertionError("depth 6 should fail")
except SwarmConfigError as e:
    ok("depth" in str(e), "depth cap enforced")

# ── reference topology builds and stays valid ──────────────────────────────
prefix = ("You are a sub-agent in a hierarchical swarm routed through "
          "tools.distributed.systems. ") * 250
swarm = build_swarm(prefix)
swarm.validate_depth()
ok(swarm.model_id == "claude-fable-5", "root is fable-5")
ok(len(swarm.children) == 3, "2 sonnet supervisors + 1 opus reviewer")
ok(all(c.model_id == "claude-haiku-4-5"
       for s in swarm.children if s.model_id == "claude-sonnet-5"
       for c in s.children), "leaves are haiku")
d = swarm.agent_definition()
ok("agents" in d and len(d["agents"]) == 3, "AgentDefinition nests children")

# ── cost envelope matches the spec's worked examples ───────────────────────
# "a fully-loaded Haiku 4.5 wrapper with 1M-token cache hit and 64k output
#  costs roughly $1 + $5 = $6" — note haiku context is 200K, the spec's
#  arithmetic is 1M*0.1*$1 + 64k*$5/M ≈ $0.1 + $0.32; the $6 figure prices the
#  1M tokens at FULL input rate + 1M output. We verify the exact math instead:
h = ModelInstanceWrapper(model_id="claude-haiku-4-5")
env = h.cost_envelope(1_000_000, 0, 64_000)
ok(abs(env["cache_read_usd"] - 0.10) < 1e-9, "haiku 1M cache read = $0.10")
ok(abs(env["out_usd"] - 0.32) < 1e-9, "haiku 64k out = $0.32")
s = ModelInstanceWrapper(model_id="claude-sonnet-5")
env = s.cost_envelope(1_000_000, 0, 128_000)
ok(abs(env["cache_read_usd"] - 0.20) < 1e-9, "sonnet 1M cache read = $0.20")
ok(abs(env["out_usd"] - 1.28) < 1e-9, "sonnet 128k out = $1.28")
budgets = {
    "claude-fable-5": (500_000, 20_000, 32_000),
    "claude-opus-4-8": (400_000, 20_000, 32_000),
    "claude-sonnet-5": (1_000_000, 30_000, 128_000),
    "claude-haiku-4-5": (150_000, 10_000, 64_000),
}
total = swarm.tree_cost_envelope(budgets)
ok(0 < total < 50, f"tree cost envelope sane: ${total}")

# ── headers ────────────────────────────────────────────────────────────────
w = ModelInstanceWrapper(model_id="claude-opus-4-8", fast_mode=True,
                         skills=["playbook-x"])
h = w.headers("test-credential")
ok(h["anthropic-version"] == "2023-06-01", "version header")
ok("compact-2026-01-12" in h["anthropic-beta"], "compaction beta")
ok("fast-mode-2026-02-01" in h["anthropic-beta"], "fast beta")
ok("skills-2025-10-02" in h["anthropic-beta"], "skills beta")

print(f"ALL {PASS} ASSERTIONS PASSED")
