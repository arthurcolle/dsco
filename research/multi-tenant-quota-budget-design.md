# Multi-tenant quota and budget enforcement for hosted agents

## Goal
Design a quota/budget system that works for hosted agents with:

- hard multi-tenant isolation
- fast local admission control
- deterministic no-overdraft behavior
- low per-agent memory overhead
- a **tenant hot index under 1 KB per agent**

This design assumes the existing single-session budget helper (`include/cost_budget.h`) remains the local arithmetic layer, but tenant-aware enforcement sits above it.

---

## Problem decomposition

We need to enforce several different constraints at once:

1. **Money budget**: USD spend per tenant, workspace, project, or agent.
2. **Rate budget**: requests/minute, tokens/minute, concurrency.
3. **Time budget**: wall-clock execution caps.
4. **Compute budget**: GPU/CPU seconds or weighted effort units.
5. **Policy budget**: allowed models, tools, regions, and risk tiers.

The key tension is that enforcement must be:

- fast enough for every tool/LLM dispatch
- accurate enough to prevent overspend
- small enough to cache locally on every agent
- resilient to network partitions

---

## Recommended architecture

### 1) Authoritative control plane
A central quota service owns the true source of truth:

- append-only spend ledger
- tenant policy registry
- reserved vs committed spend
- budget periods and resets
- audit trail

This service is authoritative, but not on the hot path for every token.

### 2) Regional quota shards
Shard by `tenant_id` hash or `(tenant_id, workspace_id)` hash.

Each shard maintains:

- current tenant counters
- token buckets / leaky buckets for rate limits
- reservation state
- lease state for agents
- policy epoch

The shard can answer reserve/commit/release in one round-trip.

### 3) Agent-local hot index
Each agent carries a compact cached view of the tenant state.

Used for:

- preflight checks before dispatch
- local fast-fail when clearly over budget
- estimating whether a request should even hit the model/router
- avoiding a control-plane call when obviously safe or obviously blocked

### 4) Durable ledger
All spend is recorded as immutable events:

- `reserve`
- `commit`
- `release`
- `adjust`
- `reset`
- `policy_change`

The ledger is the final reconciliation source.

---

## Enforcement model

Use a **reservation + commit** flow.

### Preflight
Before starting work, the agent estimates cost.

It checks the local hot index:

- if clearly blocked, fail immediately
- if clearly safe, proceed with a reservation
- if ambiguous, refresh from shard

### Reserve
The quota service grants a bounded reservation:

- money reservation in micro-dollars
- token reservation
- wall-clock or compute reservation

Reservations have TTLs so abandoned work does not hold budget forever.

### Commit
When the job finishes:

- actual usage is committed
- unused reservation is released
- ledger records the delta

### Release
If the job is canceled, failed, or preempted:

- release the unused portion
- keep any consumed portion as committed spend

---

## Hierarchical budgets

Budgets should be enforced at multiple scopes:

1. **Org / master account**
2. **Tenant**
3. **Workspace / project**
4. **Agent pool**
5. **Individual agent session**

A request must satisfy every applicable scope.

Effective budget = minimum remaining budget across all scopes.

This is the same pattern already used by the current session/day helper, but extended to tenant hierarchy.

---

## Hot index design

### Requirements
- resident on each agent
- refreshed periodically or on policy epoch change
- must fit under **1 KB / agent**
- enough to make a local admit/deny decision
- no need to store the full policy blob locally

### Recommended size target
**256–512 bytes typical, hard cap < 1 KB**.

### Suggested contents
Store only:

- stable IDs / hashes
- current counters
- current lease metadata
- policy epoch / budget epoch
- local pessimistic safety margin
- a few scalar limits

Do **not** store:

- full tenant policy documents
- full routing tables
- detailed audit trail
- per-request history

Those live in shared service memory or durable storage.

### Example packed structure
```c
typedef struct __attribute__((packed)) {
    uint64_t tenant_id;
    uint64_t workspace_id;
    uint64_t lease_id;
    uint64_t policy_hash;

    int64_t  reserve_usd_u6;        /* micro-dollars */
    int64_t  spent_usd_u6;
    int64_t  daily_cap_usd_u6;
    int64_t  session_cap_usd_u6;

    int64_t  token_bucket;
    int64_t  token_burst;
    int64_t  tokens_reserved;
    int64_t  tokens_spent;

    uint32_t policy_epoch;
    uint32_t budget_epoch;
    uint32_t last_sync_seq;
    uint32_t lease_expiry_s;

    uint32_t refresh_after_ms;
    uint32_t max_drift_usd_u6;
    uint32_t max_drift_tokens;
    uint32_t risk_flags;

    uint16_t priority;
    uint16_t scope_flags;
    uint16_t deny_reason;
    uint16_t reserved0;

    uint8_t  safety_margin_pct;
    uint8_t  utilization_pct;
    uint8_t  state;
    uint8_t  reserved1;

    uint64_t router_key;
    uint64_t model_key;
    uint64_t window_key;
    uint64_t rate_key;
} tenant_hot_index_t;
```

