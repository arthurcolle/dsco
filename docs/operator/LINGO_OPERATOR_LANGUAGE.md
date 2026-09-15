# Lingo as the DSCO operator language

**Target language contract 1.0 — September 9, 2026.** This defines how scripts operate DSCO against GraphSub. The [native Lingo 0.1 core](../LINGO.md) and [live GraphSub browse binding](LIVE_BROWSE.md) are implemented. The persistent contexts and broader operator methods below remain target interfaces. The [main definition](../DSCO_GRAPHSUB_OPERATOR.md), [world protocol](WORLD_PROTOCOL.md) and [operator surfaces](OPERATOR_SURFACES.md) define their shared semantics. Examples marked **target** use future methods; use LIVE_BROWSE for currently executable attachment/list/read scripts.

Lingo uses portable Lua 5.1 syntax executed by LuaJIT. It adds a world model and host libraries, not another parser. Lua supplies local variables, functions, closures, tables, loops and control flow. Lingo supplies typed object boundaries, contextual value reads, dependency capture, scenarios and explicit operator commands. The same semantic operations serve a person in the workbench, an agent, a script and a headless client.

## 1. Implemented core and target bindings

| Surface | Available in 0.1 | Target operator contract |
|---|---|---|
| `require("lingo")` | Yes | Retained for local semantic worlds and core constructors |
| `l.world()`, class declarations, typed objects | Isolated in-memory world per invocation | Local experiments remain explicit; persistent objects use a world attachment |
| Derived reads, dependency capture, invalidation | Yes, within the local world | Same calculation contract over pinned GraphSub revisions |
| Callback scenarios | Yes, scoped and transient | Also serializable scenario definitions and retained comparisons |
| `l.call(tool, args)` | Same-process, capability-gated native tool call | Retained low-level bridge; durable commands add intent and receipt handling |
| `why`, `describe`, `events` | Local bounded diagnostics | Contextual inspection and retained evaluation/trace evidence |
| `require("lingo.operator")` | Live `BrowseScope`: attach, status, schema, list, read | Persistent session, world, source, draft, scenario, command, workflow and view services |
| Additional package imports | Embedded `lingo.dsco`, `lingo.autobot`, `lingo.graphsub` and `lingo.chimera`; see [cross-system scripting](../lingo/SYSTEMS.md) | Additional package artifacts resolved through an immutable manifest |
| Shared commits, source releases, durable runs | No | Adapter-backed operations using existing GraphSub facilities and defined integration contracts |

The target does not change `l.world()` into an implicit remote connection. It also does not change `l.call` into an exactly-once workflow operation. Those would silently change existing programs.

The facade is written as `local o = require("lingo.operator")`. Current browse methods dispatch the registered `graphsub_operator` tool through the existing Lingo gate. The target methods below extend that facade into semantic operator operations; they do not create independent implementations of storage, authorization or scheduling. The [operation catalog](contracts/operations.json) owns target operation IDs and admission classes; a convenience method cannot weaken them. Current unsupported methods raise an explicit unavailable-method error; the target structured protocol uses `UNSUPPORTED_FEATURE`.

## 2. Names, modules and execution modes

Lua identifiers and module names are case-sensitive. Class names, object aliases and field names are strings; use `ctx:get(ref, "estimated cost", {})` for names containing spaces. Display names may change, while durable object IDs do not. No identifier spelling grants capabilities.

An operator module is a text artifact with a content digest, package identity, imports, exports, interface versions and compatible runtime profile. Its top-level chunk returns a declaration record. Import resolution occurs before execution; `require` loads only artifacts pinned by the manifest. There is no fallback to an ambient filesystem module, network loader, mutable `package.path` or raw FFI.

Three modes have distinct host libraries:

| Mode | Work permitted |
|---|---|
| Value expression | Read and calculate within one captured context |
| Workspace script | Inspect, create local drafts, prepare source overlays and invoke admitted world operations |
| Command script | Explicitly submit real commands or workflow transitions |

Calculations run in value mode even when called from a command script. A closure cannot inherit the caller's effectful libraries into a derived value. Inspection is separately classified: opening metadata must remain possible without authorizing the calculation itself.

The current host's `args`, bounded `print`, single return value and `l.try` remain useful entry conventions. Target script arguments are validated against their declared schema. Returned handles serialize as typed references, never native addresses or Lua closures.

## 3. Types and declarations

Local Lua variables remain dynamically typed. Stored fields, calculation parameters/results, tool requests, persisted records and workflow transitions have checked contracts.

