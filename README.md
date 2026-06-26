<h1 align="center">DSCO</h1>

<p align="center">
  <strong>A local-first, self-introspecting agent runtime written in C.</strong>
</p>

<p align="center">
  <a href="https://github.com/arthurcolle/dsco/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/arthurcolle/dsco/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/arthurcolle/dsco/actions/workflows/docs.yml"><img alt="Docs" src="https://github.com/arthurcolle/dsco/actions/workflows/docs.yml/badge.svg"></a>
  <a href="https://github.com/arthurcolle/dsco/actions/workflows/security.yml"><img alt="Security" src="https://github.com/arthurcolle/dsco/actions/workflows/security.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-blue.svg"></a>
</p>

<p align="center">
  <a href="docs/INDEX.md">Docs</a> ·
  <a href="docs/TOOL_CATALOG.md">Tool Catalog</a> ·
  <a href="docs/ARCHITECTURE.md">Architecture</a> ·
  <a href="docs/OPERATIONS.md">Operations</a> ·
  <a href="SECURITY.md">Security</a>
</p>

---

**Dsco** is a tool-first agentic CLI
that lives where serious work already happens: your terminal, your filesystem,
your git repos, your local state.

It is **not** a thin wrapper around a model API. DSCO is a native runtime with its
own tool registry, hierarchical sub-agents, local provenance, provider routing,
AST-level code intelligence, streaming pipelines, governance primitives, and a
portable build lane. It can inspect, edit, test, and document the codebase it is
running from.

If cloud-hosted agents optimize for convenience, DSCO optimizes for **sovereign
control**: local execution, explicit tools, auditable traces, reproducible builds,
and no mandatory hosted control plane.

## Contents

