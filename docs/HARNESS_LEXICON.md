# HX/1 — Harness Execution Lexicon

A shared vocabulary for describing DSCO execution in conversation, design notes,
incident reports, and compact DSL-shaped records.

**Status:** documentation vocabulary, not a new executable language, telemetry
schema, scheduler, or policy. Existing API names and state strings are unchanged.
Lingo remains the executable language. HX expressions below are descriptive
notation, not commands accepted by `dsco lingo` or the tool registry.

**Source basis:** inspected checkout `~/dsco/dsco-cli`, HEAD
`be2ba22`, with pre-existing working-tree changes. This is a source-grounded map,
not an installed-binary attestation. In particular, `execution_contract.c` and
`headless_tool_guard.c` were untracked local additions during inspection.

## 1. The organizing rule

Name five different things separately:

| Kind | Question | Example |
|---|---|---|
| **Entity** | What exists? | session, task, worker, tool attempt |
| **Phase** | What work is happening? | assemble, route, dispatch, verify |
| **State** | What condition is it in? | `tool_attempt.state=denied` |
| **Plane** | What responsibility applies throughout? | authority, context, accounting |
| **Evidence** | What establishes the claim? | observation, receipt, artifact, verdict |

A hierarchy of responsibilities is not a timeline. A process tree is not a task
tree. A trace is not a conversation. Do not compress these into one fake tree.
Short spoken names are the final component of the qualified phase name; retain
the qualifier whenever it would otherwise be ambiguous.

## 2. Entity hierarchies and relationships

### 2.1 Runtime and conversation

```text
harness                         executable system and its adapters
  instance                      one running DSCO OS process
    attachment                  this instance's attachment to session state
      session                   conversation + mutable session settings
        episode                 one operator request through answer or boundary
          cycle                 one model-loop iteration
            exchange            one logical provider request/response
              provider_attempt  one actual transport attempt
            tool_call           requested tool name + arguments + call identity
              tool_attempt      one entry through tools_execute_for_tier()
                nested_attempt  a child tool attempt, if the tool invokes another
```

This tree is a **local execution view**, not universal storage ownership:

- A saved session can be attached to a later instance. A session is not a PID.
- `episode`, `cycle`, and `attachment` are HX analytical terms, not claims that
  the runtime persists objects or IDs with these names.
- A cycle can have no tools or many tools. A logical exchange can have multiple
  provider attempts. A direct tool call does not require an enclosing model cycle
  or provider exchange; the invoked tool may itself use a provider.
- Nested attempts have parent linkage; an HTTP request inside a tool is not
  automatically a separately recorded tool attempt.
- Say **operator turn** for submitted input, **model turn** for a model-loop
  iteration, and **lease** for a goal-controller work opportunity. Avoid bare
  "turn" in metrics.
- Preserve real run identities as `run[owner=chronicle,id=...]`,
  `run[owner=swarm,id=...]`, or `run[owner=autobot,id=...]`. These run types are
  not interchangeable and are not automatically one-per-episode.

### 2.2 Work and scheduling

```text
objective                       desired outcome, whether recorded or informal
  contract                      target + acceptance + authority + limits
  goal                          optional active goal-controller record
    task                        plan-lane or work-lane node; may have children
      lease                     one revision-fenced opportunity to advance task
```

Keep the other implemented work representations distinct:

```text
plan_engine:  plan -> step -> nested step or atom
workflow:     specification -> execution -> named step result
swarm:        group -> worker assignments -> child runs
```

A plan atom is an execution unit, **not** an ACID transaction. A controller task
can take many cycles and leases. A task may delegate to a swarm; delegation is a
relationship, not proof that every task owns a worker. Dependency edges such as
`A needs B` are separate from parent-child ownership. Goal-queue siblings are not
implicitly ordered dependencies. In the goal queue, only plan-lane tasks may own
children; work-lane tasks are leaves. Restoring an interrupted active task requeues
it and adjusts its attempt count; this is not proof its external effects are absent.

### 2.3 Evidence and causality

