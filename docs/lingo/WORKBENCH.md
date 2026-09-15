# Lingo workbench

The workbench keeps a Lingo world open inside one native DSCO process. A program declares a view of an object value and the stored fields an operator may vary. DSCO displays the value, explains its dependencies, accepts typed control changes, and recomputes the result in a local scenario. An explicit save retains the base observations, selected value address and scenario overrides for a later process.

This is an implemented CLI and MCP surface over the embedded LuaJIT runtime. It uses the same world, types, dependency tracking and capability gate as [ordinary Lingo programs](../LINGO.md) and [portable worlds](WORLDS.md). It does not introduce a second interpreter or a remote session service.

## Open the workspace desk

From this checkout, run:

```sh
dsco lingo open examples/lingo/workspace-desk.lingo
```

The defaults use this checkout's parent workspace and its collector. To run from another directory, supply an argument record with absolute `root` and `collector` paths; relative collector paths resolve from `root`. The [program](../../examples/lingo/workspace-desk.lingo) invokes the existing [workspace collector](../../scripts/verify_lingo_workspace_review.py) once through the governed `bash` tool. The collector reads Git metadata for five explicit scopes: CLI, SDK, Chimera, Autobot ToolManagement and GraphSub. It reports changed source paths, conflicts, observation times and provenance. It does not edit those repositories.

The desk initially ranks scopes with conflicts first, then the largest changed-source-path count. It excludes scopes with no reported work and requires usable observations before selecting a scope. Counts guide inspection priority; they are not test results or measures of code quality. The queue includes a suggested next action, such as inspecting conflicts.

Try these commands at `lingo> `:

```text
why
controls
set 1 "smallest"
set 2 "sdk"
set 3 3600
reset
inspect review
save /tmp/workspace-desk.session.json
quit
```

Control 1 changes the ranking order; control 2 selects a component. Control 3 advances the assumed elapsed seconds from the recorded capture time. Control 4 sets the maximum observation age. These last two controls let the operator examine stale-evidence behavior explicitly. Waiting at the prompt does not advance a hidden clock or refresh Git.

Reopen the saved desk with the same program:

```sh
./dsco lingo open examples/lingo/workspace-desk.lingo --restore /tmp/workspace-desk.session.json
```

The restore uses saved arguments and observations. It does not run the collector again. To obtain a fresh observation, open a new desk without `--restore`.

## Declare a view

A small complete program demonstrates the mechanism:

```lua
local l = require("lingo")
local t = l.types
local w = l.world{id="estimate-desk"}

-- Define classes every time the source is loaded.
w:define("Estimate", {
  units = l.stored(t.integer, 3),
  unit_cost = l.stored(t.number, 2.5),
  total = l.derived(t.number, function(self)
    assert(self.units >= 0, "units must be nonnegative")
    return self.units * self.unit_cost
  end),
}, {version="1"})

return l.view{
  title="Estimate desk",
  world=w,
  target={object="estimate", field="total"},
  controls={
    {object="estimate", field="units", label="Units"},
    {object="estimate", field="unit_cost", label="Unit cost"},
  },
  initialize=function(world)
    -- Initial collection and object creation belong here.
    world:new("Estimate", "estimate", {})
  end,
}
```

`initialize` runs only for a fresh open and receives the declared world. It may use normal governed tools to collect observations and create objects. On restore, declarations run, the saved world is validated and loaded, and `initialize` is skipped. All nested host calls are forbidden during restoration, including calls made before `l.view`. Put effectful collection inside the initializer.

The target names an object and field, with an optional `args` record for parameterized derived values. The runtime binds it to a source-bound `lingo.value/1` address. An address identifies the world, object, class, field, arguments and definition/runtime identities. It does not encode the current scenario: the same address can be evaluated against base or overridden values.

Controls name mutable stored fields. Derived fields and fields on immutable objects cannot be controls. A label is optional; optional `choices` must be nonempty, unique and valid for the field's type. There may be at most 16 controls and 32 choices per control. Duplicate object/field controls are rejected.

Control values use ordinary JSON representation with declared type validation. Strings, integers and booleans remain distinct: `5`, `"5"` and `false` are not interchangeable. References are represented by object IDs and resolved against their declared class. Use JSON `null` for a nullable field; it does not mean that a command's required value is absent.

## Operator actions

| Terminal command | Effect |
| --- | --- |
| `show` | Read the selected target in the current base or scenario. |
| `why` | Show the selected value's dependency explanation. |
| `controls` | Show typed controls, base values, current values and overrides. |
| `set 1 "smallest"` | Validate and apply a scenario override to control 1. Indices are 1-based. |
| `reset` | Remove all overrides and retain the selected target. |
| `select OBJECT FIELD` | Bind and display a different field. Use JSON mode for parameterized selection. |
| `inspect [OBJECT]` | Describe a named object or the currently selected object. |
| `save PATH` | Write a reconstructable session through the normal gated `write_file` tool. |
| `quit`, `q`, `close` | Release the native session. EOF also closes it. |

