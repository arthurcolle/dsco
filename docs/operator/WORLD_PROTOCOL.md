# World protocol for DSCO and GraphSub

**Target binding contract 1.0.** This defines the common semantics through which DSCO composes GraphSub's existing storage, schema, compute, event and trace facilities. It does not rename or replace those implementations. [The adapter review](GRAPHSUB_ADAPTER_REVIEW.md) maps concrete implementations and routes to these requirements and identifies specific wiring or semantic differences. [The main definition](../DSCO_GRAPHSUB_OPERATOR.md) establishes product ownership.

## 1. Protocol boundary and feature negotiation

DSCO exposes semantic operator operations. Its world adapter translates them to supported GraphSub services. Native engine endpoints, authenticated gateway routes, operator RPC and MCP tool names are different namespaces. An operation ID such as `object.read` does not imply an HTTP URL `/object/read` already exists.

Every connection handshake returns protocol version, backend instance/generation, authenticated tenant/realm/principal, authorization epoch, effective durability mode, feature flags, limits, retention, schema catalog generation and a diagnostics receipt. Credentials are supplied out of band. The server derives identity from authentication; request fields are checked against that identity.

Negotiated features describe demonstrated semantics, not merely the presence of a route:

| Profile | Required features | What DSCO may offer |
|---|---|---|
| `browse` | Authenticated scoped reads, typed identity mapping, pagination with declared guarantees | Live object browsing with explicit non-snapshot provenance |
| `snapshot` | Stable snapshots, revisioned reads, immutable schema/source resolution, snapshot-bound queries | Reproducible pinned inspection and pure evaluation |
| `transaction` | Snapshot profile plus atomic conditional commit, read/predicate conflict checks, durable commit status and commit outbox | Shared workspaces, changesets and conflict-safe commits |
| `reactive` | Transaction profile plus durable cursors, atomic snapshot/stream handoff, explicit gaps and replay | Correct resumable follow views and invalidation |
| `workflow` | Transaction profile plus fenced run leases, durable state transitions, action intents and reconciliation records | Durable workflow scheduling; external adapter guarantees still vary |

Profiles can be unavailable independently: workflow scheduling does not inherently require a live UI stream. No profile substitutes for target-specific authorization or durability evidence. A volatile backend may demonstrate transaction isolation but cannot advertise durable commits or durable workflows.

A connection to a basic engine can be useful in `browse` mode. DSCO labels it correctly and disables dependent commands. Polling a basic endpoint does not upgrade it to `snapshot` or `reactive`.

Attachment first yields an authenticated session and a live `BrowseScope`: connection generation, tenant/realm, visibility epoch and bounded root/catalog selections. This scope does not claim a coherent snapshot. `object.resolve/read/query` and `source.resolve` may take this scope without a `WorldContext`; their responses identify live observation times/revisions and their declared consistency. Exact source artifacts can be resolved from this authenticated catalog before a full context exists. `context.open` then combines a snapshot, roots, resolved manifest and observations into a `WorldContext`. Calculated values, scenarios and reproducibility require that pinned context. A live browser must never invent a snapshot token to fit its UI model.

## 2. Requests, responses and errors

The logical request envelope contains `protocol`, `request_id`, `session_id`, semantic `operation`, optional immutable `context_id`, arguments and an idempotency key where required. Live browsing uses an authenticated `browse_scope_id` instead of a context. The two selectors are mutually exclusive; neither grants authority. The authenticated transport supplies principal and connection identity. Requests that name context must resolve that exact context before doing work.

Every response contains request correlation, operation, status, backend generation, authoritative provenance where applicable and either a typed payload or structured error. Mutating operations distinguish **rejected**, **committed**, **pending** and **outcome_unknown**. A generic network failure never stands in for a transaction outcome.

Error records contain:

- Stable code and operator-readable message.
- Origin: local validation, DSCO admission, world service, evaluator, adapter or external target.
- Request/command/run/evaluation references as appropriate.
- Retry disposition: `never`, `same_request`, `new_context`, `after_reconciliation`, `after_authorization`, or `after_backoff`.
- Structured details such as conflict references, missing source, failed constraint or cursor reset token.
- Redacted source locations and trace references where permitted.

