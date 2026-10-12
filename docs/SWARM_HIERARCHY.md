# Cross-provider swarm hierarchies

The `swarm` tool supports `action:"hierarchy"`. A hierarchy is a rooted,
parent-first dependency tree. Each node can use native DSCO with an explicit
provider/model or a registered external harness such as `interop:codex` or
`interop:opencode`. Workers use the existing swarm lifecycle, receipts, events,
and native/TUI progress projection.

## Start and drive a tree

```json
{
  "action": "hierarchy",
  "operation": "start",
  "name": "cross-provider-study",
  "max_concurrency": 3,
  "max_worker_turns": 3,
  "nodes": [
    {"id": "planner", "task": "Produce a concise investigation plan.", "executor": "dsco"},
    {"id": "implementation", "parent": "planner", "task": "Investigate implementation using the parent plan.", "executor": "interop:codex"},
    {"id": "review", "parent": "planner", "task": "Review the plan and identify gaps.", "executor": "interop:opencode"},
    {"id": "validation", "parent": "implementation", "task": "Validate the implementation findings.", "executor": "dsco"}
  ]
}
```

Start returns a unique `hierarchy_id` and submits the root. The controller pumps
scheduling when the caller advances it:

```json
{"action":"hierarchy","operation":"advance","hierarchy_id":"<returned handle>","timeout":1}
```

Repeat `advance` until `complete:true`. Each call collects one wave for at most
the requested observation window (1–20 seconds), reconciles terminal results,
and submits the next ready wave. A wide level is split into bounded waves.
Hierarchy waves hold completed group/worker slots until a complete result receipt
is collected, so competing groups cannot recycle an unconsumed dependency result.
Observation deadlines preserve workers. An incomplete or mismatched receipt
prevents dependent work from starting and keeps the handle available for
reconciliation.

Nodes can appear in any input order. Omitted/null `parent` identifies the single
root. IDs contain 1–48 ASCII letters, digits, underscores, hyphens, or periods.
Duplicate IDs/JSON keys, missing parents, cycles, embedded NULs, malformed
options, unknown adapters, and unsupported fields reject before dispatch.
`dry_run:true` validates and returns the tree without submitting workers.

## Parent results and routes

A child starts only after its parent finishes successfully. Its prompt includes
the logical node ID, parent ID, depth, original task, and a JSON-quoted parent
result marked as data from another worker. This is fixed task delegation;
parents do not dynamically add nodes and there is no implicit bottom-up reduce.
The final receipt contains the status of every node.

Parent context is bounded to 2048 bytes per node. Text is cut at a UTF-8 boundary.
External completion receipts are unwrapped to their output; binary output uses
the exact receipt's base64 field with `encoding:"base64"`. Context excerpts carry
explicit truncation metadata. Full output stays in normal swarm artifacts;
retain the hierarchy receipt and its references before ending the controller.

Top-level `provider`, `executor`, and `model` supply defaults. A node with an
explicit provider or executor replaces the inherited route as a pair; this lets
an external child override a native default without inheriting that provider.
Per-node models override the default model. `provider` plus an external executor
on the same selected route is rejected. Native routes support `executor:"dsco"`;
external routes require a registered `interop:<adapter>`. Provider/model defaults
are unchanged by this feature.

`cwd` is resolved and checked once before dispatch. `max_worker_turns` and
`max_tokens` reach native worker configuration through normal instance setup.
The native OpenAI API fixture verifies its wire token ceiling. The existing
Codex subscription adapter does not emit a wire token ceiling; the option does
not establish one for that lane. `run_timeout_ms` bounds external adapters,
not native provider wall-clock execution. Existing native runtime limits apply.

## Observe and cancel a branch

```json
{"action":"hierarchy","operation":"status","hierarchy_id":"<returned handle>"}
{"action":"hierarchy","operation":"status","hierarchy_id":"<returned handle>","node_id":"planner"}
{"action":"hierarchy","operation":"abort","hierarchy_id":"<returned handle>","node_id":"implementation"}
```

Status returns the controller's last reconciled snapshot and a readable tree.
Select `node_id` to include that node's bounded output and artifact directory.
Live worker output still appears through normal swarm/native/TUI progress.
Worker prompts start with a hierarchy label so those cards identify their node.

Abort selects the node and its descendants; omit `node_id` to select the root.
Queued nodes become `cancelled`. Active nodes are killed through the governed
`agent` tool with both `run_id` and `group_generation`, then become
`cancel_pending` until collection reconciles them. Completed work remains
available. Other branches continue. A failed parent blocks its descendants
while successful sibling branches can still run.

Receipts use `dsco.swarm_hierarchy.v1`, report `outcome`, `complete`, logical
depth/width, wave/concurrency, and each node's dependency state and execution
reference. Outcomes are `running`, `success`, `failed`, `cancelled`, `planned`,
or `reconciliation_required`. A partial dispatch is explicitly cleaned up by
group identity; a denied cleanup retains the active handle for control.

## Ownership and bounds

All spawn, collect, and cancellation calls re-enter `tools_execute_for_tier`
with the caller's tier. Logical parentage grants no worker authority. Normal
credential isolation and execution denials remain in force. All OS workers
are owned by the central DSCO process; logical tree depth is reported separately
from worker process depth.

This version supports 64 nodes, eight process-owned controllers, and at most
32 workers per wave, further restricted by `DSCO_SWARM_MAX_CHILDREN` and
`max_concurrency`. Logical depth respects `DSCO_SWARM_MAX_DEPTH`. Completed
controller slots may be reused after all eight slots have been occupied; old
handles then expire. Restart does not resume the dependency controller. Normal
completed worker artifacts remain durable. Store returned receipts in the
experiment driver if cross-run analysis is needed.

Dollar budgets are not accepted as hierarchy fields. External harness billing
is unknown, and a per-wave estimate cannot establish a hierarchy-wide spending
ceiling. The offline regression fixtures make no paid provider requests.

## Verification

```sh
make test_swarm_hierarchy test_swarm_hierarchy_mcp
make test-gate-claims test-agent-interop
```

Unit tests cover validation, route inheritance, dependency ordering, cancellation,
failure isolation, slot reuse, bounded Unicode/binary context, partial dispatch,
and a 64-node tree in eight-worker waves. The MCP fixture runs real DSCO child
processes against a loopback provider and fake Codex/OpenCode binaries. It proves
three-level mixed execution, native API token forwarding, group/worker recycling,
failure isolation, branch cancellation, and execution denial. It writes JSON
receipts, fixture calls, native requests, a transcript, and a binary hash under
`build/swarm-hierarchy-mcp/`.
