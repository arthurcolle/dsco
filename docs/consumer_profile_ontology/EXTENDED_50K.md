# Extended 50,000-Facet Consumer Profile Ontology

Generated: `2026-06-28T01:27:33+00:00`  
Version: `2026.06.extended50k`

This is an expansion of the base governed 10,000-facet ontology.

## Counts

| Set | Facets |
|---|---:|
| Base ontology | 10,000 |
| Expansion delta | 40,000 |
| Extended total | 50,000 |

## Files

| Path | Purpose |
|---|---|
| `data/consumer_profile_ontology/facet_definitions_extended_50k.jsonl` | Full 50,000-facet registry. |
| `data/consumer_profile_ontology/facet_expansion_delta_40k.jsonl` | Only the 40,000 newly generated facets. |
| `data/consumer_profile_ontology/facet_definitions_extended_50k.csv` | Spreadsheet-friendly 50,000-facet registry. |
| `data/consumer_profile_ontology/summary_extended_50k.json` | Counts and hashes. |

## Delta Allocation

| Domain | Added facets |
|---|---:|
| `comm` | 1,500 |
| `commerce` | 6,500 |
| `content` | 9,000 |
| `entertainment` | 3,500 |
| `geo` | 2,500 |
| `gov` | 1,000 |
| `life` | 1,500 |
| `lifestyle` | 3,500 |
| `privacy` | 1,500 |
| `social` | 2,000 |
| `tech` | 2,000 |
| `values` | 1,000 |
| `work` | 4,500 |


## Safety Boundary

The extension preserves the base policy boundary:

- no S4 activation facets,
- no protected-trait inference facets,
- no sensitive-place targeting facets,
- no medical diagnosis, addiction, pregnancy, immigration, criminal-history, or precise-financial-distress inference facets,
- every facet carries allowed/disallowed uses and user-control metadata.

## Verification

```bash
wc -l data/consumer_profile_ontology/facet_definitions_extended_50k.jsonl
jq . data/consumer_profile_ontology/summary_extended_50k.json
```

SHA-256 full JSONL:

```text
bd94e027098b8e023e6e26b593f36314b028ada29dd5bf41e23689032aeb173e
```

SHA-256 delta JSONL:

```text
cd9f9314b92b00e47d9b5dbf7964bdc5ffbf32d86dfc39cc5221b684b0f92ed3
```
