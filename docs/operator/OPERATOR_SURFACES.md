# The DSCO operator workbench

**Target interaction definition 1.0.** These surfaces specify the intended SecView-like product. They extend the current buffer, window, Lingo and execution infrastructure; they are not descriptions of a completed UI. See [current implementation evidence](IMPLEMENTATION_MAP.md).

## 1. One context-aware workspace

The operator starts by attaching to a named connection and opening a workspace. A layout may be restored, but credentials, current authorization and snapshot validity are rechecked. The operator never silently resumes a command merely because its old pane reappears.

A practical default layout is:

```text
┌ Connection · tenant/realm · workspace · snapshot · source · scenario · freshness ┐
│ Browser / search       │ Main view: object, source, table, graph or comparison  │
│ projects               │                                                        │
│ objects and classes    │ Selected identity and context breadcrumb              │
│ source packages        │ Typed fields / calculated values / returned results   │
│ saved scenarios        │                                                        │
│ runs and artifacts     ├────────────────────────────────────────────────────────┤
│                        │ Inspector: dependencies, provenance, changes, errors  │
├────────────────────────┴────────────────────────────────────────────────────────┤
│ Lingo console / output / event trace / active commands                           │
└ Draft count · active runs · watches · resource use · unresolved effects ──────────┘
```

This layout is a default, not a requirement for a mouse, Kitty graphics or a specific terminal. A compact text client exposes the same semantic operations sequentially. Screen readers and headless clients receive structured state, not information encoded only in color or position.

The context bar always identifies connection/tenant, data snapshot mode, writable workspace, source manifest, scenario and freshness. Summarized identifiers expand to their full records. Changing a source manifest or scenario is visually distinguishable from changing an object's value.

A live browse attachment can precede a pinned context. It shows its authenticated scope, observed revision/time and `live / not snapshot-pinned`; source and scenario indicators remain unbound until selected. Creating a reproducible evaluation context explicitly resolves the snapshot, source manifest and observations. The browser does not fabricate those pins merely to populate the header.

Selection is local navigation state. Context is semantic state. Following a link normally preserves context. Opening a link into a different context displays that transition and preserves a back link to the original context.

## 2. Views are bindings, not copies of truth

A `ViewSpec` binds a kind, semantic target, context ID, projection, renderer, follow policy and demand policy. The session gives it a view ID and any required subscription/evaluation handles. A renderer consumes authorized typed results; it cannot independently issue ungoverned network requests or tools.

Multiple views may share a buffer or cached evaluation while retaining independent selections and lifetimes. Closing a view releases its subscriptions and evaluation demand, not the object or detached run. A “close workspace” action reports retained drafts and active attachments according to their declared retention policy; it does not turn them into implicit commits or cancellation.

Views support open, focus, split, pin, follow, clone with another context, rebind, refresh, copy reference, export and close. Rebind creates a new semantic binding and cache identity. Refresh on a pinned view revalidates/refetches the same snapshot, or reports expiry. “Follow latest” explicitly creates newer contexts.

## 3. Object browser and search

The browser can show projects, namespaces, class instances, object references, source packages, workspaces, saved scenarios, runs and artifacts. It distinguishes effective roots from workspace-only objects. Each row carries stable identity, class/version, display name, origin, revision and state badges.

Search spans object names/fields permitted by policy, class/value definitions, source, buffers, run/receipt IDs and transaction history. Each search domain declares its index/watermark and completeness. Results remain navigable references. A text match in a log is not presented as an authoritative object.

Queries expose filters, projections, sort and pagination. “All results” requires complete pagination at one snapshot. Changing a filter creates a new query/cursor. The interface indicates a truncated or stale result set rather than silently using it as a complete collection.

Selecting a reference opens it with the original snapshot where possible. A tombstone opens deletion/history information. A forbidden reference displays the authorized unavailable state without leaking hidden identity details.

## 4. Object and value inspectors

The object header shows stable identity, display name, class/schema, origin root, exact revision, snapshot, workspace provenance and source bindings. Tabs or equivalent commands expose fields, references, history, source, validation and related runs/artifacts.

Stored-field rows show type, value, unit, validation and draft/base differences. Derived-field rows show signature, implementation identity, evaluation state, result and context. A derived field does not automatically run just because its row is visible; the view's bounded demand policy controls evaluation.

The value inspector supports:

