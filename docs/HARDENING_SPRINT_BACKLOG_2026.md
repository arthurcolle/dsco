# Hardening Sprint Backlog (2026)

Date created: March 3, 2026  
Companion document: `docs/HARDENING_MONETIZATION_ROADMAP_2026.md`  
Planning horizon: March 2026 through August 2026

## Sprint Calendar

1. Sprint 11: March 9, 2026 - March 20, 2026
2. Sprint 12: March 23, 2026 - April 3, 2026
3. Sprint 13: April 6, 2026 - April 17, 2026
4. Sprint 14: April 20, 2026 - May 1, 2026
5. Sprint 15: May 4, 2026 - May 15, 2026
6. Sprint 16: May 18, 2026 - May 29, 2026
7. Sprint 17: June 1, 2026 - June 12, 2026
8. Sprint 18: June 15, 2026 - June 26, 2026
9. Sprint 19: June 29, 2026 - July 10, 2026
10. Sprint 20: July 13, 2026 - July 24, 2026
11. Sprint 21: July 27, 2026 - August 7, 2026

## Execution Status (as of March 4, 2026)

- `P0-TRUST-001`: implemented (session trust tier model + persistence + `/status` visibility + tests).
- `P0-TRUST-002`: implemented (tier-based tool allow/deny enforcement + structured deny output + tests).
- `P0-TRUST-003`: in progress (untrusted shell-like tool routing to `sandbox_run`, tier-bound sandbox policy injection, strict fallback behavior when policy cannot be enforced).
- `P0-EXT-001`: in progress (manifest/lock schemas, `/plugins validate` + `plugin_validate` command/tool path, and documentation examples).

## P0 Issues (Revenue Gate)

### P0-TRUST-001: Session Trust Tier Policy Engine
- Priority: P0
- Target sprint: Sprint 11
- Size: L
- Owner: Runtime
- Dependencies: none
- Acceptance criteria:
1. Add trust tiers (`trusted`, `standard`, `untrusted`) to session state and config.
2. Tier is visible via `/status` and persisted in saved sessions.
3. Default tier is `standard`; unknown values fail validation.
4. Unit tests cover tier parsing and persistence round-trip.

### P0-TRUST-002: Tool Permission Matrix Enforcement
- Priority: P0
- Target sprint: Sprint 11
- Size: L
- Owner: Runtime + Tools
- Dependencies: P0-TRUST-001
- Acceptance criteria:
1. Tool execution path enforces allow/deny by trust tier before dispatch.
2. Denied calls return deterministic structured errors and are baseline-logged.
3. High-risk tool classes are blocked in `untrusted` tier by default.
4. Regression tests confirm deny behavior for at least 10 high-risk tools.

### P0-TRUST-003: Sandbox Routing for Untrusted Commands
- Priority: P0
- Target sprint: Sprint 12
- Size: L
- Owner: Runtime + Tools
- Dependencies: P0-TRUST-002
- Acceptance criteria:
1. Untrusted shell-like executions route to sandboxed backend by default.
2. Filesystem/network policies are explicit and tier-bound.
3. Timeout and cancellation behavior remain intact in sandbox mode.
4. Integration tests verify blocked host escape attempts.

### P0-EXT-001: Plugin Manifest + Lockfile Spec
- Priority: P0
- Target sprint: Sprint 12
- Size: M
- Owner: Platform
- Dependencies: none
- Acceptance criteria:
1. Define `plugin-manifest.json` schema including name, version, hash, signer, capabilities.
2. Define `plugins.lock` format with pinned versions and digests.
3. CLI command validates manifests and lockfile syntax.
4. Spec is documented with examples.

### P0-EXT-002: Plugin Signature and Integrity Verification
- Priority: P0
- Target sprint: Sprint 13
- Size: L
- Owner: Platform + Security
- Dependencies: P0-EXT-001
- Acceptance criteria:
1. Plugin load fails closed on signature/hash mismatch.
2. Verification runs on startup and plugin reload.
3. Failure output identifies plugin and mismatch reason.
4. Tampered plugin fixtures fail in CI.

### P0-EXT-003: MCP Server Allowlist + Pinning
- Priority: P0
- Target sprint: Sprint 13
- Size: M
- Owner: MCP
- Dependencies: P0-EXT-001
- Acceptance criteria:
1. `~/.dsco/mcp.json` supports explicit allowlist and binary hash pinning.
2. MCP servers outside allowlist are rejected.
3. Hash mismatch blocks server initialization.
4. `/mcp` output clearly reports verification status.

### P0-OPS-001: `/doctor` Diagnostics Command
- Priority: P0
- Target sprint: Sprint 14
- Size: M
- Owner: Runtime + Ops
- Dependencies: none
- Acceptance criteria:
1. Add `/doctor` command that checks env keys, storage paths, provider connectivity, plugin/MCP health.
2. Output includes pass/fail state and next action per check.
3. Exit status can be consumed by non-interactive automation.
4. Checks are covered by test fixtures for success/failure paths.

### P0-OPS-002: `/doctor --repair` Automated Remediation
- Priority: P0
- Target sprint: Sprint 15
- Size: M
- Owner: Runtime + Ops
- Dependencies: P0-OPS-001
- Acceptance criteria:
1. Add `--repair` mode for safe remediations (path creation, permissions, config normalization).
2. Dry-run mode prints planned actions without mutation.
3. Repair actions are idempotent.
4. Repair summary includes changed files and remaining failures.

