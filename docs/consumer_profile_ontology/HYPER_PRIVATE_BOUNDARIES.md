# Hyper-Private Boundary Facets

Generated: `2026-06-28T01:32:12+00:00`  
Version: `2026.06.boundary_s4`

This adds an explicit **negative ontology** to the base consumer profile model.

These facets are named and modeled so the system can represent what it must **not** collect, infer, store, activate, target, export, or use as a proxy. They are intentionally S4 and deactivated.

## Counts

| Set | Count |
|---|---:|
| Active base facets | 10,000 |
| Hyper-private boundary facets | 512 |
| Base with boundaries total | 10,512 |

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
{
  "facet_state": "deactivated_boundary",
  "collection_allowed": false,
  "inference_allowed": false,
  "evidence_allowed": false,
  "profile_storage_allowed": false,
  "activation_allowed": false,
  "ads_allowed": false,
  "ranking_allowed": false,
  "recommendation_allowed": false
}
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
{
  "facet_id": "u.boundary.hyper_private.protected_class.race_or_ethnicity.inference_boundary",
  "display_name": "Boundary: race or ethnicity inference boundary",
  "description": "S4 negative-ontology boundary facet. This names a hyper-private or prohibited surface so pipelines can block collection, inference, storage, activation, and proxy use. It is not a collectible user-profile attribute.",
  "path": [
    "boundary",
    "hyper_private",
    "protected_class",
    "race_or_ethnicity",
    "inference_boundary"
  ],
  "domain": "boundary",
  "domain_name": "Hyper-private boundary and negative ontology",
  "subdomain": "hyper_private",
  "subdomain_name": "Terms-of-service prohibited or deactivated sensitive surfaces",
  "boundary_class": "protected_class",
  "topic": "race_or_ethnicity",
  "facet_kind": "negative_ontology_boundary_facet",
  "relationship_type": "inference_boundary",
  "value_type": "null",
  "value_range": null,
  "scope": "policy_boundary_not_user_profile",
  "cardinality": "not_applicable",
  "source_classes": [],
  "confidence_model": "not_applicable_do_not_infer",
  "decay_model": "not_applicable_do_not_store",
  "half_life_days": null,
  "ttl_days": null,
  "minimum_evidence_count": null,
  "minimum_confidence_for_activation": null,
  "sensitivity_class": "S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION",
  "proxy_risk_class": "P4_PROHIBITED_PROXY_SURFACE",
  "facet_state": "deactivated_boundary",
  "collection_allowed": false,
  "inference_allowed": false,
  "evidence_allowed": false,
  "profile_storage_allowed": false,
  "activation_allowed": false,
  "ads_allowed": false,
  "ranking_allowed": false,
  "recommendation_allowed": false,
  "measurement_allowed": false,
  "user_visible": false,
  "user_editable": false,
  "user_deletable": false,
  "allowed_uses": [
    "ontology_governance",
    "policy_blocking",
    "negative_feature_filtering",
    "schema_validation",
    "terms_enforcement",
    "red_team_test_generation"
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
    "proxy_targeting"
  ],
  "explanation_template": "This is a policy boundary, not a user profile facet. The system is configured not to collect, infer, store, or activate it.",
  "policy_notes": "Boundary-only S4 facet. May appear in registries, policy tests, and feature-denylist checks. Must not appear in user_facet_values or activation payloads except as a blocked reason.",
  "storage_location": "ontology_registry_only_not_user_profile",
  "tos_policy": "prohibited_sensitive_inference_or_activation",
  "owner_team": "profile_ontology_policy",
  "review_status": "approved_boundary_only",
  "version": "2026.06.boundary_s4",
  "ordinal": 0,
  "created_at": "2026-06-28T01:32:12+00:00",
  "updated_at": "2026-06-28T01:32:12+00:00"
}
```