| Action | Meaning |
|---|---|
| Inspect | Read metadata and retained results without calculating |
| Evaluate | Request this exact address under this exact context |
| Explain inputs | Show actual dependencies and their revisions |
| Show dependents | Show known/materialized consumers; label incomplete discovery |
| Open source | Open the exact artifact and export used by the selected evaluation |
| Show arguments | Display canonical typed arguments; changing them opens another address |
| Trace | Observe selected evaluation events with provenance and limits |
| Override | Create a typed scenario override, never a stored edit |
| Evict local cache | Remove retained local materialization; preserve historical evidence |
| Copy/export result | Include value state, units, context and evaluation identity |

Known dependents are not the set of every function that might someday read this value. Static potential dependencies and realized runtime dependencies have separate labels. Cache eviction and logical invalidation are distinct; ordinary refresh cannot rewrite stored input revisions.

## 5. Value display states

Computation, freshness and authority are separate axes. A result may be a successful historical calculation while its following view is stale.

| Axis | States | Display rule |
|---|---|---|
| Computation | unread, queued, running, succeeded, failed, cancelled | Never show a failed calculation as a new valid result |
| Context freshness | pinned, current-at-watermark, invalidated, stale, resyncing, expired | Preserve the snapshot/watermark supporting the claim |
| Availability | available, disconnected, missing, tombstoned, redacted, source-unavailable | Redaction removes prohibited content, not just its label |
| State layer | committed, draft, hypothetical | Always visible in editing and comparison views |
| Action outcome | proposed, admitted, running, succeeded, failed, cancelled, effect-unknown | An unknown effect is neither safe failure nor success |

Last-good content may remain visible with its original evaluation/context and a clear stale/historical label, except where authorization requires redaction. Loading spinners do not erase useful provenance. A changed context invalidates the display's claim of currentness even when the scalar happens to be numerically equal.

## 6. Source editor and expression console

Buffers have explicit kinds:

- **Object source:** editable Lingo text targeting a GraphSub workspace source artifact.
- **Local file:** text targeting a specific local path, subject to normal filesystem capabilities.
- **Scratch:** retained or ephemeral text with no implicit persistence destination.
- **Output:** result/error/trace text with immutable originating evaluation or command references.
- **Object draft:** typed edits against base object revisions; serialized text is only one representation.

The current four historical buffer concepts remain recognizable; typed object drafts are an explicit extension. The header displays kind, target, base revision, dirty state, local revision and active source/context binding. Writing a local file does not publish a GraphSub source artifact. Saving a GraphSub source artifact does not promote a release.

The expression console has a captured context for every cell. A cell records exact source digest, arguments, evaluation mode, results, diagnostics, duration and resource use. Re-running an old cell defaults to its recorded context, with an explicit action to run against current state. History can be exported as a reproducible script/context bundle subject to retention and permissions.

Evaluation modes are named:

| Mode | Allowed work |
|---|---|
| Value expression | Pure value reads/calculations under a pinned context |
| Workspace script | Local draft/source operations and explicit world operations under admission |
| Command script | Explicit real tool/model/workflow actions through DSCO |

Syntax highlighting is insufficient to establish mode safety. Host libraries and admission enforce the boundaries. Evaluating unsaved source freezes the buffer into a temporary content-addressed overlay, resolves a manifest containing it, and creates a new context. The resulting evidence names the digest, manifest and draft status. Existing views and runs remain on their prior contexts; saving a buffer is not hot reload.

Errors link to exact source, relevant values, arguments and a redacted stack. Full instruction-level debugging is a future runtime facility; value tracing and source-linked failures are required before that. A debugger pause cannot keep an external action lease alive indefinitely or authorize effects from an old checkpoint.

## 7. Scenario and comparison workspace

A scenario panel shows its baseline snapshot, source manifest, parent scenario, typed overrides, author, digest and saved/draft state. Each override has a full value address, previous value where available, replacement, type and reason. Overrides of derived nodes explain that their lower dependencies are suspended.

The comparison view has aligned targets with baseline/candidate results, units, validity, context dimensions, differences, resource use and dependency changes. Invalid results appear as invalid results. “Why different?” follows changed inputs, branch choices, code or overrides through recorded dependencies; it distinguishes explained differences from unknown effects of nondeterministic observations.

Actions include create from selected value, add/remove override, nest, save definition, evaluate selection, compare, export assumptions, and prepare promotion. Saving retains a hypothetical definition. Preparing promotion selects eligible stored changes into a draft; commit is a subsequent version-checked operation. Real capabilities are never modified by a hypothetical policy override.

## 8. Changes, conflicts and releases

