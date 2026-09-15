# 60 independent feature prompts and concurrent delivery design

This package proposes 60 bounded features for dsco-cli. Each linked file is a complete, standalone implementation prompt: copy the entire file into a fresh session opened in the repository or an assigned private worktree. The prompts require checking current code before implementing a missing behavior, because the checkout already contains substantial work beyond the older roadmap.

The features are not implemented by this package. The concurrent coordinator is designed, not deployed. Validation here checks the catalog, source references, hashes and word counts; it does not prove feature correctness or the future coordinator's runtime guarantees.

- [All 60 prompts in one document](PROMPTS.md)
- [Concurrent delivery system](DELIVERY_SYSTEM.md)
- [Machine-readable feature catalog](catalog.json)
- [Proposed delivery configuration](delivery-config.json)
- [Candidate contract example](candidate.example.json)
- [Prime Intellect evaluation and learning integration](PRIME_INTELLECT_LEARNING.md)

Start with one coordinator and three implementation sessions in isolated worktrees, using the existing blackboard for atomic claims and acceptance. Rotate a free session into independent review, and keep a single integration writer. Explicit no-install builds prevent feature workers from overwriting the user's executable. The design explains how to freeze the dirty baseline, coordinate shared hooks, recover crashed attempts and verify merged revisions.

For a single independent session, choose any prompt below. For coordinated delivery, the coordinator adds baseline, scope, frozen checks, resource budget and attempt identity. No numbered prompt requires another numbered prompt to be completed first.

Run the static validation from the repository root:

```sh
python3 docs/feature-delivery/validate.py
```

After intentionally editing the source prompt files or the three domain manifests, regenerate the catalog and combined document with `python3 docs/feature-delivery/validate.py --refresh`, then validate again. This command only writes generated documents in this package; it never launches agents.

Verified catalog: **60 prompts, 294–349 words each, 19,445 words total.** Counts include each prompt's title.

