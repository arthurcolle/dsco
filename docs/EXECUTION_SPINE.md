# Execution Spine

The execution spine is DSCO's mandatory lifecycle record for tool calls. It is
inserted at `tools_execute_for_tier()`, before the capability and governance
decisions, and closes on every return from that gate. Direct CLI, nested tool,
MCP, ACP, surface, worker and agent-loop callers all enter through this point.

## Runtime path

```text
caller
  -> tools_execute_for_tier()
       -> proposed
       -> capability and governance gates
       -> denied, or admitted
       -> alias/sandbox routing and input normalization
       -> started
       -> tools_execute_internal()
       -> succeeded or failed
```

`tools_execute_internal()` remains private. Its only bypass is the
`DSCO_INTERNAL_TESTS` test adapter. The spine observes gate behavior and does
not change a gate decision, tool input, result, or return value.

## Attempt contract

Every attempt has a UUID `execution_id`, a process-monotonic sequence, optional
parent/session/trace/turn/tool-call IDs, requested and dispatched tool names,
trust tier, capability mask and names, effect class, principal, route context,
timestamps, terminal state, elapsed time, result size, and SHA-256 hashes of
the requested input, dispatched input, and result.

Raw arguments and results are absent from attempt payloads. Hashes can still be
sensitive for low-entropy values, so Chronicle classifies attempt events as
`private_user_content`. A direct call that was not produced by an active model
route records null provider/model fields. The executor names the leaf runtime,
currently `dsco-native`.

The current states are:

| State | Meaning |
|---|---|
| `proposed` | The call reached the production gate. |
| `denied` | A capability or governance decision rejected it before admission. |
| `admitted` | All applicable policy checks passed. |
| `started` | Routing and normalization completed and the leaf is about to run. |
| `succeeded` | The leaf returned success. This is not independent outcome verification. |
| `failed` | An admitted call did not return success, including pre-leaf routing failure. |

`cancelled` and `effect_unknown` are reserved states. They become active when
cancellation provenance and crash reconciliation are connected to durable
attempt leases.

## Storage and durability

The process keeps the 512 newest receipts in a mutex-protected ring for bounded
in-process queries. Late completion of an evicted attempt cannot overwrite a
newer row. Nested calls use thread-local parent linkage, so concurrent calls do
not share a last-receipt slot.

When Chronicle is active, each transition emits
`execution.attempt.<state>` with payload schema `execution.attempt.v1` and also
appends the same payload to Chronicle's framed WAL. The `started` and terminal
frames of effectful calls (`fs_write`, `net`, `exec`, or `control`) request an
fsync. Chronicle failure is observational in this slice: it does not change an
otherwise allowed tool result.

View persisted events through the Chronicle timeline or its JSON export:

```sh
./dsco --timeline-server --timeline-port 8421
```

## Verification

```sh
make test-execution-kernel
make test-execution-spine-mcp
make test-gate-claims
```

The unit fixture covers lifecycle ordering, hashes, raw-data exclusion,
effectful durability, terminal idempotence, denial versus failure, nesting,
concurrency, bounded eviction, and the stale-finish race. The MCP fixture drives
the real binary through denied, successful read, failed read, and successful
process-effect calls, then validates Chronicle JSONL and CRC-framed WAL output.
Its prerequisite structural fixture rejects any production call to the private
leaf dispatcher outside the instrumented gate.

## Inspecting interrupted runs

```sh
./dsco runs inspect <run-id>
```

This read-only command projects the CRC-validated journal snapshot into an
uncertainty report. A started effectful call without a successful terminal
record is reported as `effect_unknown`, including calls that failed after
starting: failure alone does not prove that no external effect occurred.
The command never executes a tool, changes a receipt, or authorizes a retry.
It also does not infer whether a process is still alive.

Exit status is `0` for complete evidence with no unresolved attempts, `1` for
incomplete/invalid evidence, `2` for invalid arguments, and `3` for unresolved
attempts. Every report contains `retry_authorized:false`. The parser validates
run/attempt identities, lifecycle transitions and effect metadata, and stops
at explicit frame, snapshot and attempt-count limits without evicting evidence.

Chronicle now serializes all journal producers, uses a serialized SQLite
connection explicitly, and preserves a partial-write failure as the last
frame by refusing later appends. See
[the pinned Codex review](../reports/codex-harness-20260908/REVIEW.md) and run
`make test-harness-reliability` for concurrency, crash, context and process tests.

Durable attempt leases, automatic reconciliation and retry decisions remain
the next layer. Inspection reports uncertainty; it does not append an
`effect_unknown` state or provide exactly-once external execution.