Required codes include `UNSUPPORTED_FEATURE`, `UNAUTHORIZED`, `TENANT_MISMATCH`, `NOT_FOUND`, `TOMBSTONED`, `SCHEMA_MISMATCH`, `SNAPSHOT_EXPIRED`, `SOURCE_UNAVAILABLE`, `VALIDATION_FAILED`, `CONFLICT`, `IDEMPOTENCY_CONFLICT`, `OUTCOME_UNKNOWN`, `LEASE_EXPIRED`, `STALE_FENCE`, `CURSOR_GAP`, `BACKPRESSURE`, `CALCULATION_FAILED`, `DEPENDENCY_CYCLE`, `BUDGET_EXHAUSTED` and `CANCELLED`.

Neither HTTP status nor a success-shaped JSON body alone establishes a successful semantic operation. The adapter validates both the transport result and the typed contract.

## 3. Identity and serialization

An object identity is `(tenant, realm, object_id)`. A revisioned reference adds `revision`. All components are opaque strings except documented display names. An object ID never gets reassigned after deletion. Cross-realm references are explicit and are not transactionally dereferenced by default.

A value address adds `field` and canonical typed arguments. An evaluation ID identifies an attempt, not a reusable object identity. Buffer, panel, session, subscription, command, commit, run, step and attempt IDs belong to separate kinds. API validation rejects accidental interchange.

The value address is context-independent. An evaluated node is `(context_id, value_address)`; separate attempts against the same node have distinct evaluation IDs. This distinction supports comparing the same field across snapshots or scenarios without conflating their caches.

Storage serializes strings, booleans, null, finite numbers, exact integers, lists, records and tagged domain scalars/references. Integers beyond Lua's exact numeric range are decimal strings in a tagged type. Decimal quantities use coefficient+scale+unit; timestamps distinguish recorded time from effective/source time. Content digests are tagged by algorithm. Missing, null, unavailable, redacted and failed are distinct states.

Canonical argument hashing sorts record keys, preserves list order, includes type tags, normalizes representations under a versioned canonicalization profile and rejects non-finite values, functions and cyclic data. String normalization is not silently applied. Native floating-point calculations declare their numeric profile; a source hash alone is not a portability guarantee.

