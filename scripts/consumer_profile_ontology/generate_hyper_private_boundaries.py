#!/usr/bin/env python3
"""
Generate hyper-private / S4 boundary facets for the consumer profile ontology.

These are NOT collectible user-profile facets. They are named negative ontology
entries used so policy, schemas, feature pipelines, reviews, and tests can reason
about the surface that must not be collected, inferred, stored in profiles, or
activated.

Outputs:
- hyper_private_boundary_facets.jsonl/csv: S4 boundary registry only
- facet_definitions_base_with_boundaries.jsonl/csv: 10k active base + S4 boundaries
- summary_base_with_boundaries.json
- docs/HYPER_PRIVATE_BOUNDARIES.md
"""
from __future__ import annotations

import csv
import hashlib
import json
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

import generate_ontology as base  # noqa: E402

OUT_DATA = ROOT / "data" / "consumer_profile_ontology"
OUT_DOCS = ROOT / "docs" / "consumer_profile_ontology"
VERSION = "2026.06.boundary_s4"
GENERATED_AT = datetime.now(timezone.utc).replace(microsecond=0).isoformat()

RELATIONS = [
    "inference_boundary",
    "collection_boundary",
    "activation_boundary",
    "proxy_boundary",
]

# 32 classes x 4 concepts x 4 relation-boundary types = 512 S4 boundary facets.
# These names are governance handles only. They must not become profile values.
BOUNDARY_CLASSES: Dict[str, List[str]] = {
    "protected_class": [
        "race_or_ethnicity",
        "skin_tone_or_phenotype",
        "national_origin_sensitive",
        "caste_or_ancestry_sensitive",
    ],
    "religion_belief": [
        "religious_affiliation",
        "religious_practice",
        "religious_conversion_interest",
        "atheism_or_nonbelief_identity",
    ],
    "sexual_orientation": [
        "sexual_orientation_identity",
        "same_sex_relationship_inference",
        "lgbtq_identity_inference",
        "sexual_life_or_preference",
    ],
    "gender_identity": [
        "gender_identity_sensitive",
        "transgender_identity_inference",
        "gender_transition_context",
        "gender_dysphoria_context",
    ],
    "political_persuasion": [
        "party_affiliation",
        "ideology_inference",
        "candidate_support",
        "issue_persuasion_sensitive",
    ],
    "union_labor": [
        "union_membership",
        "labor_organizing_activity",
        "strike_participation",
        "workplace_collective_action",
    ],
    "health_diagnosis": [
        "diagnosed_condition",
        "chronic_illness_inference",
        "medical_treatment_context",
        "prescription_medication_inference",
    ],
    "mental_health": [
        "depression_inference",
        "anxiety_disorder_inference",
        "self_harm_risk_inference_for_targeting",
        "therapy_or_counseling_inference",
    ],
    "addiction_recovery": [
        "substance_use_disorder_inference",
        "addiction_recovery_status",
        "rehab_or_treatment_context",
        "relapse_vulnerability_inference",
    ],
    "reproductive_health": [
        "pregnancy_inference",
        "fertility_treatment_inference",
        "abortion_context",
        "contraception_sensitive_context",
    ],
    "disability_access": [
        "disability_inference",
        "mobility_impairment_inference",
        "cognitive_disability_inference",
        "sensory_disability_inference",
    ],
    "genetic_biometric": [
        "genetic_information",
        "biometric_identifier_faceprint",
        "biometric_identifier_voiceprint",
        "dna_or_family_genetic_risk",
    ],
    "immigration_citizenship": [
        "immigration_status",
        "visa_status_sensitive",
        "asylum_or_refugee_context",
        "deportation_risk_context",
    ],
    "criminal_legal": [
        "criminal_history",
        "arrest_record_inference",
        "incarceration_history",
        "probation_or_parole_context",
    ],
    "financial_distress": [
        "bankruptcy_inference",
        "eviction_risk_inference",
        "debt_distress_inference",
        "payday_loan_vulnerability",
    ],
    "housing_insecurity": [
        "homelessness_inference",
        "shelter_visit_context",
        "housing_insecurity_inference",
        "domestic_instability_context",
    ],
    "domestic_safety": [
        "domestic_violence_resource_context",
        "stalking_vulnerability_context",
        "protective_order_context",
        "safe_house_context",
    ],
    "sensitive_places_health": [
        "clinic_visit_context",
        "hospital_visit_context",
        "reproductive_health_clinic_context",
        "mental_health_facility_context",
    ],
    "sensitive_places_belief": [
        "place_of_worship_visit_context",
        "religious_school_visit_context",
        "spiritual_retreat_context",
        "faith_community_center_context",
    ],
    "sensitive_places_political": [
        "protest_site_visit_context",
        "campaign_office_visit_context",
        "political_meeting_context",
        "polling_place_visit_context",
    ],
    "sensitive_places_support": [
        "addiction_center_visit_context",
        "legal_aid_visit_context",
        "immigration_services_visit_context",
        "crisis_center_visit_context",
    ],
    "minor_youth_sensitive": [
        "minor_vulnerability_inference",
        "school_location_precise_context",
        "youth_behavioral_targeting",
        "child_interest_exploitation_risk",
    ],
    "intimate_relationships": [
        "divorce_inference",
        "infidelity_inference",
        "dating_status_sensitive",
        "relationship_breakdown_context",
    ],
    "sexual_content_vulnerability": [
        "adult_content_consumption_sensitive",
        "sexual_exploitation_vulnerability",
        "sex_work_inference",
        "intimate_image_context",
    ],
    "precise_location": [
        "home_address_precise",
        "work_address_precise",
        "real_time_location_for_ads",
        "persistent_location_trail",
    ],
    "identity_documents": [
        "government_id_number",
        "passport_number",
        "tax_identifier",
        "driver_license_number",
    ],
    "payment_secrets": [
        "full_payment_card_number",
        "bank_account_number",
        "credit_score_raw",
        "income_exact_sensitive",
    ],
    "security_secrets": [
        "password_or_passphrase",
        "mfa_secret",
        "private_key_material",
        "session_token_secret",
    ],
    "communications_content": [
        "private_message_content_sensitive",
        "email_body_sensitive",
        "call_audio_content_sensitive",
        "contact_list_sensitive",
    ],
    "legal_sensitive": [
        "lawsuit_party_status",
        "legal_strategy_context",
        "attorney_client_context",
        "victim_witness_context",
    ],
    "vulnerability_state": [
        "grief_inference",
        "loneliness_vulnerability",
        "employment_desperation",
        "crisis_state_for_targeting",
    ],
    "proxy_constructs": [
        "protected_trait_proxy_cluster",
        "sensitive_place_proxy_cluster",
        "vulnerability_proxy_cluster",
        "regulated_eligibility_proxy_cluster",
    ],
}