### P0-SEC-001: Security Regression Test Suite
- Priority: P0
- Target sprint: Sprint 15
- Size: M
- Owner: Security + QA
- Dependencies: P0-TRUST-003, P0-EXT-002, P0-EXT-003
- Acceptance criteria:
1. Add suite for traversal, injection, sandbox escape, and extension tampering.
2. CI runs suite on every PR touching runtime/tools/mcp/plugin code.
3. Failing security tests block merge.
4. Test report includes severity tag and owning module.

### P0-BILL-001: Immutable Usage Ledger
- Priority: P0
- Target sprint: Sprint 16
- Size: L
- Owner: Billing + Data
- Dependencies: none
- Acceptance criteria:
1. Append-only usage events stored with stable schema and monotonic IDs.
2. Events cover model usage, tool usage, retries, and provider fallback.
3. Ledger writes are atomic and resilient to process interruption.
4. Data retention and compaction policies are documented and tested.

### P0-BILL-002: Quota Enforcement (Shadow then Hard)
- Priority: P0
- Target sprint: Sprint 17
- Size: L
- Owner: Billing + Runtime
- Dependencies: P0-BILL-001
- Acceptance criteria:
1. Implement workspace/user quotas for spend, token, and API invocations.
2. Run in shadow mode for one sprint with delta reports.
3. Promote to hard enforcement with explicit over-quota errors.
4. Override and emergency bypass are auditable and role-gated.

### P0-API-001: Authenticated Control Plane API
- Priority: P0
- Target sprint: Sprint 18
- Size: L
- Owner: API + Security
- Dependencies: P0-TRUST-001, P0-BILL-001
- Acceptance criteria:
1. Remote API requires scoped bearer token auth.
2. Token scopes map to allowed actions (read, invoke, admin).
3. Unauthorized requests are denied with no action side effects.
4. Authentication success/failure is baseline-logged.

### P0-API-002: API Rate Limits + Audit Trail
- Priority: P0
- Target sprint: Sprint 19
- Size: M
- Owner: API + Security
- Dependencies: P0-API-001
- Acceptance criteria:
1. Per-token and per-workspace rate limits are enforced.
2. All API invokes emit audit records with actor, scope, target, result.
3. Abuse controls include temporary lockout and cooldown.
4. Pen-test checklist for API abuse paths passes.

## P1 Issues (Scale and Reliability)

### P1-OPS-003: `/doctor --non-interactive` for CI and Fleet Checks
- Priority: P1
- Target sprint: Sprint 16
- Size: S
- Owner: Ops
- Dependencies: P0-OPS-001
- Acceptance criteria:
1. Non-interactive mode emits machine-readable JSON.
2. Exit codes map to severity (`0 ok`, `1 warning`, `2 critical`).
3. Output excludes secrets by default.

### P1-BILL-003: Billing Reconciliation Job
- Priority: P1
- Target sprint: Sprint 18
- Size: M
- Owner: Billing + Data
- Dependencies: P0-BILL-001
- Acceptance criteria:
1. Daily reconciliation compares ledger totals and provider metering.
2. Variance report generated with alerting when >1%.
3. Replay mode recomputes totals from raw ledger for audits.

### P1-PLAN-001: Plan Feature-Gate Engine
- Priority: P1
- Target sprint: Sprint 18
- Size: M
- Owner: Billing + Runtime
- Dependencies: P0-BILL-002
- Acceptance criteria:
1. Feature gate config supports per-plan capabilities and limits.
2. Runtime enforces gates for API access, concurrency, and high-risk tools.
3. Gate violations are user-visible and audit-logged.

### P1-EXT-004: One-Release Compatibility Mode for Unsigned Extensions
- Priority: P1
- Target sprint: Sprint 17
- Size: S
- Owner: Platform
- Dependencies: P0-EXT-002
- Acceptance criteria:
1. Compatibility mode is opt-in and time-boxed to one release.
2. Unsigned loads show strong deprecation warning.
3. Metrics capture unsigned usage for migration planning.

### P1-OPS-004: Session Store Reliability Upgrades
- Priority: P1
- Target sprint: Sprint 19
- Size: M
- Owner: Runtime + Data
- Dependencies: P0-BILL-001
- Acceptance criteria:
1. Add session compaction and corruption detection.
2. Add automated recovery path for partial writes.
3. Add backup/restore smoke tests.

## P2 Issues (Post-Beta Hardening)

### P2-SEC-002: External Security Assessment and Remediation Sprint
- Priority: P2
- Target sprint: Sprint 20
- Size: L
- Owner: Security
- Dependencies: all P0 security/API items
- Acceptance criteria:
1. External test engagement completed.
2. Critical and high findings fixed or formally accepted with mitigation.
3. Final report archived with remediation evidence.

### P2-OPS-005: Incident Drill Program
- Priority: P2
- Target sprint: Sprint 21
- Size: M
- Owner: Ops + Security
- Dependencies: P0-API-002, P1-OPS-004
- Acceptance criteria:
1. Run two tabletop drills and one live failover drill.
2. Capture MTTR and communication timeline.
3. Produce postmortems with action items tracked to closure.

## Delivery Rules

1. No P1 or P2 issue begins before all predecessor P0 dependencies are complete.
2. Every issue must ship with tests and runbook updates.
3. Every security-affecting issue requires sign-off from security owner.
4. Every billing-affecting issue requires reconciliation validation before production rollout.
