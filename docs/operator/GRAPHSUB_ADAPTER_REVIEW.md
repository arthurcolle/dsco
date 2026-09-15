# GraphSub implementation map for the DSCO operator

**Expanded source review, September 9, 2026.** This supports the [operator definition](../DSCO_GRAPHSUB_OPERATOR.md) and [world protocol](WORLD_PROTOCOL.md). It supersedes the earlier route-focused assessment in this file.

## 1. Build the operator over the existing system

**GraphSub already implements substantial storage, transaction, snapshot, replay, incremental-computation, typed-compute, execution-trace and source-hydration machinery. DSCO/Lingo should bind these facilities into one working environment.** The main engineering question is which existing path owns each operation, how that path is connected, and what its verified semantics are.

An API exposure gap is not an absent backend implementation. A library implementation is not automatically wired into every API. A registered route is not evidence that a production deployment runs that revision. This review keeps these three distinctions explicit.

The inspected checkout is `/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub`, at HEAD `52ade087bd3d07797d3631b7d0b37ae54f07dd9b`, with extensive existing working-tree changes. Evidence below describes that working tree, not a clean release at the commit. The exact suggested `/Users/arthurcolle/dsco/dsco-graphsub` path was absent during direct checks; this does not establish that no other checkout exists.

No GraphSub files were changed, credentials inspected, services started, deployments performed or runtime tests run for this review. Source anchors may move after edits. Runtime acceptance remains separate from this implementation inventory.

## 2. Existing storage and transaction lanes

| Existing facility | Implementation and reuse opportunity | Wiring and precise qualification |
|---|---|---|
| Graph MVCC and historical state | [specialized/mvcc.rs:431](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/mvcc.rs:431) reconstructs node state from timestamp-visible delta chains. The module includes transactions, read/write sets, conflict checks, locks, deadlock handling and GC. | [ExecutionContext:76](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/agents/execution_context.rs:76) constructs this manager when MVCC is enabled. The manager is passed to Cypher, but targeted searches of that executor found initialization/assignment rather than lifecycle or visibility calls. Trace the selected execution path before labeling ordinary queries snapshot reads. |
| Conditional delta commit | [mvcc.rs:519](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/mvcc.rs:519) implements expected-head checking and delta installation. | This is existing conditional-update machinery. [Lines 638 onward](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/mvcc.rs:638) explicitly document partial failure if edge validation fails after node updates; arbitrary node-plus-edge atomicity cannot be inferred from `commit_atomic`. |
| HLC transaction coordinator | [txn/coordinator.rs:77](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/txn/coordinator.rs:77) provides reads, writes, deletes, intent commit/abort, locking and read/write sets over `MVCCStore`. | Exported through the `txn` module. Construction references found in the bounded search were tests, not API handlers. [Validation:257](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/txn/coordinator.rs:257) explicitly relies on locking rather than implementing SSI validation. |
| Persistent MVCC | [txn/persistent_store.rs:18](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/txn/persistent_store.rs:18) provides LSM-backed intents, timestamped versions, transaction status and flush. [Commit:98](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/txn/persistent_store.rs:98) batches committed versions, intent deletions and the transaction record. | A real persistence implementation to reuse. The inspected coordinator holds `Arc<MVCCStore>`, so it must not be assumed to use `PersistentMVCCStore` automatically. |
| SQLite Honker graph and outbox | [sqlite_honker.rs:204](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/storage/sqlite_honker.rs:204) puts graph writes and mirror jobs in the same SQLite transaction. Node/edge creation, property updates and deletion use this coupling. | Feature-gated by `storage-sqlite-honker`; used in module tests and footprint/trading examples. No API-handler use was found. This is an existing transactional outbox foundation, not a new subsystem to invent. |
| Honker queue lifecycle | [sqlite_honker.rs:377](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/storage/sqlite_honker.rs:377) atomically claims jobs with visibility deadlines and attempt counts; acknowledgment, retry, expiry reclamation and dead-letter handling follow. | Current `ack(job_id)` deletes by ID without an ownership generation. [Initialization:120](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/storage/sqlite_honker.rs:120) uses SQLite WAL and `synchronous=NORMAL`. Bind and test those actual semantics before promising fenced acknowledgments or a stronger durability level. |
| Persistent graph and recovery | [storage/persistence.rs:329](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/storage/persistence.rs:329) implements `PersistentGraph`, group commit, WAL recovery, durable writes, sync and shutdown. | This exists alongside the API mmap/WAL/checkpoint lane. The adapter should identify its chosen storage owner instead of combining guarantees from independent implementations. |

