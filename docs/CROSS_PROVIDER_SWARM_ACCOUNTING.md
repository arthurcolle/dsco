# Cross-provider swarm execution and accounting

Use `swarm` through the normal governed tool interface. `create` and `map_reduce` accept per-task `provider`, `model`, and `effort`. `map_reduce` also accepts `coordinator_provider`, `coordinator_model`, and `coordinator_effort`; set these to pin the reducer's endpoint, model, and supported reasoning effort. Explicit models are preserved even when absent from a cached catalog. Invalid effort values are rejected before workers launch.

Example map/reduce arguments:

```json
{
  "action": "map_reduce",
  "name": "cross-provider-check",
  "tasks": [
    {"task": "Return MAP_A=19", "provider": "openrouter", "model": "qwen/qwen3.8-flash", "effort": "none"},
    {"task": "Return MAP_B=23", "provider": "openai", "model": "gpt-5.4-nano", "effort": "none"}
  ],
  "coordinator": "Add MAP_A and MAP_B from the worker results.",
  "coordinator_provider": "openrouter",
  "coordinator_model": "qwen/qwen3.8-flash",
  "coordinator_effort": "none",
  "max_worker_turns": 2,
  "max_coordinator_turns": 2,
  "timeout": 30,
  "map_total_timeout": 30,
  "map_refill_rounds": 0,
  "coordinator_fallback": false,
  "budget": 0.20
}
```

These model IDs were discovered during the September 4, 2026 test. Choose from the current catalog; availability and request-parameter support can change. The tested OpenAI Chat Completions tool path used `DSCO_EFFORT=none`.

`collect` returns promptly when all workers are terminal; inspect each worker's status because terminal completion can include errors. A collection timeout preserves active workers for later observation. Zero refill rounds means no replacement attempts. Worker budget caps cannot be widened by inherited environment values; exhausted swarm budgets reject new native workers. A request already in flight may overshoot its cap.

Native workers transfer canonical `dsco.inference_cost.v1` records through a dedicated inherited descriptor, separate from model/tool output. Parent collection, status and durable artifacts retain reported cost, estimated inference cost, budget-accounted cost, sample count and unpriced responses. Reported zero and unknown are distinct. Cumulative spend survives worker-slot reuse. Subscription inference is retained separately; the swarm's existing admission budget is explicitly a metered-credit budget.

Every launch, including short commands and MCP, requests background catalog refresh. Cached data is usable immediately. A detached worker refreshes OpenRouter metadata/prices, first-party OpenAI Standard pricing, and the Codex catalog, with a 45-second ceiling and cross-process refresh coalescing. Failures retain the last good cache. `DSCO_PRICING_OFFLINE=1` keeps cached operation offline. Missing prices remain unknown. Current OpenAI estimates represent standard short-context pricing, not invoices or every service tier.

Verification: `DSCO_PRICING_OFFLINE=1 DSCO_TEST_ONLY=swarm-chain ./test_runner`, `make test-self-swarm-core`, and `make test-gate-claims`. Live evidence and failure reproductions are in `reports/cross-provider-chain-20260904/`.

Expanded live coverage is in `reports/cross-provider-subsets-20260904/`: all 15 nonempty subsets of OpenRouter, OpenAI, xAI, and DeepSeek have passing latest evaluated attempts, including six pairs, four triples, and the four-provider group. Single-provider controls exercise actual tool writes and reads; mixed groups exercise map/reduce with all four providers used as coordinators. Earlier failures, recovery attempts, and every available cost record remain in the report. Use `status` with `group_id`; use `inspect` for the global view. Both group and global status expose each worker's actual provider.

The September 5 expanded prompt review exercised 14 configured provider/auth lanes; eight produced reviews. See `reports/all-provider-prompt-review-20260905/README.md` for actual coverage and blocked lanes. Catalog availability and credential discovery do not prove inference access.

Kimi subscription identities `k3` and `kimi-code/k3` resolve reference prices through the live `moonshotai/kimi-k3` catalog entry without changing their execution route. An all-zero static subscription fallback is unknown inference cost when no usable quote exists. A reported zero bill remains recorded independently. Historical zero records are retained, with derived reference-price reconciliation stored separately.

Normal owner exit now drains worker output/accounting, cancels and reaps owned workers, and persists final receipts before destroying the swarm. `DSCO_SWARM_PRESERVE_CHILDREN` retains explicit handoffs; inherited fork cleanup cannot take ownership. The shutdown regression uses controlled native worker processes and nested tool process groups without paid inference. SIGKILL of the owner bypasses atexit cleanup; bounded reaping cannot guarantee removal of uninterruptible processes.

## Endurance and route-specific prices (2026-09-05)

`swarm {"action":"health"}` exposes bounded JSON health and subscription-inclusive
accounting without requiring run/net/control grants. Reported, estimated, unknown,
and retired/unclassified amounts remain distinct; output silence is an observation,
not proof of a stalled model.

Canonical inference records now include pricing source, scope, observation time and
source URL. Direct DeepSeek uses its official cached price table and time bands;
a missing direct quote is explicitly marked reference fallback. A provider's
reported amount still wins accounting. No historical receipt is overwritten.

See [durable planning and frontier usage](ENDURANCE_PLANNING.md) and
[the live campaign](../reports/endurance-swarm-20260905/README.md).
