# Architecture

```text
                    ┌────────────────────┐
                    │  Ontology Registry │
                    │  10,000 facets     │
                    └─────────┬──────────┘
                              │
                              ▼
┌──────────────┐      ┌──────────────────┐      ┌────────────────┐
│ Raw Signals  │ ───▶ │ Evidence Builder │ ───▶ │ Profile Store  │
└──────────────┘      └──────────────────┘      └───────┬────────┘
                                                         │
                                                         ▼
                    ┌────────────────────┐      ┌────────────────┐
                    │ Consent / Privacy  │ ───▶ │ Policy Engine  │
                    └────────────────────┘      └───────┬────────┘
                                                         │
                                                         ▼
         ┌────────────────────────────────────────────────────────┐
         │ Activation: ranking, recs, ads, measurement, safety     │
         └────────────────────────────────────────────────────────┘
                                                         │
                                                         ▼
                    ┌────────────────────┐
                    │ Audit + Explanation│
                    └────────────────────┘
```

## Storage Tables

Recommended tables:

- `facet_definitions`
- `user_facet_values`
- `facet_evidence`
- `consent_grants`
- `activation_audit_log`
- `user_suppressions`
- `policy_decisions`

See the JSON Schemas for concrete object contracts.
