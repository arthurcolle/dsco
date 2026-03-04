# Hardening-to-Monetization Roadmap (2026)

Date created: March 3, 2026  
Horizon: March 2026 through August 2026  
Primary goal: harden reliability, safety, and operability before broad paid rollout.

Related execution backlog: `docs/HARDENING_SPRINT_BACKLOG_2026.md`

## 1) Outcomes We Need Before Scaling Revenue

1. Trust boundaries are explicit and enforceable (session/tool sandbox policy).
2. Extension surface is supply-chain hardened (plugins, MCP servers, skills).
3. Operations are self-healing and diagnosable (`/doctor`, repair paths, runbooks).
4. Usage and cost accounting are auditable for billing and quota enforcement.
5. Remote/control-plane APIs are authenticated, rate-limited, and logged.

## 2) Scope and Non-Goals

In scope:
- Runtime hardening in CLI + tool execution surfaces.
- Packaging and extension integrity controls.
- Billing-adjacent infrastructure (usage ledger, quotas, plan enforcement).
- Reliability and incident-readiness improvements.

Out of scope (for this cycle):
- New model vendors beyond currently supported provider architecture.
- UI redesign or major TUI visual refresh.
- Marketplace growth motions before trust gate completion.

## 3) Workstreams

`WS1 Trust & Sandboxing`
- Policy tiers: `trusted`, `standard`, `untrusted`.
- Enforce per-tier tool allowlists and execution constraints.
- Isolate untrusted sessions using `sandbox_run`/containerized execution paths.

`WS2 Extension Supply Chain`
- Signed plugin manifests and lockfile pinning.
- MCP server allowlist + signature/hash pinning.
- Skill/package provenance checks with fail-closed defaults.

`WS3 Operability & Repair`
- Add `/doctor`, `/doctor --repair`, `/doctor --non-interactive`.
- Expand health checks for setup/env, provider connectivity, MCP/plugin health, storage.
- Runbook automation for common failure classes.

`WS4 Billing Readiness`
- Immutable usage ledger events.
- Workspace and user quotas.
- Plan policy engine (feature flags and hard caps).

`WS5 Control Plane API Hardening`
- Authenticated HTTP API with scoped tokens.
- Rate limits + abuse controls.
- Audit log coverage for all remote invocations.

## 4) Timeline

## Phase 0: Baseline + Spec Lock (March 3, 2026 - March 20, 2026)

Deliverables:
- Threat model for session/tool/plugin/MCP surfaces.
- PRD + technical specs for WS1-WS5.
- KPI baseline dashboard:
  - crash-free sessions
  - tool timeout rate
  - MCP/plugin load failure rate
  - mean time to recovery

Exit criteria:
- All specs approved.
- Existing observability baseline captured.
- Risk register created and staffed.

## Phase 1: Trust/Sandbox Foundation (March 23, 2026 - April 24, 2026)

Deliverables:
- Session trust tier selection and enforcement path in runtime.
- Tool policy matrix (per tier) with deny-by-default for untrusted.
- Sandboxed execution for high-risk commands in untrusted sessions.
- Security tests for path traversal, command injection, and permission bypass attempts.

Exit criteria:
- 0 known bypasses in internal security test suite.
- Untrusted tier blocks prohibited tools deterministically.

## Phase 2: Extension Hardening + Doctor (April 27, 2026 - May 29, 2026)

Deliverables:
- Signed manifest + lockfile for plugins and MCP configs.
- Verification on startup and on reload (`/mcp reload`, plugin load).
- `/doctor` command family with actionable remediation output.
- CI policy checks for extension provenance and signature validation.

Exit criteria:
- Tampered extension artifacts fail to load with clear diagnostics.
- `/doctor --repair` resolves top 80% setup/ops failures without manual edits.

## Phase 3: Usage Ledger + Quotas + Plan Gates (June 1, 2026 - July 3, 2026)

Deliverables:
- Append-only usage ledger with stable event schema.
- Workspace/user quota enforcement at runtime.
- Plan feature gates:
  - max concurrent sessions
  - high-risk tool classes
  - control-plane API access
- Billing reconciliation job and alerting for drift.

Exit criteria:
- Ledger-to-invoice variance <1%.
- Quota enforcement tested under load and adversarial usage patterns.

## Phase 4: Authenticated Control Plane + Revenue Readiness (July 6, 2026 - August 7, 2026)

Deliverables:
- Authenticated HTTP API (scoped tokens, revocation, rate limiting).
- Full audit logs for remote invoke flows.
- Abuse controls and lockout strategy.
- Paid beta readiness checklist and SLO gate.

Exit criteria:
- API pen-test critical/high issues = 0 open.
- SLOs met for 30 consecutive days in staged beta.

## 5) Milestones

1. March 20, 2026: spec lock + baseline metrics complete.
2. April 24, 2026: trust tiers and sandbox policy in production.
3. May 29, 2026: signed extension pipeline + `/doctor` live.
4. July 3, 2026: usage ledger + quota + plan gating live.
5. August 7, 2026: authenticated control plane ready for paid beta.

## 6) KPI Targets

- Crash-free sessions: >=99.5%
- Tool timeout rate: <=1.5%
- MCP/plugin load failure rate: <=1.0%
- Mean time to recovery (P1): <30 minutes
- Billing variance (ledger vs invoice): <1%
- Security SLA for critical vulns: patch within 48 hours

## 7) Risks and Mitigations

`Risk: hardening velocity slows feature output`
- Mitigation: freeze non-critical new feature work through Phase 2.

`Risk: extension verification breaks existing community workflows`
- Mitigation: compatibility mode for 1 release, then enforce signing.

`Risk: quota enforcement creates false positives`
- Mitigation: shadow-mode enforcement for 2 weeks before hard caps.

`Risk: API auth rollout complexity`
- Mitigation: start with internal-only token scopes before external beta.

## 8) Team Cadence

- Weekly architecture/security review (Tuesday).
- Weekly roadmap checkpoint with metric deltas (Friday).
- Monthly go/no-go gate based on phase exit criteria.

## 9) Implementation Order (Immediate)

1. Build the trust-tier policy engine and enforcement hooks first.
2. Add extension signature + lockfile validation.
3. Ship `/doctor` with repair flows.
4. Add immutable usage ledger and quotas.
5. Ship authenticated control-plane API.