```text
trace -> span -> event                    causal/timing view
attempt -> receipt                       execution report
operation -> artifact                    produced or changed material
observation + criterion -> verdict       acceptance decision
```

A receipt records an attempt; a verdict evaluates evidence against a criterion.
An artifact is the actual file, buffer, object, or result, not its description.
Correlate these views using recorded IDs; never invent missing runtime linkage.

## 3. Process lifecycle: Boot, Bind, Serve, Drain, Halt

| Qualified phase | Spoken name | Subphases and completion condition |
|---|---|---|
| `process.boot` | **Boot** | `parse`, `configure`, `initialize`: resolve invocation, profile/configuration, and mode-specific runtime dependencies. Enough state exists to enter the selected mode. |
| `process.bind` | **Bind** | `identity`, `authority`, `session`, `adapters`: attach principal/tier, applicable limits, new/restored session, registry and required adapters. Binding records authority; it does not grant more. |
| `process.serve` | **Serve** | `wait`, `accept`, `run`: receive input, route it to the selected adapter, execute episodes. Interactive/server modes repeat; one-shot mode can finish after one episode. |
| `process.drain` | **Drain** | `cancel`, `join`, `persist`, `flush`, `release`: stop owned activity as appropriate, save final state, flush output and close resources. Ownership and mode determine which children are stopped. |
| `process.halt` | **Halt** | Return an exit status or record abnormal termination. An exit status alone does not prove task acceptance or external rollback. |

These are semantic boundaries, not new functions. Initialization and binding
interleave in the existing code; metadata-only subcommands skip unnecessary
subsystems. A crash may bypass Drain. A detached durable agent is not necessarily
owned by the exiting interactive session. `swarm_destroy()` normally terminates
owned children with bounded reaping, but preserves them during an explicitly
configured handoff. Its `worker.reap_pending` event means teardown did not observe
a child's exit; do not report that child as confirmed reaped.

## 4. Episode lifecycle: Intake, Frame, Pursue, Verify, Deliver, Record

| Qualified phase | Spoken name | Subphases | Exit evidence |
|---|---|---|---|
| `episode.intake` | **Intake** | `receive`, `decode`, `normalize`, `classify` | Input and origin are understood; distinguish question, action, command, and event. |
| `episode.frame` | **Frame** | `target`, `criteria`, `scope`, `limits` | Correct account/location/path/environment, acceptance criteria, authority boundary, and bounds. Unknown target stays unresolved. |
| `episode.pursue` | **Pursue** | `survey`, `decompose`, `schedule`, `cycle` | Evidence or candidate answer/artifact; alternatively an evidenced boundary. Subphases repeat as needed. |
| `episode.verify` | **Verify** | `inspect`, `test`, `compare`, `verdict` | Named criteria evaluated against actual evidence. Failed checks can return to Pursue. |
| `episode.deliver` | **Deliver** | `present`, `place`, `confirm` | Answer returned or artifact placed at the requested destination; placement/visibility checked where required. |
| `episode.record` | **Record** | `checkpoint`, `receipt`, `account`, `summarize` | Applicable state and evidence persisted, accounting basis retained, unresolved risk stated. Persistence failure remains explicit. |

This is the **outcome vocabulary**, not a claim that every runtime path has an
automatic verifier or receipt publisher. Questions need proportionate grounding,
not invented goals. Delivery may be a chat answer, a saved file, or a specifically
requested installation; do not silently upgrade one into another. Presentation
follows the requested surface; this vocabulary does not enable desktop windows.

Recording and provisional streaming also happen during Pursue. The final
Record phase closes the episode's evidence; it is not the first log write.
A streamed draft is not final acceptance. Some deliverables require verification
again *after* placement, so Deliver can feed back into Verify.

## 5. Model cycle: Assemble, Route, Infer, Act, Ingest, Decide

```text
assemble -> route -> infer -> [act] -> ingest -> decide
    ^                                              |
    +--------------- continue/repair -------------+
```

The ordering is conceptual: routing may precede model-specific assembly, and a
route change may require assembly again. No single new linear pipeline is implied.

