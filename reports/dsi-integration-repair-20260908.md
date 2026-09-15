# DSI integration repair — 2026-09-08

Authority: Arthur requested execution following company swarm review. Local code/doc changes only; no deployment or external publication.

## Changed
- dsco-autobot/ToolManagement/src/tool_management/services/graphsub_client.py: restored missing gateway adapter, controlled parameter rendering, bounded statement/byte batches, failure propagation. Multi-batch writes are not globally atomic; idempotent MERGE permits replay.
- services/graph_client.py: re-export and missing substrate factory; GraphSub default, explicit legacy compatibility; canonical GRAPHSUB_BEARER_TOKEN with API-key fallback.
- deploy/modal/modal_app.py: shared configuration and private, explicitly invoked reconciliation function. Existing auth settings and legacy vault-key defaults preserved. No Modal import/deploy executed. Existing embedded vault defaults merit separate credential-remediation review; no key rotation authorized or performed.
- tests/test_graphsub_adapter_boundaries.py: 7 regression cases for data binding, invalid values, bearer contract, preflight, and fail-fast batches.
- GraphSub graphsub-context-pack/03-product-facts.md: replaced retired performance/comparison table with dated canonical embedded/HTTP measurements. Pricing untouched.
- dsco-router/README.md: distinguished documented promoted 2B adapter from 27B target and actual deployed default.

## Verification
- Baseline Tools graph suite failed collection because GraphSubClient was absent; after adapter restoration, three further deployment-wiring checks failed. Final unchanged existing tests plus new boundary cases: 20 passed, 14 dependency-deprecation warnings.
- Actual GraphSub gateway suite: 20 passed.
- Chimera gate test: 1 passed.
- Actual announce function -> actual adapter -> actual ASGI gateway -> mocked native backend: success reaches commit barrier; wrong bearer returns 401 before native backend. Two test-driver setup errors (import path and batch payload assertion) were corrected before final pass.
- Product-facts benchmark table checked for exact equality with canonical BENCHMARKS.md table; Router wording checked; tracked code diff whitespace check passed.

## State and remaining boundary
Changed and locally tested, NOT deployed. Production deployment requires Arthur approval. GRAPHSUB_URL and bearer must be supplied by deployment configuration; no destination or credential fabricated. No claim of live native persistence, cross-account isolation, or complete Tools/Chimera/GraphSub production workflow. Existing legacy vault-key defaults were noticed and remain untouched; remediation/rotation requires scoped authority. No active workers from prior review.

Economics: no new workers or paid provider evaluations; total coordinator inference cost unknown. Reuse: bounded adapter and tests. Next: approve and verify production deployment configuration and credential posture, then deploy and test actual durable round-trip. Rollback: use reviewed Git diffs for tracked edits; remove new adapter only together with factory/re-export and its tests. Do not reset unrelated concurrent edits.