The existing `l.types` primitives and descriptors remain: finite `number`, exact-range `integer`, `string`, `boolean`, `json`, `ref`, `list`, `record` and `optional`. In 0.1, optional means explicit `l.null`; records reject unexpected fields; lists are dense and one-based. `l.array{}` preserves an empty JSON array. These behaviors must not drift during integration.

Target `o.types` adds tagged exact integers, decimals, quantities, timestamps, durations, bytes/content references, enums, tagged unions and interface references. A decimal contains a coefficient and scale; a quantity also has a unit. Ordinary Lua `+` does not silently convert USD, tokens or seconds. Explicit library operations validate compatible units and declared conversions. Large integers are tagged decimal representations across the Lua/JSON boundary, avoiding loss beyond Lua's exact numeric range.

Missing, null, redacted, unavailable, failed and incomplete are distinct outcomes. A missing required field is an error; it is not zero or null. Redacted data cannot participate as if it were an ordinary string. Runtime values containing functions, cycles or process handles cannot be stored as domain data.

**Target declaration example:**

```lua
local l = require("lingo")
local o = require("lingo.operator")
local t = l.types

return o.package {
  name = "delivery.routing",
  version = "1.0.0",
  classes = {
    o.class {
      name = "delivery.Model",
      schema_version = "1",
      fields = {
        unit_cost = l.stored(t.integer), -- illustrative accounting units
      },
    },
    o.class {
      name = "delivery.Task",
      schema_version = "1",
      fields = {
        units = l.stored(t.integer),
        urgent = l.stored(t.boolean, false),
        economy = l.stored(t.ref("delivery.Model")),
        priority = l.stored(t.ref("delivery.Model")),
        selected = l.derived(t.ref("delivery.Model"), function(self)
          if self.urgent then return self.priority end
          return self.economy
        end),
        cost = l.derived(t.integer, function(self)
          return self.units * self.selected.unit_cost
        end),
      },
    },
  },
}
```

`o.package` and `o.class` construct declaration records; they do not publish or mutate the running world. Loading a manifest binds the derived closures to exact source artifacts and export identities. `self` is a context-bound read proxy. Assigning `self.units = 10` is invalid.

Target interfaces declare fields, calculated signatures and permitted command signatures. Class inheritance is single-parent with explicitly declared interface implementations. Cycles and ambiguous inherited definitions are rejected. Stored-field overrides must preserve their storage contract; calculated overrides must satisfy the inherited argument/result interface. Changing incompatible storage types requires a schema migration, not a new implementation silently interpreting old bytes.

## 4. Sessions, references and contexts

**Target attachment:**

```lua
local o = require("lingo.operator")
local session = o.attach {
  connection = args.connection,
  workspace = args.workspace,
}
local ctx = session:context {
  snapshot = "latest",
  source_manifest = args.source_manifest,
}
```

`latest` is resolved once when opening this context. `ctx` then pins exact roots, snapshot, source manifest, workspace/draft digest, scenario, observations, evaluator profile and principal visibility scope. Session navigation does not mutate it. Following live state means receiving successive new contexts, not making `ctx` silently mutable.

`ctx:reference()` returns the immutable context reference and `ctx:manifest()` its exact source manifest reference. Opening with `commit=commit_id` instead of `snapshot=...` requests a retained coherent state at that commit; failure to provide that exact state is explicit. A commit watermark is not silently treated as a snapshot lease.

`ctx:resolve(name, class)` returns an `ObjectRef` after ordered whole-binding resolution. `ctx:ref(id)` obtains a reference by stable identity and checks visibility/presence. An `ObjectRef` contains tenant, realm and object ID; an exact revision reference additionally names a revision. Reading a contextual reference chooses its revision under `ctx`. An exact reference must be compatible with the context or fail; it cannot silently substitute another revision.

`ctx:address(ref, field, arguments)` constructs a **context-independent ValueAddress**. Evaluated-node identity is `(context_id, ValueAddress)`. An evaluation ID identifies one attempt. A result record includes all three identities where relevant. This lets a comparison pair the same address under baseline and scenario contexts without confusing their cached values.

`ctx:object(ref)` returns a read proxy bound to this exact context. Its `object.field` syntax is shorthand for `ctx:get(ref, field, {})`. Use `ctx:get` for parameterized values and explicit code. Cross-context proxies cannot be passed into a calculation to smuggle an untracked read.

## 5. Value evaluation and dependencies

`ctx:get(ref, field, arguments)` demands a complete typed value or raises a structured failure. `ctx:evaluate(address, options)` exposes the evaluation handle and result certificate when callers need pending states, budgets or provenance. `ctx:inspect(address)` reads retained metadata/results without evaluating.

