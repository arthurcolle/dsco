# Agent-system interoperability

For native and external harnesses in one parent-child dependency tree, see
[Cross-provider swarm hierarchies](SWARM_HIERARCHY.md).

DSCO exposes one machine-readable interoperability contract and a bounded
operator bridge instead of treating every agent harness as a provider.

```sh
dsco interop manifest
dsco interop status --json
```

The manifest is versioned as `dsco.agent.interop/v1`. Its inbound surfaces are
DSCO's MCP server, ACP agent, and headless process entry point. Its outbound
surfaces are named CLI adapters and a generic direct-argv/stdin adapter. The
`run_receipt` section declares the shared process outcomes and measurement
units for coordinators that compare executors.

## Named adapters

The initial registry covers the installed harnesses reviewed by the harness
radar:

| Adapter | One-shot process | ACP command | DSCO MCP client |
| --- | --- | --- | --- |
| `codex` | yes | through a separately installed ACP adapter | yes |
| `claude-code` / `claude` | yes | through a separately installed ACP adapter | yes |
| `opencode` | yes | `opencode acp` | yes |
| `omp` / `oh-my-pi` / `pi` | yes | `omp acp` | yes |
| `hermes` | yes | version-dependent | yes |

`status` proves executable discovery. It does not claim that an account is
authenticated or that a remote provider is healthy.

Run a named harness through its non-interactive interface:

```sh
dsco interop run opencode --prompt 'review this checkout' --cwd "$PWD" --json
dsco interop run omp --prompt 'find the failing test' --model opus --cwd "$PWD"
dsco interop run hermes --prompt 'summarize the repository'
```

Arguments are passed directly to the executable. DSCO never builds a shell
command from the prompt. The bridge uses a private process group, closed stdin,
a ten-minute default deadline, and an 8 MiB output bound. `--timeout-ms` and
`--max-output` can tighten those bounds. `--json` returns one
`run.completed` envelope with exit, timeout, truncation, and output fields.
The provider-neutral `outcome` is `success`, `exit_error`, `timeout`, or
`spawn_error`; it describes process execution, not the meaning of a provider's
answer. The receipt also reports `duration_ms` measured with a monotonic clock
around the child run and `captured_bytes` for the child output bytes retained
before JSON encoding. These fields use the same units for every adapter;
`captured_bytes` is capped by
`--max-output` when `truncated` is true. A clock failure reports `-1` for
`duration_ms`.
If a child writes embedded NULs or invalid UTF-8, `output` remains valid JSON
and an exact `output_base64` field accompanies it.

The `interop run` command is also the process boundary used by registered
external workers in the governed `swarm` tool. Native DSCO workers remain the
default executor.

## Mixed swarms

Select a named harness explicitly with `executor: "interop:<adapter>"` in
`swarm spawn_executor` or a `create_executor_swarm` task object. The named
registry is the allowlist; `generic` commands cannot be spawned by the swarm.

```json
{"action":"create_executor_swarm","name":"harness-study","run_timeout_ms":600000,
 "tasks":[{"task":"compare protocol behavior","executor":"interop:codex"},
          {"task":"compare protocol behavior","executor":"interop:opencode"},
          {"task":"summarize local evidence","provider":"openai-codex","model":"gpt-6-astra"}]}
```

The outer tool call passes through `tools_execute_for_tier()`. Each adapter is
then a normal swarm child with a group ID, monotonic per-session `run_id`,
process ID, start/output/exit events, status, collect, abort, and a durable
result envelope. `interop_outcome` distinguishes `success`, `exit_error`,
`timeout`, `spawn_error`, and swarm `cancelled`; a missing or malformed receipt
is `invalid_receipt`. The TUI shows adapter runs alongside native workers and
marks adapter timeouts as terminal failures. `run_timeout_ms` defaults to ten
minutes and is independent of the collect window.
Group launch, status, and collect responses include `group_generation`. Pass
that string with `group_id` to `abort`, `collect`, or `spawn_executor` when
adding to an existing group. Those controls reject a missing or stale token,
including after a numeric group slot is reused or DSCO restarts. `status` can look up the
current generation by group ID alone; if supplied a stale token, it rejects
the request. A generation identifies a group within this DSCO runtime and is
distinct from a child `run_id` or durable run artifact ID.
`agent kill` also requires the current `group_generation` for a grouped
worker. Its optional numeric `run_id` must match the current worker; malformed
or stale values are rejected. Legacy ungrouped native workers still accept
ID-only kill requests. A successful kill receipt includes the child `run_id`.
Every backend retains `run-<run_id>.RESULT.json` beside the compatible
child-slot result file, so slot reuse preserves prior receipts. These private
files use `dsco.swarm_result.v2` and include executor, confirmed reaping, and
cancellation/escalation evidence. Unmeasured cost is null, with reported,
estimated, and budget accounting carried separately.
Completed receipts remain readable after an orderly DSCO restart; this does
not resume running workers. Identity is scoped to the owning process directory
and its `run_id`, not a globally unique ID. A reused OS PID can revisit an old
directory; receipt hard links are never replaced on collision.