| Qualified phase | Spoken name | Fine-grained vocabulary |
|---|---|---|
| `cycle.assemble` | **Assemble** | `instructions`: applicable scoped guidance; `recall`: needed saved evidence; `disclose`: active tool schemas; `budget`: token allocation; `compact`: bounded reduction; `encode`: provider request construction. |
| `cycle.route` | **Route** | `resolve`: provider/model/profile; `qualify`: availability and constraints; `select`: permitted lane; `bind`: concrete request route. Requested and actually reported model remain separate. |
| `cycle.infer` | **Infer** | `submit`, `stream`, `decode`, `finish`: conduct the exchange, parse content/tool-call deltas and usage, and observe its protocol terminal condition. |
| `cycle.act` | **Act** | Validate and execute requested tool calls through the tool-attempt lifecycle. May fan out independent calls or dispatch a swarm through the same gate. |
| `cycle.ingest` | **Ingest** | `correlate`, `normalize`, `append`, `account`: associate results with calls, retain errors and bounded content, update conversation and usage. |
| `cycle.decide` | **Decide** | `continue`, `repair`, `pause`, `stop`, `escalate`: choose the next bounded transition using results, goal state, limits, and operator input. |

**Disclose** means make a schema visible. It does not mean authorize execution.
**Infer** describes model execution, not access to hidden internal reasoning.
**Ingest** is receipt of data; it does not certify that data as trusted or true.
**Stop** ends a loop segment, not necessarily the goal, session, or process.

### 5.1 Provider exchange microphases

```text
exchange.encode
exchange.submit
  transport.resolve -> connect -> tls -> send -> first_byte -> receive
exchange.decode
exchange.finish
```

Transport phases overlap decoding, can be skipped by connection reuse, and differ
for local inference and realtime WebSockets. Use `ttfb` for time to first byte,
`ttft` for time to first token, and `ttft_tool` for time to first tool-call output.
A protocol terminal event is distinct from socket closure and application acceptance.
An HTTP success is not a correct answer. A retry is a new provider attempt, not
proof the preceding attempt did no work.

### 5.2 Tool-attempt microphases: Propose, Gate, Dispatch, Invoke, Return

| Phase | Meaning |
|---|---|
| `tool.propose` | Call reaches the governed entry point and gains attempt identity. |
| `tool.gate` | Apply applicable hooks, capability floor, policy, approval, budget and safety checks. Admission means applicable checks passed; report actual policy context. |
| `tool.dispatch` | Resolve the executable route and normalize input for that route. |
| `tool.invoke` | Enter the private leaf dispatcher, which performs lookup, validation and execution as applicable. Watchdogs and cooperative cancellation bound work. |
| `tool.return` | Record the leaf/gate outcome and return the result to the caller. Outcome verification remains separate. |

Input validation can occur at multiple boundaries. Do not assume every invalid
argument is rejected before admission or that every `started` call produced an
effect. Tool execution routes through `tools_execute_for_tier()`; the private leaf
is not an alternate public authority path.

Preserve the Execution Spine's actual state machine:

```text
proposed -> denied
proposed -> admitted -> failed                  # pre-leaf failure possible
proposed -> admitted -> started -> succeeded
                              \-> failed
```

`cancelled` and `effect_unknown` exist in the enum but are reserved in the
ordinary kernel lifecycle described by the Execution Spine. Recovery inspection
can project `effect_unknown` without appending that state. Failed effectful work
can have partial effects; an uncertain result requires reconciliation, not a
blind replay. The kernel's `succeeded` is not an independent acceptance verdict.

## 6. Optional orchestration branch: Split, Assign, Spawn, Monitor, Gather, Reduce, Reap

| Qualified phase | Spoken name | Completion condition |
|---|---|---|
| `swarm.split` | **Split** | Independent tasks, acceptance criteria and non-conflicting write scopes identified. |
| `swarm.assign` | **Assign** | Provider/model lanes, task ownership, budget, concurrency and deadlines specified. |
| `swarm.spawn` | **Spawn** | Child DSCO process created; its identity and launch outcome recorded. |
| `swarm.monitor` | **Monitor** | Observe process state, stream output, progress and resource use; do not equate output silence with death. |
| `swarm.gather` | **Gather** | Collect results and failures, preserving task identity and missing coverage. |
| `swarm.reduce` | **Reduce** | Reconcile contradictions and integrate selected evidence; Verify still evaluates the resulting outcome. |
| `swarm.reap` | **Reap** | Owned children terminal/reaped and results collected before slot reuse; deliberately detached work remains explicitly accounted for. |