The structural [record schemas](contracts/records.schema.json) use [JSON Schema 2020-12](https://json-schema.org/draft/2020-12). Complex value schemas and canonicalization are versioned semantic contracts; a schema-valid record can still be unauthorized or inconsistent.

## 4. Object and source resolution

`object.resolve` takes a logical binding, class constraint and context. It returns the winning stable identity/revision, origin root, resolution token and permitted shadowing diagnostics. `object.read` takes an exact reference or an object identity under a snapshot; projection never changes the referenced revision.

Ordered roots resolve whole bindings. No field-level merge occurs across roots. An explicit tombstone in a higher workspace hides an inherited binding. A forbidden higher binding yields a permission-aware failure rather than silently returning a lower binding. Authorization must not reveal names of hidden candidates in diagnostics.

Stored references resolve by ID, not by display name. Name lookup inside a calculation creates a binding-resolution dependency. A failed existence test creates an existence dependency so a later creation can invalidate a calculation that handled the absence.

Source resolution is separate. Development roots resolve names to artifacts; execution pins their complete content manifest. A manifest identifies class/interface versions, exported functions, native kernel versions, standard library version and compatible runtime ABI. Missing source fails explicitly. A running evaluation never follows a moving `latest` binding.

Evaluating an unsaved source buffer materializes a temporary content-addressed source overlay, resolves a manifest containing it, and creates a new context. The buffer revision and artifact digest are evidence, not mutable import locations. Existing views and runs keep their original contexts. Saving the buffer does not hot-reload them.

## 5. Snapshots and contexts

`snapshot.open` returns a token identifying a coherent committed state, schema/source catalog generation, watermark, retention lease and transaction domain. Every subsequent read/query under that token sees that state or fails `SNAPSHOT_EXPIRED`. It must not fall forward to a newer revision without telling the caller.

A `WorldContext` binds a snapshot, ordered data roots, source manifest, optional workspace/draft/scenario digests, principal visibility scope and authorization epoch. Its content-derived `context_id` is immutable. Session changes produce new contexts. The semantic digest excludes presentation state such as pane size and selection.

An initial context belongs to one transaction domain. Imported root snapshots are immutable references included in the context. Federated contexts are read-only snapshot vectors with explicitly weaker cross-realm coherence until a distributed transaction protocol is implemented and verified.

Snapshot leases have declared duration and renewal behavior. Renewal cannot alter snapshot contents. Releasing one client's handle does not invalidate another holder's independent lease. Historical reads are subject to authorization, retention and schema/source availability; pinning does not grant permanent access or force infinite retention.

Lease release and watch closure require verified handle ownership or explicitly delegated management authority. A caller-supplied owner ID is a selector, not proof of ownership. Cleanup of one's own retained handles remains permitted after read visibility is revoked; it returns cleanup status only and does not restore access to protected contents.

## 6. Queries and predicate dependencies

`object.query` uses a structured typed query or an actually bound parameterized query. Concatenating user values into raw Cypher is not an implementation of binding. Current native route behavior must be checked through the adapter capability test.

The read binding validates the parsed query and registered procedure effects. It rejects writes such as `CREATE`, `MERGE`, `SET`, `DELETE`, schema mutation and effectful procedure calls, even when perfectly parameterized. Keyword filtering is insufficient. Mutations belong to changeset admission; external-effect procedures belong to command admission. Arbitrary Cypher console execution cannot be reclassified as a read solely because it entered through a query box.

Queries specify roots, class/schema constraint, predicate, projection, deterministic sort with identity tie-breaker, page size and snapshot. Pagination cursors bind query digest, snapshot, authorization scope and sort position. Reusing a cursor with different arguments fails.

The response contains rows, snapshot, next cursor, completeness status and a query dependency token. Recording only returned object IDs is insufficient: an insertion or field change can create a new match. The backend supplies predicate/index revisions or conservatively invalidates the relevant class/collection. A query that cannot provide sound dependency tracking can be displayed for browsing but cannot silently become a cache-safe calculation input.

Query bounds cover rows, traversal depth, execution time, intermediate work and returned bytes. Partial results carry a continuation or explicit partial status. A timed-out prefix is not labeled a complete portfolio or full dependency set.

## 7. Workspaces, drafts and atomic commits

A workspace is a writable root over pinned parent roots. Editing an inherited object creates a draft/workspace revision preserving its logical object identity and origin relationship. It does not mutate the parent root. Effective-union browsing and workspace-only browsing are distinct query scopes.

A changeset contains creates, stored-field patches, links/unlinks, tombstones, alias changes and source-binding changes as supported. It identifies workspace head, exact expected object revisions, expected binding/index revisions, validators, source manifest and the evaluation read set used to justify the write. Protected fields such as run completion cannot be set through arbitrary object patches.

`changeset.validate` checks syntax, types, references, immutable fields, invariants and provisional authorization without committing. It returns a preview with a digest and read-set certificate. Commit repeats authoritative validation and authorization; a preview is not a lasting permission grant.

`changeset.commit` requires an idempotency key bound to tenant, transaction domain and request digest. Under a serializable conditional transaction, the backend:

1. Looks up the key; same request returns its recorded result, different request returns `IDEMPOTENCY_CONFLICT`.
2. Validates workspace head, read set, predicate tokens, expected revisions and authorization.
3. Evaluates declared commit invariants without external effects.
4. Atomically installs all object revisions, reference/index updates, workspace head and commit receipt.
5. Writes the committed change batch to an outbox in the same transaction.
6. Acknowledges only at the advertised durable commit level.

A rejected transaction publishes no subset of its writes. “All statements parsed before execution” and “one process lock” do not establish this guarantee. A server-generated graph mutation counter is not an MVCC snapshot or conditional transaction by itself.

Commit receipts identify commit ID, idempotency key, request digest, prior/new workspace heads, changed object revisions, durable watermark and change-feed cursor. `changeset.status` resolves the key after a timeout. If idempotency receipts expire, the API reports the reconciliation limit; a client does not assume the expired key is new and retry blindly.

Rebase is three-way: base, current, proposed. Independent fields may be merged according to declared schema policy; lists, counters and rich text require explicit merge semantics. Cross-object invariants rerun even when field edits do not overlap. Conflicting edits retain both drafts and diagnostics.

## 8. Evaluation and result certificates

`value.evaluate` captures an immutable context and value address, validates admission/budget, resolves exact code, and schedules the calculation. It returns an evaluation handle or a complete evaluation record. `value.inspect` can read metadata/retained results without invoking code. This keeps denial diagnosis available when execution is disabled.

The evaluator records actual field reads, name/existence reads, predicate/index reads, source/kernel versions and explicit observation revisions. It replaces conditional read edges after each attempt. A derived override supplies a value and cuts that node's lower read edges within the scenario.

Evaluation lifecycle: `queued -> running -> succeeded | failed | cancelled`. Budget and snapshot failures carry structured errors. `invalidated` is a cache/projection condition, not retroactive rewriting of a successful historical evaluation. Following views move to a new evaluation when their context changes.

A later commit does not invalidate the truth of a successful value in a retained pinned context. It invalidates a following view's claim to represent current state and creates demand under a new context. Cached historical evaluations remain valid for their old inputs, subject to current authorization and retention. Local mutable-world invalidation in Lingo 0.1 is adapted into this context transition at the persistent boundary.

A successful evaluation record contains result/type/unit, context ID, address, source manifest, input dependency certificate, evaluator version/profile, start/finish times, measured resource use, completeness and provenance. Partial computations cannot masquerade as complete values. Model-generated confidence remains an observation, not an acceptance status.

The record separates execution status from result availability and completeness. An inspection request can finish with an explicitly partial/unavailable result record. A synchronous typed value read and any value-based commit/acceptance evidence require `succeeded`, `complete` and `result.state=available`; missing, redacted or unavailable states cannot justify a change as if they were a value.

Equivalent requests may coalesce only when context, visibility, code, numeric profile and arguments match. Each requester retains a separate demand handle. Cancelling one request removes that demand; it does not cancel a shared calculation still needed elsewhere. No shared cache crosses authorization partitions without an explicit proven public-value classification.

Pure evaluation must not execute tools, mutate base state or change authority. Arbitrary Lua upvalue purity is not statically proven by the current core. Trusted module review, declared effects, deterministic fixtures and optional process isolation remain part of admission. A process/JIT profile must state the resource bounds it actually enforces.

## 9. Watches, ordering and reconnect

A subscription binds principal, authorization epoch, context or follow policy, query/value targets, projection, demand policy and limits. It has an owner, lease and resume cursor. Watching an invalidation is separate from asking to recompute an expensive value.

The initial snapshot and starting cursor must be obtained without a race: either one backend operation returns both, or the client opens the stream first and buffers events around a snapshot watermark. A basic “read then subscribe” sequence with a gap is insufficient.

Ordering is per transaction domain/partition. A commit's changes form one batch. Cursors are opaque, scoped and durable within declared retention; clients compare them only through protocol rules. A delivered record includes event ID, cursor, predecessor or sequence/generation sufficient for gap detection, commit reference, scope and payload.

Delivery is at least once. Clients deduplicate event IDs, reject old generations, apply coherent commit batches and advance a cursor only after applying them. There is no assumed global total order across realms. A watch can coalesce presentation updates while retaining the information needed to validate its state.

Queue overflow, retention expiry, server generation change or an unavailable resume position returns `CURSOR_GAP`/reset. A following view becomes stale/resyncing, obtains a fresh snapshot and replaces its projection. A pinned view retains its original context and reconstructs that snapshot if retained; otherwise it reports `SNAPSHOT_EXPIRED`. Only an explicit rebind advances a pinned view. Silence, a keepalive or a TCP reconnect does not establish freshness. A best-effort engine event is a refresh hint only.

A gap event may include a replacement-snapshot hint. The hint is optional, does not itself grant a retention lease and never changes a pinned view's context. If a following view cannot acquire a coherent replacement, it remains stale with the error visible.

A follow view includes a recomputation policy: manual, visible-demand, or bounded periodic. Hidden panels may release demand. Rate limits and budgets limit work. The UI shows the last coherent watermark and whether it is following current state, pinned history or disconnected history.

## 10. Scenarios and comparisons

A scenario record identifies baseline context, optional parent scenario, typed overrides, author and content digest. Saving its description is ordinary metadata persistence outside evaluation; applying it is hypothetical. An override key is a full value address, including arguments. Duplicate overrides at one scope are rejected unless represented as a new scenario revision.

Nested resolution checks inner then outer scopes. Each materialized evaluator context has its own cache identity. Base objects are not cloned wholesale. Exiting a temporary scope releases it; saved scenario definitions can be reopened against their exact baseline if retained.

All members of a parent/child chain share baseline roots, snapshot, source manifest and observation set. A parent reference with a different baseline is rejected. Moving to another baseline is `scenario.rebase`, producing a new definition and rechecking addresses, types and comparisons; it never mutates an existing scenario identity.

A comparison names all dimensions allowed to differ and captures two context/evaluation sets. The default permitted difference is the override digest. The service rejects a claimed scenario-only comparison whose snapshot or manifest differs. Differences preserve units, errors, null/missing/redacted states and incompleteness; a failed side is not a zero.

Promotion selects valid stored-field override entries into a new changeset. It retains the scenario as evidence, resolves current target revisions and reruns validation. Saving scenario definitions and promoting their conclusions are different operations.

## 11. Commands, workflows and fencing

Command admission records requested operation, normalized arguments hash, principal, live authority, target, source/context, budget and idempotency/retry class. Script fields cannot downgrade a registry-classified effect into a pure read.

For a workflow, GraphSub records the run's durable logical state, named checkpoint, input snapshot, manifest, run revision, lease owner/generation and action intents. State transitions use conditional transactions. A local DSCO journal records execution attempts and receipts before they are reconciled to those records.

Replayable step handlers receive only named-action effect bindings. Immediate `l.call`, direct tool submissions, arbitrary transport and inherited effectful closures are unavailable in that execution scope, including through imported modules. All effects and fresh observations pass through a stable step/action identity and the journal. This restriction is enforced by the host environment, not a convention in example scripts.

The worker's fencing generation must be checked before new dispatch and before accepting a state transition. A target that supports fencing receives it as well. A lease can expire between validation and external dispatch. For targets without fencing, a replacement owner must not redispatch an unresolved intent until reconciliation. Fencing shared run state does not retract a previously sent external request. The protocol ingests immutable late receipts under the historical attempt identity independently of permission to advance the current run generation. It retains and reconciles them instead of pretending the old operation never happened.

Action identity is stable for one logical step occurrence. Attempt IDs are unique per dispatch. A deliberate loop iteration gets a distinct occurrence ID; restart of the same occurrence retains its action/idempotency identity. Binding a key to different normalized inputs fails.

A step reaches completion only with required outputs, acceptance evidence, resolved action outcomes and committed checkpoint. Run success requires all mandatory terminal criteria. Failure, cancellation and compensation do not erase receipts or incurred cost. An uncertain attempt prevents automatic retry unless the adapter proves safe replay or reconciliation resolves it.

## 12. Retention, export and deletion

Retention classes cover snapshots, source manifests, blobs, observations, journals, receipts and UI history separately. A release/run requiring replay pins dependencies according to a declared retention policy and storage budget. Garbage collection follows references and leases; it never deletes a pinned artifact merely because no current pane displays it.

Exports contain authorized object/source/observation revisions, manifest, context, digests and redaction manifest. They explicitly declare omissions. Import validates digests and schemas and creates new local identity mappings where necessary; it does not import authority or executable trust automatically.

Deletion creates a tombstone and applies retention policy. Sensitive-content erasure may make old replay impossible; history then retains permitted metadata indicating why content is unavailable. Backup recovery must preserve tenant boundaries, version references and idempotency/action records needed to avoid replaying effects. Restoration is a tested operational procedure, not an inference from a successful export.

## 13. Minimum protocol conformance

A backend profile is enabled only after its applicable probes pass: authenticated cross-tenant rejection; pinned reads under concurrent writes; schema/source pinning; deterministic query pagination; phantom detection; atomic rollback on mid-commit failure; duplicate-key replay and digest conflict; commit-timeout reconciliation; durable restart; race-free snapshot/stream handoff; duplicate/gap/overflow handling; authorization revocation; lease fencing; and uncertain-effect recovery.

These are required future behavioral tests. The record fixtures shipped with this definition validate schema shape and selected cross-record consistency only. They are not evidence that GraphSub already passes the protocol suite.
