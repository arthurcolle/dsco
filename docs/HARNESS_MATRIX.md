# Harness matrix

`bench/harness_matrix.py` runs the same task and deterministic graders against
multiple agent harnesses. It is designed for current DSCO native subscription
routes and for explicit external comparison adapters.

## What it records

Each attempt freezes:

- binary path, version and SHA-256;
- provider, model and reasoning effort when the adapter is DSCO;
- prompt identity and estimated prompt tokens;
- input, output, turn, time, money and captured-output budgets;
- workspace identities before and after execution plus added, removed and
  changed paths;
- stdout and stderr identities, process outcome and grader evidence;
- peak resident memory for the full child process group when host process
  accounting is available;
- DSCO input, output, cache and reasoning tokens, TTFT, latency, decode rate,
  connection timing, billing class and cost bases.

Results are committed after every state transition. If the runner stops after a
child starts but before its terminal record is written, the next invocation
marks the attempt `effect_unknown` and does not replay it. `--retry-unknown`
creates a new attempt identity and preserves the uncertain record.

## Fleet Mesh gate

An optional `fleet_mesh` manifest block names the Fleet Mesh JSON harness,
every expected node, the transport policy, and the minimum ready-node count.
Before any model call, the matrix asks Fleet Mesh to execute a fixed read-only
identity command on every target. It freezes the address-free readiness receipt
under `OUTPUT/fleet/`, links its digest from every attempt, and refuses to begin
when the minimum is not met. Static cloud inventory and renewable workstation
leases remain distinct; only a successful execution receipt marks a node ready.

The model harness process still runs in the attempt workspace on the controller.
The Fleet Mesh gate proves that the declared compute fabric is executable and
records its state alongside the comparison; it does not claim that a local
attempt ran on a remote node.

## DSCO native adapter

The `dsco_native` adapter constructs this route itself:

```text
dsco --profile worker --provider PROVIDER --model MODEL \
  --effort EFFORT --prompt PROMPT
```

It never accepts `-e`, `--exec` or an executor template. The runner passes
`DSCO_MAX_INPUT_TOKENS`, `DSCO_MAX_TOKENS`, `DSCO_HARD_TURN_CEILING`,
`DSCO_BUDGET`, and an isolated Chronicle directory. Set
`DSCO_HEADLESS_RECEIPT=1` to make a native one shot emit the same structured
response receipt without the matrix runner. The receipt contains metrics and
identities, not the prompt, response or credentials.

## External adapters

An `argv` adapter is an explicit process contract for Codex, Claude Code, omp,
Qwen Code, OpenCode, Hermes, mini-SWE-agent, or another harness. Arguments are
executed directly. No shell is used. Prompts may be supplied through one
`{prompt}` argv placeholder or through stdin. `{workspace}`, `{provider}`,
`{model}`, and `{effort}` are also available. Budget placeholders are
`{timeout_seconds}`, `{max_input_tokens}`, `{max_output_tokens}`, `{max_turns}`,
`{max_cost_usd}`, and `{max_output_bytes}`. The invocation receipt separates
limits enforced by the runner from limits passed to the child. A generic
adapter cannot claim a child-enforced money or token ceiling unless its argv
actually contains the corresponding placeholder.

Credentials must be named in `inherit_env`. Their values are inherited by the
child and never copied into the manifest, invocation receipt, or report. Literal
environment entries whose names look secret are rejected.

The example manifest includes disabled templates for Codex, Claude Code, omp,
OpenCode, Hermes, Qwen Code, and mini-SWE-agent. Disabled adapters stay out of a
default run but can be selected explicitly with `--adapter ID`. This keeps the
default smoke on DSCO's native subscription route while making every named
comparison an inspectable argv contract.

## Run

Validate and inspect the selected matrix without inference:

```sh
python3 bench/harness_matrix.py bench/harness_matrix.example.json \
  --output reports/harness-matrix-smoke --list
```

Run the enabled native lane:

```sh
python3 bench/harness_matrix.py bench/harness_matrix.example.json \
  --output reports/harness-matrix-smoke
```

Add adapters by copying the disabled `argv` example and pinning the exact binary
and invocation contract. Use task `adapters` allowlists when a task cannot be
run fairly by every harness. Reports from different manifests must remain
separate; an output directory refuses a changed manifest.

## Evidence layout

```text
OUTPUT/
  manifest.json
  selection.json
  events.jsonl
  results.jsonl
  summary.json
  fleet/readiness.latest.json
  fleet/readiness-TIMESTAMP.json
  state/ATTEMPT.json
  attempts/ATTEMPT/{invocation,result,workspace.*}.json
  attempts/ATTEMPT/{stdout,stderr}.txt
  attempts/ATTEMPT/graders/GRADER/
  workspaces/ATTEMPT/
```

`verified` requires a successful harness process and at least one passing
independent grader. A clean process exit with no graders stays `unverified`.
