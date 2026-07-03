#!/usr/bin/env python3
"""Combine the 50k extended ontology with S4 hyper-private boundary facets."""
from __future__ import annotations

import csv
import hashlib
import json
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

import generate_extended_ontology as ext  # noqa: E402
import generate_hyper_private_boundaries as hp  # noqa: E402

OUT_DATA = ROOT / "data" / "consumer_profile_ontology"
OUT_DOCS = ROOT / "docs" / "consumer_profile_ontology"
VERSION = "2026.06.extended50k_plus_s4_boundaries"
GENERATED_AT = datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def main() -> None:
    base_facets = ext.base.generate()
    existing = {f["facet_id"] for f in base_facets}
    delta = ext.generate_delta(existing)
    full_50k = base_facets + delta
    boundaries = hp.generate_boundaries()
    all_rows = full_50k + boundaries
    ids = [r["facet_id"] for r in all_rows]
    assert len(ids) == len(set(ids)), "duplicate facet IDs"
    assert len(full_50k) == 50000
    assert len(boundaries) == 512
    assert len(all_rows) == 50512

    OUT_DATA.mkdir(parents=True, exist_ok=True)
    OUT_DOCS.mkdir(parents=True, exist_ok=True)

    jsonl = OUT_DATA / "facet_definitions_extended_50k_with_boundaries.jsonl"
    with jsonl.open("w") as f:
        for row in all_rows:
            f.write(json.dumps(row, sort_keys=True) + "\n")

    fields = [
        "facet_id", "display_name", "domain", "subdomain", "boundary_class", "topic", "context", "relationship_type",
        "facet_kind", "facet_state", "sensitivity_class", "proxy_risk_class", "value_type", "collection_allowed",
        "inference_allowed", "evidence_allowed", "profile_storage_allowed", "activation_allowed", "user_visible",
        "user_editable", "user_deletable", "allowed_uses", "disallowed_uses", "source_classes", "version",
    ]
    with (OUT_DATA / "facet_definitions_extended_50k_with_boundaries.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for row in all_rows:
            out = {k: row.get(k) for k in fields}
            for k in ["allowed_uses", "disallowed_uses", "source_classes"]:
                if isinstance(out.get(k), list):
                    out[k] = ";".join(out[k])
            w.writerow(out)

    summary = {
        "generated_at": GENERATED_AT,
        "version": VERSION,
        "active_extended_facets": len(full_50k),
        "boundary_facets": len(boundaries),
        "total_extended_with_boundaries": len(all_rows),
        "counts_by_domain": dict(sorted(Counter(r["domain"] for r in all_rows).items())),
        "counts_by_sensitivity": dict(sorted(Counter(r["sensitivity_class"] for r in all_rows).items())),
        "boundary_class_count": dict(sorted(Counter(r.get("boundary_class", "none") for r in boundaries).items())),
        "sha256_facet_definitions_extended_50k_with_boundaries_jsonl": hashlib.sha256(jsonl.read_bytes()).hexdigest(),
    }
    (OUT_DATA / "summary_extended_50k_with_boundaries.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    doc = f"""# Extended 50k Ontology with Hyper-Private Boundaries

Generated: `{GENERATED_AT}`  
Version: `{VERSION}`

This combines:

- 50,000 active governed facets, and
- 512 S4 deactivated hyper-private boundary facets.

Total registry size: **{len(all_rows):,}** rows.

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
{summary['sha256_facet_definitions_extended_50k_with_boundaries_jsonl']}
```
"""
    (OUT_DOCS / "EXTENDED_50K_WITH_BOUNDARIES.md").write_text(doc)

    print(json.dumps({
        "generated": True,
        "active_extended_facets": len(full_50k),
        "boundary_facets": len(boundaries),
        "total_extended_with_boundaries": len(all_rows),
        "counts_by_sensitivity": summary["counts_by_sensitivity"],
        "sha256": summary["sha256_facet_definitions_extended_50k_with_boundaries_jsonl"],
    }, indent=2))


if __name__ == "__main__":
    main()
