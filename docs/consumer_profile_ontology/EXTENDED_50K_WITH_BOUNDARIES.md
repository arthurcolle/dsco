# Extended 50k Ontology with Hyper-Private Boundaries

Generated: `2026-06-28T01:33:15+00:00`  
Version: `2026.06.extended50k_plus_s4_boundaries`

This combines:

- 50,000 active governed facets, and
- 512 S4 deactivated hyper-private boundary facets.

Total registry size: **50,512** rows.

## Files

| Path | Purpose |
|---|---|
| `data/consumer_profile_ontology/facet_definitions_extended_50k_with_boundaries.jsonl` | Full extended ontology plus S4 negative ontology boundaries. |
| `data/consumer_profile_ontology/facet_definitions_extended_50k_with_boundaries.csv` | Spreadsheet-friendly combined registry. |
| `data/consumer_profile_ontology/summary_extended_50k_with_boundaries.json` | Counts and hash. |

## Invariant

S4 boundary facets are modeled only so the system knows what not to collect, infer, store, activate, or proxy. They are registry/policy/test artifacts, not user profile values.

SHA-256:

```text
f6afb8e3281fcb470fe92a0a4ef08dfbdf0b1334e349d12cb435270ee0f04a13
```
