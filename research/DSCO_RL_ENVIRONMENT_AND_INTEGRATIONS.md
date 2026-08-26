# DSCO RL Environment Substrate & Top-45 Integration Portfolio

**Status:** research synthesis / implementation guidance; not a promotion decision  
**Date:** 2026-07-10  
**Scope:** DSCO as a small, local-first, governed agent runtime that can inhabit many environments, produce replayable trajectories, and optionally export experiment telemetry.

## Executive thesis

DSCO should not compete on a superficial connector-logo count or prematurely turn itself into an RL trainer. Its differentiated substrate is a compact native agent runtime that can **observe, decide, act under explicit authority, checkpoint, recover, verify, and emit durable provenance** across heterogeneous environments.

The useful research framing, prompted by Will Brown / `@willccbb` and the open-environments discussion, is that agentic RL does not scale with rollout FLOPs alone. Important constraints include:

1. environment diversity and fidelity;
2. effective (semantic/causal) horizon;
3. credit-assignment bandwidth;
4. exploration reachability and trajectory diversity;
5. reset, replay, checkpoint, and recovery economics;
6. verifier faithfulness under optimization pressure;
7. adaptive curriculum/environment-frontier generation;
8. long-run model plasticity retention.

The direct source attributed to Brown says only that there are “like 3 or 4 distinct scaling axes in RL that nobody talks about for some reason.” It **does not enumerate them**. The taxonomy in this document is a literature-backed reconstruction and design synthesis, not a claim about Brown’s exact list.

Primary post located during research: https://x.com/willccbb/status/2070022399530348933

## Current DSCO hook foundation

A minimal, local-only trajectory hook was added:

- `include/rl_hooks.h`
- `src/rl_hooks.c`
- narrow integration from `src/agent_event.c`
- `Makefile` entry for `rl_hooks.c`

### Event flow

```text
Environment/user/tool transition
        -> canonical DSCO agent event
        -> Chronicle per-run journal (source of truth)
        -> rl_hooks projection
        -> rl.trajectory.event frame in the same durable journal
```

The hook is intentionally **observe-only**:

- no model/policy update;
- no reward assignment;
- no automatic trajectory promotion;
- no external export;
- no change to authorized tool execution;
- failed trajectory projection cannot block a canonical event or agent action;
- `DSCO_RL_HOOKS=off` disables projection.

### Current projected schema

```json
{
  "schema": "dsco.rl_trajectory.v1",
  "run_id": "...",
  "sequence": 17,
  "timestamp_ms": 0,
  "event": "tool.result",
  "category": "tool",
  "status": "ok",
  "cost": {
    "usd_delta": 0.0021,
    "input_tokens": 1240,
    "output_tokens": 380
  },
  "rl_context": {
    "environment_id": "...",
    "environment_fidelity": "sandbox",
    "verifier_id": "...",
    "verifier_version": "...",
    "branch_id": "...",
    "checkpoint_id": "...",
    "semantic_step": 0,
    "semantic_horizon": 0
  },
  "payload": {}
}
```

The `rl_context` fields deliberately correspond to the environment-RL research dimensions:

| Field | Scaling/reliability purpose |
|---|---|
| `environment_id`, `environment_fidelity` | Measure diversity and realism of causal worlds, rather than confusing repeated variants with new environments. |
| `verifier_id`, `verifier_version` | Preserve evaluator provenance; make proxy-vs-outcome divergence measurable. |
| `branch_id` | Count distinct plans/recovery paths, rather than raw correlated samples. |
| `checkpoint_id` | Link trajectories to reset/restore lineage and recovery cost. |
| `semantic_step`, `semantic_horizon` | Distinguish causally meaningful decision depth from token length. |

No field should be inferred as a reward merely because a tool returned `ok`.

## Corrected RL-axis taxonomy

### Core axes

| Rank | Axis | Design implication for DSCO |
|---:|---|---|
| 1 | Environment diversity and fidelity | Versioned environment manifests, fixtures, sandbox vs real labels, deterministic seed where possible. |
| 2 | Verifier faithfulness under optimization pressure | Version every verifier; retain held-out/outcome evidence; never optimize only an agent-controlled proxy. |
| 3 | Effective semantic horizon | Track meaningful state transitions and subgoals, not merely tokens or wall-clock time. |
| 4 | Credit-assignment bandwidth | Preserve action/result chronology, evaluator receipts, artifact IDs, and causal context. |
| 5 | Exploration reachability and diversity | Support branch IDs, worktrees/checkpoints, diversified swarms, and comparable alternate trajectories. |
| 6 | Reset/replay/checkpoint economics | Make state reset, replay, and recovery first-class environment properties. |

