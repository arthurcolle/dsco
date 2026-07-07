# Activation Scheduler Leases and Heartbeats for Disposable Agent Workers

**Status:** proposed
**Scope:** local-first activation scheduler, per-worker leases, worker heartbeats, expiry/reclaim, and a file-backed prototype

## 1. Problem

Disposable workers need a simple, local coordination primitive:

- only one controller should own a worker at a time
- workers must prove liveness while they are active
- dead workers should be reclaimed quickly
- stale workers should not be confused with live ones after restart
- the mechanism should work without a database, queue, or network service

This is especially useful for agent workers that are:

- short-lived
- restartable
- bounded in scope
- safe to kill and replace

## 2. What exists already in dsco

Relevant code already present in the tree:

- `src/scheduler.c` — task scheduling, states, wakeups, budgets
- `src/waiter.c` — monotonic-friendly waits and stop signaling
- `src/heartbeat.c` — periodic runtime heartbeat / local persistence

The current scheduler is task-centric, not worker-centric. It has no lease ownership model, no heartbeat-based reclamation, and no durable activation record.

That makes it a good base, but not yet a disposable-worker control plane.

## 3. Design goals

1. **Single-owner activation**
   - a worker slot is owned by at most one controller lease at a time

2. **Liveness proof**
   - active workers emit heartbeats before their lease expires

3. **Fast failure detection**
   - if heartbeats stop, the controller can reclaim the slot

4. **Crash safety**
   - process crash, SIGKILL, or machine reboot should not create permanent limbo

5. **Local-first implementation**
   - file-backed state, no external dependency

6. **Minimal state surface**
   - simple JSON records and atomic file replacement

## 4. Core model

### Entities

- **Controller**: decides which worker slot is active and renews leases
- **Worker**: disposable process that performs work and emits heartbeats
- **Lease**: durable ownership record for a worker slot
- **Heartbeat**: durability-limited liveness record emitted by the worker

### State machine

```text
new -> leased -> active -> renewing -> expired -> reclaimed -> released
                   \-> failed -> reclaimed
```

A worker may die at any point. The controller is the final authority on whether a slot is still usable.

## 5. File layout for the prototype

Use a local directory such as `~/.dsco/activation/` or `./.dsco-activation/`.

```text
activation/
  controller.json
  workers/
    <worker_id>/
      lease.json
      heartbeat.json
      events.log
      tombstone.json
```

### `lease.json`

Canonical ownership record.

```json
{
  "schema": "dsco.activation.lease.v1",
  "worker_id": "w-001",
  "lease_id": "b0d4...",
  "controller_id": "ctl-01",
  "pid": 12345,
  "seq": 7,
  "state": "active",
  "ttl_ms": 15000,
  "expires_at_ms": 1730000000000,
  "renew_by_ms": 1730000009000,
  "task_id": "task-abc",
  "created_at_ms": 1730000000000,
  "updated_at_ms": 1730000005000
}
```

### `heartbeat.json`

Worker-emitted liveness record.

```json
{
  "schema": "dsco.activation.heartbeat.v1",
  "worker_id": "w-001",
  "lease_id": "b0d4...",
  "pid": 12345,
  "seq": 7,
  "ts_ms": 1730000005000,
  "progress": 0.42,
  "phase": "executing",
  "note": "processing shard 12"
}
```

### Why both lease and heartbeat?

- `lease.json` answers: who owns this slot and until when?
- `heartbeat.json` answers: is the worker still alive and making progress?

The lease is authoritative. The heartbeat is an liveness hint and recovery aid.

## 6. Protocol

### 6.1 Lease acquisition

Controller acquires a lease by writing a fresh `lease.json` atomically.

Rules:

- generate a new `lease_id`
- increment `seq`
- set `expires_at_ms = now + ttl_ms`
- set `renew_by_ms` to an earlier deadline, typically `now + ttl_ms * 2/3`
- write via temp file + `fsync` + `rename`

A worker only becomes active after it can read a valid lease record matching its slot and lease id.

### 6.2 Heartbeat emission

Worker emits a heartbeat on a fixed cadence:

- interval should be less than `ttl_ms / 3`
- write atomically to `heartbeat.json`
- include the lease id and current lease sequence

### 6.3 Lease renewal

Controller periodically renews the lease if the heartbeat is healthy.

Renewal updates:

- `seq += 1`
- `updated_at_ms = now`
- `expires_at_ms = now + ttl_ms`
- optional `phase` / `task_id` / `metadata`

### 6.4 Expiry and reclaim

A lease is considered expired if any of these are true:

- current time exceeds `expires_at_ms`
- heartbeat is missing for longer than grace period
- heartbeat lease id does not match current lease id
- heartbeat sequence stalls while controller still expects renewal

On expiry:

