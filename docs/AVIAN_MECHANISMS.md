# Avian Mechanisms

DSCO's Wings layer now includes explicit bird-inspired coordination primitives beyond pheromones.

## Mechanisms

| Mechanism | Runtime concept | Use when | Safety invariant |
|---|---|---|---|
| Nesting | Bounded workspace/context container | A task needs lined context, materials, constraints, and stability before execution | No candidate should incubate without a nest/context boundary |
| Brooding | Incubation loop for fragile candidates | A plan, patch, strategy, or self-improvement idea is promising but immature | Repeated tending cycles + evidence are required before promotion |
| Fledging | Promotion of a mature candidate | Brooded work reaches readiness and cycle thresholds | High-risk candidates require higher readiness |
| Roosting | Quiescent cooldown state | A nest is hot, failing, costly, or cognitively saturated | Pause without destroying lineage |
| Molting | Refresh stale lining/material | Context has drifted or assumptions became stale | Reduce confidence/stability but preserve provenance |

## Tool surface

A new `avian` tool is registered:

```json
{"action":"nest","op":"create","name":"release-nest","purpose":"stabilize release candidate","warmth":0.4,"stability":0.5}
{"action":"nest","op":"material","nest_id":1,"material":"test plan","quality":0.9,"lining":true}
{"action":"brood","op":"lay","nest_id":1,"name":"patch candidate","kind":"code-change","risk":0.4,"required_cycles":3}
{"action":"brood","op":"tend","egg_id":1,"warmth":0.8,"evidence":"build clean"}
{"action":"brood","op":"fledge","egg_id":1}
{"action":"status"}
```

## Promotion rule

An egg can fledge only when:

1. `readiness >= 0.80`
2. `cycles >= required_cycles`
3. if `risk > 0.65`, then `readiness >= 0.92`

This makes brooding a real promotion gate, not just a metaphor.

## Architecture placement

- Layer: **Wings** — autonomous coordination and emergence
- Governed by: **Immune System** through the normal tool governance path
- Exposed in: `wings_talons_status` under `wings.avian`
- Source: `include/avian.h`, `src/avian.c`, `src/tools.c`
