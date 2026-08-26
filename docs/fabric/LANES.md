# Provider Fabric — Swimlane Rollout (v1)

**Status:** rolled out on `emergency/wip-snapshot-20260707` @ `a0a171d`
**Verified:** 2026-07-12, 60-lane sweep, 58/60 green (96.7%), $0.35
**Manifest:** `docs/fabric/lane-manifest-v1.txt` (53 env lanes + 7 built-ins)

## What a lane is

A fabric lane is `(provider, model, effort)`. Each distinct triple is an
independent worker process pinned via `--provider <p> -m <model>` with
`DSCO_EFFORT` set per-lane. The swarm cap is `SWARM_MAX_CHILDREN` (64).

## Usage

```sh
# built-in default lanes (all-subscription): race mode
dsco --provider-fabric -p "task"

# full lane manifest, collect mode
DSCO_FABRIC_SUBLANES="$(tr '\n' ',' < docs/fabric/lane-manifest-v1.txt)" \
  dsco --provider-fabric --fabric-mode collect \
       --fabric-max-agents 64 --fabric-replicas 1 -p "task"

# effort sub-lanes: same model, different reasoning budgets
DSCO_FABRIC_SUBLANES="openai-codex:gpt-5.6-sol@xhigh,openai-codex:gpt-5.6-sol@low" ...
```

## Default built-in lanes (subscription-only)

| provider | model | auth |
|---|---|---|
| sakana | fugu (base; ultra is opt-in — cost) | FUGU/SAKANA subscription allocation key |
| anthropic | claude-sonnet-5 | Claude Code OAuth |
| openai-codex | gpt-5.6-luna (issued default), gpt-5.6-sol, gpt-5.6-terra, gpt-5.5 | ChatGPT OAuth |
| kimi-code | kimi-code/k3 | Kimi Code OAuth |
| zai | glm-5.2 | Z.AI Coding Plan key |

Direct OpenAI and Anthropic API keys, Sakana PAYG keys, and OpenRouter keys are
metered lanes. They are never promoted to subscription lanes merely because a
credential exists.

`gpt-5.6-luna` was live-verified on the native ChatGPT subscription backend on
2026-07-16. An earlier 2026-07-12 lane sweep returned 404; that result is stale.

## Native tier-1 inventory and benchmark

The subscription probe bypasses executor CLIs and worker processes. Each ready
lane is built and streamed through its in-process HTTP transport:

```sh
# Credential class, endpoint, model, and readiness; JSON goes to stdout.
./dsco --subscription-lanes | jq .

# Three simultaneous cross-provider waves with enough output to measure decode.
./dsco --subscription-bench \
  --subscription-bench-rounds 3 \
  --subscription-bench-concurrency 1 \
  --subscription-bench-max-tokens 128 \
  -p 'Write one compact paragraph of about 80 tokens.' | jq .
```

The report includes per-lane TTFT, total latency, subscription queue time,
provider-service latency, HTTP outcome, reasoning/decode token counts, and
decode throughput plus aggregate fanout throughput. This separates ChatGPT's
cross-process anti-429 gate from upstream service time. Decode throughput is
`null` when a provider returns a single buffered chunk, omits token usage, or
leaves too short a decode interval; estimated token counts are labeled
separately. A missing credential leaves that lane `ready:false` rather than
silently using a metered key or external executor.

Kimi Code access tokens are refreshed natively before expiry. A 401 causes one
refresh-and-retry, coordinated by a cross-process lock; the rotated OAuth cache
is written atomically with mode 0600. The Kimi CLI is not spawned. The ChatGPT
subscription backend does not accept a per-request `max_output_tokens` field,
so `--subscription-bench-max-tokens` is provider-managed for that lane while
remaining a hard request parameter for providers that expose one.

## Verified lane inventory (sweep evidence, 2026-07-12)

- **anthropic** 8/10: sonnet-5, fable-5, sonnet-4-6, opus-4-8/4-7/4-6,
  opus-4-1-20250805, haiku-4-5-20251001. Dated opus-4-5/sonnet-4-5 snapshots
  400 on current request shape.
- **openai** 21/21: gpt-4o/4.1 families, gpt-5 → 5.6 (sol/terra/luna) + minis/nanos.
- **zai** 8/8: glm-4.5 → glm-5.2 incl 4.5-air, 5-turbo.
- **xai** 4/4: grok-4.20 (both variants), 4.3, 4.5.
- **deepseek** 2/2: v4-flash, v4-pro. **cerebras** 1: gemma-4-31b.
- **openrouter :free mesh** 9: poolside x2, cohere north-mini-code,
  tencent hy3, nvidia nemotron x4 (upstream weather varies), gpt-oss x2.

## Known failure classes (fix queue)

1. o-series rejects `parallel_tool_calls`; groq rejects `prompt_cache_key`/
   `cache_control`; cerebras rejects `prompt_cache_retention` → per-provider
   request-shape gating needed.
2. Legacy models (gpt-3.5/4 classic) need `max_tokens` clamp from catalog.
3. `*-codex` models require the subscription backend (404 on metered API).
4. Small models drift on lane discipline (misreport identity, call tools).
   Trust lane metadata, never model self-report.
5. Account states: openrouter balance negative, together $0, moonshot
   suspended, zai coding-plan token expired, jina balance empty.
   parallel-ai is a task/search API, not a chat lane.

## Ops notes

- Struct changes under `include/swarm.h` require `make clean` — stale
  incremental objects caused two mfm_free heap-corruption crashes (2026-07-12).
- Per-provider throttling not yet implemented: 8-parallel on one vendor can
  self-inflict 429s (observed: moonshot, sakana).