Stored reads, derived reads, reference resolution, handled absence and queries all contribute dependencies. The evaluator records actual reads, including branch selectors, and replaces obsolete conditional edges after an attempt. Derived fields do not maintain their own manual scheduler. Failed evaluation remains failed; displaying a previous result requires its original context and a historical label.

The existing `l.derived(..., {params={...}, cache="memo"})` shape remains valid. Arguments are typed and canonically hashed; different arguments produce different addresses. Target argument defaults, if declared, are expanded before hashing. `cache="none"` controls local result retention, not permission to read hidden changing state.

Pure calculations cannot call tools, refresh observations, edit drafts, create scenarios, commit, submit actions or change authority. Time, random samples, file contents, model responses and prices enter as explicit versioned observations. The runtime blocks host effects but does not magically prove arbitrary Lua closure purity; review, deterministic tests and execution profiles remain necessary.

A bounded solver is an explicit kernel with convergence and failure criteria. An accidental dependency cycle produces a path diagnostic, not implicit fixed-point iteration. A model invocation is a command producing an observation, never a hidden calculator.

## 6. Queries and collections

`ctx:query(spec)` returns a page plus snapshot, completeness and dependency token. Predicates are structured typed expressions; user data is never interpolated into raw Cypher. `o.query` supplies constructors such as `eq`, `and_`, `in_` and `order`; the underscore avoids the Lua keyword `and`.

This is a read binding. Any optional Cypher form is parsed and checked for write clauses and procedure effects; parameterization alone does not make a query read-only. Writes use drafts/commits, and effectful procedures use command admission.

```lua
local page = ctx:query {
  class = "delivery.Task",
  where = o.query.eq("project", args.project_ref),
  order = {o.query.order("name", "asc"), o.query.order("id", "asc")},
  limit = 100,
}
```

The query result tracks membership/predicate dependencies, not just returned IDs. A later insertion may change an aggregate. Pagination is pinned to one query and snapshot. A calculation that needs the complete collection must consume all pages within its budget or fail as incomplete. It cannot aggregate a timed-out prefix and label it a total.

Collections and portfolios are ordinary typed definitions combining membership and declared aggregations. Reuse GraphSub's query/index layer and incremental collection/map/filter/reduce facilities where they satisfy the required token and update contract. An incremental operator's `process(delta)` is not, by itself, automatic discovery of Lingo field reads.

## 7. Drafts, validation and commits

`session:draft(ctx)` creates a local draft bound to exact base revisions. `draft:create`, `draft:set`, `draft:remove`, `draft:rename` and `draft:link` propose typed changes. They cannot patch protected authoritative run/acceptance fields as arbitrary JSON.

`draft:context()` produces a new immutable context including the draft digest. Editing the draft again produces a new digest; an earlier context still describes the earlier draft. Preview calculations are clearly marked draft values.

`draft:validate()` returns diagnostics, affected objects and a preview digest. `draft:commit{key=...}` dispatches the conditional world commit and returns its disposition/receipt. A preview is not permanent authorization. Commit checks current permissions, expected revisions, read/predicate certificates and invariants. Timeout yields pending or unknown outcome; `session:commit_status(key)` reconciles the same request identity.

Undoing local edits changes the draft. Undoing a committed change proposes a new conditional changeset. None of these operations undoes an external action.

## 8. Scenarios and comparisons

The implemented `world:scenario(name, callback)` remains a transient local facility. Target `ctx:scenario{...}` constructs a serializable immutable definition and an evaluation context, without persisting it automatically:

```lua
local urgent = ctx:scenario {
  name = "urgent delivery",
  overrides = {
    {address = ctx:address(task, "urgent", {}), value = true},
  },
}
local comparison = session:compare {
  baseline = ctx,
  candidate = urgent:context(),
  targets = {ctx:address(task, "cost", {})},
  dimensions = {"scenario"},
}
```

A child scenario inherits the same snapshot, object roots, source manifest, draft baseline and observation versions as its parent. It adds or replaces typed overrides by full ValueAddress. A mismatched baseline is rejected; changing it requires explicit rebase into a new definition. The scenario record points to the baseline/parent, avoiding a self-referential context digest.

Inner overrides win. Overriding a calculated value supplies its result directly and suspends its lower reads. Removing an inner override reveals the parent override or original computation. Evaluating a scenario cannot execute commands or refresh observations.

`session:save_scenario(definition)` is an explicit metadata write outside evaluation. `session:promote_scenario(definition, selection)` prepares eligible stored-field overrides as a new draft against an identified current baseline; it does not commit. Calculated overrides cannot silently become stored truth. Policy overrides never grant real authority.