This vocabulary applies to direct fan-out, map/reduce, races, topologies, and
provider-fabric strategies; it does not make every strategy run every phase.
"Reduce" here means synthesis, not a guarantee of a distributed shuffle engine.
A race's first successful worker is a candidate, not automatically an accepted
answer. A provider lane is a routing choice, not another runtime identity.
`swarm_spawn_executor()` routes every supported executor enum through native DSCO
spawn functions; legacy executor names select providers. Old enum comments about
external CLIs do not describe the current dispatch path.

## 7. Context branch: Recall, Pin, Evict, Compact, Rehydrate

| Qualified phase | Meaning |
|---|---|
| `context.recall` | Retrieve relevant saved material by supported lookup or content key. |
| `context.pin` | Mark material for retention where supported; not an unlimited context budget. |
| `context.evict` | Archive complete old tool exchanges and replace with bounded excerpts and recoverable references. |
| `context.compact` | Reduce in-window representation; not all compaction is lossless. |
| `context.rehydrate` | Restore needed archived evidence into the active context. Missing archives remain missing. |

A context archive is not a session checkpoint. A session checkpoint is not a
process image. A reconstructed conversation is not proof that external effects
were completed or undone. Keep existing `ck:<kind>:<id>` addresses; do not invent
new tool names for HX verbs. The live recall tool is `context_recall`.

## 8. Recovery branch: Detect, Diagnose, Reconcile, Recover, Reverify

```text
recover.detect -> recover.diagnose -> recover.reconcile
                                      |
           +--------------------------+------------------------+
           |                          |                        |
    retry / reroute           restore / requeue          pause / block
           |                          |
           +-------------> recover.reverify
```

- **Detect:** observe failure, overflow, timeout, interruption, or stale lease.
- **Diagnose:** distinguish malformed request, unsupported route, transient
  outage, exhausted budget, policy denial, and uncertainty about effects.
- **Reconcile:** inspect the recorded attempt and actual destination before any
  action that could duplicate an effect. Inspection does not authorize retry.
- **Retry:** another attempt at the same logical operation, only when safe and
  authorized. **Reroute:** an explicitly permitted alternative lane or approach;
  never silently discard a provider pin or bypass a denial.
- **Restore:** load persisted local state. **Requeue:** make interrupted work
  eligible again under a fresh fence/revision. Neither proves exactly-once effects.
- **Rollback:** restore a known earlier local state where a tested rollback exists.
  **Compensate:** perform a separate authorized effect to counter an earlier one.
  Neither is a universal automatic feature of the harness.
- **Reverify:** reevaluate the original acceptance criteria, not merely the retry's
  exit code. **Block:** record the concrete boundary and smallest missing input.

Existing generic retry/backtrack helpers, goal-queue restoration, Autobot key
reconciliation, and read-only execution-journal inspection are separate recovery
mechanisms. A universal durable attempt lease and automatic external-effect
reconciliation are not established by this map.

## 9. Exact states: keep the owner's vocabulary

| Owner | Existing state/transition vocabulary |
|---|---|
| Tool attempt / Execution Spine | `proposed`, `admitted`, `started`, `denied`, `succeeded`, `failed`; reserved ordinary-kernel terminals `cancelled`, `effect_unknown` |
| Execution layer receipt | `pending`, `denied`, `executed`, `failed`; separate `verified` boolean, not synonymous with `executed` |
| Goal | `none`, `active`, `paused`, `blocked`, `complete`, `budget_limited` |
| Goal-queue task | `unused`, `queued`, `active`, `waiting`, `complete`, `failed`, `blocked` |
| Swarm child | `pending`, `running`, `streaming`, `done`, `error`, `killed` |
| IPC task | `pending`, `assigned`, `running`, `done`, `failed` |
| Lingo platform Run | terminal predicate recognizes `succeeded`, `failed`, `cancelled`; `cancel_requested` is not terminal |