- mark `state = expired`
- write a tombstone or event entry
- start replacement worker if needed
- if the old worker later wakes up, it must self-terminate on lease mismatch

## 7. Recommended invariants

1. **Monotonic sequence per lease**
   - every renewal increments `seq`

2. **Lease id changes on reactivation**
   - a fresh worker start gets a fresh `lease_id`

3. **Heartbeat never outruns lease**
   - a heartbeat with stale seq or stale lease id is ignored

4. **No in-place partial writes**
   - write temp + fsync + rename only

5. **Controller is pessimistic**
   - if records disagree, treat the worker as dead until proven otherwise

6. **Grace window is bounded**
   - allow a small grace window for scheduler jitter, not an open-ended extension

## 8. Failure modes and mitigations

| Failure mode | Symptom | Detection | Mitigation |
|---|---|---|---|
| Worker crash | heartbeat stops | expired heartbeat / missing renewals | reclaim slot, relaunch replacement |
| Controller crash | no renewals | lease ages out | next controller instance reaps stale leases |
| SIGKILL / power loss | no clean tombstone | stale lease file remains | TTL expiry plus startup sweep |
| File write torn | invalid JSON | parse failure | rewrite atomically; ignore corrupt temp files |
| Clock jump forward | premature expiry | wall clock delta spikes | add grace window; prefer monotonic for in-process checks |
| Clock jump backward | lease lingers too long | renewal appears delayed | use sequence + grace + startup sweep |
| PID reuse | stale PID looks alive | PID alone insufficient | require lease_id and seq, not PID only |
| Duplicate worker start | two workers contend | conflicting heartbeats | only one lease id is valid; loser exits |
| Split brain controller | two controllers renew | lease seq conflict | single controller lock file or advisory flock |
| Disk full | write fails | fsync/write error | fail closed, stop accepting new activations |
| FS latency / stall | missed heartbeat | renewal delay | ttl should be comfortably larger than p99 write latency |
| Process suspension | heartbeat pauses | renew misses deadline | grace window + reclaim + restart |
| NFS / unstable filesystem | rename semantics weaken | inconsistent state | keep prototype local disk only |

## 9. Local file-backed prototype

### Recommended prototype shape

A small local tool can prove the mechanism without needing the full runtime:

- `controller init` — create activation directory
- `controller lease` — create / renew a worker lease
- `worker run` — emit heartbeats until lease expires or task completes
- `controller scan` — classify workers as active / stale / expired
- `demo` — start a worker, renew for a few cycles, then stop renewing to verify reclaim

### Implementation notes

- Use `os.replace()` for atomic file replacement
- Call `fsync()` on both the temp file and the directory after rename
- Keep JSON minimal and versioned
- Never depend on PID alone
- Use a random `lease_id` and a strictly increasing `seq`

### Suggested directories and env vars

- `DSCO_ACTIVATION_DIR` — override base directory
- `DSCO_LEASE_TTL_MS` — default lease TTL
- `DSCO_HEARTBEAT_MS` — heartbeat interval
- `DSCO_LEASE_GRACE_MS` — expiry grace window

## 10. Integration path into dsco

### Option A: extend current scheduler

Add worker lease fields to `sched_task_t` or a companion structure:

- `worker_id`
- `lease_id`
- `lease_expires_ms`
- `last_heartbeat_ms`
- `lease_seq`
- `controller_epoch`

Add scheduler helpers:

- `sched_lease_acquire()`
- `sched_lease_renew()`
- `sched_lease_expire()`
- `sched_worker_heartbeat()`

### Option B: keep scheduler pure, add activation layer

Preferred if you want to preserve the current task scheduler’s simplicity.

- scheduler stays responsible for task dispatch
- activation layer owns lease files and worker lifecycle
- heartbeats are a control-plane concern, not a task concern

This is cleaner if the eventual system includes multiple topologies and external executors.

## 11. Practical recommendation

For dsco, I would do this:

1. implement the file-backed prototype first
2. keep it local-only
3. validate failure behavior under crash and missed heartbeats
4. then decide whether lease state belongs in `scheduler.c` or a new activation module

The prototype should prove three things:

- stale workers are reclaimed
- duplicate workers do not survive lease mismatch
- file corruption or partial writes fail closed

## 12. Success criteria

The design is good if all of the following are true:

- a worker that stops heartbeating is reclaimed within bounded time
- a restarted controller can identify stale leases after crash
- workers self-terminate when they detect lease loss
- no external service is required
- the file model is simple enough to inspect by hand

## 13. Next implementation step

Build the prototype as a small local script and add a smoke test that:

1. creates a lease
2. starts a disposable worker
3. validates a few heartbeats
4. stops renewal
5. confirms expiry and reclaim
6. verifies the old worker cannot continue under a stale lease
