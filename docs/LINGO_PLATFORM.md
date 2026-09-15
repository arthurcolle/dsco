# Lingo platform objects

`require("lingo.platform")` supplies shared domain declarations over the core
Lingo world. The module does not call tools, fetch state, consult a clock, choose
a scheduler, submit work, or write a database. Its objects let a script calculate
over explicit observations from those systems.

```lua
local p = require("lingo.platform")
local w = p.world {id="operations-review"}
local observation = p.observe(w, "autobot/catalog/1", {
  origin={system="autobot", operation="discover", source_id="tool_management_api"},
  observed_at=args.recorded_at,
  consistency="recorded",
  payload=args.actual_discovery_result,
})
local service = w:new("Service", "autobot", {
  observation=observation, availability="ready",
})
local policy = w:new("Policy", "review-policy", {
  allow_effects=true, max_cost_usd=0.10, max_calls=2,
})
local task = w:new("Task", "proposed-work", {
  service=service, policy=policy, operation="explicit-operation-name",
  inputs=args.inputs, estimated_cost_usd=args.estimated_cost_usd, expected_calls=1,
})
local base = task.summary
local hypothetical = w:scenario("lower budget", function(s)
  s:override(policy, "max_cost_usd", 0)
  return task.summary
end)
return {base=base, hypothetical=hypothetical, after=task.summary}
```

`availability="ready"` is an explicit normalization of the supplied observation.
It is not a fresh health probe. A successful discovery response establishes that
discovery worked at the recorded time; it does not prove every discovered tool
will execute successfully. The script author must preserve that distinction.

## Definitions

All classes have declaration version `0.1`. The core records their actual
definition-source fingerprints in snapshots.

| Class | Required stored fields | Defaults and calculated values |
|---|---|---|
| `Observation` | `origin:{system,operation,source_id}`, `observed_at`, `consistency`, `payload` | Immutable base fields; calculated provenance `summary`. |
| `Service` | `observation:Observation` | `enabled=true`, `availability="unknown"`; `ready` is enabled and explicitly ready, and `summary` is advisory. |
| `Object` | `service:Service`, `remote_id`, `observation:Observation` | `payload` reads the observation; remote IDs remain opaque strings. |
| `Artifact` | `observation:Observation`, `locator` | `sha256=""`; `summary` explicitly says the language did not verify the artifact. A supplied digest is a claim until checked externally. |
| `Policy` | `max_cost_usd:number`, `max_calls:integer` | `allow_effects=false`; `valid` requires nonnegative cost and at least one call. |
| `Task` | `service:Service`, `policy:Policy`, `operation`, `estimated_cost_usd:number` | `inputs={}`, `expected_calls=1`; `operation_valid`, `estimates_valid`, `within_budget`, `readiness`, `eligible`, and `summary`. |
| `Run` | `task:Task`, `observation:Observation`, `remote_id` | `state="unknown"`; `terminal` recognizes `succeeded`, `failed`, `cancelled`. `cancel_requested` remains nonterminal. |
| `Model` | `service:Service`, `model_id`, `observation:Observation` | `service_ready` says only that the associated service is locally considered ready. |
| `Agent` | `service:Service`, `agent_id`, `observation:Observation` | `service_ready` makes no claim about an individual agent's liveness. |

Reference fields are checked for both class and world identity. JSON payloads
are copied on ingestion and read. Unknown declared fields are rejected by core
object creation. `p.observe` additionally rejects unknown envelope/origin fields,
missing provenance, control characters in metadata, and unsupported consistency
labels. Its accepted labels are `live`, `snapshot`, `recorded`, and `local`.
These describe the supplied evidence; they do not authenticate the claim.
Timestamps are preserved as supplied nonempty strings, without guessing a time.

An `Observation` cannot change in the base world. A new observation gets a new
identity. Scenario overrides can temporarily replace its values for analysis,
but scenario snapshots are forbidden and the original values are restored on
exit. The example uses a policy override so the original evidence remains easy
to distinguish from the hypothetical choice.

## What eligibility means

`Task.readiness` returns `invalid_policy`, `invalid_estimate`, `invalid_operation`,
`service_unavailable`, `effects_disabled`, `over_budget`, or `eligible`, in that
order. Invalid numeric estimates or budgets cannot accidentally yield eligible
work. Core types reject nonfinite numbers, fractional integers and foreign
references; the calculated predicates check meaningful numeric ranges.

`eligible` means the proposed task satisfies these local planning conditions.
The summary marks `advisory=true`, `grants_authority=false` and
`submits_effects=false`. Host capability gates, authenticated service authority,
credit admission, idempotency and actual execution remain separate. Calling a
tool still crosses the normal DSCO gate even when a Policy permits it locally.

The derived fields are ordinary Lingo calculations. Reading a task records its
dependencies on service and policy fields. Changing a policy invalidates the
affected values; reading in a scenario calculates against that scenario's
overrides. No scheduler or remote effect runs because a value was read.

## Addresses and snapshots

```lua
local address = w:address(task, "summary")
local saved = w:snapshot()
local reopened = p.world {id="operations-review"}
reopened:load(saved)
local same_value = reopened:read(address)
```

Declare the same platform classes before loading. The core validates world
identity, declaration/source fingerprints, references and stored types before
installing a snapshot into an empty declared world. A snapshot stores local
language state. It is not a GraphSub transaction, an Autobot workflow commit,
an OTP checkpoint, or evidence that a remote service remains available.

The [recorded observation example](../examples/lingo/platform-observations.lingo)
accepts actual GraphSub list, ToolManagement discovery and Chimera planning
envelopes. It retains each payload, calculates a task against the reported
route cost/call estimate, changes policy in a scenario, and reconstructs the
calculation from a snapshot. All observations are explicitly labeled recorded;
it performs zero backend calls and executes no inference.

Run the offline semantic proof against the built binary:

```sh
python3 tests/test_lingo_platform.py ./dsco
```

The fixture tests dependency invalidation, hypothetical-policy isolation,
immutable observations, strict provenance input, class/world reference checks,
artifact and run semantics, fresh-process reopening, and the inability of a
local Policy to override native capability denial.
