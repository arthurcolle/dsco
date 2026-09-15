# DSCO operator implementation map

**September 10 workbench update:** [The local worksheet implementation](../lingo/WORKBENCH.md)
now retains an owned Lingo VM behind `lingo_session`, with shared terminal/MCP
read, explain, control, selection, save and close operations. Source-bound restore
reconstructs stored worlds and overrides without rerunning initializers. This
supersedes the one-shot-only host limitation in the September 9 audit below;
arbitrary expression consoles, remote conditional edits and durable workflow
resumption remain separate bindings.

Status: source audit, 2026-09-09, expanded from DSCO CLI into `/Users/arthurcolle/Dsco/dsco-graphsub-complex`: the Rust engine, web operator, platform services, domain application and context documents. The nested engine Git root is `dsco-graphsub-complex/dsco-graphsub`, HEAD `52ade087bd3d07797d3631b7d0b37ae54f07dd9b`; dirty source is the evidence here. The separate home-root `dsco-graphsub-complex` is a two-file fragment, not that checkout.

This maps foundations into [the operator architecture](../DSCO_GRAPHSUB_OPERATOR.md). [Lingo 0.1](../LINGO.md) documents the current language. The original source audit did not exercise services. The subsequent [live browse binding](LIVE_BROWSE.md) has now passed an isolated native-engine read and capability checks; [retained results](../../reports/lingo-operator-20260909/REVIEW.md) describe exactly what was tested. That proof does not establish the remaining snapshot, transaction or deployment guarantees. The narrow DSCO `graphsub` tool does **not** inventory the backend: existing GraphSub infrastructure must be reused before adding services.

The target assigns GraphSub authority over a versioned world, DSCO ownership of calculation and execution, and Lingo the programming interface. An operator context pins data, source, workspace, scenario and authority. Reading a value, editing a draft, committing data, evaluating a scenario and executing an external effect are distinct operations.

The labels below mean:

- **Current:** the named implementation or entry point exists in the cited source.
- **Verified partial:** useful behavior exists, but it does not satisfy the complete operator contract.
- **Required binding:** an operator connection or guarantee not established by this audit. This does not mean the underlying GraphSub facility is absent. Suggested module names are proposals, not callable APIs.

## Existing implementation and its boundary