### Meta-axes

| Axis | Role |
|---|---|
| Adaptive curriculum / environment frontier | Select tasks near the current competence boundary; do not conflate this scheduler with an inherent environment property. |
| Plasticity retention | Long-lived continual RL can stop learning even when environment, verifier, and rollout inputs remain adequate. |

### Important correction

The correct label is **verifier faithfulness under optimization pressure**, not merely “verifier hardness.” A difficult verifier can remain gameable; an easy deterministic check can be very faithful. The relevant question is whether the proxy score continues to track true task success as policy/search capability increases.

Reference: Leo Gao, John Schulman, Jacob Hilton, *Scaling Laws for Reward Model Overoptimization* (ICML 2023), https://arxiv.org/abs/2210.10760

## Proposed DSCO Environment ABI

DSCO should expose an environment contract before building dozens of bespoke integrations.

```c
typedef struct dsco_environment_v1 {
    const char *environment_id;
    const char *fidelity;           /* simulation | sandbox | real */
    const char *verifier_id;
    const char *verifier_version;

    bool (*reset)(const char *seed, char *observation, size_t observation_len);
    bool (*observe)(char *observation, size_t observation_len);
    bool (*act)(const char *action_json, char *result, size_t result_len);
    bool (*checkpoint)(char *checkpoint_id, size_t checkpoint_id_len);
    bool (*restore)(const char *checkpoint_id);
    bool (*verify)(char *result_json, size_t result_len);
    void (*close)(void);
} dsco_environment_v1_t;
```

A subprocess JSONL adapter should precede a C plugin ABI for rapid interoperability:

```text
stdin:  {"op":"reset","seed":"42"}
stdout: {"observation":{...},"checkpoint_id":"..."}

stdin:  {"op":"act","action":{...}}
stdout: {"observation":{...},"terminated":false,"info":{...}}

stdin:  {"op":"verify"}
stdout: {"outcome":"pass","score":1.0,"verifier_version":"..."}
```

This allows environments to be written in Python, Rust, Go, Node, shell, or benchmark-specific frameworks while keeping DSCO itself small and native.

### Required environment manifest

```json
{
  "schema": "dsco.environment.v1",
  "environment_id": "dsco.repo-repair.v1",
  "fidelity": "sandbox",
  "seed": "issue-1042",
  "action_space": "capability-scoped-tools",
  "reset": {"mode": "git-worktree", "replayable": true},
  "checkpoint": {"supported": true, "strategy": "git-commit-plus-journal"},
  "verifier": {
    "id": "build-test-heldout.v1",
    "version": "2026-07-10",
    "kind": "deterministic+heldout",
    "proxy_risk": "medium"
  },
  "semantic_horizon_target": 12
}
```

## Weights & Biases and experiment-tracking architecture

W&B should be an optional egress sink, not DSCO’s system of record. Chronicle remains local canonical truth.

```text
Chronicle local journal
        -> RL trajectory projection
        -> redaction-aware W&B/MLflow outbox spool
        -> explicit network sync
        -> external experiment tracker
```

### W&B phases

| Phase | W&B role |
|---|---|
| 1 | Run metadata, scalar metrics, configuration. |
| 2 | Event timeline / trajectory tables. |
| 3 | Versioned eval reports and environment/verifier manifests as artifacts. |
| 4 | Replay bundles: journal, fixture hashes, checkpoint references, redacted artifacts. |
| 5 | Sweeps over environment, verifier, curriculum, and agent configuration matrices. |

Recommended configuration:

```bash
# Local capture; no external egress by default.
export DSCO_RL_HOOKS=metadata

# Explicit external sync only.
export DSCO_WANDB_ENABLE=1
export WANDB_API_KEY=...
export DSCO_WANDB_PROJECT=dsco-agent-envs
export DSCO_WANDB_ENTITY=distributed-systems
export DSCO_WANDB_MODE=offline
```

Recommended command surface:

```bash
dsco rl status
dsco rl trajectory show <run-id>
dsco rl export <run-id> --format jsonl
dsco wandb status
dsco wandb sync <run-id>
```

Never export raw prompts, source files, tool arguments, credentials, or tool results automatically. Export requires explicit network authority, destination-aware consent, a redaction policy, durable outbox records, retry/idempotency handling, and local inspection.