Interactive loop transition labels from `turn_transition_name()` are:

```text
continue: tool_results | compact_retry | max_output_escalate |
          max_output_recover | pause_resume
stop:     done | budget | interrupted | max_turns | error | exit_requested
```

These are names already in code, not a claim that every adapter emits each one.
Write `goal.state=complete` or `swarm_child.state=done`, not an unqualified
"complete" that conflates controller, process, application, and verified outcome.

## 10. Cross-cutting planes and existing names

| Plane | Responsibility | Existing DSCO vocabulary retained |
|---|---|---|
| `coordination` | Decomposition, scheduling, ownership, communication | Wings, goals, swarm, IPC |
| `execution` | Bounded pursuit, selection, attempts, recovery | Talons, agent loop, provider routes, tools |
| `authority` | Principal, capability, approvals, taint, denials | Immune, capability gate, trust tiers |
| `context` | Instructions, conversation, retrieval, disclosure | Context Fabric, session memory, `ck:` addresses |
| `evidence` | Attempt provenance, observations, causal records | Execution Spine, Chronicle, audit/event streams |
| `economics` | Token usage, estimates, reported costs, ceilings | TokenLedger, headless accounting, swarm accounting |
| `presentation` | Input/output, provisional streams, final delivery | Terminal, buffers, MCP/ACP responses, opted-in surfaces |

Wings/Talons/Immune are responsibility labels, **not chronological phases**.
Execution Spine is the governed attempt-lifecycle record; Chronicle is a recording
substrate, not an automatic verifier. The dated root `GLOSSARY.md` rename ledger
contains older capability and naming claims; HX does not apply its proposed
renames or revive filename-based restrictions. Current source and live gates
remain authoritative.

Use **budgeted**, **estimated**, **provider-reported**, and **settled** as different
cost qualifiers. Unknown cost is not zero; subscription execution still has usage.
Use **requested**, **selected**, and **reported** as different model qualifiers.
Use **claimed**, **observed**, and **verified** as different evidence qualifiers.

## 11. Adapter dialects: same vocabulary, different paths

| Mode | Typical path and important omission |
|---|---|
| Interactive conversation | Boot/Bind once; Serve repeats episodes and cycles; final Record returns to input wait. |
| One-shot/headless | Boot/Bind -> one bounded episode -> Drain/Halt. Goal continuation can include many cycles. |
| Direct tool or inbound MCP tool call | Intake/Frame -> Act's tool lifecycle -> protocol result/Record. No provider inference required. |
| ACP agent session | Protocol intake and session binding -> agent work -> protocol events/results. Do not equate ACP session IDs with every internal run ID. |
| Native worker | Parent assigns/spawns; child boots/binds and pursues its task; parent gathers/reaps. |
| Durable-agent activation | Persisted identity/task -> activation -> claim/fenced work -> record; later activation is a new process, not the old stack resumed. |
| Realtime voice | Audio capture/VAD -> input commit -> provider response -> audio/text playback; tool calls use Act. Barge-in interrupts output; it does not roll back effects. |
| Lingo calculation | Load definitions -> resolve value -> capture dependencies -> evaluate/type-check -> cache/invalidate. Pure calculation skips provider/tool execution. |
| Lingo effect or workflow | Explicit command -> governed tool lifecycle; Autobot owns workflow execution receipts. Scenario analysis alone does not submit work. |
| Inspection/status/metadata | Parse and read applicable state -> output. Skip full agent loop and avoid inventing a goal. |

The table above is descriptive. Optional adapters, persisted stores, accounting,
and presentation are active only when the selected mode/configuration wires them.

## 12. HX notation: one vocabulary, three writing densities

### 12.1 Speech

> "Frame the target, assemble the evidence, route once, then verify before delivery."
>
> "The worker is done; the task is not yet verified."
>
> "We are transport-complete but outcome-unknown. Reconcile before retry."
>
> "The cycle stopped on budget; the session is still open."