Comparisons declare all changed dimensions. A scenario-only comparison requires identical source, data and observations. A source comparison explicitly names the changed manifest. Both sides retain evaluation identities, errors, units, completeness, costs and dependency changes; failure is not zero.

## 9. Source editing and release

`session.source:open(binding)` returns a source buffer with exact origin and base revision. Buffer operations edit local text; they do not hot-reload running code.

`session.source:preview(buffer, ctx)` captures an immutable copy of the text, stores it as a temporary content-addressed source artifact, resolves its imports into a complete candidate manifest, and returns a **new context**. It identifies which module/export the buffer replaces. Existing contexts and runs retain their old manifest. A digest recorded only as a comment or result attribute is insufficient.

`buffer:check()` reports syntax/declaration diagnostics. `session.source:test(candidate, fixtures)` runs explicitly identified fixtures. `buffer:save()` persists to its declared local-file or workspace-source target. Build, publish and release-binding promotion are separate admitted operations with distinct receipts. Source changes do not imply schema migration or deployment.

The package catalog should reuse GraphSub's schema/type history and provenance facilities through an explicit mapping. Existing extraction type records are not automatically executable module manifests; the adapter must supply content identity, imports, export binding and runtime compatibility without conflating these schemas.

## 10. Commands and observations

The current `l.call` returns `{ok, tool, result, sequence}`; `result` is the native tool's text. It executes immediately under the existing gate and has no transaction spanning calls.

Target `session.commands:submit{operation, arguments, context, key}` records an admitted intent and returns a command handle. `command:status()` and `command:wait{timeout_ms=...}` return explicit dispositions. A wait timeout means the command may still be running; it does not authorize a fresh dispatch. `command:receipt()` returns observed evidence when available.

A convenience `session.tools:submit{name, arguments, context, key}` maps to the registered DSCO tool operation. Real dispatch always uses `tools_execute_for_tier()` with inherited authority and taint. The key binds normalized request content and is an identity for reconciliation, not a universal external idempotency guarantee.

An observation is captured from a receipt and stored as an immutable, provenance-bearing input. An unsuccessful or truncated tool result cannot be promoted into a complete observation by relabeling it. Provider output, inferred confidence, independent verification and accepted truth remain different records.

Model routing, worker selection and budgets use versioned domain objects. Naming a `Model` object never invokes it. The operation registry supplies actual tool schemas and effect classifications; user scripts cannot describe a network action as pure to bypass admission.

## 11. Durable workflows

`o.workflow{...}` declares named steps, typed inputs/outputs, source dependencies, budgets, waits and terminal criteria. It is a definition, not a running process. `session.runs:start{definition, inputs, context, key}` creates a durable run against exact source and inputs.

A step handler receives a restricted `step` service plus serialized inputs and prior checkpoint data. It returns a transition such as complete, wait or fail. Checkpoints contain named next steps, ordinary typed values and durable references. They do not contain Lua stacks, closures, coroutine frames or native pointers.

Inside a replayable step, the host removes immediate `l.call`, direct `session.tools:submit`, unrestricted command submission and raw transport capabilities. Imported code and captured closures cannot regain them. Every real effect or fresh observation uses `step:action` with a stable logical identity; the ordinary command-script bridge remains available outside replayable step scope. Without this separation, restarting a handler could duplicate an unrecorded action.

**Target workflow shape:**

```lua
local o = require("lingo.operator")

return o.workflow {
  name = "delivery.verify",
  version = "1",
  inputs = {artifact = o.types.ref("delivery.Artifact")},
  first = "check",
  steps = {
    check = function(step, input)
      local action = step:action("verify-artifact", {
        operation = "artifact.verify",
        arguments = {artifact = input.artifact},
      })
      if action.state == "pending" then
        return step:wait {action = action.id, resume = "check"}
      end
      if action.state ~= "succeeded" then
        return step:fail {code = "CALCULATION_FAILED", receipt = action.receipt}
      end
      return step:complete {verification = action.output}
    end,
  },
}
```

`artifact.verify` is a proposed semantic example and requires a registered adapter; it is not an existing tool name asserted by this document. `step:action` uses the run, step occurrence and local action name to identify one logical action. On re-entry it reads the existing intent/receipt; it does not dispatch again merely because Lua began the handler again. Deliberate loop iterations get explicit new occurrence IDs. Changed arguments under an existing action identity fail.

The scheduler validates transitions, output contracts, lease generation, acceptance evidence and budget. Calling `step:complete` requests a transition; it does not let arbitrary script data overwrite authoritative run completion. An unresolved external outcome blocks unsafe retry. Historical receipts remain evidence even when their expired owner cannot advance current run state.