| ID | Feature | Domain | Words |
| --- | --- | --- | ---: |
| 01 | [Recover unsent drafts after interruption](prompts/01-draft-recovery.md) | interactive-ux | 331 |
| 02 | [Edit queued follow-up messages](prompts/02-editable-followups.md) | interactive-ux | 326 |
| 03 | [Stage large pasted input before submission](prompts/03-paste-staging.md) | interactive-ux | 342 |
| 04 | [Bookmark and revisit terminal evidence](prompts/04-scrollback-bookmarks.md) | interactive-ux | 336 |
| 05 | [Search prompt history without losing the draft](prompts/05-prompt-history-search.md) | interactive-ux | 342 |
| 06 | [Provide an accessible linear terminal mode](prompts/06-accessible-terminal.md) | interactive-ux | 340 |
| 07 | [Freeze attachment content at submission](prompts/07-attachment-snapshots.md) | context-knowledge | 343 |
| 08 | [Fork a session from a chosen turn](prompts/08-session-fork.md) | session-continuity | 343 |
| 09 | [Preview workspace drift before resuming](prompts/09-resume-drift-preview.md) | session-continuity | 338 |
| 10 | [Export a structured session handoff capsule](prompts/10-session-handoff.md) | session-continuity | 346 |
| 11 | [Inspect and recover compacted context](prompts/11-compaction-inspector.md) | context-knowledge | 340 |
| 12 | [Preview the assembled context budget](prompts/12-context-budget-preview.md) | context-knowledge | 349 |
| 13 | [Expose source freshness for recalled context](prompts/13-context-source-freshness.md) | context-knowledge | 344 |
| 14 | [Explain active workspace instructions](prompts/14-instruction-explainer.md) | context-knowledge | 344 |
| 15 | [Track explicit project decisions and supersession](prompts/15-decision-ledger.md) | context-knowledge | 344 |
| 16 | [Assemble local evidence bundles for completed work](prompts/16-evidence-bundles.md) | developer-workflow | 344 |
| 17 | [Show workspace changes since a session checkpoint](prompts/17-workspace-change-digest.md) | developer-workflow | 346 |
| 18 | [Navigate compiler diagnostics from build output](prompts/18-build-diagnostics.md) | developer-workflow | 348 |
| 19 | [Save and launch explicit local terminal recipes](prompts/19-terminal-recipes.md) | developer-workflow | 349 |
| 20 | [Capture reproducible local bug-report packets](prompts/20-repro-packets.md) | developer-workflow | 347 |
| 21 | [Account-scoped offline tool catalog](prompts/21-account-scoped-tool-cache.md) | tool-management | 321 |
| 22 | [Generation-pinned external tool schemas](prompts/22-generation-pinned-tool-schemas.md) | tool-management | 319 |
| 23 | [Explicit schema capacity and hydration errors](prompts/23-schema-capacity-reporting.md) | tool-management | 317 |
| 24 | [Retry-After-aware Tool Management admission](prompts/24-tool-api-retry-after.md) | tool-management | 313 |
| 25 | [Remote mutation outcome reconciliation](prompts/25-remote-mutation-reconcile.md) | tool-management | 313 |
| 26 | [Per-call MCP cancellation](prompts/26-mcp-per-call-cancellation.md) | mcp | 314 |
| 27 | [Capability-gated MCP resource reads](prompts/27-mcp-resource-reader.md) | mcp | 323 |
| 28 | [Scoped MCP catalog-change refresh](prompts/28-mcp-catalog-change-refresh.md) | mcp | 324 |
| 29 | [Structured tool-output contract validation](prompts/29-tool-output-validation.md) | tool-contracts | 323 |
| 30 | [Read-only capability decision preview](prompts/30-capability-decision-preview.md) | governance | 320 |
| 31 | [External tool origin pinning](prompts/31-external-tool-origin-pinning.md) | tool-contracts | 329 |
| 32 | [External tool concurrency policies](prompts/32-external-tool-concurrency-policy.md) | tool-contracts | 331 |
| 33 | [Capability-constrained provider selection](prompts/33-provider-capability-constraints.md) | provider-routing | 327 |
| 34 | [Billing-lane-preserving fallback policy](prompts/34-billing-lane-fallback-policy.md) | provider-routing | 337 |
| 35 | [Safe streamed-provider failover boundary](prompts/35-streamed-failover-boundary.md) | provider-reliability | 334 |
| 36 | [Explicit local-provider capability probes](prompts/36-local-provider-capability-probes.md) | provider-routing | 329 |
| 37 | [Account-scoped quota reset handling](prompts/37-provider-quota-reset.md) | provider-reliability | 329 |
| 38 | [Conservative inference cost admission](prompts/38-inference-cost-admission.md) | resource-cost | 339 |
| 39 | [Process-local concurrent spend reservations](prompts/39-concurrent-spend-reservations.md) | resource-cost | 333 |
| 40 | [Versioned pricing evidence for decisions](prompts/40-versioned-pricing-evidence.md) | resource-cost | 333 |
| 41 | [Durable retry schedules and poison-task inspection](prompts/41-durable-retry-schedules.md) | durable-execution | 298 |
| 42 | [Event-driven scheduling of accepted blackboard dependencies](prompts/42-blackboard-ready-scheduler.md) | fleet-collaboration | 301 |
| 43 | [Actionable execution recovery plans with uncertainty preserved](prompts/43-execution-recovery-plans.md) | durable-execution | 302 |
| 44 | [Durable cancellation requests and deadline completion receipts](prompts/44-durable-cancellation-receipts.md) | durable-execution | 303 |
| 45 | [Recipient-specific durable mailbox acknowledgements](prompts/45-recipient-mailbox-acks.md) | fleet-collaboration | 294 |
| 46 | [Content-addressed large artifacts for worker handoffs](prompts/46-large-artifact-handoffs.md) | fleet-collaboration | 301 |
| 47 | [Fair bounded admission across concurrent swarm groups](prompts/47-fair-swarm-admission.md) | fleet-collaboration | 306 |
| 48 | [Declared resource reservations for cooperative worker edits](prompts/48-worker-resource-reservations.md) | fleet-collaboration | 305 |
| 49 | [Evidence-based worker quarantine and controlled re-entry](prompts/49-worker-health-quarantine.md) | fleet-collaboration | 308 |
| 50 | [Verified map-reduce coverage and disagreement reports](prompts/50-verified-swarm-reduction.md) | fleet-collaboration | 306 |
| 51 | [Checker-environment attestations for accepted worker evidence](prompts/51-checker-environment-attestations.md) | evaluation-proof | 304 |
| 52 | [Offline declarative runtime scenario runner](prompts/52-runtime-scenario-runner.md) | evaluation-proof | 307 |
| 53 | [Crash-boundary fault-injection corpus for durable work](prompts/53-durable-fault-corpus.md) | evaluation-proof | 306 |
| 54 | [Matched run comparisons with outcome and cost provenance](prompts/54-matched-run-comparisons.md) | evaluation-proof | 304 |
| 55 | [Runtime responsiveness budgets with regression evidence](prompts/55-runtime-responsiveness-budgets.md) | observability | 303 |
| 56 | [Read-only fleet event timeline and precise cursor queries](prompts/56-fleet-event-timeline.md) | observability | 304 |
| 57 | [Causal lineage across tasks, attempts, and worker events](prompts/57-fleet-causal-lineage.md) | observability | 310 |
| 58 | [Offline fleet incident bundle with cross-worker evidence joins](prompts/58-fleet-incident-bundle.md) | observability | 309 |
| 59 | [Release readiness from exact merged-artifact evidence](prompts/59-release-readiness-evidence.md) | release-delivery | 309 |
| 60 | [Deterministic local release staging with reproducibility reports](prompts/60-deterministic-release-staging.md) | release-delivery | 315 |

