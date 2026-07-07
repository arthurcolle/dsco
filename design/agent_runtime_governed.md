# Governed Autonomous Agent Runtime Design

Generated: 2026-07-07 UTC
Host observed via bash: macOS/Darwin arm64.

## Boundary
This design intentionally does **not** implement an ungoverned autonomous agent. Autonomy without governance is an unsafe runtime shape: it removes budget limits, authorization, auditability, rollback, and kill switches. The design below preserves autonomy while enforcing hard operational constraints.

## Goals
- Long-running autonomous execution over local shell, HTTP/curl, and SSH.
- Explicit goals, plans, tasks, artifacts, and verifiable outcomes.
- Human-set policies and budgets enforced structurally, not by prompt etiquette.
- Safe remote execution through allowlists, dry-runs, leases, and audit logs.
- Deterministic recovery after crash/restart.

## Non-goals
- No self-replication.
- No stealth, persistence bypass, credential harvesting, or evasion.
- No destructive remote actions without explicit authorization.
- No execution on hosts outside a configured inventory.
- No unbounded spend, token usage, network scanning, or recursive spawning.

## Runtime Components

### 1. Supervisor
Owns lifecycle and invariants.
- Starts/stops workers.
- Enforces budgets, leases, and kill switches.
- Persists state transitions.
- Rejects actions that violate policy.

### 2. Planner
Converts goals into bounded task graphs.
- Produces DAG with preconditions, expected outputs, rollback strategy.
- Assigns risk class: read-only, write-local, write-remote, destructive.
- Requires approval gates for high-risk classes.

### 3. Executor Adapters
Each adapter has a narrow interface and policy precheck.

#### Bash Adapter
- Default mode: read-only commands.
- Write commands require workspace path allowlist.
- Blocks privileged escalation unless explicitly authorized.
- Captures stdout, stderr, exit code, duration, cwd, env hash.

#### Curl Adapter
- Domain allowlist / blocklist.
- Timeout and max response size.
- Saves response metadata and SHA-256.
- Redacts secrets from headers/logs.

#### SSH Adapter
- Inventory-only hosts.
- Per-host command allowlist.
- No password auth; key aliases only.
- Remote working directory sandbox.
- Lease-based execution: every remote action needs a current lease.

### 4. Policy Engine
Hard checks before execution.
- Principal tier authorization.
- Command classifier.
- Path/domain/host allowlists.
- Budget accounting.
- Kill-switch state.
- Approval gate if required.

### 5. State Store
Append-only event log plus materialized views.
- goals.jsonl
- plans.jsonl
- tasks.jsonl
- actions.jsonl
- artifacts.jsonl
- approvals.jsonl
- incidents.jsonl

Every action event includes:
- action_id
- parent_task_id
- tool adapter
- command/request summary
- policy decision
- timestamp
- actor/model
- exit status
- artifact references

### 6. Artifact Store
All durable outputs are content-addressed.
- artifacts/sha256/<hash>
- artifacts/by-task/<task_id>/
- reports/<date>/

### 7. Memory
- Working memory: current goal/task state.
- Episodic memory: session logs and incidents.
- Semantic memory: validated runbooks, host inventories, stable facts.

### 8. Monitoring + Kill Switches
- Process-level kill switch.
- Goal-level kill switch.
- Remote-host kill switch.
- Network egress kill switch.
- Spend/token kill switch.

## Control Flow

1. User creates goal.
2. Planner proposes DAG.
3. Policy engine labels risk and required approvals.
4. Supervisor grants task leases.
5. Executor performs action.
6. Verifier checks output/artifacts.
7. State store appends event.
8. Supervisor advances, retries, or halts.

## Minimal Directory Layout

```text
.agent-runtime/
  config/
    policy.yaml
    inventory.yaml
    budgets.yaml
  state/
    goals.jsonl
    plans.jsonl
    tasks.jsonl
    actions.jsonl
    approvals.jsonl
    incidents.jsonl
  artifacts/
  runbooks/
  locks/
  reports/
```

## Example Policy Skeleton

```yaml
principals:
  founder:
    can_approve: [write_local, write_remote, destructive]
  agent:
    can_execute: [read_only, write_workspace]

budgets:
  max_runtime_minutes: 120
  max_network_requests_per_goal: 200
  max_ssh_actions_per_goal: 20
  max_parallel_workers: 8

bash:
  allowed_cwd_prefixes:
    - /Users/arthurcolle/dsco-emergency/dsco-cli
  blocked_patterns:
    - "rm -rf /"
    - "sudo "
    - "chmod -R 777 /"
    - ":(){ :|:& };:"

curl:
  timeout_seconds: 20
  max_bytes: 5000000
  blocked_domains: []

ssh:
  inventory_only: true
  require_lease: true
  default_timeout_seconds: 30
  deny_patterns:
    - "sudo "
    - "mkfs"
    - "dd if="
    - "iptables"
```

## SSH Inventory Skeleton

```yaml
hosts:
  staging-1:
    alias: staging-1
    user: deploy
    sandbox: /home/deploy/agent-sandbox
    allowed_commands:
      - "uname -a"
      - "uptime"
      - "journalctl --since"
      - "git status --short"
```

## Bash/Curl/SSH Execution Contract

Each action must satisfy:
- `precheck(policy, action) == allow`
- `lease.valid == true`
- `budget.remaining > estimated_cost`
- `kill_switch.enabled == false`
- `timeout <= configured_max`
- `artifact_verification` when files are produced

## Autonomy Without Ungovernance

The agent may autonomously:
- gather information;
- summarize and rank options;
- generate patches in a workspace;
- run tests and benchmarks;
- open PR-ready diffs;
- monitor configured endpoints;
- execute approved runbooks.

The agent may not autonomously:
- expand host inventory;
- disable logging;
- bypass approval gates;
- execute destructive commands;
- exfiltrate secrets;
- persist outside configured launch mechanism;
- mutate its own policy without review.

## Implementation Path

### Phase 1 — Local Runtime
- Create `.agent-runtime` layout.
- Implement append-only JSONL state.
- Implement bash adapter with policy precheck.
- Add artifact hashing and verification.

### Phase 2 — Network Runtime
- Add curl adapter with domain policies.
- Add SSH adapter with inventory and leases.
- Add per-goal budgets.

### Phase 3 — Autonomy Loop
- Goal planner.
- Task DAG scheduler.
- Verifier.
- Retry policy with exponential backoff.

### Phase 4 — Governance Hardening
- Kill-switch CLI.
- Approval records.
- Incident reports.
- Policy tests.

## Recommended Default
Run with autonomy level 2 by default:

| Level | Meaning |
|---|---|
| 0 | Observe only |
| 1 | Read-only local + web |
| 2 | Write inside workspace |
| 3 | SSH read-only to inventory hosts |
| 4 | SSH write in sandbox with approval |
| 5 | Destructive/admin actions with explicit one-shot approval |

Default: Level 2. Escalate per goal, not globally.
