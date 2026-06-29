# Governed Consumer Profile Ontology

Generated: `2026-06-28T01:19:50+00:00`  
Version: `2026.06`  
Leaf facets: **10,000**

This package implements a product-grade consumer profile ontology as a governed 10,000-leaf facet taxonomy. It is intentionally not a flat ad-interest list. Each facet carries sensitivity, provenance, decay, allowed uses, disallowed uses, user-control metadata, explanation hooks, and policy notes.

## Artifacts

| Path | Purpose |
|---|---|
| `data/consumer_profile_ontology/facet_definitions.jsonl` | Canonical 10,000 facet definitions, one JSON object per line. |
| `data/consumer_profile_ontology/facet_definitions.csv` | Spreadsheet-friendly facet registry. |
| `data/consumer_profile_ontology/facet_budget.csv` | Domain/subdomain budget allocation. |
| `data/consumer_profile_ontology/summary.json` | Counts by domain, sensitivity class, and value type. |
| `data/consumer_profile_ontology/prohibited_patterns.json` | S4 prohibited inference/activation boundaries. |
| `schemas/consumer_profile_ontology/*.schema.json` | JSON Schemas for definitions, values, evidence, and activation. |
| `scripts/consumer_profile_ontology/generate_ontology.py` | Deterministic generator. |

## Core Invariant

A facet is not a tag. A facet is a modeled user attribute with governance.

Every facet includes:

- `facet_id`
- semantic path
- value type
- source classes
- confidence model
- half-life / TTL where relevant
- sensitivity class
- proxy risk class
- user visibility/editability/deletability
- allowed uses
- disallowed uses
- explanation template
- policy notes

## Sensitivity Classes

| Class | Meaning |
|---|---|
| `S0_OPERATIONAL` | Technical, delivery, consent, security, reliability, localization, or control-plane state. |
| `S1_STANDARD` | Ordinary personalization facets such as broad interests, formats, and non-sensitive commerce categories. |
| `S2_PRIVATE` | Potentially private context or life-stage information. Requires stronger controls. |
| `S3_RESTRICTED` | Explicit/necessary/user-visible restricted data for accessibility, youth safety, precise controls, or security. |
| `S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION` | Boundary class. Do not infer or activate for general personalization/targeting. |

## Domain Budget

| Domain | Facets |
|---|---:|
| `gov` — Governance, provenance, and profile administration | 300 |
| `demo` — Demographics, life stage, and household context | 700 |
| `geo` — Geography, mobility, and local context | 700 |
| `tech` — Device, connectivity, and technical environment | 500 |
| `comm` — Language, communication, and media preferences | 350 |
| `social` — Social graph, communities, and influence context | 650 |
| `content` — Content interests and knowledge domains | 2,200 |
| `entertainment` — Entertainment, culture, and fandoms | 850 |
| `commerce` — Commerce, brands, shopping, and product preferences | 1,000 |
| `life` — Life events, planning, and temporal intent | 650 |
| `work` — Work, education, skills, and professional context | 600 |
| `lifestyle` — Wellness, fitness, and lifestyle needs | 500 |
| `values` — Values, causes, civic, and nonprofit context | 300 |
| `privacy` — Privacy, safety, accessibility, and personalization constraints | 700 |

| **Total** | **10,000** |

## Usage

Regenerate artifacts:

```bash
python3 scripts/consumer_profile_ontology/generate_ontology.py
```

Inspect exact counts:

```bash
jq -r '.counts_by_domain, .counts_by_sensitivity' data/consumer_profile_ontology/summary.json
wc -l data/consumer_profile_ontology/facet_definitions.jsonl
```

Example facet:

```json
{
  "facet_id": "u.gov.profile.completeness.score",
  "display_name": "Completeness score",
  "description": "Governed score facet for completeness in profile quality and identity resolution.",
  "path": [
    "gov",
    "profile",
    "completeness",
    "score"
  ],
  "domain": "gov",
  "domain_name": "Governance, provenance, and profile administration",
  "subdomain": "profile",
  "subdomain_name": "Profile quality and identity resolution",
  "topic": "completeness",
  "facet_kind": "governed_profile_facet",
  "relationship_type": "score",
  "value_type": "float",
  "value_range": [
    0,
    1
  ],
  "scope": "user",
  "cardinality": "single_value",
  "source_classes": [
    "explicit",
    "system_state",
    "policy_engine"
  ],
  "confidence_model": "bayesian_decay_v3",
  "decay_model": "state_until_changed",
  "half_life_days": null,
  "ttl_days": null,
  "minimum_evidence_count": 1,
  "minimum_confidence_for_activation": 0.55,
  "sensitivity_class": "S0_OPERATIONAL",
  "proxy_risk_class": "P0_LOW",
  "user_visible": true,
  "user_editable": false,
  "user_deletable": false,
  "allowed_uses": [
    "delivery",
    "security",
    "localization",
    "product_reliability",
    "user_controls",
    "measurement_aggregate"
  ],
  "disallowed_uses": [
    "eligibility_decisioning",
    "protected_trait_inference"
  ],
  "explanation_template": "This facet may be used because of your settings or interactions related to completeness. You can review available controls in profile settings.",
  "policy_notes": "Do not combine with S4 prohibited traits or sensitive-place signals. Enforce purpose limitation before activation.",
  "owner_team": "profile_ontology",
  "review_status": "approved_generated",
  "version": "2026.06",
  "ordinal": 0,
  "created_at": "2026-06-28T01:19:50+00:00",
  "updated_at": "2026-06-28T01:19:50+00:00"
}
```

## Policy Boundary

The generated ontology intentionally does **not** create activation facets for protected traits, sensitive-place visits, medical diagnosis inference, addiction status, pregnancy inference, immigration status, criminal history, precise financial distress, or other S4 categories.

Those categories are represented only in `prohibited_patterns.json` as governance boundaries.