REST and Cypher currently share the same shard graph through [get_or_create_context:4117](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:4117). Older warnings that they necessarily use separate graphs are superseded for this inspected path.

## 3. Existing history, snapshot and replay services

`specialized/event_store.rs` already contains sequenced events, node/edge/time indexes, snapshots, bounded retention, subscriptions, event queries and a replay engine. Relevant entry points include [append:376](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs:376), [create_snapshot:582](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs:582), [get_snapshot_at:680](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs:680) and [ReplayEngine:754](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs:754).

These are already exposed by the [route registry:16407](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:16407):

| Routes | Existing function |
|---|---|
| `GET /api/v1/events`, `/events/stats`, `/events/{seq}` | Sequenced event listing, statistics and retrieval |
| `GET /api/v1/events/node/{node_id}`, `/events/edge/{edge_id}` | Entity history |
| `GET /api/v1/events/stream` | Event-store SSE with sequence numbers in payloads |
| `GET/POST /api/v1/snapshots` | List/create graph snapshots |
| `GET /api/v1/snapshots/{seq}` | Snapshot retrieval |
| `POST /api/v1/replay`, `/replay/verify` | State reconstruction and determinism comparison |

The inspected [listing handler:21397](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:21397) interprets `offset` as an after-sequence position. This surface must not be confused with the simpler `/events` broadcast route.

The next binding work is concrete. Event-store `new` and [append:381](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs:381) currently initialize/update memory maps, a bounded deque and a broadcast channel. Configuration fields for directory/fsync do not themselves perform disk persistence. The bounded search did not find ordinary graph mutation handlers appending to the global event store; that coupling must be traced or connected, potentially using the existing transactional outbox.

Snapshot creation currently selects shard 0. Snapshot retrieval exposes the first 100 nodes/edges. Most significantly, [replay_to_point:21563](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:21563) takes the live shard graph write lock and replays into it. DSCO must present this as a mutation/reconstruction action, not invoke it as an innocent historical read. A pinned read binding needs the intended immutable snapshot path, retention and schema/source context explicitly connected.

## 4. Existing computation and execution facilities

[storage/streaming.rs:98](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/storage/streaming.rs:98) implements CDC deltas and handlers. The same module contains incremental collections, map/filter/reduce, windowed aggregation, streaming PageRank and connected components. [ReactiveQueryManager:780](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/storage/streaming.rs:780) supplies subscription/result callbacks. These are exported library facilities; the searched references did not establish mutation-handler wiring. Its `notify_change` currently republishes the last result rather than rerunning the query. Bind recomputation and change propagation deliberately.

`compute/graph.rs` supplies typed ports, tensor/composite types, effects, linearity, token flow, graph validation and schedulers. [ComputeRuntime:377](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/compute/runtime.rs:377) loads/executes graphs, holds tensors, emits events and supports runtime controls. [Compute routes:16238](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:16238) already expose graph creation/inspection/execution, pause/resume/step, tensors and events.

The storage bridge is a specific unfinished connection: [ComputeGraphBridge:2403](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/compute/graph.rs:2403) currently returns an empty graph when loading and returns success without persisting when saving. Likewise, [compute checkpoint:21062](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:21062) returns a checkpoint-shaped response while explicitly noting that runtime checkpointing is unimplemented. Neither diminishes the existing compute engine; both identify where operator bindings need real behavior.

The [HTTP graph constructor:20932](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:20932) currently builds default identity nodes and `Unit` ports rather than honoring the requested operation/port type. The inspected runtime's builtin path creates output tensor-reference tokens with zero-length storage; [step:887](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/compute/runtime.rs:887) returns `Ok(None)`. A separate [MlxExecutor:3479](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/bindings/mlx_backend.rs:3479) has builtin dispatch. A Lingo native-kernel binding must name the actual working executor and operation, not infer calculation support from a graph label or route response.

