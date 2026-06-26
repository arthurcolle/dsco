# Distributed Agent Performance Doctrine

This document is the implementation doctrine for making `dsco` a fast,
observable, local-first CLI for distributed agents. Private study notes from the
repo-local systems corpus live under `.workspace/book_index/` when present; this
tracked document contains only original engineering guidance and repo policy.

## Goal

`dsco` should feel instant for inspection commands, predictable for local tool
commands, and explainable for provider-backed agent commands. The runtime should
scale from one local prompt to many coordinated workers without hiding routing,
auth, startup, or failure behavior.

## Critical Paths

Optimize these paths separately:

- Info: `--version`, `--help`, `--models-json`, `--tools-json`
- Local tools: `--tool-exec`, `--tool-exec-raw`, core filesystem/status tools
- Route inspection: model, provider, auth mode, endpoint, fallback decision
- One-shot agent: prompt parse, provider selection, request build, first token
- Interactive agent: terminal setup, session restore, first usable prompt
- Worker: spawn argv, environment export, profile load, first heartbeat
- Orchestrated swarm: topology choice, worker launch, join, result merge

Each path should have a capability plan before startup. Loading tools, vector
stores, provider transports, TUI state, IPC, or memory tiers is only acceptable
when that path actually needs them.

## CLI Grammar

The command line is a grammar, not a string bag.

- Options consume exactly their operands.
- Prompt text can be positional or supplied by `-p`, `--prompt`, or `--p`.
- Prompt collection must not stop later model/provider options from binding.
- `--provider` forces the transport/provider, not the underlying model family.
- `-m` and `--model` bind model only.
- Dry-run inspection commands must show what execution would do before any
  provider stream starts.

## Provider Routing

Provider routing must be visible and stable:

- Preserve explicit native namespaces such as `openai/...`, `anthropic/...`,
  `moonshotai/...`, and `sakana/...`.
- Use `openrouter/...` only when explicitly requested or when policy allows
  fallback from an unnamespaced model.
- Prefer official subscription/executor lanes over private request-shape clones.
- Report auth mode without printing secrets.
- Report custom API-base use separately from credential presence.
- Treat local endpoints as providers with connection health, not as magic modes.

The user should be able to answer, without streaming: selected model, model
family, detected provider, route provider, override, auth mode, endpoint, and
whether fallback is active.

## Memory And Locality

The hot path should be small and contiguous:

- Use per-turn arenas for scratch allocations with uniform lifetime.
- Avoid repeated small allocations in parser, routing, and worker launch loops.
- Keep generated registries compact and size-budgeted.
- Prefer lazy loading of rarely used catalogs, embeddings, and integration data.
- Treat object size and BSS growth as performance regressions.

## Distributed Runtime

Worker and mesh behavior should follow network-system rules:

- Every task gets a correlation ID.
- Every worker has a lease and heartbeat.
- Every request has a timeout, retry policy, and cancellation path.
- Parent and worker agree on argv, profile, provider, model, and credential mode.
- Tool results are structured and bounded.
- Merge behavior is deterministic and auditable.

Hosted providers and remote accelerators are optional execution surfaces. Local
state, policy, credentials, audit logs, and recovery data remain authoritative.

## Error Discipline

Errors must carry enough context for action:

- operation
- provider route
- model family
- auth mode
- endpoint class
- retryability
- fallback decision
- reset time for rate limits or credits when available

Do not collapse missing API base, missing credentials, refused local connection,
billing exhaustion, rate limit, context overflow, and transport timeout into one
generic stream failure.

## Benchmark Contract

Every performance-sensitive change should be checked against:

- cold `dsco --version`
- cold `dsco --models-json`
- cold `dsco --tools-json`
- cold `dsco --route-explain MODEL`
- `dsco --tool-exec cwd '{}'`
- one-shot prompt request build
- worker profile startup
- swarm topology launch
- terminal teardown after interrupt

Use `DSCO_PERF=1` for human timelines and `DSCO_PERF=json` where automation
needs stable events.

## Implementation Checklist

- Does this command need full startup?
- Which capability bits does it require?
- Can it run before secure-store/keychain access?
- Does it allocate in a hot loop?
- Does it preserve provider namespace semantics?
- Does it print secrets?
- Does it expose enough route/error context?
- Is there a focused test or smoke for the path?
- Does it increase always-loaded registry size?
- Does it leave terminal state clean on exit and interrupt?