Support **MLflow** alongside W&B: W&B is a strong externally shared/research lane; MLflow is the more local/self-hostable enterprise lane.

## Top-45 integration portfolio

### Selection rule

Optimize for environments DSCO can safely inhabit, not raw logo count. Build protocol multipliers first, then native adapters for high-frequency developer, operational, RL/data, and business environments whose semantics/auth/events cannot be reduced cleanly to generic REST.

### Tier 0: protocol multipliers

| # | Integration / protocol | Purpose |
|---:|---|---|
| 1 | MCP | Tool discovery/call interoperability; existing DSCO substrate. |
| 2 | OpenAPI 3.x importer | Governed generation of REST tools; avoids a long tail of bespoke wrappers. |
| 3 | OAuth 2.1 / OIDC + PKCE credential vault | Multi-user/provider authorization and least-privilege connector access. |
| 4 | Webhook ingress + durable callback outbox | Event-driven environments and reliable receipts. |
| 5 | Git / Git worktrees | Source state, branches, diffs, rollback, and environment reset. |
| 6 | PostgreSQL wire protocol | Default durable app data plane; covers many hosted Postgres providers. |
| 7 | SQLite / DuckDB | Embedded/local state, artifact/eval data, analytics. |
| 8 | S3-compatible object storage | AWS S3, R2, MinIO, Backblaze, Wasabi, artifact stores. |
| 9 | AWS Signature V4 | Shared AWS authentication primitive. |
| 10 | OpenTelemetry | Portable tracing/metrics/log export; Chronicle remains canonical. |
| 11 | Docker / OCI | Resettable, reproducible executable environments. |
| 12 | Browser CDP + WebDriver BiDi | Browser environment support without a Playwright/Selenium core dependency. |

### Tier 1: developer and operational core

| # | Integration | Primary agent-environment value |
|---:|---|---|
| 13 | GitHub | Repositories, pull requests, issues, review, automation. |
| 14 | GitHub Actions | CI logs/status/artifacts/dispatch; verifier boundary. |
| 15 | GitLab | SCM and CI/CD, including self-hosted enterprise environments. |
| 16 | Linear | Issue-to-branch-to-PR workflow. |
| 17 | Jira + Confluence | Enterprise work tracking and knowledge. |
| 18 | Slack | Human-in-the-loop escalation, approval, alerts, receipts. |
| 19 | Microsoft Teams + Microsoft Graph | Enterprise collaboration, files, mail, identity-adjacent operations. |
| 20 | Google Workspace | Gmail, Drive, Calendar, Docs, Sheets. |
| 21 | Notion | Structured specifications and operational memory. |
| 22 | Sentry | Production errors, traces, releases, issue state. |
| 23 | PagerDuty | Incident lifecycle and escalation. |
| 24 | Datadog | Mature-team observability and diagnosis workflows. |
| 25 | Kubernetes | Governed staging/production diagnosis, rollout, rollback. |
| 26 | Terraform | Plan/apply infrastructure-as-code boundary. |
| 27 | Cloudflare | DNS, edge, Workers, R2, security, deploy operations. |
| 28 | Vercel | Web deployment previews and feedback loops. |
| 29 | Temporal | Durable workflow integration and recoverable task state. |
| 30 | Redis / Valkey | Cache, queue, pub/sub, locks, coordination. |

### Tier 2: RL, data, model, and evaluation substrate

| # | Integration | Primary value |
|---:|---|---|
| 31 | Weights & Biases | Optional cloud experiment tracking, artifacts, sweeps, reports. |
| 32 | MLflow | Open/self-hostable experiment tracking and registry. |
| 33 | Langfuse | LLM-native traces, datasets, and evaluation. |
| 34 | Arize Phoenix | Open-source LLM trace/eval analysis. |
| 35 | Ray | Distributed rollout/evaluation/simulation. |
| 36 | Apache Airflow | Scheduled data collection and batch evaluation. |
| 37 | Kafka / Redpanda | Durable high-volume environment event streams. |
| 38 | ClickHouse | Fast trajectory/event analytics. |
| 39 | BigQuery | Enterprise eval and operational data analysis. |
| 40 | Snowflake | Governed enterprise warehouse. |
| 41 | Qdrant | Open/self-hostable vector retrieval environment. |
| 42 | Postgres pgvector | Practical vector baseline via Postgres. |

### Tier 3: business execution