def slug(s: str) -> str:
    return base.slug(s)


def make_boundary(boundary_class: str, concept: str, relation: str, ordinal: int) -> Dict:
    boundary_class = slug(boundary_class)
    concept = slug(concept)
    relation = slug(relation)
    facet_id = f"u.boundary.hyper_private.{boundary_class}.{concept}.{relation}"
    return {
        "facet_id": facet_id,
        "display_name": f"Boundary: {concept.replace('_', ' ')} {relation.replace('_', ' ')}",
        "description": (
            "S4 negative-ontology boundary facet. This names a hyper-private or prohibited "
            "surface so pipelines can block collection, inference, storage, activation, and proxy use. "
            "It is not a collectible user-profile attribute."
        ),
        "path": ["boundary", "hyper_private", boundary_class, concept, relation],
        "domain": "boundary",
        "domain_name": "Hyper-private boundary and negative ontology",
        "subdomain": "hyper_private",
        "subdomain_name": "Terms-of-service prohibited or deactivated sensitive surfaces",
        "boundary_class": boundary_class,
        "topic": concept,
        "facet_kind": "negative_ontology_boundary_facet",
        "relationship_type": relation,
        "value_type": "null",
        "value_range": None,
        "scope": "policy_boundary_not_user_profile",
        "cardinality": "not_applicable",
        "source_classes": [],
        "confidence_model": "not_applicable_do_not_infer",
        "decay_model": "not_applicable_do_not_store",
        "half_life_days": None,
        "ttl_days": None,
        "minimum_evidence_count": None,
        "minimum_confidence_for_activation": None,
        "sensitivity_class": "S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION",
        "proxy_risk_class": "P4_PROHIBITED_PROXY_SURFACE",
        "facet_state": "deactivated_boundary",
        "collection_allowed": False,
        "inference_allowed": False,
        "evidence_allowed": False,
        "profile_storage_allowed": False,
        "activation_allowed": False,
        "ads_allowed": False,
        "ranking_allowed": False,
        "recommendation_allowed": False,
        "measurement_allowed": False,
        "user_visible": False,
        "user_editable": False,
        "user_deletable": False,
        "allowed_uses": [
            "ontology_governance",
            "policy_blocking",
            "negative_feature_filtering",
            "schema_validation",
            "terms_enforcement",
            "red_team_test_generation",
        ],
        "disallowed_uses": [
            "collection",
            "inference",
            "profile_storage",
            "feed_ranking",
            "recommendation",
            "ads_personalized",
            "ads_contextual_user_profile",
            "lookalike_modeling",
            "audience_creation",
            "eligibility_decisioning",
            "third_party_sharing",
            "measurement_user_level",
            "protected_trait_inference",
            "exploitative_targeting",
            "proxy_targeting",
        ],
        "explanation_template": "This is a policy boundary, not a user profile facet. The system is configured not to collect, infer, store, or activate it.",
        "policy_notes": "Boundary-only S4 facet. May appear in registries, policy tests, and feature-denylist checks. Must not appear in user_facet_values or activation payloads except as a blocked reason.",
        "storage_location": "ontology_registry_only_not_user_profile",
        "tos_policy": "prohibited_sensitive_inference_or_activation",
        "owner_team": "profile_ontology_policy",
        "review_status": "approved_boundary_only",
        "version": VERSION,
        "ordinal": ordinal,
        "created_at": GENERATED_AT,
        "updated_at": GENERATED_AT,
    }