The top-level `cwd` applies to registered workers in both executor actions.
DSCO resolves relative paths before spawning and rejects missing paths and
files. Output capture reserves space for JSON escaping and a binary base64
copy under `DSCO_SWARM_MAX_OUTPUT`; raw output can be truncated while its outer
completion receipt remains valid. A deadline is a failure even if a TERM
handler exits with code zero.

On macOS and Linux, abort and deadline cleanup stop the owned root and walk
its still-attached descendants, including children in separate process groups,
before killing descendants and releasing the root. Enumeration is bounded to
16 levels (and 256 children per parent on macOS). A process already orphaned
before cleanup is outside this tree; this is not a hostile-process sandbox.
Receipt files use checked writes, file sync, exclusive hard-link publication,
and checked directory sync. The mutable child-slot alias uses atomic rename.
Status, output, collect, and kill replies expose `reaped`,
`cancellation_requested`, `forced_kill`, `result_persisted`, `result_errno`,
and `result_path`. A published file with a failed directory sync retains its
path but reports `result_persisted:false`; it must not be treated as durable.
Collisions and storage errors do not overwrite prior immutable receipts.
The in-memory result remains available when persistence fails. Offline tests
cover orderly restarts; power-loss recovery is untested.

Group replies include `lifecycle` with live process, cancelling, reaped,
persisted-result, and persistence-error counts. `shutdown_complete` means no
owned worker remains in the active reaping set. It does not establish successful
task completion or prove that an already-orphaned descendant has stopped.
Explicit abort retains `complete:false` and returns this separate shutdown
state; callers can inspect the same group again if cleanup is still pending.
The `cancellation` object reports TERM requests, forced kills, poll errors,
and measured elapsed milliseconds for the latest abort.

Group abort and individual kill share a cancellation engine. The requested
grace uses a monotonic elapsed-time deadline, followed by a bounded two-second
reap window after escalation. Unrelated ready pipes cannot shorten the grace.
Killed workers keep their output pipes in the reactor while shutting down, so
termination handlers can flush output larger than a pipe buffer. End time is
recorded on actual reap. Repeated abort or kill of the same confirmed cancelled
run succeeds without sending more signals; generation and run fences still
apply. Events use `abort.pending` while owned workers remain active and
`abort.completed` after reap. Descendant enumeration, stream callbacks and
filesystem sync can extend the elapsed time beyond the wait deadlines; these
are bounded wait windows rather than hard real-time guarantees.
The event stream also distinguishes `worker.exit` from
`worker.result.persisted` or `worker.result.persistence_failed`.

Run `make test_swarm_lifecycle test_swarm_progress test-agent-interop
test-gate-claims` for offline process, persistence, MCP, and governance checks.

The worker receives a scrubbed environment: inherited provider credentials
and authority override flags are removed. The harness may use its own local
login. Its internal tools remain governed by that harness. External CLI usage
is unpriced in DSCO accounting, so dollar-budgeted sessions and groups reject
these workers rather than claiming a bound they cannot enforce. The result
envelope reports `cost_usd: null` for them.
Persisted swarm workers also report `billing_class: "unknown"`; installing or
logging into a harness does not establish its billing lane.

## Generic adapter

Any agent with a process interface can join without a DSCO release. Put the
prompt on stdin:

```sh
dsco interop run generic --prompt 'inspect the tree' --stdin --json -- agent-binary --batch
```

Or place exact tokens in argv. `{prompt}`, `{model}`, and `{cwd}` are replaced
only when an argument equals the whole token. If `{prompt}` is absent, DSCO
appends it as the final argument.

```sh
dsco interop run generic \
  --prompt 'inspect the tree' --model model-id --cwd "$PWD" --json -- \
  agent-binary run --model '{model}' --workspace '{cwd}' '{prompt}'
```

There is no shell interpolation. Quotes, semicolons, `$()`, and newlines remain
prompt bytes.
`--cwd` accepts a relative path from the invoking directory. DSCO enters that
directory and passes its absolute resolved path to provider cwd flags and
`{cwd}`, so an adapter cannot accidentally resolve the path a second time.