## 12. Compute, inspection and views

GraphSub already supplies typed compute graph structures, effect metadata, schedulers, runtime/event surfaces, trace types and dependency inspection. `session.compute` should adapt those facilities rather than introduce a second graph engine. A pure registered kernel can be bound as a value implementation only when its adapter supplies deterministic input/output contracts, source/kernel version, numeric profile and measured bounds.

Not every exposed compute operation currently satisfies that contract. The [adapter review](GRAPHSUB_ADAPTER_REVIEW.md) distinguishes graph construction/inspection from incomplete runtime operation dispatch, stepping and checkpoint behavior. Unsupported kernels fail explicitly. Arbitrary GraphSub compute labels are not sufficient proof of executable numerical semantics.

`ctx:inspect`, `ctx:describe`, `ctx:dependencies` and `ctx:dependents` expose retained metadata with completeness and authorization. Dependencies belong to a particular evaluated node/context; workflow trace edges and calculation-read edges retain different kinds. `session.trace` adapts existing trace facilities while preserving evaluation, command, actor and source identities.

`session.views:open{kind, target, context, follow, demand}` opens a presentation binding. It cannot grant execution or bypass the operator service. A watch observes changes under an explicit recomputation policy; a pinned watch never silently advances its context after a gap. Closing a view releases its demand, not the underlying object or detached run.

## 13. Errors and recovery

Target service failures expose stable code, origin, correlation IDs, authorized details and retry disposition. `o.try(fn)` returns `ok, value_or_error` for recoverable target errors, while terminal resource exhaustion remains nonrecoverable. It must not replace the existing `l.try` error representation without versioning.

Pure synchronous reads raise on failure; asynchronous evaluations and commands return state records so pending, failed and effect-unknown remain distinguishable. A script may branch on an error code, not parse a human message. `CONFLICT` requires a new conditional changeset; `OUTCOME_UNKNOWN` requires reconciliation; `SOURCE_UNAVAILABLE` never triggers an unpinned import. Cancellation removes demand or requests work cessation; it cannot erase effects or receipts.

## 14. Complete target operator journey

The following command-mode script uses a preexisting task and source manifest. Its fixture would use the declaration above, model costs 2 and 7, and 100 task units. It inspects 200 baseline units, compares 700 under urgency, stages the selected stored change and commits it. External work is submitted only after a confirmed commit.

```lua
local o = require("lingo.operator") -- target binding, not installed in 0.1
local s = o.attach {connection=args.connection, workspace=args.workspace}
local base = s:context {snapshot="latest", source_manifest=args.manifest}
local task = base:resolve(args.task_name, "delivery.Task")
local cost = base:address(task, "cost", {})

local before = base:get(task, "cost", {})
local explanation = base:inspect(cost)
local urgent = base:scenario {
  name="urgent delivery",
  overrides={{address=base:address(task, "urgent", {}), value=true}},
}
local comparison = s:compare {
  baseline=base, candidate=urgent:context(),
  targets={cost}, dimensions={"scenario"},
}

local draft = s:draft(base)
draft:set(task, "urgent", true)
local preview = draft:validate()
assert(preview.valid, "draft validation failed")
local outcome = draft:commit {key=args.commit_key}
if outcome.state ~= "committed" then
  return {state=outcome.state, commit=outcome, comparison=comparison}
end

local committed = s:context {
  commit=outcome.receipt.commit_id,
  source_manifest=base:manifest(),
}
local command = s.tools:submit {
  name="read_file", arguments={path=args.input_path},
  context=committed, key=args.action_key,
}
local status = command:wait {timeout_ms=1000}
return {
  baseline=before, explanation=explanation, comparison=comparison,
  commit=outcome.receipt, command=command:reference(), status=status,
}
```

The example returns a nonterminal command state honestly if one second is insufficient. It does not equate a committed task setting with a successful file read. Production fixtures must verify all returned contracts against the negotiated backend, rather than only checking that this Lua parses.

## 15. Language conformance boundary

Required checks include exact module pinning; runtime type/schema agreement; stable reference resolution; conditional dependency pruning; query membership invalidation; context-separated caches; scenario baseline rejection and unwind; source preview without hot reload; draft/commit separation; conflict and timeout reconciliation; same-authority tool dispatch; named-step re-entry without duplicate dispatch; redacted inspection; and pinned-watch recovery.

The existing 0.1 conformance suite establishes its local semantics. It does not certify the proposed attachment, persistence, manifest, command or workflow bindings. Integration should expose existing GraphSub capabilities one by one behind these contracts, retain their source-backed evidence, and enable each binding only when its observable behavior passes the applicable checks.
