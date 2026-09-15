# Agent-system interoperability

DSCO exposes one machine-readable interoperability contract and a bounded
operator bridge instead of treating every agent harness as a provider.

```sh
dsco interop manifest
dsco interop status --json
```

The manifest is versioned as `dsco.agent.interop/v1`. Its inbound surfaces are
DSCO's MCP server, ACP agent, and headless process entry point. Its outbound
surfaces are named CLI adapters and a generic direct-argv/stdin adapter.

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
If a child writes embedded NULs or invalid UTF-8, `output` remains valid JSON
and an exact `output_base64` field accompanies it.

These commands are explicit operator entry points. DSCO's goal controller and
swarm continue to spawn native DSCO workers, preserving the same capability
gate, accounting, and execution receipts for autonomous work.

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
make test-agent-interop-live
make test-agent-interop-live-invoke
```

The default target is provider-free. It verifies aliases, exact argv for all
five named adapters, generic argv/stdin, shell-literal prompts, failure exit
propagation, timeouts, output limits, binary output, MCP initialize/ping/tool
listing, and an ACP session through a fake DSCO child.

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