## Suggested three-worker batches

These mix domains to distribute work. They are selection suggestions, not dependency waves or a guarantee of nonoverlapping shared hooks. Check reservations and actual diff scope before dispatch. Priorities may change after baseline discovery.

| Batch | Worker A | Worker B | Worker C |
| --- | --- | --- | --- |
| 01 | [01](prompts/01-draft-recovery.md) Recover unsent drafts after interruption | [21](prompts/21-account-scoped-tool-cache.md) Account-scoped offline tool catalog | [41](prompts/41-durable-retry-schedules.md) Durable retry schedules and poison-task inspection |
| 02 | [02](prompts/02-editable-followups.md) Edit queued follow-up messages | [22](prompts/22-generation-pinned-tool-schemas.md) Generation-pinned external tool schemas | [42](prompts/42-blackboard-ready-scheduler.md) Event-driven scheduling of accepted blackboard dependencies |
| 03 | [03](prompts/03-paste-staging.md) Stage large pasted input before submission | [23](prompts/23-schema-capacity-reporting.md) Explicit schema capacity and hydration errors | [43](prompts/43-execution-recovery-plans.md) Actionable execution recovery plans with uncertainty preserved |
| 04 | [04](prompts/04-scrollback-bookmarks.md) Bookmark and revisit terminal evidence | [24](prompts/24-tool-api-retry-after.md) Retry-After-aware Tool Management admission | [44](prompts/44-durable-cancellation-receipts.md) Durable cancellation requests and deadline completion receipts |
| 05 | [05](prompts/05-prompt-history-search.md) Search prompt history without losing the draft | [25](prompts/25-remote-mutation-reconcile.md) Remote mutation outcome reconciliation | [45](prompts/45-recipient-mailbox-acks.md) Recipient-specific durable mailbox acknowledgements |
| 06 | [06](prompts/06-accessible-terminal.md) Provide an accessible linear terminal mode | [26](prompts/26-mcp-per-call-cancellation.md) Per-call MCP cancellation | [46](prompts/46-large-artifact-handoffs.md) Content-addressed large artifacts for worker handoffs |
| 07 | [07](prompts/07-attachment-snapshots.md) Freeze attachment content at submission | [27](prompts/27-mcp-resource-reader.md) Capability-gated MCP resource reads | [47](prompts/47-fair-swarm-admission.md) Fair bounded admission across concurrent swarm groups |
| 08 | [08](prompts/08-session-fork.md) Fork a session from a chosen turn | [28](prompts/28-mcp-catalog-change-refresh.md) Scoped MCP catalog-change refresh | [48](prompts/48-worker-resource-reservations.md) Declared resource reservations for cooperative worker edits |
| 09 | [09](prompts/09-resume-drift-preview.md) Preview workspace drift before resuming | [29](prompts/29-tool-output-validation.md) Structured tool-output contract validation | [49](prompts/49-worker-health-quarantine.md) Evidence-based worker quarantine and controlled re-entry |
| 10 | [10](prompts/10-session-handoff.md) Export a structured session handoff capsule | [30](prompts/30-capability-decision-preview.md) Read-only capability decision preview | [50](prompts/50-verified-swarm-reduction.md) Verified map-reduce coverage and disagreement reports |
| 11 | [11](prompts/11-compaction-inspector.md) Inspect and recover compacted context | [31](prompts/31-external-tool-origin-pinning.md) External tool origin pinning | [51](prompts/51-checker-environment-attestations.md) Checker-environment attestations for accepted worker evidence |
| 12 | [12](prompts/12-context-budget-preview.md) Preview the assembled context budget | [32](prompts/32-external-tool-concurrency-policy.md) External tool concurrency policies | [52](prompts/52-runtime-scenario-runner.md) Offline declarative runtime scenario runner |
| 13 | [13](prompts/13-context-source-freshness.md) Expose source freshness for recalled context | [33](prompts/33-provider-capability-constraints.md) Capability-constrained provider selection | [53](prompts/53-durable-fault-corpus.md) Crash-boundary fault-injection corpus for durable work |
| 14 | [14](prompts/14-instruction-explainer.md) Explain active workspace instructions | [34](prompts/34-billing-lane-fallback-policy.md) Billing-lane-preserving fallback policy | [54](prompts/54-matched-run-comparisons.md) Matched run comparisons with outcome and cost provenance |
| 15 | [15](prompts/15-decision-ledger.md) Track explicit project decisions and supersession | [35](prompts/35-streamed-failover-boundary.md) Safe streamed-provider failover boundary | [55](prompts/55-runtime-responsiveness-budgets.md) Runtime responsiveness budgets with regression evidence |
| 16 | [16](prompts/16-evidence-bundles.md) Assemble local evidence bundles for completed work | [36](prompts/36-local-provider-capability-probes.md) Explicit local-provider capability probes | [56](prompts/56-fleet-event-timeline.md) Read-only fleet event timeline and precise cursor queries |
| 17 | [17](prompts/17-workspace-change-digest.md) Show workspace changes since a session checkpoint | [37](prompts/37-provider-quota-reset.md) Account-scoped quota reset handling | [57](prompts/57-fleet-causal-lineage.md) Causal lineage across tasks, attempts, and worker events |
| 18 | [18](prompts/18-build-diagnostics.md) Navigate compiler diagnostics from build output | [38](prompts/38-inference-cost-admission.md) Conservative inference cost admission | [58](prompts/58-fleet-incident-bundle.md) Offline fleet incident bundle with cross-worker evidence joins |
| 19 | [19](prompts/19-terminal-recipes.md) Save and launch explicit local terminal recipes | [39](prompts/39-concurrent-spend-reservations.md) Process-local concurrent spend reservations | [59](prompts/59-release-readiness-evidence.md) Release readiness from exact merged-artifact evidence |
| 20 | [20](prompts/20-repro-packets.md) Capture reproducible local bug-report packets | [40](prompts/40-versioned-pricing-evidence.md) Versioned pricing evidence for decisions | [60](prompts/60-deterministic-release-staging.md) Deterministic local release staging with reproducibility reports |