| Area | Status and source evidence | Reuse and remaining boundary |
|---|---|---|
| Lingo language | **Current.** Objects, typed fields, calculations, dependencies, invalidation and callback scenarios in [runtime.lua](../../lingo/runtime.lua#L136). Inspection begins at [runtime.lua:261](../../lingo/runtime.lua#L261). | Reuse the semantic core. It currently owns an in-memory world, not a GraphSub-backed revisioned world. |
| Lingo host | **Verified partial.** [lingo.c:496](../../src/lingo.c#L496) creates a Lua state and [lingo.c:535](../../src/lingo.c#L535) closes it after the invocation. | Existing `run`, `eval` and `check` are bounded invocations. They do not establish a persistent expression-console session. |
| Tool authority | **Current.** [lingo.c:290](../../src/lingo.c#L290) invokes `tools_execute_for_tier()` with inherited tier. [runtime.lua:286](../../lingo/runtime.lua#L286) prohibits tool calls in calculations and scenarios. | Keep this gate on every operator effect path. A context must not retain authority after its principal or grants change. |
| DSCO GraphSub client | **Verified partial.** [graphsub_client.h:8](../../include/graphsub_client.h#L8), [graphsub_tools.c:278](../../src/graphsub_tools.c#L278). | Existing coordination actions expose only a subset. Bind the existing engine's object/query, schema, transaction, snapshot and event surfaces below; do not infer backend absence from this client. |
| Live Lingo operator binding | **Current browse implementation.** [operator.lua](../../lingo/operator.lua), [graphsub_operator.c](../../src/graphsub_operator.c), [runnable contract](LIVE_BROWSE.md). | Embedded module uses the same governed tool bridge for bounded native health/schema/list/read GETs. Live observations retain their scope and time; no pinned context, transaction or persistent derived-value claim. |
| Persistent buffers | **Current.** [buffer_store.h:6](../../include/buffer_store.h#L6) defines content revisions, expected-revision writes, retry deduplication and checked saves. [schema](../../include/buffer_store.h#L19). | Reuse for Lingo source and drafts. Local text buffers are not canonical GraphSub objects. Limits include 1 MiB content, 256 buffers per workspace and bounded reads. |
| External buffer views | **Current.** [buffer_view.h:6](../../include/buffer_view.h#L6). | Reuse edit/view/follow/TextEdit views. Multiple views share saved content. File-follow mode is not a graph-value subscription. |
| Native DSCO panels | **Verified partial.** [native_windows.h:18](../../include/native_windows.h#L18), [public contract](../../include/native_windows.h#L114). | Note, buffer and workflow panels, layout, focus, events and explicit refresh exist. IDs are session-local; there are at most 12 panels with 8,191-byte text. Typed browser, value inspector and scenario comparison bindings are required. Reported workflow status is not independent completion evidence. |
| Native editing | **Current.** [native_windows.h:72](../../include/native_windows.h#L72), [native_window_tool.c:123](../../src/native_window_tool.c#L123). | Reuse edit-generation tracking and expected-revision saves. The adapter releases the model mutex before gated I/O and preserves newer edits when a save finishes. |
| Terminal surfaces | **Current.** [surface_registry.h:5](../../include/surface_registry.h#L5). | Reuse owned Kitty surfaces, stable IDs and launch reconciliation. A surface, panel, buffer, object, evaluation and run each retain distinct identities. |
| UI notifications | **Verified partial.** [tui.h:1356](../../include/tui.h#L1356) defines `tui_event_subscribe`, unsubscribe and emit with bounded history. | Reuse in-process rendering notifications. This is not a durable GraphSub feed. Lingo [graph events](../../lingo/runtime.lua#L277) are likewise local diagnostics. |
| Execution receipts | **Current.** [execution_kernel.h:11](../../include/execution_kernel.h#L11). | Reuse attempt IDs, parent/session/trace linkage, principal, capabilities, input/result hashes and timestamps. The operator must represent `effect_unknown`, not classify it as safe to retry. |
| Command identity | **Verified partial.** [command_plane.h:8](../../include/command_plane.h#L8). | SQLite/WAL store and begin-new/replay/conflict outcomes exist. The header explicitly says command dispatch is not wired. This does not establish globally exactly-once effects. |
| Detached supervision | **Current.** [swarm_daemon.h:4](../../include/swarm_daemon.h#L4) describes independent run ownership, atomic snapshots and append-only events. [CLI entries](../../src/main.c#L2844). | Reuse the ownership pattern for runs that outlive the operator. Agent mailboxes, swarm supervision and Lingo workflow checkpoints remain distinct resources. |
| Recovery | **Verified partial.** [supervisor.h:7](../../include/supervisor.h#L7) handles host process restarts. [execution_recovery.h:7](../../include/execution_recovery.h#L7) reports receipt evidence. | Receipt recovery is explicitly read-only and never authorizes retries. Neither facility alone implements resumable Lingo workflows. |
| Existing workspace | **Current, different scope.** [workspace.h:7](../../include/workspace.h#L7). | Existing APIs manage skills, identity/doctrine documents and prompts. Define the operator workspace separately, then connect its identity to these facilities where appropriate. |

## Existing GraphSub foundations beyond the CLI

All links below address the corporate complex, not the home-root fragment. Source presence establishes reuse candidates; the selected transport and deployed behavior require verification.

| Foundation | Implemented source | Operator binding and evidence boundary |
|---|---|---|
| Versioned types | **Current:** [VersionedTypeRegistry](../../../dsco-graphsub-complex/dsco-graphsub/src/dynamic_schema/versioning.rs#L66), register/mutate, [version-preserving rollback and historical reads](../../../dsco-graphsub-complex/dsco-graphsub/src/dynamic_schema/versioning.rs#L220). [GraphBridge](../../../dsco-graphsub-complex/dsco-graphsub/src/dynamic_schema/graph_bridge.rs#L59) materializes typed entities/relations. | Bind schema inspection and class compatibility to these registries. Registry history is not by itself a published Lingo source manifest. |
| Transactions and storage | **Current:** [MVCCStore](../../../dsco-graphsub-complex/dsco-graphsub/src/txn/mvcc_store.rs#L90), [LSM-backed PersistentMVCCStore](../../../dsco-graphsub-complex/dsco-graphsub/src/txn/persistent_store.rs#L18), and [public transaction exports](../../../dsco-graphsub-complex/dsco-graphsub/src/lib.rs#L419). | Adapt timestamp reads, intents, commit/abort and conflict handling. Verify which store the chosen serving path owns; do not build another MVCC engine. |
| Events, snapshots and replay | **Current:** [EventStore append](../../../dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs#L376), [events-after and subscribe](../../../dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs#L555), [ReplayEngine](../../../dsco-graphsub-complex/dsco-graphsub/src/specialized/event_store.rs#L754), [snapshot/replay HTTP routes](../../../dsco-graphsub-complex/dsco-graphsub/src/api/handlers.rs#L16420). | Reuse these paths for history and watches. This particular EventStore uses memory buffers; a restart-durable cursor requires checking its persistence/serving integration. |
| Existing web operator | **Current:** [graph visualization](../../../dsco-graphsub-complex/graphsub-web/src/app/dashboard/page.tsx#L528), [Cypher editor/server query](../../../dsco-graphsub-complex/graphsub-web/src/app/dashboard/page.tsx#L1192), [node inspector](../../../dsco-graphsub-complex/graphsub-web/src/app/dashboard/page.tsx#L2144), path finder and exports in the same file. | Reuse navigation/query/inspection behavior and components. Bind to DSCO semantic contexts rather than create another detached dashboard. |
| Browser-local graph | **Current, separate mode:** [BrowserGraphSubRuntime](../../../dsco-graphsub-complex/graphsub-web/src/lib/graphsub-browser-runtime.ts#L146) stores nodes in a TypeScript Map and edges in an array, with local `snapshot()` at 177. | Keep its `browser-wasm` identity visible. Its copied snapshot is not a remote MVCC snapshot; it must not masquerade as the attached authoritative world. |
| Platform tasks and events | **Current:** [TaskManager](../../../dsco-graphsub-complex/graphsub-platform/gs_platform/tasks.py#L12), [EventBus](../../../dsco-graphsub-complex/graphsub-platform/gs_platform/events.py#L88), [atomic event/webhook enqueue](../../../dsco-graphsub-complex/graphsub-platform/gs_platform/store.py#L194), [task/event/SSE routes](../../../dsco-graphsub-complex/graphsub-platform/gs_platform/api.py#L178). | Reuse tenant task identity, recorded transitions and delivery plumbing. SSE fanout is bounded best-effort; exact resume/gap semantics and run fences need conformance. A lifecycle event alone does not prove an actual backup or effect occurred. |
| Operational ontology | **Current vocabulary/catalog:** [ObjectType, PropertyType, ActionType, Function, Interface, ObjectSet, Scenario, PermissionGrant](../../../dsco-graphsub-complex/graphsub_domain_app.py#L145), [served catalogs](../../../dsco-graphsub-complex/graphsub_domain_app.py#L2253). | Extend these existing concepts with Lingo bindings and version pins. A cataloged scenario/function is not proof that its execution semantics are connected. |
| Alternate native access | **Current:** [Python Graph wrapper](../../../dsco-graphsub-complex/dsco-graphsub/src/bindings/python.rs#L20) exposes node/edge access, payloads, ontology/entity builders and context windows; [module registration](../../../dsco-graphsub-complex/dsco-graphsub/src/bindings/python.rs#L537). | Native Rust library and Python paths are legitimate integration candidates alongside HTTP. Do not assume the coordination REST wrapper is the sole API. DSCO effects still pass its gate. |
| Computation | **Current:** [ComputeGraph](../../../dsco-graphsub-complex/dsco-graphsub/src/compute/graph.rs#L1185) with preceding typed ports, effects, token places and transitions. | Inspect existing scheduling/kernel boundaries before creating another worker graph. Its exact mapping to Lingo demand-driven value dependencies must be explicit. |

Targeted searches for Slang, SecView, LuaJIT, Lingo, diddles and source/workspace overlays across these source and document paths did not identify another named Lingo host. That is a bounded discovery result, not a claim that equivalent primitives or another checkout do not exist. The ontology, type history, MVCC, query UI and event services above are substantive existing foundations.

## Thin binding work, not replacement infrastructure

Proposed C modules get separate headers/Makefile entries and small dispatcher hooks under AGENTS.md. Names below are suggestions. Do not replace a backend registry, store, event log, task service or graph browser just to match these names.

| Binding | Scope and reuse |
|---|---|
| `operator_context.c` / `operator_session.c` | Bind the selected GraphSub connection and tenant to data/source/workspace/scenario/authority pins, console history and evaluation handles. Extend the existing Lingo host's lifetime deliberately; persist reconstruction metadata, not Lua stacks. |
| `lingo_graphsub.c` | Adapt existing graph/schema/MVCC APIs into Lingo object reads, checked writes and dependency evidence. Negotiate a transport profile and verify the actual serving store. Controlled snapshot reads differ from arbitrary tool effects. |
| `operator_drafts.c` | Connect existing buffer revisions and native editor generations to typed GraphSub changesets, validation and conditional commit. Keep save, commit, scenario promotion and execution separate. |
| `operator_views.c` / `operator_watch.c` | Bind existing native/web views to semantic identities and adapt existing GraphSub/platform event feeds into UI notifications. Add only the missing cursor, ownership, resnapshot and freshness handling. |
| `operator_commands.c` | Connect DSCO command identity, capability admission and receipts to existing GraphSub/platform task records. Preserve uncertain outcomes and stale-attempt evidence. Never call tool leaves directly. |
| `operator_runs.c` | Bind named Lingo checkpoints to existing detached supervision, task transitions and event records. Identify canonical run ownership before adding storage; process restart never implies effect replay. |

The next task is to trace and exercise these bindings against the actual chosen GraphSub profile. Any extension must name the missing guarantee and the existing module being extended. A client adapter cannot manufacture server-side consistency, but the existence of several backend interfaces also cannot be reduced to the smallest one.

## Staged end-to-end acceptance slices

Each slice requires a working operator path and observed state at both ends. Fixtures can establish semantics; deployment claims require exercising the actual configured backend. Avoid expanding UI scope before the underlying state transitions work.

1. **Pinned read and inspect.** Connect to a known GraphSub test tenant, resolve one schema and object, pin data/source revisions and calculate one field. The inspector shows exact inputs, dependency edges and evaluation identity. Change the remote object; the pinned context remains stable until explicitly advanced. Unauthorized or unavailable objects produce distinct errors. Establishes the first usable object browser and inspector.

2. **Draft, conflict and commit.** Open the same object in two contexts. Edit without changing the canonical object; validate without committing. Commit one draft, then demonstrate a revision conflict for the other without overwriting either version. Retrying the same successful request returns its recorded outcome. Changing the payload under the same request identity fails. Reopen DSCO and resolve the committed revision.

3. **Scenario comparison.** Compare baseline and override views using identical data/source pins and arguments. Prove conditional dependencies, overriding a calculated node, nested restoration and error unwinding. Canonical GraphSub state and effect receipts remain unchanged. A saved scenario reloads its explicit overlay and baseline identity; it never implicitly commits its values.

4. **Persistent console and source publication.** Enter successive expressions in one owned session, inspect previous evaluations and edit source in a revisioned buffer. Publish a new validated source generation while an old context remains pinned to its prior generation. Restart DSCO and reconstruct named contexts/history from metadata. Runtime heaps, arbitrary closures and execution stacks are not serialized.

5. **Watch and reconnect.** Watch a changing value in follow mode while retaining a pinned comparison view. Exercise duplicate delivery, disconnect, reconnect and a cursor older than retention. Show last-good evaluation identity and freshness explicitly. Resnapshot after a gap; never display unverified cached state as current. Closing one view releases only its own subscription ownership.

6. **Command and run supervision.** Execute a reversible local action through the operator, observe inherited authority and linked receipts, and independently verify its postcondition. Demonstrate a denied action and an interrupted attempt with uncertain outcome. Recovery exposes evidence without blindly replaying. Detach a bounded run, close DSCO, reconnect and collect its verified terminal result. Cancellation must report what stopped and what already happened.

7. **One coherent operator workspace.** Restore browser selection, source buffer, inspector, baseline/scenario comparison and run monitor after restart. All views show their context and freshness. Closing a panel leaves its object, source and detached run intact. Human commands, Lingo scripts and agent-issued operations reach the same governed dispatcher and produce comparable evidence.

For each slice retain source/data identities, command/evaluation/run IDs, expected transitions, observed results and residual limitations. Run applicable live capability-gate checks for new dispatch paths. The milestone is a reproducible operator interaction with authoritative state, not the presence of a panel or an API-shaped stub.
