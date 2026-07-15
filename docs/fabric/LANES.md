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
| sakana | fugu (base; ultra is opt-in — cost) | FUGU/SAKANA key |
| anthropic | claude-sonnet-5 | Claude Code OAuth / API key |
| openai-codex | gpt-5.6-sol, gpt-5.6-terra, gpt-5.5 | ChatGPT OAuth |
| zai | glm-5.2 | GLM key |

`gpt-5.6-luna` 404s on the ChatGPT subscription backend (2026-07-12) but works
on metered `openai:` — env-inject it if needed.

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
