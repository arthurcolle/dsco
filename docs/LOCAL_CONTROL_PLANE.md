# Local Control Plane for dsco

## Purpose

`dsco` already has good execution primitives:

- human-editable workspace state in [`workspace.c`](../src/workspace.c)
- session and tool telemetry in [`llm.c`](../src/llm.c)
- event and trace persistence in [`baseline.c`](../src/baseline.c)
- inter-agent state in [`ipc.c`](../src/ipc.c)
- swarm process orchestration in [`swarm.c`](../src/swarm.c)

What it does not yet have is a strong local control plane that learns per user, per project, per model, per topology, and per tool. That is the missing layer if `dsco` is going to become a genuinely optimized agentic CLI rather than just a capable tool-using chat loop.

This document defines the additional local data/configuration surfaces needed to support:

- fast static topology execution
- validated dynamic topology planning
- per-project model/tool routing
- cost and latency optimization
- safer autonomous execution
- better onboarding than Codex/Claude Code style systems

## Design Principles

1. Keep policy editable, keep telemetry queryable.
   Use Markdown or JSON for user-authored state. Use SQLite for learned runtime state.

2. Separate durable memory from transient execution state.
   Long-lived preferences should not be mixed with one noisy session.

3. Optimize by project, not just globally.
   The correct topology, model, and tool set depends heavily on repo shape and task mix.

4. Make routing explainable.
   If `dsco` auto-selects a topology or model, the decision should be reconstructable from local state.

5. Treat dynamic topologies as planned-and-validated graphs.
   Do not allow unconstrained graph generation with no stored validation context.

## Recommended Storage Layout

Use four layers of local state.

### 1. User-edited global config

Path:

- `~/.dsco/config.json`

Use for:

- default model family
- fallback chain
- default topology mode
- cost ceilings
- trust defaults
- preferred providers
- TUI defaults
- MCP/plugin preferences

This is the equivalent of a clean, intentional version of `~/.claude.json`, but narrower and more explainable.

### 2. Project-local policy and onboarding

Paths:

- `.dsco/project.json`
- `.dsco/skills/`
- `.dsco/policies/`

Use for:

- allowed and denied tools for this repo
- project trust tier
- topology allowlist
- default topology for common task classes
- MCP context sources relevant to the repo
- example files and hot paths
- coding conventions and build/test commands
- repo-specific budget guardrails

This should be checked into the repo when it represents team policy, and ignored when it is purely local.

### 3. Global learned control database

Path:

- `~/.dsco/control_plane.db`

Use for cross-project learned behavior:

- per-model cost/latency history
- topology success priors
- tool reliability priors
- provider failure patterns
- terminal performance history
- onboarding summaries for visited projects

This is where the system should store the kind of fields shown in the pasted Claude config:

- `lastCost`
- `lastAPIDuration`
- `lastToolDuration`
- `lastModelUsage`
- `lastSessionMetrics`
- `exampleFiles`
- trust/onboarding flags

### 4. Project runtime database

Path:

- `.dsco/runtime.db`

Use for local, high-cardinality, project-scoped runtime data:

- topology executions
- node runs
- per-tool outcomes in this repo
- repo graph snapshots
- prompt/cache fingerprints
- artifact lineage
- dynamic planner traces

This is the right place for topology-aware scheduling state. It should not live only in process memory.

## Core Data Families

### A. Project Policy

Needed fields:

- `trust_tier`
- `allowed_tools`
- `blocked_tools`
- `default_model`
- `default_topology`
- `topology_auto_mode`
- `max_parallel_agents`
- `max_depth`
- `budget_per_session_usd`
- `budget_per_day_usd`
- `required_checks`
- `preferred_build_commands`
- `preferred_test_commands`

Why it matters:

- static and dynamic topologies need hard safety rails
- tool freedom should vary by repo risk
- this is the policy layer above the current session-level toggles

### B. Project Fingerprint and Onboarding State

Needed fields:

- repo root
- git remote(s)
- dominant languages
- build system
- test commands
- framework tags
- important directories
- generated example files
- onboarding complete flag
- onboarding summary

Why it matters:

- topologies should be chosen partly from repo shape
- tool ranking should know whether this is a C repo, monorepo, docs repo, infra repo, and so on

### C. Model and Provider Routing Memory

Needed fields:

- model family
- provider
- task class
- prompt size bucket
- topology used
- success rate
- average TTFT
- average total latency
- tokens per second
- average output tokens
- average cost
- timeout rate
- fallback rate

Why it matters:

- current `dsco` tracks session telemetry, but not reusable routing priors
- the scheduler needs to know when Sonnet beats Opus for a class of task in a given repo

### D. Tool Reliability and Affinity Memory

Needed fields:

- tool name
- task class
- repo class
- success/failure counts
- timeout counts
- average latency
- cache hit rate
- average output size
- downstream usefulness score
- prompt-following score

Why it matters:

- model routing is only half of optimization
- topology choice should be tied to which tools actually work well locally

### E. Topology Registry and Runtime Performance

Needed fields:

- topology name
- topology category
- execution kernel
- allowed edge types
- required node roles
- recommended task classes
- average total cost
- average wall time
- average API time
- success rate
- abort rate
- retry rate
- human override rate

Why it matters:

- you need empirical evidence to decide when `triage` beats `assembly_line` or `expert_panel`
- dynamic topology selection should be data-backed

### F. Node-Level Execution History

Needed fields:

- topology run id
- node id
- node tag
- role
- tier
- chosen concrete model
- input size
- output size
- start/end timestamps
- status
- retry count
- predecessor ids
- produced artifacts

Why it matters:

- dynamic topologies need node-level observability
- this is the minimum useful substrate for adaptive scheduling and postmortem analysis

### G. Dynamic Planner State

Needed fields:

- planned graph spec
- validation result
- chosen template or generated graph
- budget at plan time
- rationale summary
- route confidence
- branch decisions
- stop conditions
- failure policy

Why it matters:

- “dynamic topology” should mean validated plan plus execution trace
- without persisted planner state, failures will be opaque and hard to replay

### H. Prompt and Cache Intelligence

Needed fields:

- system prompt fingerprint
- workspace prompt fingerprint
- skill prompt fingerprint
- tool schema fingerprint
- repo context fingerprint
- cache write/read tokens
- compaction count
- summary reuse count

Why it matters:

- the pasted Claude metrics show cache creation and cache read dominating cost
- dsco should optimize prompt construction and reuse with explicit local memory

### I. MCP and Plugin State

Needed fields:

- server name
- command
- startup latency
- failure rate
- tool count
- auth state
- last healthy timestamp
- project affinity
- preferred contexts

Why it matters:

- external tool surfaces are part of the routing problem
- topology planners need to know which MCP backends are worth using

### J. UI and Terminal Performance State

Needed fields:

- frame duration distribution
- slow render percentile
- render mode
- terminal type
- feature toggles
- throughput during long streams
- topology panel render cost

Why it matters:

- this is an area where advanced CLIs often feel slow or unstable
- topology rendering should adapt to terminal performance, not just exist

### K. Failure Memory

Needed fields:

- failure signature hash
- tool or model involved
- repo class
- suggested mitigation
- auto-disable rule
- last seen
- resolved flag

Why it matters:

- repeated failures should change behavior locally
- this is one of the easiest ways to become more sophisticated than a stateless CLI

## Recommended Schema Direction

Use JSON for editable configuration and SQLite for learned state.

Suggested SQLite tables:

- `projects`
- `project_policies`
- `project_onboarding`
- `repo_snapshots`
- `model_stats`
- `provider_stats`
- `tool_stats`
- `tool_chain_stats`
- `topology_templates`
- `topology_runs`
- `topology_node_runs`
- `dynamic_plans`
- `mcp_server_stats`
- `plugin_stats`
- `cache_stats`
- `failure_memory`
- `session_summaries`
- `ui_perf_stats`

Suggested editable JSON files:

- `~/.dsco/config.json`
- `.dsco/project.json`
- `.dsco/policies/tools.json`
- `.dsco/policies/topologies.json`
- `.dsco/policies/budgets.json`

## Minimum High-Value Additions Beyond Existing dsco State

If implementation time is tight, add these first.

### Phase 1: Fastest useful additions

1. `~/.dsco/config.json`
   Add global defaults for model family, fallback chain, topology mode, and budgets.

2. `.dsco/project.json`
   Add project trust, tool allowlist, default topology, build/test commands, and important paths.

3. `~/.dsco/control_plane.db`
   Add project summaries, model stats, tool stats, topology stats, and session summaries.

4. `.dsco/runtime.db`
   Add topology runs, node runs, and planner traces.

### Phase 2: Makes topology execution smarter

1. Persist per-topology win rates and cost/latency.
2. Persist per-node output envelopes and routing decisions.
3. Add repo fingerprinting and example-file generation.
4. Add failure memory and automatic guardrail adjustments.

### Phase 3: Makes dynamic topology practical

1. Add dynamic plan validation and storage.
2. Add budget-aware scheduling.
3. Add provider-aware tier resolution.
4. Add recovery and replay of interrupted topology runs.

## Concrete Example

For a C repo like `dsco-cli`, the project-local config should be able to answer:

- Is this repo trusted?
- Which tools are always allowed here?
- What are the preferred test/build commands?
- Should code review default to `expert_panel` or `code_review`?
- Should debugging default to `clinic`?
- When should Sonnet be preferred over Opus?
- Which files are useful onboarding anchors?
- What was the cheapest successful topology for similar changes last week?

That is the level of local memory needed to make `dsco` feel optimized instead of generic.

## Recommended Implementation Sequence

1. Add `~/.dsco/config.json` and `.dsco/project.json`.
2. Add `control_plane.db` with `projects`, `model_stats`, `tool_stats`, `topology_runs`, and `session_summaries`.
3. Wire session closeout to persist a normalized summary, not just baseline events.
4. Add topology execution persistence into `.dsco/runtime.db`.
5. Add `/topology`, `/route`, and `/state` surfaces to inspect and override decisions.
6. Add dynamic planner validation only after static topology metrics are real.

## Bottom Line

To outperform Codex- or Claude Code-style systems locally, `dsco` needs to remember more than chat history and tool timings. It needs a real control plane:

- editable local policy
- learned routing memory
- topology performance history
- repo fingerprints
- failure memory
- provider and tool health
- node-level execution traces

Without that layer, dynamic topologies will be expensive theater. With it, they become an optimization system.