### 12.2 Compact technical prose

> `episode=E1; phase=cycle.act; tool_attempt.state=denied; gate=capability`
>
> `episode=E2; exec=succeeded; acceptance=unverified; delivery=not_observed`
>
> `episode=E3; phase=context.evict; next=context.rehydrate; reason=context_pressure`

These example IDs and fields are documentary, not observations from a real run.
`exec` summarizes an explicitly identified attempt; qualify its owner in real
multi-system reports. Do not fill absent IDs, costs, durations, or evidence with
invented values.

### 12.3 DSL-shaped notation

```text
phase       := lower_ident ('.' lower_ident)*
reference   := kind '#' id
value       := identifier | quoted_string | number | reference
properties  := '[' key '=' value (',' key '=' value)* ']'
node        := phase properties? | '(' expression ')'
sequence    := node ('>' node)*
expression  := sequence ('||' sequence)*
```

- `.` qualifies a phase: `tool.gate`, not an unrelated bare "gate".
- `#` refers to an entity: `episode#E1`, `tool_attempt#A7`.
- `[...]` annotates: `[state=denied]`. It neither tests a guard nor grants authority.
- `>` means ordered progression in the described trace or intended plan.
- `||` means explicitly independent concurrent branches; it does not itself
  prove independence or authorize parallel writes. `>` binds more tightly.
- Parentheses group branches. Label an expression **observed** or **planned**
  outside the expression; never present an intended sequence as an executed trace.
- Retry counts, budgets and constraints are descriptive properties in HX/1.
  There is no executable loop, policy override, or implicit retry operator.

Planned independent review:

```text
episode.frame[target="canonical checkout"] >
(swarm.assign[task="provider audit"] || swarm.assign[task="tool audit"]) >
swarm.gather > swarm.reduce > episode.verify > episode.deliver
```

Illustrative interrupted effect:

```text
tool.invoke[attempt=tool_attempt#A7] >
recover.detect[reason=timeout] >
recover.reconcile[effect=unknown] >
cycle.decide[action=block]
```

### 12.4 Lingo compatibility

HX/1 text is not Lua syntax. A Lingo program can instead carry the same vocabulary
as ordinary data, for example:

```lua
return {
  vocabulary = "hx/1",
  kind = "planned",
  episode = "E1",
  phases = {"episode.frame", "cycle.assemble", "cycle.route", "episode.verify"},
  acceptance = {state = "unverified"},
}
```

This declares data only: it does not execute these phases or register new platform
classes. A future executable binding needs explicit schema, authority, identity,
recovery and compatibility semantics; this document does not install one. Existing
Lingo `Task`, `Run`, `Observation`, and `Artifact` retain their current contracts.

## 13. Delivery and improvement extensions

When the requested outcome is software delivery, distinguish:

```text
delivery.propose -> delivery.change -> delivery.build -> delivery.test
                -> delivery.install -> delivery.observe
```

These are evidence milestones, not automatic permission to deploy. Use
`delivery.observe` for observed user-facing behavior of the actual installed target.
"Built", "tested", "installed", and "observed-running" are separate claims.
If work concerns improvement reuse, an optional later chain is:

```text
reuse.distill -> reuse.qualify -> reuse.promote -> reuse.distribute -> reuse.measure
```

Distillation creates a candidate reusable pattern. Qualification checks evidence.
Promotion and distribution require the applicable authority; neither follows
merely because a tool returned success. No ordinary question implicitly requests
an improvement campaign, policy change, or external publication.

## 14. Source concordance

These anchors locate mechanisms, not newly implemented HX symbols:

| Area | File and symbol/reference |
|---|---|
| Launch/modes | `src/main.c`: `main`, `main_runtime_profile`, `main_plan_startup_caps` |
| Interactive loop and final persistence | `src/agent.c`: `agent_run`, `autosave`, `turn_transition_name` |
| Conversations and provider telemetry | `include/llm.h`: `conversation_t`, `session_state_t`, `stream_result_t`, `stream_telemetry_t` |
| Assembly and context pressure | `src/llm.c`: `conv_trim_old_results`, `conv_auto_compact`; `src/agent.c`: model-loop request construction |
| Tool disclosure | `include/toolmgmt.h`: `toolmgmt_discover_tools`; `src/tools.c`: `tool_is_progressive_schema` |
| Provider transport | `include/provider_transport.h`: `provider_transport_perform`; `include/provider_events.h`: `provider_event_start`, `provider_event_body`, `provider_event_finish`, `provider_event_retry` |
| Governed attempts | `src/tools.c`: `tools_execute_for_tier`, `tools_execute_internal`; `include/execution_kernel.h`: `execution_attempt_t` |
| Execution receipt versus verification | `include/execution_layer.h`: `execution_receipt_t`, `execution_verifier_fn`, `execution_submit` |
| Goal and task states | `include/llm.h`: `dsco_goal_status_t`; `include/goal_queue.h`: `goal_queue_task_t`, `goal_queue_prepare`, `goal_queue_apply`, `goal_queue_load_fields` |
| Separate plan hierarchy | `include/plan.h`: `plan_t`, `step_t`, `atom_t` |
| Swarm ownership and states | `include/swarm.h`: `swarm_child_t`, `swarm_group_t`, `swarm_poll`, `swarm_kill`; `src/swarm.c`: `swarm_destroy`, `swarm_spawn_executor`, `swarm_group_reclaim` |
| Restored task fencing and hierarchy | `src/goal_queue.c`: `goal_queue_load_fields`, `bump_revision` |
| IPC and durable activation | `include/ipc.h`: `ipc_task_t`, `ipc_agent_info_t`; `include/durable_agents.h`: `durable_agents_wake` |
| Context retention | `include/context_eviction.h`: `context_evict`, `context_archive_recall`; `include/context_fabric.h`: `ctxkey_t`, `ctx_scope_create` |
| Recording and causality | `include/agent_event.h`: `agent_event_ctx_t`; `include/event_stream.h`: `event_stream_emit`, `event_stream_checkpoint`, `event_stream_close` |
| Uncertain-effect inspection | `include/execution_recovery.h`: `execution_recovery_report`; `docs/EXECUTION_SPINE.md` |
| Generic recovery helpers | `include/recovery.h`: `execute_with_retry`, `execute_with_fallback`, `backtrack_and_replay` |
| Protocol/voice adapters | `src/mcp_server.c`: `mcp_server_run`; `src/acp_server.c`: `acp_server_run`; `src/realtime.c`: `realtime_voice_run`, `rt_dispatch_tool_call` |
| Lingo and workflows | `src/lingo.c`: `lingo_cli`; `include/lingo_workflow.h`: `lingo_workflow_execute`; `docs/LINGO.md`; `docs/lingo/WORKFLOWS.md` |
| Optional local acceptance additions | `include/execution_contract.h`: `execution_contract_check`; `src/main.c`: `DSCO_VERIFY_COMMAND` handling; working-tree-only at inspection |

Related references: [Architecture](ARCHITECTURE.md),
[Execution Spine](EXECUTION_SPINE.md), [Goal Controller](GOAL_CONTROLLER.md),
[Lingo](LINGO.md), [Platform Objects](LINGO_PLATFORM.md),
[Workflow Semantics](lingo/WORKFLOWS.md), [Harness Control](HARNESS_CONTROL.md).

## Pocket lexicon

```text
PROCESS   boot / bind / serve / drain / halt
EPISODE   intake / frame / pursue / verify / deliver / record
CYCLE     assemble / route / infer / act / ingest / decide
TOOL      propose / gate / dispatch / invoke / return
SWARM     split / assign / spawn / monitor / gather / reduce / reap
CONTEXT   recall / pin / evict / compact / rehydrate
RECOVERY  detect / diagnose / reconcile / recover / reverify
```

**One sentence:** Boot and bind the instance; frame the episode; cycle through
assembly, routing, inference and governed action; ingest results, verify the
outcome, deliver and record; drain only when the instance is ending.