def generate_boundaries() -> List[Dict]:
    rows: List[Dict] = []
    ordinal = 0
    for boundary_class, concepts in BOUNDARY_CLASSES.items():
        for concept in concepts:
            for relation in RELATIONS:
                rows.append(make_boundary(boundary_class, concept, relation, ordinal))
                ordinal += 1
    assert len(rows) == 512, len(rows)
    ids = [r["facet_id"] for r in rows]
    assert len(ids) == len(set(ids))
    return rows


def write_outputs(base_facets: List[Dict], boundaries: List[Dict]) -> None:
    OUT_DATA.mkdir(parents=True, exist_ok=True)
    OUT_DOCS.mkdir(parents=True, exist_ok=True)
    combined = base_facets + boundaries

    files = {
        "hyper_private_boundary_facets.jsonl": boundaries,
        "facet_definitions_base_with_boundaries.jsonl": combined,
    }
    for name, rows in files.items():
        path = OUT_DATA / name
        with path.open("w") as f:
            for row in rows:
                f.write(json.dumps(row, sort_keys=True) + "\n")

    fields = [
        "facet_id", "display_name", "domain", "subdomain", "boundary_class", "topic", "relationship_type",
        "facet_kind", "facet_state", "sensitivity_class", "proxy_risk_class", "value_type", "collection_allowed",
        "inference_allowed", "evidence_allowed", "profile_storage_allowed", "activation_allowed", "allowed_uses",
        "disallowed_uses", "storage_location", "tos_policy", "version",
    ]
    for name, rows in {
        "hyper_private_boundary_facets.csv": boundaries,
        "facet_definitions_base_with_boundaries.csv": combined,
    }.items():
        with (OUT_DATA / name).open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            w.writeheader()
            for row in rows:
                out = {k: row.get(k) for k in fields}
                for k in ["allowed_uses", "disallowed_uses"]:
                    if isinstance(out.get(k), list):
                        out[k] = ";".join(out[k])
                w.writerow(out)

    boundary_jsonl = OUT_DATA / "hyper_private_boundary_facets.jsonl"
    combined_jsonl = OUT_DATA / "facet_definitions_base_with_boundaries.jsonl"
    summary = {
        "generated_at": GENERATED_AT,
        "version": VERSION,
        "base_active_facets": len(base_facets),
        "boundary_facets": len(boundaries),
        "total_base_with_boundaries": len(combined),
        "boundary_relation_count": dict(sorted(Counter(r["relationship_type"] for r in boundaries).items())),
        "boundary_class_count": dict(sorted(Counter(r["boundary_class"] for r in boundaries).items())),
        "combined_counts_by_sensitivity": dict(sorted(Counter(r["sensitivity_class"] for r in combined).items())),
        "sha256_hyper_private_boundary_facets_jsonl": hashlib.sha256(boundary_jsonl.read_bytes()).hexdigest(),
        "sha256_facet_definitions_base_with_boundaries_jsonl": hashlib.sha256(combined_jsonl.read_bytes()).hexdigest(),
    }
    (OUT_DATA / "summary_base_with_boundaries.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    doc = f"""# Hyper-Private Boundary Facets

Generated: `{GENERATED_AT}`  
Version: `{VERSION}`

This adds an explicit **negative ontology** to the base consumer profile model.

These facets are named and modeled so the system can represent what it must **not** collect, infer, store, activate, target, export, or use as a proxy. They are intentionally S4 and deactivated.

## Counts

| Set | Count |
|---|---:|
| Active base facets | {len(base_facets):,} |
| Hyper-private boundary facets | {len(boundaries):,} |
| Base with boundaries total | {len(combined):,} |

## Files

| Path | Purpose |
|---|---|
| `data/consumer_profile_ontology/hyper_private_boundary_facets.jsonl` | Boundary-only S4 registry. |
| `data/consumer_profile_ontology/hyper_private_boundary_facets.csv` | Spreadsheet-friendly boundary registry. |
| `data/consumer_profile_ontology/facet_definitions_base_with_boundaries.jsonl` | Active 10k base + 512 S4 boundaries. |
| `data/consumer_profile_ontology/facet_definitions_base_with_boundaries.csv` | Spreadsheet-friendly combined registry. |
| `data/consumer_profile_ontology/summary_base_with_boundaries.json` | Counts and hashes. |

## Boundary Semantics

Every S4 boundary facet has:

```json
{{
  "facet_state": "deactivated_boundary",
  "collection_allowed": false,
  "inference_allowed": false,
  "evidence_allowed": false,
  "profile_storage_allowed": false,
  "activation_allowed": false,
  "ads_allowed": false,
  "ranking_allowed": false,
  "recommendation_allowed": false
}}
```

Allowed uses are governance-only:

- ontology governance,
- policy blocking,
- negative feature filtering,
- schema validation,
- terms enforcement,
- red-team test generation.

## Important Invariant

A boundary facet may exist in the ontology registry, but it must not appear as a user-profile value. It may appear only as:

1. a policy-block reason,
2. a schema validation denylist hit,
3. a feature pipeline rejection reason,
4. a red-team or audit test case,
5. a TOS enforcement concept.

## Example

```json
{json.dumps(boundaries[0], indent=2)}
```
"""
    (OUT_DOCS / "HYPER_PRIVATE_BOUNDARIES.md").write_text(doc)


def validate(base_facets: List[Dict], boundaries: List[Dict]) -> None:
    assert len(boundaries) == 512
    base_ids = {f["facet_id"] for f in base_facets}
    boundary_ids = {f["facet_id"] for f in boundaries}
    assert not (base_ids & boundary_ids)
    for f in boundaries:
        assert f["sensitivity_class"] == "S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION"
        assert f["facet_state"] == "deactivated_boundary"
        assert f["collection_allowed"] is False
        assert f["inference_allowed"] is False
        assert f["profile_storage_allowed"] is False
        assert f["activation_allowed"] is False
        assert f["allowed_uses"] == [
            "ontology_governance",
            "policy_blocking",
            "negative_feature_filtering",
            "schema_validation",
            "terms_enforcement",
            "red_team_test_generation",
        ]


def main() -> None:
    base_facets = base.generate()
    boundaries = generate_boundaries()
    validate(base_facets, boundaries)
    write_outputs(base_facets, boundaries)
    summary = json.loads((OUT_DATA / "summary_base_with_boundaries.json").read_text())
    print(json.dumps({
        "generated": True,
        "base_active_facets": summary["base_active_facets"],
        "boundary_facets": summary["boundary_facets"],
        "total_base_with_boundaries": summary["total_base_with_boundaries"],
        "combined_counts_by_sensitivity": summary["combined_counts_by_sensitivity"],
        "sha256_boundaries": summary["sha256_hyper_private_boundary_facets_jsonl"],
        "sha256_combined": summary["sha256_facet_definitions_base_with_boundaries_jsonl"],
    }, indent=2))


if __name__ == "__main__":
    main()