Existing [counterfactual evaluation:11093](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:11093) also supplies a concrete what-if path: it overrides a numeric evidence map and compares weighted-parent propagation, bounded to depth ten. This can be exposed as that particular calculation. It does not replace Lingo's nested typed field-and-argument scenarios, and should not be renamed to imply those semantics.

[TraceStore:1307](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/observability/trace.rs:1307) and [FlowRunner:2051](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/observability/trace.rs:2051) already model typed flows/steps, dependency graphs, execution, resume and cached output replay. They are used by pipeline code and trace APIs. The inspected store holds maps and broadcast state; its comments do not establish disk-backed restart recovery. DSCO should integrate those records and execution services with the selected persistence and receipt path.

## 5. Existing object, schema and source representation

[Schema field types:115](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/specialized/schema.rs:115) include primitives, collections, references, vectors and JSON. `dynamic_schema/versioning.rs` implements versioned schemas, parent versions, authors, history and rollback-as-new-version. `dynamic_schema/graph_bridge.rs` materializes typed data into graph objects. Their registry lifetimes and persistence should be mapped to class publication semantics rather than replaced with another disconnected type system.

[hydrate_from_source_full:1959](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/agents/ontology.rs:1959) explicitly performs SecDB-style hydration of actual source contents into blobs and graph relationships. This is a direct foundation for DSCO source navigation. The operator adds exact artifact/module binding and release context around that existing representation.

The current hydrator walks Rust `.rs` files and uses `syn` for parsed relationships. Reuse its blob/file/definition relationships for Lingo source; bind the Lua loader and module/export metadata explicitly. A source blob already in GraphSub is not automatically a registered executable Lingo package.

The separate [domain application catalog:1330](/Users/arthurcolle/Dsco/dsco-graphsub-complex/graphsub_domain_app.py:1330) contributes OpenOntology vocabulary. Its [query_instance:1521](/Users/arthurcolle/Dsco/dsco-graphsub-complex/graphsub_domain_app.py:1521) returns predefined rows according to query text. Catalog vocabulary and native execution remain distinct evidence; that file does not establish current deployed behavior.

## 6. Bind the right semantics at the operator boundary

The current [engine boundary:22](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/main.rs:22) rejects non-loopback HTTP binding and assigns public authentication/tenant isolation to the gateway. Existing client tool gates and server authorization remain complementary.

A few exposed-wire details still matter: [ordinary node reads:7224](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:7224) lack an object-revision/snapshot envelope; [Cypher parameters:17229](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:17229) are accepted but ignored; and [Cypher batches:17372](/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs:17372) explicitly disclaim general cross-statement atomicity. These qualify those routes, not all GraphSub internals.

The world protocol should bind its operations to the existing owner, then negotiate the resulting verified path:

| Operator mode | Existing candidates to bind | Acceptance before labeling the mode available |
|---|---|---|
| Live browse | Object/query APIs and schema registry | Authorized bounded reads with honest live-observation provenance |
| Pinned inspection | MVCC historical reads, event snapshots and source artifacts | Coherent retained data/schema/source context across reads and pagination |
| Transactional editing | MVCC/conditional commits, persistent store or SQLite graph lane | Whole-changeset atomicity, expected revisions, invariants and reconcilable durable receipts |
| Reactive workbench | Honker outbox, sequenced event store, CDC/incremental operators | Mutation coupling, durable replay, explicit gaps and coherent snapshot-to-stream handoff |
| Recoverable runs | FlowRunner/TraceStore, compute runtime, queue lifecycle | Durable checkpoints and effect receipts, ownership fencing and safe unknown-outcome reconciliation |

This is integration and semantic unification over substantial existing work. The next evidence should follow each selected operation from DSCO through its actual GraphSub implementation, then exercise concurrency, restart, conflicts and reconnect on that path. No blanket claim that GraphSub lacks these subsystems is warranted by the inspected public API limitations.