This is comfortably below 1 KB even with future growth.

### Memory budget estimate
A realistic hot index can be kept around **160–320 bytes** with room for:

- 4 x 64-bit IDs = 32 B
- 8 x 64-bit counters = 64 B
- 8 x 32-bit fields = 32 B
- 8 x 16-bit fields = 16 B
- padding / future expansion = 64–128 B

Total: ~208–272 B typical.

Even a conservative version with extra safety fields stays below 1 KB.

---

## Local decision logic

The agent should use three states:

### 1) Admit immediately
If all of these hold:
- lease valid
- policy epoch matches or is within safe grace window
- remaining budget exceeds estimated cost + safety margin
- rate bucket has capacity

### 2) Refresh then decide
If:
- lease nearly expired
- drift too large
- utilization close to threshold
- policy epoch changed

### 3) Deny immediately
If:
- hard cap reached
- tenant suspended
- policy prohibits the action/model/tool
- quota service explicitly revoked the lease

This avoids burning tokens on requests that are likely to fail.

---

## Drift and correctness

Because the hot index is cached, it must be pessimistic:

- local remaining budget should be slightly lower than authoritative remaining budget
- reservations should subtract before dispatch
- release should be conservative on failure

Use a safety margin, e.g.:

- 1–5% of current cap, or
- a fixed micro-dollar / token buffer

The control plane can also enforce a **max drift** rule:

- if local state differs too much from authoritative state, force refresh

---

## Multi-dimensional quota enforcement

A hosted agent typically needs multiple counters:

- USD spend
- input tokens
- output tokens
- cache tokens
- requests
- concurrent runs
- tool invocations
- wall-clock seconds
- retries / escalations

Use the same reservation/commit flow for each dimension.

A request is admissible only if **all constrained dimensions pass**.

For compound requests, reserve against the estimated worst-case, then reconcile actual usage at completion.

---

## Failure modes and handling

### Network partition
If the agent cannot reach the quota service:

- use the local hot index only if lease is still valid
- otherwise fail closed for high-risk / paid actions
- optionally allow a tiny emergency budget if explicitly configured

### Lost commit
If a commit response is lost:

- make reservation IDs idempotent
- ledger commit must be deduplicated by reservation ID

### Replay / double spend
Prevent with:

- monotonic sequence numbers
- reservation IDs
- lease epochs
- idempotent commit semantics

### Clock skew
Do not rely on wall clock alone for correctness.

Use:

- lease TTLs with generous margins
- monotonic sequence numbers for settlement
- server-side authoritative expirations

---

## Security and isolation

- tenant IDs must be opaque, unguessable identifiers
- local hot index should not expose other tenants’ state
- all quota mutations must be authenticated and authorized
- hot index refreshes must be signed or come over an authenticated channel
- policy changes should be versioned and auditable

If hosted agents are untrusted, the local hot index is only an optimization; the quota shard remains authoritative.

---

## Implementation sketch

### Data plane
1. agent requests work
2. agent reads local hot index
3. if safe, it reserves from regional shard
4. shard grants reservation + lease
5. agent executes
6. agent commits actual spend
7. shard updates ledger and counters
8. hot index is refreshed asynchronously

### Control plane
1. policy update
2. budget update
3. tenant suspension / reactivation
4. lease revocation
5. audit export

---

## Integration with existing dsco budget helpers

The current helper in `include/cost_budget.h` is a good single-session arithmetic layer:

- session spend
- session budget
- daily spend
- daily budget
- effective remaining

For hosted multi-tenant agents, extend this with a tenant-facing layer that computes:

- tenant remaining
- workspace remaining
- agent remaining
- effective remaining = min(all applicable scopes)

In other words:

- **`cost_budget.h`** = local arithmetic primitive
- **tenant quota service** = authority and reservation manager
- **hot index** = compact cached view for fast admission control

---

## Recommended policy defaults

A safe initial policy:

- hard caps on money and requests
- soft caps for warnings / throttling
- reservation TTL: 30–120 seconds
- refresh every 5–30 seconds under active load
- force refresh on policy epoch change
- deny on stale lease for paid actions
- small emergency budget only for explicit recovery paths

---

## Practical recommendation

If you want the simplest robust version:

- use a central quota shard per tenant hash
- use reservation + commit + release
- keep the agent hot index to ~256 B
- store only the current lease, epochs, counters, and safety margins
- make the hot path pessimistic
- make settlement idempotent

That gives you strong safety, low latency, and a clean path to scale.

---

## Next step

If useful, I can turn this into one of these:

1. a C header / struct proposal
2. a control-plane API spec
3. a PostgreSQL / Redis / event-sourcing schema
4. a full implementation plan with rollout phases and failure tests