The changes view groups local drafts by destination workspace and base revision. It shows added/modified/deleted objects, alias/reference changes, source changes, schema migration requests, affected values and validation failures. Generated code is inspectable alongside human edits.

The commit action identifies scope, request digest, expected revisions and required policy. Preexisting authorization is honored; the product does not introduce a confirmation prompt for every reversible edit. Where a real policy requires review, the review binds the exact changeset digest and becomes stale if that digest changes.

A conflict view presents base, ours and current with constraint-level failures. An automatic merge can propose a new draft only under a declared merge policy; it cannot silently commit. A commit timeout displays pending reconciliation and queries status using the same idempotency identity.

Release views separate source artifact save, package build, test execution, baseline acceptance, publication, binding promotion and external deployment. Each transition has its own receipt. Historical runs keep their pinned source, even after the preferred release binding moves.

## 9. Runs, agents, receipts and budgets

The run supervisor shows workflow definition/manifest, input snapshot, owner/lease generation, current named steps, waits, attempts, artifacts, checks, approvals when required, budgets and unresolved effects. Logical agent identity, operating system process, tool executor and provider model are separate fields.

“Completed” is derived from required terminal criteria and accepted receipts, not from an agent writing a panel label. The user can open a tool attempt, inspect its admitted authority and request/result hashes, then follow authorized content/evidence references. Monetary cost is shown with currency, source, timestamp and estimated versus observed status.

Actions include inspect, follow, pause future dispatch, request cancellation, reconcile, resume a permitted checkpoint, propose retry and open artifacts. Retry displays adapter safety classification and unresolved prior outcomes. It cannot create a fresh action identity merely to evade an idempotency conflict.

A stopped worker with an uncertain external action remains visible. A late receipt can add evidence without advancing a newer run generation. Cancellation may be pending while an action is still in flight. Pausing a view or closing the client does not cancel durable work.

## 10. Event and dependency tools

The dependency view navigates value nodes, typed read edges, query/existence dependencies, overrides and source bindings. It pages large neighborhoods and declares traversal completeness. Cycles show an actual path. Expanded nodes are scoped to an evaluation/context, preventing a misleading graph assembled from incompatible snapshots.

The trace console filters by object, field, arguments, evaluation, command, actor or event kind. It links read, calculate, invalidate, override, commit, dispatch and reconcile events to their provenance. Break-on-value-event is a target diagnostic mode; exact breakpoints require runtime support and authorization, and do not imply the current Lingo host already implements a debugger.

Live UI events are presentation notifications. Durable world cursors and execution journal positions are separately identified. A local event bus cannot repair a missed backend commit. Queues and traces have bounded retention, cursor-gap behavior and explicit dropped/coalesced counts.

## 11. Semantic command interface

The [operation catalog](contracts/operations.json) is the shared naming layer. UI controls, CLI, MCP and Lingo bindings refer to these IDs. Arguments are typed references and context IDs, not scraped panel text.

Command discovery returns schema, description, required features, minimum capabilities, effect class and relevant current availability. Unavailable commands explain the missing feature or authorization and link to read-only diagnostics. A command palette does not offer a fake “commit succeeded” path when the backend only supports basic writes.

The proposed headless shape is `dsco operator <operation> ...`; it is **not implemented by this definition**. Current `dsco lingo run/eval/check` remains the executable entry point documented in [LINGO.md](../LINGO.md). A future CLI renderer prints the same structured records and exit dispositions consumed by the TUI.

## 12. A complete operator journey

1. Open the `delivery` project workspace. The context bar shows pinned shared data, local write root and source manifest.
2. Search for a release task and open `Readiness`. The result is blocked by an artifact verification value.
3. Follow that dependency to the artifact, then to its checker source. The source editor opens the exact version used.
4. Fork the source into the workspace, edit it, run fixtures and inspect the candidate evaluation under an explicitly changed manifest.
5. Open a scenario for a different model/worker allocation using the same input observations. Compare cost, readiness and acceptance evidence.
6. Prepare selected stored changes. Review the draft and commit conditionally; resolve any concurrent conflict.
7. Launch the authorized workflow. Follow actual attempts and receipts, then open the verified artifact.
8. Another operator's follow view moves to the committed snapshot. A disconnected view instead says stale and resynchronizes on reconnect.
9. Close the panes, reopen the session, and recover the same saved objects, source, scenario definitions and run identities.

Every transition is navigable and scriptable. The user can move from an observed number to the objects, source, assumptions and real operations that produced it.