| # | Integration | Primary value |
|---:|---|---|
| 43 | Stripe | Payments, subscriptions, customer state, webhooks. |
| 44 | HubSpot | SMB/mid-market CRM and GTM workflows. |
| 45 | Salesforce | Enterprise CRM/procurement requirement. |

## Build sequence

### Wave 0: become an environment runtime

1. Environment manifest and verifier-receipt schema.
2. Generic JSONL subprocess environment adapter.
3. Git/worktree reset-and-branch adapter.
4. Webhook/outbox spine.
5. OAuth/OIDC vault and redaction policies.
6. OpenAPI importer.
7. Postgres, SQLite, DuckDB, S3-compatible storage.
8. Docker/OCI adapter.
9. OTel exporter.
10. `dsco rl trajectory show/export` local CLI.

### Wave 1: engineering agent wedge

1. GitHub + GitHub Actions.
2. Linear.
3. Slack.
4. Sentry.
5. Kubernetes.
6. Terraform.
7. Temporal.
8. Cloudflare.
9. Vercel.
10. Redis/Valkey.

### Wave 2: experiment and enterprise legibility

1. W&B durable exporter.
2. MLflow exporter.
3. Langfuse/Phoenix integration.
4. Ray and Kafka/Redpanda environment modes.
5. ClickHouse analysis sink.
6. Google Workspace and Microsoft Graph.
7. Jira/Confluence.

### First DSCO-native evaluation environments

1. **Repository repair:** repair a known regression in a temporary worktree.
2. **Governed tool workflow:** inspect -> modify -> build -> test -> report under a fixed capability envelope.
3. **Recovery task:** inject failure mid-run; require checkpoint restoration and prove no duplicate side effects.

These test semantic horizon, diversity, verifier provenance, reset economics, branch quality, cost, and capability compliance.

## What not to build natively first

| Candidate | Better route / reason |
|---|---|
| Long-tail SaaS wrappers | MCP, OpenAPI, n8n, Zapier. Avoid auth/test-drift maintenance burden. |
| Netlify | Vercel first; overlapping initial deployment category. |
| Bespoke Supabase/Neon clients | Postgres wire protocol plus OAuth/API only when needed. |
| Prefect/Dagster | Temporal and Airflow cover enough initial workflow surface. |
| Aim/ClearML/Neptune | MLflow and W&B first. |
| Playwright/Selenium core | CDP and WebDriver BiDi protocol first. |
| SQL Server/Oracle wire implementations | ODBC/JDBC sidecar or customer-funded adapter. |
| Crypto payment rails / AP2 / x402 | Strategically interesting, but volatile and financially consequential. |
| ROS/MQTT first wave | Physical-world lane is customer-driven, not initial developer GTM. |

## Governed connector contract

Every connector must implement this lifecycle:

```text
discover -> authenticate -> describe capability -> plan -> authorize ->
execute -> record receipt -> verify -> compensate/rollback
```

Every operation must retain:

- capability classification;
- destination and principal authority boundary;
- idempotency key;
- durable request/response receipt;
- redaction policy;
- checkpoint/reset/rollback semantics where applicable;
- trajectory event projection;
- optional export routing only after local durable recording.

## Literature anchors

- Will Brown / `@willccbb`, short unenumerated post on distinct RL scaling axes: https://x.com/willccbb/status/2070022399530348933
- Gao, Schulman, Hilton, *Scaling Laws for Reward Model Overoptimization*, ICML 2023: https://arxiv.org/abs/2210.10760
- Laidlaw, Russell, Dragan, *Bridging RL Theory and Practice with the Effective Horizon*, NeurIPS 2023.
- Arjona-Medina et al., *RUDDER: Return Decomposition for Delayed Rewards*, NeurIPS 2019.
- Ecoffet et al., *Go-Explore: a New Approach for Hard-Exploration Problems*, Nature 2021.
- Sharma et al., *Autonomous Reinforcement Learning: Formalism and Benchmarking* / EARL, 2021.
- Jiang et al., *Prioritized Level Replay*, ICML 2021.
- Dennis et al., regret-based unsupervised environment design / PAIRED, NeurIPS 2020.
- Dohare et al., *Loss of Plasticity in Deep Continual Learning*, Nature 2024.

## Decision rule

DSCO should win as the **governed, local-first, protocol-multiplied agent runtime** that can safely read, act, resume, audit, and reverse across valuable environments. W&B and other external systems are optional, redaction-aware sinks. Chronicle, capability gating, durable records, and verifier provenance remain the sovereign core.
