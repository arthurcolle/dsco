# Distributed Systems, Inc. — company swarm review

Date: 2026-09-08. Authority: Arthur requested a swarm review of his company under ~/dsco. Scope: sampled read-only repository review, not deployment, image-design review, or production certification.

## Conclusion
Tools, Chimera/Router and GraphSub are a deliberately integrated portfolio, supported by the DSCO local runtime. The opportunity is to make the combined authority-aware execution and persistent evidence path easy to adopt and independently verify, not replace the company with a generic consulting offer.

## Verified observations
- dsco-router/README.md:90-101 explicitly joins Tools execution, Router metering/routing/billing, and GraphSub state coordination.
- dsco-autobot/ToolManagement/docs/UNIFIED_CUSTOMER_IDENTITY.md:86-102 defines shared RS256 issuer/audience/account contracts across products; Tools is explicitly this repository. This establishes documented integration, not live deployment verification.
- dsco-graphsub-complex/dsco-graphsub/BENCHMARKS.md:24-27 retires old 67us/24.7M ops/s and competitor-ratio claims. graphsub-context-pack/03-product-facts.md:1-18 still commands reuse of those numbers. Confirmed documentation contradiction.
- dsco-router/README.md:51-59 leads with a 27B controller, while docs/chimera-qwen38-training.md:163-178 records a promoted 2B adapter and 80-row holdout. Clarify versions, candidate versus deployed default, and performance scope. Holdout correctness is not customer-workload cost/latency proof.
- GraphSub README.md:71-73 requires loopback engine exposure behind its authenticated platform gateway. Integration proof should use that boundary, not expose the raw engine.

## Rejected or qualified worker claims
- Old OAuth injection: current ToolManagement/src/tool_management/oauth.py:453 restricts script mode to signup/login.
- Old unverified-value aggregation: current dsco-cli/src/value_ledger.c:146-157 adds value only when passed and retains all attempt costs.
- Old missing Python discovery: current dsco-cli/src/mcp_server.c:42-43 includes dsco-python-3x.
These cited defects are corrected in source; production installation and regressions were not tested here.
- A worker inferred dsco-chat was the customer front door. That inference is not established and is excluded; canonical identity documentation explicitly identifies Tools.
- An old hold on Chimera automatic promotion must not override later promoted-2B documentation. Actual deployed controller remains unverified.
- Absence of an integrated proof in the sampled files is an evidence gap, not proof no integration exists.

## Recommended next milestone
One existing-product reference workflow: authenticate a scoped account, discover/authorize a tool, route a bounded task, execute, retain evidence in GraphSub, restart/retrieve it in a second task. Include denial, bounded recovery, account isolation, trace IDs, measured latency, and clearly labeled actual/estimated costs. First locate any existing proof and close its gaps rather than build a competing demo.

## Swarm accounting
Run: swarm_1788900656_g0_create_dsi-company-review. Three native DSCO workers, openai-codex subscription lane, openai/gpt-5.6-luna. Two completed; engineering worker terminated after approximately 10 minutes without a completed review. Final collect: active=0, done=2, killed=1. Worker inference estimates: $0.06970784, $0.05762060, $0.08339396; total $0.21072240 against $1.50 worker budget. Subscription estimates are not billed cash cost; total coordinator cost unknown. Raw artifacts: .swarm/runs.jsonl (run-keyed history); .swarm/latest.json is mutable.

No source changes, deployments, external publication, or production service tests performed by coordinator. This report is not a full engineering/security audit. Reuse: benchmark-claim reconciliation and integrated reference-workflow acceptance criteria. Residual risk: deployment state, fresh test results, customer activation and paid adoption unverified.