Every ordinary command after open blocks nested host calls. A derived calculation cannot invoke a tool while responding to a control change. `save` is the explicit native exception: it serializes the current state and invokes gated `write_file`. The normal execution gate also applies to opening and operating a session; `DSCO_ALLOW_RUN=0` denies the entry point, and `DSCO_ALLOW_WRITE=0` denies saving.

A candidate control change is checked and evaluated before its override is retained. A bad type, invalid choice or calculation failure leaves the previous overrides intact. A failed `select` likewise preserves the previous target. A scenario's result is shown alongside the base result; if the base calculation fails while the scenario succeeds, the response carries `baseline_error` instead of inventing a baseline value.

Inspection describes object identity, class/version, revision and field definitions. `why` reports the dependencies actually read by the selected computation, including parameter values. It is an explanation of that calculation, not a guarantee about outside systems' current state.

## JSON protocol and MCP

Add `--json` to `lingo open` for newline-delimited JSON. The process emits an initial open response and one response per input command. Each command is an object with an action:

```json
{"action":"read"}
{"action":"set","control":1,"value":"smallest"}
{"action":"select","object":"review","field":"scaled","args":{"factor":4}}
{"action":"why"}
{"action":"inspect","object":"review"}
{"action":"save","path":"/tmp/desk.session.json"}
{"action":"close"}
```

These illustrate the wire syntax; object, field and control names must exist in the opened program. The CLI owns its `session_id`, so including that field in a command is rejected. Unknown actions, duplicate keys and action-inappropriate fields are rejected. An explicit close emits its response and terminates; EOF closes without adding an unsolicited JSON response.

Successful native responses contain `session_id`, `source_sha256`, `tool_calls`, `command_tool_calls` and `view`. `tool_calls` counts cumulative attempted nested tools; `command_tool_calls` is the delta for that successful command. A denied save is still an attempted tool call and appears in the next successful response's cumulative count. These counts exclude the outer `lingo_session` dispatch itself.

For `read`, `set`, `reset` and `select`, `view` contains:

```text
format: lingo.view.result/1
title, context: base | scenario, target: bound value address
value, explanation, selected: object description
controls: [{index, object, field, label, type, base, current, overridden, choices?}]
baseline? or baseline_error?: base comparison when a scenario is active
```

`why` returns the explanation directly in `view`; `controls` returns an array; `inspect` returns the object description. Save returns `{saved:true,path,sha256,bytes}` and close returns `{closed:true}`.

Errors use an `error` field. Native semantic errors may be strings, with nested details in the message; CLI validation errors use `{code,message}`. Capability denials may have a separate `reason`. Clients should test for `error` before reading a success payload. Ordinary command errors keep the process open. The CLI ultimately exits 1 if any command failed, 2 for usage errors, and 0 for a clean session.

In a persistent `dsco mcp serve --toolsets all --tier trusted` process, call the registered `lingo_session` tool with:

```json
{"action":"open","path":"/absolute/desk.lingo","args":{}}
```

Open optionally accepts `restore_path`. Later calls use the returned process-local handle:

```json
{"action":"set","session_id":"RETURNED_ID","control":1,"value":5}
{"action":"close","session_id":"RETURNED_ID"}
```

The same native implementation serves CLI and MCP. MCP failures have `isError:true`. Session IDs do not transfer between DSCO processes, and a closed ID cannot be reused.

## Saved state and bounds

The outer file format is `lingo.session/1`: program `source_sha256`, informational `source_path`, saved `args`, view implementation `view_sha256`, and a `lingo.view/1` packet. That packet contains the base world snapshot, declared title/controls, selected address and a separate list of control overrides. Saving a scenario never writes those overrides into the base objects.

Restoration requires an explicit program. Its source bytes and current view implementation must match; the world loader also checks runtime identity, class definitions, stored types and references. The declared title and normalized controls must match the packet. Explicitly supplied arguments must equal the saved arguments. Source or runtime changes can therefore make an old session incompatible; there is no automatic migration.

The file contains captured data and program arguments. Its save receipt identifies the written bytes with SHA-256; it is not a signature or a credential. A local save is not a GraphSub commit, a distributed snapshot, a transaction over tools, or a suspended remote workflow. Initial tool effects are not rolled back if opening later fails.

Each process supports at most four sessions. Each VM has a 32 MiB allocation budget and each command a five-million-instruction budget. Source and saved files are bounded to 256 KiB; CLI argument records and command lines are bounded to 64 KiB. Instruction or memory exhaustion makes that VM unavailable until it is closed and reopened. Ordinary validation errors do not exhaust a session.

## Verification

Run the native conformance suite from this checkout:

```sh
python3 tests/test_lingo_session.py ./dsco --report reports/lingo-workbench-20260910/session-tests.json
```

It exercises deterministic scenarios and rollback, typed parameter selection, forbidden derived effects, save/restore identity checks, execution/write denials, malformed commands, memory/instruction exhaustion, MCP session capacity and recovery, and a real PTY interaction. It then opens the workspace desk against the five real scoped Git observations, compares ranking with an independent Python calculation, and verifies a separate process reopens the saved result without collecting again. No model or network request is needed. `--skip-workspace` explicitly omits that last live collection step.