## Connect another harness to DSCO tools

Generate a standard MCP process configuration:

```sh
dsco interop mcp-config json --tier trusted
dsco interop mcp-config toml --tier trusted
```

The generated command starts `dsco mcp serve`. Every call entering that server
still reaches `tools_execute_for_tier()`, so exposure does not bypass capability
or governance decisions. To make DSCO the authority layer under another
harness, enable the DSCO MCP server there and restrict that harness's competing
local tools.

ACP clients can launch DSCO directly:

```sh
dsco interop acp-command
```

Inspect a native peer command without starting it:

```sh
dsco interop acp-command opencode
dsco interop acp-command omp
```

The generic process bridge does not govern tools implemented inside the child
harness. Those actions remain subject to that harness's own permission and
sandbox model. Use MCP when DSCO must own the tool decision and execution
receipt.

## Verification

```sh
make test-agent-interop
make test_swarm_scale_mcp
make test-agent-interop-live
make test-agent-interop-live-invoke
```

The default target is provider-free. It verifies aliases, exact argv for all
five named adapters, generic argv/stdin, shell-literal prompts, failure exit
propagation, timeouts, output limits, binary output, MCP initialize/ping/tool
listing, an ACP session through a fake DSCO child, and a mixed native/external
swarm using mock CLIs and a loopback provider. The swarm fixture checks
identity, outcomes, credential isolation, and cancellation without paid calls.

The live target checks the installed versions and help contracts for Codex,
Claude Code, OpenCode, OMP, and Hermes, including native ACP entry points. It
does not call a model. The live-invoke target additionally sends one minimal
marker prompt through every available harness and fails unless every child
returns a successful DSCO receipt containing the marker. Failures are classified
as adapter/receipt, timeout, authentication, rate-limit, or billing failures;
the receipt marks provider and account failures as external blockers without
persisting raw model output.

`scripts/agent_interop_conformance.py` emits
`dsco.agent.interop.conformance/v1` JSON with the DSCO binary hash and per-peer
results. The 15-minute harness radar runs its provider-free mode after every
source refresh and writes `reports/harness-radar/interop-conformance.json`.
A manually invoked real-task run can be retained separately as
`reports/harness-radar/interop-conformance-live.json`.


## Capture cancellation and observed outcomes

The synchronous `interop run` command scopes SIGINT/SIGTERM handling around its
capture and restores the caller's handlers/mask. Previously ignored/blocked
signals retain that behavior; custom handlers are notified after cleanup. Default
cancellation returns 128+signal after emitting a receipt. Repeated cancellation
requests do not restart cleanup. General capture callers install no signal state;
they may supply their own cancellation latch through the optional capture API.

Cleanup targets the owned direct child/private group, with existing descendant
inspection while the owned root is alive on macOS/Linux. TERM has 250ms before
KILL, followed by 100ms bounded drain/reaping; no blocking final waitpid. These are
soft scheduling bounds, not OS real-time guarantees. Detached descendants that
reparent before inspection, SIGKILL of DSCO, and escaped orphan processes cannot
be guaranteed cleaned. There is no global process sweep. `cleanup_complete`
proves only observed direct-child reaping, private-group disappearance and EOF.
Unknown cleanup yields `outcome:incomplete`, `ok:false`, and an `unreaped_pid`
when applicable. `escaped_orphans`, external side effects, and billing remain
unverified/unreconciled even when owned cleanup completes.

Receipts expose process, transport, provider, route and semantic outcomes
separately. Provider completion is not semantic acceptance or route verification.
Built-in Codex JSONL turn/error events and Claude final-result `is_error` fields
have adapter-specific parsing; explicit provider failure produces `provider_error`
even with exit 0, preserving its raw JSON event and capture output. Missing,
malformed, interrupted, or truncated terminal evidence cannot prove completion.
Failures remain visible despite later missing/truncated output. No string-matching
heuristics establish success or infer failure classes such as authentication.

OpenCode, OMP, Hermes and configured subscription paths keep their existing argv
and execution support. Where no terminal-output schema is implemented, provider
outcome stays unknown and named runs produce `incomplete`/nonzero. No undocumented
schema is invented. Generic execution remains an explicitly process-only path:
its legacy `success`/`ok` means clean process completion, with provider outcome
unknown and semantic acceptance unverified. This is an intentional tightening of
named-run receipts; callers must handle `cancelled`, `incomplete`, `provider_error`
and unknown fields rather than treating process exit as accepted work.

Offline regressions: `make test-interop-reliability`, `make test-agent-interop`,
and `make test-process-capture-lifecycle`; none invokes subscription inference.
