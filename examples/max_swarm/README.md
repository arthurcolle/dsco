# max_swarm — Max-Parameter Four-Model Hierarchical Swarm

Reference implementation of the four-tier Claude swarm on a
tools.distributed.systems-style tool router. Pure-Python, zero hard deps for
the planning path (`httpx` only for live dispatch).

## Files
- `swarm.py` — model cards, wrapper, router client, swarm builder, cost model
- `test_swarm.py` — 59 invariant assertions (`python3 test_swarm.py`)

## The four tiers

| Tier | Model | Effort default | Job |
|------|-------|----------------|-----|
| Root | `claude-fable-5` | high | Frontier delegator; one per task epic; fan-out ≤ 4 |
| Review | `claude-opus-4-8` | xhigh (fast mode OFF) | Risk-laden decisions, code review, planning |
| Orchestrate | `claude-sonnet-5` | high | Workhorse: compaction + programmatic tool calling; fan-out ≤ 8 |
| Leaf | `claude-haiku-4-5` | low | Bulk transforms/classification; fan_out_cap = 0 |

Routing: `route("transform"|"synthesis"|"decision"|"frontier")`.

## Invariants enforced at construction (never at 400 time)

1. **Sampling locked** — `temperature/top_p/top_k` raise `SwarmConfigError` on
   Fable/Opus/Sonnet (non-default values return 400 upstream). Haiku permits them.
2. **`budget_tokens` rejected** on Opus 4.8 / Sonnet 5 — adaptive thinking +
   `output_config.effort` is the only legal knob. Haiku accepts explicit
   budgets and the `max` effort tier.
3. **Adaptive thinking required** on Opus 4.8 (cannot `disable_thinking`);
   Sonnet 5 may set `thinking: {type: "disabled"}` for low-cost routing tiers.
4. **Fast mode** (`speed: "fast"` + `fast-mode-2026-02-01` beta) Opus-4.8-only.
5. **Cache floors** — a system prefix below the per-model minimum
   (512 / 1,024 / 1,024 / 4,096 tokens) is a build error, because it would
   silently pay full price on every request across the whole fan-out.
6. **≤ 4 cache breakpoints** per request: last tool (covers the whole tool
   prefix cumulatively) + system block + top-level automatic caching.
   1h TTL default for supervisors; 5m for high-cadence leaves (free refresh).
7. **Compaction** (`compact-2026-01-12`, `compact_20260112` edit at
   `0.85 × context_window`) emitted only for models in the compaction matrix
   (not Haiku). The wrapper watches `usage.iterations` for compaction cycles
   and lowers its trigger fraction adaptively.
8. **Programmatic tool calling** — `code_execution_20260120` is injected
   first; every router tool gets `allowed_callers: ["code_execution_20260120"]`;
   `container.id` from the response is retained for the next turn.
9. **Hierarchy caps** — 5 levels max; per-tier fan-out caps (4/6/8/0);
   Haiku is a leaf and cannot supervise.
10. **Output clamped** to the model card (128k / 128k / 128k / 64k).

## Router client

`ToolRouterClient` covers the Tool Management API v1.0.0 surface used by the
swarm: `/api/v1/tools/{execute,batch,names,tags,categories}`,
`/api/v1/tools/{tool}/execute`, `/api/v1/tools/dependencies`,
`/api/v1/gateway/{execute,metrics,routing/strategy}`,
`/api/v1/auth/warrants/{mint,validate}`, and OAuth token exchange via
`fetch_token()` (caller-built form — no credentials live in this module).
Live check (2026-07-03): the public endpoint answers 401 with
`Bearer token required`, confirming the auth surface.

## Cost model

`cost_envelope(cached_in, fresh_in, out)` prices cache reads at 0.1× input;
`tree_cost_envelope({model: (cached, fresh, out)})` sums the worst case over
the tree. The default reference topology (1 Fable + 2 Sonnet + 1 Opus +
8 Haiku) with generous budgets lands ≈ $9.24 worst case per full-tree pass —
dominated by Sonnet/Haiku output, as the spec predicts.

Note on the spec's "$6 Haiku / $12 Sonnet" envelope: at published rates a
1M-token *cache hit* is 0.1 × $1/MTok = $0.10 (Haiku) or $0.20 (Sonnet), and
64k/128k outputs are $0.32/$1.28. The $6/$12 figures only reconcile if the 1M
input is priced at the full uncached rate plus ~1M output tokens; the code
implements the exact published arithmetic and `test_swarm.py` pins it.
Also: Haiku's context is 200K, so a "1M-token Haiku cache hit" is not
reachable in a single request anyway.

## Usage

```python
from swarm import build_swarm, ToolRouterClient, router_tool_to_anthropic

router = ToolRouterClient(token=my_opaque_token)
tools = {
    "search": [router_tool_to_anthropic(t, True)
               for t in router.tools_in_category("search")],
    "integration": [router_tool_to_anthropic(t, True)
                    for t in router.tools_in_category("integration")],
    "finance": [router_tool_to_anthropic(t, True)
                for t in router.tools_in_category("finance")],
}
swarm = build_swarm(shared_prefix, tool_blocks=tools)
body = swarm.render_request([{"role": "user", "content": "epic goes here"}])
# or live: swarm.run(messages, credential=my_credential)
```

`agent_definition()` emits the nested `AgentDefinition` map for the Claude
Agent SDK's `query(agents={...})` if you prefer SDK-managed sessions over raw
Messages API dispatch.
