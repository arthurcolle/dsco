# Chronicle evidence graph

`dsco trace-kg export <chronicle.sqlite> [session-id]` streams a local, read-only
projection of Chronicle events as `dsco.trace-kg.v1` JSONL. No LLM or network is
required. Example:

```sh
./dsco trace-kg export ~/.dsco/chronicle/indexes/chronicle.sqlite SESSION_ID > graph.jsonl
```

Check exit status; discard partial output on failure. Export to a new file, never
the input database. Output contains metadata (including tool names and event IDs)
and is not certified anonymized; keep private unless separately reviewed.

## Contract

- Nodes: session, trace, span, typed events, tool, reported_success/reported_failure.
- Edges: contains, in_trace, in_span, child_of, uses_tool, reports.
- SHA-256 IDs use domain-separated length-prefixed tuples. Events and spans are
  session-scoped; tools are shared by exact name. IDs are deterministic.
- Every record has an evidence event ID and `observed_telemetry` epistemic status.
  Event labels retain the original Chronicle event ID for source lookup. Tool
  labels retain the exact tool name. Hashing IDs does not authenticate source data.
- Each export reads a single SQLite snapshot, ordered by session, sequence, event ID.
- JSONL is a stream of assertions, not a unique-node dump: merge nodes by ID and
  retain their evidence set. Edges include evidence in their identity.
- No prompts, argument bodies, result bodies, or blob contents are copied.
- Only boolean `ok` on `tool.call.completed` generates an outcome assertion.
  Missing/malformed payloads remain events without inferred outcomes.
- A later success is not automatically a recovery, and temporal proximity is not
  causality. Verification and recovery require explicit evidence, not inference.

This is an offline projection, not a new execution route, automatic graph upload,
policy engine, or replacement Chronicle store. Existing gated shell execution can
invoke the CLI. No governance or capability definitions are changed.

## Verification

```sh
make dsco
python3 tests/test_trace_kg.py -v
python3 tests/test_toolmgmt_parallel.py -v
make test-gate-claims
```

The fan-out test compiles the actual production function with deterministic
pthread-create failures across all 16 four-thread patterns. It also verifies the
old implementation fails the test. It is not a scheduler stress test.

## API incident

The observed `Unknown meta-tool` failure originated at the remote MCP dispatch
surface. The sibling ToolManagement checkout already has direct catalog-call
normalization through `execute_tool`; its existing dispatch/cache suite passed
10 tests during this work. Those changes were not authored or deployed here.
Remote deployment parity remains unverified and requires deployment approval.

The independently found local fan-out bug was corrected by packing successful
pthread handles before joining: failure of an earlier create no longer causes a
later live worker to be omitted from joins while its stack-owned pool expires.

## Telemetry projection v3 (current)

The adversarial-review repairs add typed `ok` to source event nodes. Changing
only success/failure now conflicts transactionally rather than retaining both
outcomes. Destination metadata and a scan of existing assertion projections
reject disjoint old-version records too. Rebuild old graphs into a NEW store.

Failure queries distinguish self-evidenced source events from derived outcome
nodes; reserved source event names no longer manufacture failed calls. Summary
counts only self-evidenced completions, excluding duplicate foreign-evidence
copies. Target attribution requires exactly one start event on the matching
span; multiple starts return `ambiguous`, not an arbitrary target.

Regression: `python3 tests/test_swarm_findings.py -v` (five cases). Independently
rerun by native worker in swarm_1788595568_g4_create_verify-swarm-repairs: 5 passed.
Thread-local contention test additionally exercises eight threads with 100,000
reset/update/check iterations each. Exact OS deadline-boundary ordering is not
claimed deterministic; no boundary race defect was demonstrated by the review.
Source deletion and changes to nonprojected fields remain outside conflict
reconciliation. This is not a universal source-integrity certificate.

## Telemetry projection v2 (historical behavior; superseded by v3)

Records now carry `projection: telemetry.v2`. Source event nodes preserve typed
`timeout`, timeout_origin, execution_status, failure_class, wrapper_tool_name,
requested_tool_name and tool_name when present. Missing historical fields stay
missing; no timeout is inferred from result text.

`dsco trace-kg failures graph.sqlite --summary` groups reported failed completions
by wrapper, requested target, timeout origin and failure class. Each group carries
a count and evidence-event ID array. Requested target can be joined from a start
on the same session-scoped span; it is not claimed to be resolved remote identity.
Unknown values remain `unknown`. This is a failure count, not a reliability rate.

Existing assertion content must match exactly on replay. A conflicting record or
old projection aborts ingestion transactionally with `projection/evidence conflict`.
Rebuild into a NEW graph for v2; the old graph is left intact. This is explicit
rebuild-based upgrade handling, not an in-place migration or full source-mutation
reconciliation mechanism.

Verification: `python3 tests/verify_telemetry_graph.py BUILD_LOG` reuses the native
link flags from a verbose `make dsco` log. It links production objects and runs a
real governed shell wall timeout concurrently with a successful shell call,
records Chronicle, ingests, checks summary counts/evidence, replays, and rejects
modified telemetry. Set `DSCO_TEST_REAL_IDLE=1` for the approximately 61-second
real shell idle-timeout case. Both wall and idle execution paths were exercised;
concurrent wall/success isolation was also verified. No LLM or remote API needed.

## Local assertion store and diagnostics

```sh
./dsco trace-kg ingest chronicle.sqlite --into graph.sqlite
./dsco trace-kg failures graph.sqlite
./dsco trace-kg explain graph.sqlite EVENT_ID
python3 tests/test_trace_kg_store.py -v
```

Ingestion completes a read-only source snapshot before starting one destination
transaction. Assertions are keyed by `(id,evidence)`; replay inserts zero duplicate
assertions. New evidence is additive. An interrupted or failed transaction rolls
back; replay resumes safely by deduplication. This currently rescans the snapshot,
not a source-offset checkpoint: destination updates are incremental, source reads
are not. A disk-backed temporary stream bounds process memory. Source/destination
inode aliasing (including symlinks and hardlinks) is rejected before opening the
store. Do not modify source traces in place: existing assertions are immutable.

`failures` returns complete assertion bundles for reported failures, with tool
labels and evidence references. `explain` accepts a graph event ID or original
Chronicle event ID. Queries are read-only and parameterized. Empty output means
no matching evidence, not a verified healthy system. Tool-level rates and summary
aggregation are not implemented. Keep graph databases private: metadata is not
certified anonymized.

## Limitations / rollback

No source-offset checkpoint, cost projection, inferred recovery classifier,
unfinished-call query, or independent trace authenticity verification is included.
Remove trace_kg.c/h, the three main.c dispatch/help hooks, and its Makefile entry
to remove this optional CLI. Preserve unrelated working-tree edits; do not use a
whole-file git reset. The build's default `all` target also refreshes local PATH
binaries; use explicit `make dsco` for build-only iterations.