- [Why DSCO](#why-dsco)
- [At a glance](#at-a-glance)
- [Runtime limits and operating constants](#runtime-limits-and-operating-constants)
- [Quick install](#quick-install)
- [First run](#first-run)
- [Provider and model routing](#provider-and-model-routing)
- [What you can do with DSCO](#what-you-can-do-with-dsco)
- [CLI quick reference](#cli-quick-reference)
- [Core capabilities](#core-capabilities)
- [Tool system](#tool-system)
- [Critical integrations and special factors](#critical-integrations-and-special-factors)
- [MetaConstruct DSL](#metaconstruct-dsl)
- [Architecture](#architecture)
- [Agent orchestration](#agent-orchestration)
- [Topology catalog](#topology-catalog)
- [Chronicle and local observability](#chronicle-and-local-observability)
- [Integrations universe](#integrations-universe)
- [Configuration and storage](#configuration-and-storage)
- [Documentation map](#documentation-map)
- [Build, packaging, and release lanes](#build-packaging-and-release-lanes)
- [Development](#development)
- [Repository layout](#repository-layout)
- [Troubleshooting](#troubleshooting)
- [Security](#security)
- [Contributing](#contributing)
- [License](#license)

## Why DSCO

<table>
<tr>
<td><strong>Local-first by default</strong></td>
<td>Runs from your terminal. Project state, traces, setup profiles, tool metadata, and Chronicle activity records live on your machine unless you deliberately export them.</td>
</tr>
<tr>
<td><strong>Tool-first runtime</strong></td>
<td>172 built-in tools plus MCP/plugin-provided tools for files, git, shell, code intelligence, data pipelines, crypto, providers, integrations, markets, observability, and orchestration.</td>
</tr>
<tr>
<td><strong>Self-introspecting C system</strong></td>
<td>AST tooling, call graphs, module references, generated API docs, and source-aware editing let DSCO reason about and patch its own implementation.</td>
</tr>
<tr>
<td><strong>Hierarchical agents</strong></td>
<td>Swarm orchestration, topology selection, worker profiles, and multi-executor routing support parallel workstreams instead of one long serial chat loop.</td>
</tr>
<tr>
<td><strong>Governed autonomy</strong></td>
<td>OODA discipline, kill switches, resource budgets, principal tiers, confirmation policy for mutating integrations, and auditable local traces.</td>
</tr>
<tr>
<td><strong>Integration-universe aware</strong></td>
<td>Models the full Codex app directory as a 1,887-entry integration universe: cataloged, installed, live, stale, OAuth-gated, mutating, sync-capable, or inaccessible.</td>
</tr>
<tr>
<td><strong>Portable distribution path</strong></td>
<td>Native macOS remains the full-feature target; the Cosmopolitan/APE lane builds <code>dsco.distributed.systems</code> for portable smoke tests and distribution work.</td>
</tr>
</table>

## At a glance

| Property | Current value |
|---|---:|
| Version | `1.0.2` |
| Runtime | Native C, default build standard `c2y` |
| Source size | ~353K LOC across `src/` + `include/` |
| Source files | 118 `.c` / 119 `.h` |
| Built-in tools | 172 generated from `src/tools.c` |
| Integration universe | 1,887 Codex app catalog entries when the current directory cache is present |
| Integration statuses | cached, installed, connected, live, inaccessible, stale, OAuth-gated, mutating, sync-capable |
| Generated docs | API reference, built-in tool catalog, external integration catalog, constants/env index, repo coverage manifest |
| Observability | Baseline SQLite timeline + Chronicle activity ledger |
| Orchestration | Hierarchical swarms, topology runner, worker profiles |
| Portable lane | Cosmopolitan/APE `dsco.distributed.systems` via `make cosmo` |
| License | MIT |

Counts are ground-truthed against the source tree. After changing tool
registrations, external integration cache, headers, constants, env usage, or
tracked repo layout, run `make docs` and commit the generated docs.

### Capability surface by numbers

| Surface | Scale | Why it matters |
|---|---:|---|
| Codex app catalog | 1,887 apps | DSCO can reason over an integration universe before an MCP server is installed or authenticated. |
| Built-in tools | 172 | Local work can proceed without waiting for external connectors. |
| Source files | 237 C/header files | The runtime is inspectable and patchable as a concrete systems codebase. |
| Source size | ~353K LOC | DSCO is a runtime with subsystems, not a prompt wrapper. |
| Generated references | 6 artifacts | API, built-in tools, external integrations, constants/env, repo coverage, and machine-readable indexes keep docs tied to code. |
| Integration profiles | 5 default bundles | engineering, GTM, finance, enterprise knowledge, governed agent runtime. |
| Action-risk flags | 8 classes | read, write, send, delete, admin, untrusted content, confirmation-required, interactive. |
| Local observability stores | 2 primary ledgers | Baseline timeline for UI/events; Chronicle for full-fidelity activity/provenance. |

## Runtime limits and operating constants

DSCO exposes many limits as named constants and generated docs rather than hiding
operational policy in prompts. The values below are the high-signal defaults a
new operator should know first.

| Constant / surface | Default / scale | Operational meaning |
|---|---:|---|
| Tool register cap | 32 tools/request | DSCO treats tool schemas like CPU registers: keep a bounded active set and page in more via discovery. |
| Always-core tool slots | 7 | Shell/Python/discovery/load/exit/loop-style primitives stay resident. |
| Cheap mode | ~5 core tools + discovery | `-C` / `DSCO_CHEAP=1` minimizes first-turn schema cost and lets the model load tools on demand. |
| Max tools in compiled config | 512 | Built-in/request-side hard cap, separate from external MCP catalog size. |
| MCP servers | 64 | Upper bound for configured MCP server processes. |
| MCP tools | 2,048 | Upper bound for MCP-discovered tools before profile/dynamic loading. |
| MCP line size | 256 KiB | JSON-RPC stdio line bound for MCP transport. |
| Default model alias | `fugu` | Local default in `include/config.h`; can be overridden with `-m` or `DSCO_MODEL`. |
| Max output reserve | 16,384 tokens | Default output token reserve; `DSCO_MAX_TOKENS` can override. |
| Agent checkpoint cadence | 40 turns | `MAX_AGENT_TURNS` is a progress checkpoint cadence, not the primary stop wall. |
| Hard turn ceiling | 100,000 | Emergency runaway backstop; normal stops are budget/context/interrupt/no-progress. |
| Context-window constant | 2,000,000 tokens | Runtime accounting constant for context management and compaction thresholds. |
| Auto-compact threshold | 80% effective window | Prevents compaction from happening after output reservation makes it too late. |
| Tool result cap | 128 KiB | Keeps tool outputs bounded before summarization/offload. |
| Request/response buffers | 512 KiB each | Native request/response construction bounds. |
| GSU costs | tool=1, LLM=10, spawn=5 | Governance accounting distinguishes cheap operations from high-impact ones. |

The exhaustive generated reference lives at
[`docs/CONSTANTS_ENV_INDEX.md`](docs/CONSTANTS_ENV_INDEX.md) and the machine
index at `data/constants_env_index.json`.

## Quick install

### npm

```bash
npm install -g @distributed.systems/dsco
dsco --version
```

Or run without a global install:

```bash
npx @distributed.systems/dsco --version
```

The npm package downloads the native DSCO binary from GitHub Releases. No Python
runtime is required.

### Homebrew

```bash
brew install arthurcolle/dsco/dsco
dsco --version
```

### Build from source

Prerequisites:

- macOS primary development environment
- Recent `clang`/`gcc` with `-std=c2y` support
- `make`
- `pkg-config` plus optional native libraries detected by the Makefile

```bash
git clone https://github.com/arthurcolle/dsco.git
cd dsco
./scripts/bootstrap.sh
make -j8
make test
```

Smoke test the local binary:

```bash
./dsco --version
./dsco --tool-exec cwd '{}'
```

### Portable APE build

```bash
make cosmo-bootstrap
make cosmo
make cosmo-selftest
./dsco.distributed.systems --version
ape ./dsco.distributed.systems --version      # macOS zsh / Apple Silicon
/bin/sh ./dsco.distributed.systems --version  # fallback without installed ape
```

See [`docs/COSMOPOLITAN.md`](docs/COSMOPOLITAN.md) for the portable build lane,
toolchain pin, and native-dependency boundaries.

## First run

Choose or configure a provider:

```bash
./dsco login          # interactive Claude Code / Codex backend login
./dsco status         # show auth state and account info
./dsco --setup        # persist detected provider keys into the DSCO env file
./dsco --setup-report # show masked setup/config status
```

Or provide a key for one run:

```bash
ANTHROPIC_API_KEY=... ./dsco "summarize this repository"
OPENROUTER_API_KEY=... ./dsco -m openrouter/anthropic/claude-sonnet-4 "review src/tools.c"
FUGU_API_KEY=... ./dsco -e fugu "write a release checklist"
```

Common entry points:

```bash
./dsco -i                                      # interactive terminal session
./dsco "write and compile a hello-world C program"
./dsco --local "use a local model if configured"
./dsco --topology-list                         # list agent topologies
./dsco --topology-auto "parallelize this audit"
./dsco --timeline-server --timeline-port 8421  # local observability server
```

## CLI quick reference

| Task | Command |
|---|---|
| Print version | `dsco --version` |
| Interactive REPL | `dsco -i` |
| One-shot prompt | `dsco "inspect this project"` |
| Login to managed backends | `dsco login` or `dsco --login` |
| Show auth/config status | `dsco status` / `dsco --setup-report` |
| Force provider/backend | `dsco -e claude`, `dsco -e codex`, `dsco --provider openrouter` |
| Use cheap/core-tool mode | `dsco -C "task"` |
| Execute a local tool directly | `dsco --tool-exec cwd '{}'` |
| List topologies | `dsco --topology-list` |
| Auto-select topology | `dsco --topology-auto "task"` |
| Launch web UI | `dsco --ui 3141` |
| Serve local timeline | `dsco --timeline-server --timeline-port 8421` |
| Regenerate docs | `make docs` |
| Verify generated docs and coverage | `make docs-check` |

For the full command surface, run `dsco --help` and see
[`docs/OPERATIONS.md`](docs/OPERATIONS.md). For a broad command cookbook, see
[`docs/EXAMPLES.md`](docs/EXAMPLES.md).

## Provider and model routing

DSCO can run against native provider transports, external CLI backends, and local
model servers. You can pin a backend explicitly, let DSCO infer from model/key
shape, or use the orchestrator/topology layer to route work to specialist workers.

| Path | Use when | Example |
|---|---|---|
| Native provider | You want DSCO to call provider APIs directly. | `dsco --provider openrouter -m openrouter/anthropic/claude-sonnet-4 "review this"` |
| External Claude Code | You want to use Claude Code auth/session machinery. | `dsco -e claude "fix the failing test"` |
| External Codex | You want OpenAI Codex CLI compatibility. | `dsco -e codex "inspect this module"` |
| Auto/smart executor | You want DSCO to choose a practical execution path. | `dsco -e auto "summarize the repo"` |
| Local model | You want local inference where configured. | `dsco --local "run a small local task"` |
| Fugu/Sakana | You want the configured Fugu/Sakana provider path. | `FUGU_API_KEY=... dsco -e fugu "draft a release note"` |

Important environment variables:

| Variable | Purpose |
|---|---|
| `DSCO_EXEC` | Default executor/provider (`claude`, `codex`, `auto`, `fugu`, `sakana`, etc.). |
| `DSCO_MODEL` | Default model override. |
| `DSCO_PROFILE` | Startup profile: `full`, `lite`, or `worker`. |
| `DSCO_ENV_FILE` | Override saved setup env-file path. |
| `DSCO_BUDGET` / `DSCO_DAILY_BUDGET` | Session/daily cost budget controls. |
| `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`, `FUGU_API_KEY` | Common provider credentials. |

Use `./dsco --setup-report` to inspect masked credential/config state without
printing secrets.

## What you can do with DSCO

| Workflow | Example prompt/command | What DSCO uses |
|---|---|---|
| Patch a C module | `dsco "fix the parser bug in src/json_util.c and run the tests"` | code search, file edits, compile/test tools, git diff awareness |
| Audit a repository | `dsco --topology-auto "find high-risk files and summarize remediation"` | topology selection, sub-agents, grep/AST, synthesis |
| Generate durable docs | `dsco "update docs for the new integration catalog behavior"` | file writes, generated-doc checks, Markdown references |
| Investigate runtime state | `dsco --timeline-server --timeline-port 8421` | Baseline SQLite, Chronicle, local web surfaces |
| Work cheaply | `dsco -C "just inspect the filenames and tell me where to start"` | cheap/core tool profile, discovery-loaded tools |
| Query local/project facts | `dsco --tool-exec grep_files '{"pattern":"chronicle_start","path":"src"}'` | direct tool execution with JSON inputs |
| Analyze market contracts | `dsco "search Kalshi for BTC contracts expiring this week"` | prediction-market tools, contract search/lookup |
| Validate integrations | `dsco --tool-exec dsco_doctor_integrations '{}'` | Codex app directory, live MCP metadata, governance flags |

The intended loop is: **ask → inspect → edit → verify → report**. DSCO should
prefer concrete filesystem changes and command output over vague narration.

## Core capabilities

| Area | What DSCO provides |
|---|---|
| Code intelligence | AST introspection, call graphs, dependency analysis, generated API/module references, in-place source edits. |
| Tool execution | Built-in local tools, external MCP registration, plugin tools, direct tool execution, schema validation, timeouts, watchdogs. |
| Data pipelines | Coroutine-based streaming transforms via the `pipeline` tool and JSON/text processing utilities. |
| Orchestration | Swarms, worker profiles, dynamic topology selection, multi-executor routing, IPC-backed coordination. |
| Integrations | Codex app-directory metadata, MCP discovery, integration doctor, confirmation policy for mutating connectors. |
| Providers | Native and external provider routing across Claude/Codex/OpenRouter/Fugu/Sakana/OpenAI-compatible/local paths. |
| Observability | Baseline timeline, trace spans, Chronicle activity ledger, local timeline web server, generated operational references. |
| Security | Sealed-store paths, tamper/security modules, kill switches, explicit env policy, sensitive-output handling. |
| Crypto | Pure-C SHA-256, MD5, HMAC-SHA256, HKDF, UUID, JWT parsing, constant-time equality helpers. |
| Distribution | Native binary, npm/Homebrew packaging, Cosmopolitan/APE portable artifact lane. |

## Tool system

The tool system is the center of DSCO. Models are planners and synthesizers; the
runtime is responsible for concrete work.

### Tool classes

| Class | Examples | Notes |
|---|---|---|
| Filesystem | `read_file`, `write_file`, `edit_file`, `find_files`, `grep_files`, `file_hash` | Durable writes use atomic/fsync-aware helpers where possible. |
| Shell/build | `bash`, `run_command`, `compile`, `docker`, `sandbox_run` | Commands can declare expected artifacts for verification. |
| Code intelligence | `code_search`, AST-backed docs, generated API/module references | Used for source-aware edits and dependency/call graph work. |
| Data/JSON | `jq`, `json_format`, `pipeline`, `python`, `calc` | Useful for transforming logs, tool results, and structured outputs. |
| Web/research | `http_request`, `WebFetch`, `WebSearch`, `jina_search`, `parallel_search`, `browser` | External information gathering is explicit and attributable. |
| Markets | `kalshi`, `polymarket`, `contract_search`, `contract_lookup`, `alpha_vantage` | Prediction-market and financial-data surfaces are native tools. |
| Orchestration | `agent`, `swarm`, `workflow`, `plan_state`, `control_flow`, `recovery` | Parallel and stateful work can be represented directly. |
| Governance | `killswitch`, `ooda`, `governance`, `self_assess`, `token_audit` | Autonomy is bounded by budget, policy, and audit state. |
| Integrations | `discover_integrations`, `dsco_doctor_integrations`, MCP/plugin tools | Catalog metadata distinguishes installed/live/mutating/stale tools. |

### Direct tool execution

Every built-in tool can be called without an LLM round trip:

```bash
./dsco --tool-exec cwd '{}'
./dsco --tool-exec read_file '{"path":"README.md","limit":40}'
./dsco --tool-exec discover_tools '{"query":"git","limit":10}'
./dsco --tool-exec json_format '{"json":"{\"ok\":true}"}'
```

This is useful for smoke tests, scripts, and debugging tool schemas. The full
built-in catalog is generated at [`docs/TOOL_CATALOG.md`](docs/TOOL_CATALOG.md).

### MCP and plugins

DSCO can load external tools from:

- MCP servers configured in `~/.dsco/mcp.json`
- dynamic plugins under `~/.dsco/plugins`
- compatibility aliases such as `Read`, `Write`, `Edit`, `Bash`, `Task`, and `Agent`

External tools can carry integration metadata: connector ID, display name,
distribution channel, categories, labels, scope, catalog status, and action flags.
That metadata feeds discovery, confirmation policy, and integration diagnostics.

## Critical integrations and special factors

DSCO is dense because it is not one integration path; it is an integration fabric.
The repository contains several distinct planes that matter operationally.

### Provider, auth, and model-routing integrations

| Integration / factor | Role in DSCO | Special handling |
|---|---|---|
| Claude Code / Anthropic | External executor and direct Anthropic-compatible provider path. | Claude Code OAuth is auto-discovered and preferred when available; can be disabled with `DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY`. |
| ChatGPT / Codex | External Codex-compatible executor and subscription/native auth path. | `dsco login`, ChatGPT/Codex OAuth discovery, and disable switches for forcing API-key routing. |
| OpenAI-compatible providers | Direct provider transport for OpenAI-shaped APIs. | Model/key shape and `--provider` route requests. |
| OpenRouter | Cross-provider model router and fallback path. | Supports provider order/allow/deny, transforms, fallback policy, ZDR/data-collection hints, price caps, quantization filters, attribution headers, and debug controls. |
| Sakana / Fugu | Native Fugu/Sakana provider path. | Uses `FUGU_API_KEY`; has Fugu-specific cost/context behavior and endpoint policy documented in `docs/SAKANA_FUGU_PROVIDER.md`. |
| Local model servers | Local OpenAI-compatible inference. | Host overrides include Ollama, LM Studio, MLX, vLLM, llama.cpp, Jan, GPT4All, KoboldCpp, TextGen, TGI, and SGLang. |
| Provider metadata registry | Machine-readable provider profiles. | JSON metadata under `provider_metadata/providers/` plus audit reports and schema docs. |

Provider metadata currently includes Anthropic, Cerebras, Cohere, DeepSeek,
Google, Groq, Mistral, Moonshot/Kimi, OpenAI, OpenRouter, Perplexity, Sakana,
Together, and xAI. The metadata layer lets DSCO separate provider policy from
prompt text.

The model registry carries context windows, max-output limits, price estimates,
cache read/write prices, and thinking-support flags. OpenRouter and Codex caches
extend lookup beyond hardcoded aliases, so DSCO can resolve runtime model slugs
without recompiling.

| Routing factor | Why it is special |
|---|---|
| Subscription OAuth vs API keys | Claude Code and Codex can use subscription/auth-store discovery; direct API keys remain supported. |
| Provider fallback policy | OpenRouter can route across providers, but deterministic runs should pin provider/order/fallback vars. |
| Cost model | Built-in prices, learned cost history, and budget gates combine to avoid blind agent loops. |
| Context tiering | Fugu/Sakana and OpenRouter models may have context-dependent pricing/limits; docs call out special caps. |
| Local server compatibility | OpenAI-compatible local servers share the same request shape but differ in tool/thinking support. |

### External app and data integrations

| Integration family | Examples | Notes |
|---|---|---|
| Developer systems | GitHub, Linear/Atlassian-style catalog apps, MCP developer tools | Used for repo search, issue/PR workflows, engineering bundles, and code-review automation. |
| Communication | Slack, Discord, Twilio, email/MCP surfaces | Treated as send/interactive/consequential when messages can leave the local machine. |
| Knowledge/productivity | Notion, Glean, Box, SharePoint, Alation, Atlan, Coveo | Retrieval surfaces are marked `untrusted_content` because they can inject instructions. |
| GTM/CRM | HubSpot, Pipedrive, Close, Intercom, Apollo, Clay, Demandbase, Attio | Profiled under `gtm` for sales/revops workflows. |
| Finance/analytics | BigQuery, MotherDuck, QuickBooks, Brex, Alpaca, FactSet, PitchBook | Profiled under `finance`; mutating/accounting operations require stronger policy. |
| Web/search | Jina, Tavily, Brave, SerpAPI, Firecrawl/browser surfaces | Explicit web retrieval, extraction, browser snapshots, and multi-provider search. |
| Data/AI services | Pinecone, Supabase, Hugging Face, ElevenLabs, Stripe | API-key backed integrations; secrets should live in setup/sealed-store paths. |
| Weather/geodata | NWS, Synoptic/METAR, geocode/weather helpers | Local tools can retrieve weather without making the model improvise. |
| Markets | Kalshi, Polymarket, Alpha Vantage, FRED, systematic trading helpers | Prediction-market and financial-data tools are native; trading credentials require isolated profiles. |
| Control plane | ToolCheck, Agent Ready, HAPI MCP Registry, AccessOwl, Vantage | Not ordinary business apps: they govern tool inventory, policy, access, cost, and agent readiness. |

### Native system and platform factors

| Factor | Why it matters |
|---|---|
| macOS frameworks | Native builds link Security, CoreFoundation, IOKit, CoreGraphics, LocalAuthentication, Foundation, Metal, MetalKit, and Accelerate. |
| Secure Enclave / Touch ID | Sealed-store and local auth paths can use hardware-backed presence/security on macOS. |
| Metal / MLX / Accelerate | Local acceleration paths and vector-store experiments are host-native, not portable APE assumptions. |
| curl + SQLite | Core HTTP and persistence substrate. |
| readline | Interactive line editing when available. |
| hiredis / Redis | Optional fast IPC/backend state path. |
| libsodium | Crypto/mesh primitives where available. |
| mbedTLS | TLS server/client support. |
| libuv | Async/event-loop support. |
| vendored/system GSL | Numerical backend support; vendored GSL is detected by the Makefile. |
| Eigen / FFTW / numerical extensions | Extension backend surfaces for linear algebra, FFT, and numerical self-tests. |
| Cosmopolitan APE | Portable `dsco.distributed.systems` lane excludes Darwin-only frameworks and native Objective-C objects. |

### Runtime-control subsystems

| Subsystem | Special factor |
|---|---|
| Chronicle TokenLedger | Full-fidelity local activity/provenance ledger with content-addressed blobs. |
| Baseline timeline | SQLite event/trace store plus local HTTP endpoints. |
| Local Control Plane | Project state, topology planning, MCP/plugin preferences, and replay/recovery direction. |
| Wings | Pheromone coordination, avian nesting/brooding/fledging/roosting/molting, three-tier memory, capability routing. |
| Talons | Goal pursuit, grip strength, strategy tournaments, learned win/loss routing. |
| Immune System | OODA discipline, kill switches, GSU budgets, principal tiers, hardcoded safety checks. |
| GraphSub | Pheromone graph/client settings for distributed coordination. |
| Plan machinery | Plan cache, plan optimizer, stateful atoms, control flow, workflows, and recovery. |
| Learned cost | k-NN cost model for prediction/record/stats over execution history. |
| Session memory | Persistent session KV memory separate from short-lived context. |
| Output guard | Repetition/degeneration guardrails for model output and terminal safety. |
| Supervisor | Restart, crash-loop, hotswap, memory-budget, and debugger/fault-injection controls. |

## MetaConstruct DSL

`StartOfLoopConstruct` is a first-class runtime mechanism, not README ornament.
It lets DSCO keep a bounded loop alive after a model turn would normally be
complete, while mutating construct-local state only. The construct can manage
loop control, ontology state, graph state, reward dynamics, learning signals,
refinement rules, MapReduce flows, metrology records, catalog/order facts, and
bounded schema rewrites.

### What the DSL can express

| DSL area | Capability | Examples / signals |
|---|---|---|
| Loop control | Bound continuation and termination. | `max_iterations`, `max_turns`, `continue when`, `break when`, `override_done`, `recursive`. |
| Meta/OORL state | Reify planning objects, goals, tasks, beliefs, inference, policy, learning. | `define`, `object`, `goal`, `task`, `belief`, `infer`, `learn`, `decide`. |
| Mutable ontology graph | Add/update/remove nodes and edges, traverse, balance, track density. | `add_node`, `add_edge`, `replace_node`, `traverse`, `node_count`, `edge_count`, `graph_density`. |
| MapReduce object flows | Model bounded distributed transforms over ontology state. | `mapreduce`, `map`, `shuffle`, `reduce`, partitions, emitted graph edges. |
| SRM/metrology | Record NIST-style reference materials, certificates, SDS, traceability, measurements, calibrations, uncertainty budgets. | `srm`, `certificate`, `measurement`, `calibration`, `uncertainty_budget`, `mean_uncertainty`. |
| SRM catalog/order state | Represent catalog availability and ordering constraints without network/payment side effects. | `catalog`, `product_search`, `availability`, `licensed_distributor`, `shipping`, `paper_checks_blocked`. |
| Reward dynamics | Reify reward primitives and OORL adaptation as graph objects. | `reward_object`, `causal_link`, `message`, `explore`, `credit`, `prune_edges`, `attractor`. |
| Effects/signals | Track tool/world/meta effects and epistemic state. | `effect.tool`, `effect.world`, `confidence`, `uncertainty`, `curiosity`, `empowerment`. |
| Refinement | One-shot numeric adaptation at clean model-done boundaries. | `refine max_iterations += 2 when belief_count >= 1`. |
| Bounded schema rewrites | One-shot DSL statement rewrites before flow decisions. | `schema_rewrite add_edge ... when credit >= 0.8`. |

### Why it matters

Most agent loops treat "done" as a model-side convention. MetaConstruct makes
continuation explicit and bounded. A construct can say: keep going while reward
objects exist, stop when uncertainty drops, add graph structure when credit
improves, record SRM/metrology facts without ordering anything, and adapt schema
within a single constrained statement. That gives DSCO an inspectable control
language for recursive work instead of relying on hidden prompt momentum.

A compact construct can combine planning, graph state, reward learning, bounded
schema adaptation, MapReduce, and SRM/metrology state in one loop-local program:

```text
max_iterations = 3;
define(sensor, state_object);
add_node planner as policy state idle;
belief uncertainty = 0.7;
effect world = 0.5;
reward_object completion valence 0.8 intensity 0.5 target planner;
causal_link sensor -> planner weight 0.7;
refine max_iterations += 2 when belief_count >= 1 and causal_link_count >= 1;
schema_rewrite add_edge planner -> reward_sink relation optimized weight 0.9 when credit >= 0.5;
mapreduce credit_flow over planner map emit_reward_pairs reduce merge_credit by objective partitions 4;
srm 2373 matrix genomic_dna property HER2 certificate current sds available traceable uncertainty 0.03;
measurement her2_ratio on 2373 value 2.1 uncertainty 0.03 unit ratio method ddPCR;
continue when iteration < max_iterations and reward_object_count >= 1;
break when turn >= 80
```

Special safety boundary: SRM catalog/order directives model facts such as
availability, current certificates, SDS presence, licensed distributors,
registration requirements, paper-check restrictions, shipping blocks, and total
price. They do **not** fetch product data, place orders, or perform payment.

See [`docs/META_CONSTRUCT_DSL.md`](docs/META_CONSTRUCT_DSL.md).

## Architecture

DSCO is organized as three cooperating layers:

| Layer | Responsibility | Mechanisms |
|---|---|---|
| **Wings** | Autonomy and emergence | Pheromone-style coordination, three-tier memory, hierarchical swarms, capability routing. |
| **Talons** | Competitive execution | Goal pursuit, grip-strength retries, strategy tournaments, learned routing. |
| **Immune System** | Survival and governance | OODA loops, kill switches, budgets, principal tiers, hardcoded safety invariants. |

The implementation is intentionally boring where reliability matters: explicit C
modules, generated references, local persistence, conservative docs drift checks,
and minimal hidden magic.

Start with:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — system architecture and runtime flows
- [`docs/DIAGRAMS.md`](docs/DIAGRAMS.md) — architecture diagrams
- [`docs/C_MODULE_REFERENCE.md`](docs/C_MODULE_REFERENCE.md) — per-module implementation map
- [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md) — generated header declarations

## Agent orchestration

DSCO can run as a single interactive process, but its runtime model is built for
parallelism when the task warrants it.

| Mechanism | What it does |
|---|---|
| `agent` tool | Spawns, waits on, races, kills, and collects sub-agent work. |
| `swarm` tool | Creates worker groups, map/reduce flows, topology runs, and executor/provider swarms. |
| `--topology-list` | Shows available orchestration topologies. |
| `--topology-auto` | Lets DSCO select a topology for the task. |
| SQLite IPC | Tracks worker registry, tasks, messages, scratchpad state, and liveness. |
| Worker profiles | Child processes run the same binary with worker/subagent environment. |

Typical patterns:

```bash
./dsco --topology-list
./dsco --topology-auto "audit docs and tests for drift"
./dsco -O -M kimi-k2.7-code-highspeed "route this implementation plan to workers"
```

Use orchestration when work can be decomposed into independent search, audit,
implementation, or review tracks. Keep simple edits single-process.

## Topology catalog

DSCO documents 60 named topologies across eight families. They are not just
marketing names; they encode latency/cost/capability tradeoffs for routing work.

| Family | Topologies | Best for | Examples |
|---|---:|---|---|
| Linear chains | 8 | Sequential refinement and escalation. | `sentinel`, `refinery`, `deepdive`, `cascade`, `telescope`, `gauntlet` |
| Fan-out / fan-in | 10 | Parallel search, map/reduce, broad audits, synthesis. | `starburst`, `scatter_gather`, `mapreduce`, `trident`, `nova`, `prism` |
| Hierarchical trees | 8 | Manager/worker decomposition and multi-level review. | `military`, `corporate`, `binary_tree`, `fractal`, `pyramid` |
| Mesh / peer networks | 6 | Debate, consensus, gossip, and peer cross-checking. | `tribunal`, `senate`, `gossip`, `ring`, `full_mesh`, `small_world` |
| Specialist/router | 8 | Triage and expert dispatch. | `switchboard`, `triage`, `expert_panel`, `clinic`, `newsroom`, `orchestra` |
| Feedback/iterative | 8 | Critique, polish, adversarial review, annealing. | `critic_loop`, `polish`, `adversarial`, `evolution`, `debate`, `mirror` |
| Competitive/redundant | 6 | Multiple candidates, risk hedging, tournaments. | `tournament`, `auction`, `ensemble`, `gladiator`, `monte_carlo`, `hedge` |
| Domain-specific | 6 | Known task classes. | `code_review`, `research`, `incident`, `data_pipeline`, `security_audit`, `creative` |

Useful facts from the topology docs:

- Average topology size is roughly 6.2 agents.
- Fastest low-latency patterns include `senate`, `triage`, and `expert_panel`.
- Most parallel patterns include `nova`, `mapreduce`, and `monte_carlo`.
- Most iterative patterns include `critic_loop` and `annealing`.
- Domain-specific routes exist for code review, research, incidents, data
  pipelines, security audits, and creative work.

See [`docs/topologies/INDEX.md`](docs/topologies/INDEX.md).

## Chronicle and local observability

Chronicle is DSCO's local activity ledger. It records process/session metadata,
user turns, LLM deltas, tool calls, results, spans, and content-addressed blob
references for replay, debugging, audit, and future training-data workflows.

```bash
./dsco --timeline-server --timeline-port 8421
open http://127.0.0.1:8421/chronicle
curl http://127.0.0.1:8421/chronicle.json
```

Controls:

| Variable | Purpose |
|---|---|
| `DSCO_CHRONICLE_MODE=off` | Disable Chronicle for a run. |
| `DSCO_CHRONICLE_DIR=/path` | Override local ledger/blob storage. |
| `DSCO_CHRONICLE_SESSION_ID` | Session correlation value set by DSCO. |

See [`docs/CHRONICLE_TOKENLEDGER.md`](docs/CHRONICLE_TOKENLEDGER.md).

## Integrations universe

DSCO's integration layer is deliberately split into two planes:

1. **Catalog plane** — the Codex app directory: the full known universe of apps,
   connectors, categories, labels, OAuth/accessibility state, sync semantics, and
   risk posture.
2. **Runtime plane** — live MCP/plugin/tool registrations that are actually
   callable in this process.

That distinction matters. A connector can be present in the 1,887-app Codex
catalog but not installed, installed but not authenticated, authenticated but
stale, live but read-only, or live and mutating. DSCO tracks those states instead
of flattening everything into "tool exists".

### The 1,887-app catalog as a map, not a menu

The Codex app directory is treated as an integration universe: an index of what
DSCO could connect to, what is currently connected, and what governance posture
should apply if it becomes callable.

| Dimension | Examples | DSCO interpretation |
|---|---|---|
| Catalog identity | `id`, `connector_id`, `display_name` | Stable handles for matching cached app metadata to live MCP tools. |
| Distribution | `distribution_channel`, marketplace/source labels | Where the connector came from and whether it is first-party, ecosystem, or custom. |
| Categories | developer tools, sales, finance, knowledge, governance | Used for search, install bundles, and profile-based discovery. |
| Access state | enabled, accessible, inaccessible, OAuth-gated | Separates "known app" from "usable right now". |
| Freshness | current vs stale connector IDs | Release-blocking signal when catalog entries no longer match runtime reality. |
| Data flow | retrievable, sync-capable, untrusted content | Read surfaces are treated as potential prompt-injection/content-contamination sources. |
| Consequence | consequential, mutating, admin, delete/send/write | Mutating surfaces become confirmation-gated policy objects. |
| Interaction | interactive connectors | Tools that can initiate external user/platform interaction are labeled separately. |

### Discovery profiles

`discover_integrations` supports opinionated profiles so the 1,887-app universe
can be sliced into installable/operator-relevant bundles:

| Profile | Typical apps/connectors | Use case |
|---|---|---|
| `engineering` | GitHub, Linear, Atlassian, Slack, Notion, Datadog, PostHog | Code, incidents, product engineering, observability. |
| `gtm` | HubSpot, Pipedrive, Close, Intercom, Apollo, Clay, Demandbase, Attio | Sales/revops workflows, CRM enrichment, customer communication. |
| `finance` | BigQuery, MotherDuck, QuickBooks, Brex, Alpaca, FactSet, PitchBook | Finance ops, analytics, markets, procurement, investor workflows. |
| `enterprise_knowledge` | Glean, Box, SharePoint, Alation, Atlan, Coveo | Internal knowledge retrieval and governed enterprise search. |
| `governed_agent_runtime` | ToolCheck, Agent Ready, HAPI MCP Registry, AccessOwl, Vantage | Control-plane and agent-governance integrations. |

### Catalog labels become policy

DSCO maps catalog labels into action flags before the model ever sees a tool as a
plain string. This is the integration fabric: metadata becomes governance.

| Catalog signal | Runtime action/policy |
|---|---|
| `retrievable` | `read` + `untrusted_content` |
| `sync` | `read` + `untrusted_content` + sync-capable status |
| `consequential` | `write` + `requires_confirmation` |
| `interactive` | `interactive` |
| delete/admin/send/write naming | mutating and confirmation-sensitive runtime posture |
| stale catalog status | surfaced by doctor tooling as release/ops risk |

### Commands

```bash
# Search the integration universe by text/profile.
./dsco --tool-exec discover_integrations '{"query":"github","limit":10}'
./dsco --tool-exec discover_integrations '{"profile":"engineering","limit":20}'
./dsco --tool-exec discover_integrations '{"profile":"governed_agent_runtime","limit":20}'

# Diagnose catalog/runtime drift.
./dsco --tool-exec dsco_doctor_integrations '{}'

# Print Nous Hermes Agent MCP/ACP compatibility presets.
./dsco --tool-exec hermes_agent '{"action":"status"}'
./dsco --tool-exec hermes_agent '{"action":"capabilities"}'
./dsco --tool-exec hermes_agent '{"action":"mcp_preset"}'
./dsco --tool-exec hermes_agent '{"action":"acp_preset"}'

# Use an explicit catalog cache instead of ~/.dsco/codex_app_directory.json.
DSCO_CODEX_APP_DIRECTORY=/path/to/codex_app_directory.json   ./dsco --tool-exec discover_integrations '{"profile":"finance","limit":25}'
```

`dsco_doctor_integrations` is intentionally opinionated. It highlights stale
connector IDs, missing catalog/auth/install state, mutating live connectors,
control-plane candidates, and live MCP tools that lack catalog metadata.

See [`docs/INTEGRATION_CATALOG.md`](docs/INTEGRATION_CATALOG.md).

## Configuration and storage

DSCO uses a layered configuration model: explicit CLI flags win, live environment
variables come next, saved setup env files follow, and built-in defaults are last.
Use command-line flags for one-off runs and `./dsco --setup` for durable local
provider configuration.

| Storage / path | Purpose |
|---|---|
| `~/.dsco` | Default local DSCO state root. |
| `DSCO_ENV_FILE` | Overrides the setup env file used for saved provider/config values. |
| `DSCO_BASELINE_DB` | Overrides the Baseline SQLite timeline path. |
| `DSCO_CHRONICLE_DIR` | Overrides Chronicle ledger/blob storage. |
| `~/.dsco/mcp.json` | MCP server configuration. |
| `~/.hermes/config.yaml`, `~/.hermes/mcp_servers.yaml` | Optional Hermes Agent MCP server configs imported by DSCO. |
| `~/.dsco/plugins` | Dynamic plugin directory. |
| `~/.dsco/codex_app_directory.json` | Default cached Codex app-directory catalog. |

Operational rules:

- Prefer `VAR=value ./dsco ...` for temporary repros.
- Prefer `./dsco --setup` for long-lived provider credentials.
- Do not persist internal runtime variables such as `DSCO_SUBAGENT`,
  `DSCO_WORKER`, `DSCO_SUPERVISED`, or `DSCO_RESUME_AFTER_CRASH`.
- Treat `~/.dsco/debug`, Chronicle blobs, and exported traces as sensitive until
  reviewed.
- Use `make docs-check` after source changes that should affect generated docs.

See [`docs/OPERATIONS.md`](docs/OPERATIONS.md) and
[`docs/CONSTANTS_ENV_INDEX.md`](docs/CONSTANTS_ENV_INDEX.md).

## Documentation map

| Document | Use it for |
|---|---|
| [`docs/INDEX.md`](docs/INDEX.md) | Full documentation entry point. |
| [`docs/HOW_TO.md`](docs/HOW_TO.md) | Practical task guides. |
| [`docs/OPERATIONS.md`](docs/OPERATIONS.md) | Runtime modes, env vars, storage, troubleshooting. |
| [`docs/TOOL_CATALOG.md`](docs/TOOL_CATALOG.md) | Generated built-in tool list. |
| [`docs/EXTERNAL_TOOL_CATALOG.md`](docs/EXTERNAL_TOOL_CATALOG.md) | Generated cached external app/tool integration universe. |
| [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md) | Generated public header declarations. |
| [`docs/CONSTANTS_ENV_INDEX.md`](docs/CONSTANTS_ENV_INDEX.md) | Generated constants and environment-variable index. |
| [`docs/REPO_COVERAGE.md`](docs/REPO_COVERAGE.md) | Generated tracked-file coverage manifest. |
| [`docs/CHRONICLE_TOKENLEDGER.md`](docs/CHRONICLE_TOKENLEDGER.md) | Local activity/provenance ledger. |
| [`docs/INTEGRATION_CATALOG.md`](docs/INTEGRATION_CATALOG.md) | Integration metadata schema, importer behavior, and governance mapping. |
| [`docs/COSMOPOLITAN.md`](docs/COSMOPOLITAN.md) | Portable APE build lane. |
| [`docs/RUNBOOKS.md`](docs/RUNBOOKS.md) | Operational procedures. |

## Build, packaging, and release lanes

DSCO has multiple artifacts because it serves different operator needs: full
native runtime, lightweight helper/worker paths, package-manager install, and
portable APE experiments.

| Artifact / lane | Produced by | Purpose |
|---|---|---|
| `dsco` | `make dsco` / `make all` | Full native runtime. |
| `dsco-lite` | `make dsco-lite` / `make all` | Smaller worker/lite binary for fast utility paths. |
| `dsc` | `make dsc` / `make all` | Companion CLI/helper surface. |
| `dsco.distributed.systems` | `make cosmo` | Hosted Cosmopolitan Actually Portable Executable lane. |
| `dsco.com` | `COSMO_TARGET=dsco.com make cosmo` | Legacy `.com` APE filename for compatibility checks. |
| npm package | `npm/` packaging assets | Installs/downloads native release binary without Python. |
| Homebrew formula | `Formula/dsco.rb` | macOS package-manager install path. |
| Generated C blobs | `src/generated/*` | Embedded indexes/assets such as constants/env and tool embeddings. |

Release hygiene is intentionally boring:

- `make docs-check` blocks generated-doc drift.
- `make check-version` validates version consistency.
- sanitizer and static-analysis targets exist for deeper validation.
- `make cosmo-selftest` verifies portable metadata/tool paths.
- Formula tests run `dsco --version` and direct `cwd` tool execution.

## Development

Common build and hygiene targets:

| Target | Purpose |
|---|---|
| `make` / `make all` | Build `dsco`, `dsc`, `dsco-new`, and `dsco-lite`. |
| `make test` | Run the main test suite. |
| `make fast-build` | Fast dev build into `build/fast`. |
| `make fast-quick` | Fast build + smoke + targeted tests. |
| `make cosmo` | Build portable `dsco.distributed.systems`. |
| `make cosmo-selftest` | Build and smoke-test the portable artifact. |
| `make docs` | Regenerate API, tool, constants, external-catalog, and coverage docs. |
| `make docs-check` | Verify generated docs and coverage are in sync. |
| `make format-check` | Check formatting. |
| `make lint` | Run format/docs/version lint chain. |
| `make asan-test` / `make ubsan-test` | Sanitizer test runners. |

Recommended pre-PR loop:

```bash
make test
make docs-check
make format-check
```

If you change the tool registry, external integration cache, header
declarations, constants, environment variable usage, or tracked repo layout, run
`make docs` and include the regenerated artifacts.

## Repository layout

```text
dsco-cli/
├── src/                  # C runtime implementation (118 .c)
├── include/              # Public/internal headers (119 .h)
├── tests/                # Runtime, unit, smoke, and regression tests
├── scripts/              # Build, packaging, docs, bootstrap, smoke helpers
├── docs/                 # Documentation set and generated references
├── data/                 # Generated machine-readable indexes
├── provider_metadata/    # Provider metadata schema/docs
├── web/                  # Local web surface
├── Formula/              # Homebrew formula
├── npm/                  # npm packaging assets
├── Makefile              # Canonical build graph
└── dsc.c                 # Companion CLI/helper surface
```

## Troubleshooting

| Symptom | First checks |
|---|---|
| `dsco` cannot find a provider key | Run `./dsco --setup-report`; verify the expected env var or saved setup file. |
| Wrong backend/model is selected | Clear or inspect `DSCO_EXEC`, `DSCO_MODEL`, `DSCO_PROFILE`; try an explicit `--provider` or `-e`. |
| Docs drift in CI | Run `make docs`; commit regenerated `docs/API_REFERENCE.md`, `docs/TOOL_CATALOG.md`, `docs/EXTERNAL_TOOL_CATALOG.md`, `docs/CONSTANTS_ENV_INDEX.md`, `docs/REPO_COVERAGE.md`, and `data/constants_env_index.json`. |
| A tool appears missing | Use `./dsco --tool-exec discover_tools '{"query":"name"}'`; check cheap/profile mode and MCP/plugin load state. |
| MCP integration looks stale | Run `./dsco --tool-exec dsco_doctor_integrations '{}'`; verify `DSCO_CODEX_APP_DIRECTORY`, `~/.dsco/codex_app_directory.json`, or `~/.codex/cache/codex_app_directory/*.json`. |
| Timeline server starts but activity is missing | Check `DSCO_BASELINE_DB`, `DSCO_CHRONICLE_MODE`, and `DSCO_CHRONICLE_DIR`. |
| Portable build fails | Run `make cosmo-info`; verify `make cosmo-bootstrap`; see `docs/COSMOPOLITAN.md`. |
| Terminal state looks broken after interruption | Open a fresh shell or run `reset`; file an issue with the command and terminal emulator. |

For deeper operational debugging, see [`docs/RUNBOOKS.md`](docs/RUNBOOKS.md).

## Security

DSCO is a local automation runtime with shell, filesystem, network, provider, and
integration surfaces. Treat it like powerful developer infrastructure:

- Review tool calls before granting broad autonomy.
- Keep provider keys and OAuth tokens out of shell history and git.
- Prefer `./dsco --setup` / sealed-store paths for durable secrets.
- Use `DSCO_CHRONICLE_MODE=off` for sessions where no local activity record
  should be retained.
- Inspect Chronicle/debug exports before sharing them; they may contain prompts,
  file paths, tool inputs/results, and provider payloads.

Report vulnerabilities through GitHub Private Vulnerability Reporting. Details
and response targets are in [`SECURITY.md`](SECURITY.md). Do not file public
issues with exploit details.

## Contributing

Contributions for runtime behavior, tools, docs, CI, packaging, and scripts are
welcome. Read [`CONTRIBUTING.md`](CONTRIBUTING.md),
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md), and [`CODEOWNERS`](CODEOWNERS).

```bash
git clone https://github.com/arthurcolle/dsco.git
cd dsco
./scripts/bootstrap.sh
make -j8
make test
make docs-check
```

## License

MIT — see [`LICENSE`](LICENSE).

---

Built by Arthur Colle ([distributed.systems](https://distributed.systems)) as the
foundation for a local-first agentic infrastructure platform.

> Big things have small beginnings.
