# Architecture & Runtime Flows

## 1) System Overview

`dsco` is a single-process CLI runtime that can spawn additional `dsco` child processes for swarm-style parallelism. The process model is:

- Root process: CLI UX, conversation state, tool execution, rendering
- Child processes: same binary, launched with `DSCO_SUBAGENT=1`, connected through SQLite IPC

Core execution chain:

1. Input ingestion (`main.c` / `agent.c`)
2. Conversation/request assembly (`llm.c` and/or `provider.c`)
3. Streaming response processing (SSE)
4. Tool execution loop (`tools.c` + integrations/MCP/plugins)
5. Message persistence + follow-up turns
6. Final rendering and telemetry

## 2) Primary Components

### CLI Entrypoints

- `main.c`
- `agent.c`

Responsibilities:

- Parse CLI flags and one-shot prompt mode
- Initialize setup/env loading and baseline logging
- Start interactive command loop and slash command handling
- Manage status bars, metrics, telemetry, session save/load, and model/runtime toggles

### LLM and Provider Layer

- `llm.c` / `llm.h`
- `provider.c` / `provider.h`

Responsibilities:

- Build structured request JSON from `conversation_t`
- Stream server events into `content_block_t` deltas
- Detect and mitigate degeneration/repetition patterns
- Maintain usage telemetry and session-level token/cost aggregates
- Abstract provider-specific request/stream implementation

### Tool Runtime

- `tools.c` / `tools.h`
- `integrations.c` / `integrations.h`
- `mcp.c` / `mcp.h`
- `plugin.c` / `plugin.h`

Responsibilities:

- Register and dispatch 288 built-in tools
- Input schema validation before dispatch
- Timeout watchdog, cooperative cancellation, and execution guards
- External API wrappers (GitHub, weather, AV/FRED, communication, etc.)
- Dynamic external tool registration from MCP servers
- Dynamic plugin loading from `~/.dsco/plugins`

### Orchestration and Distributed Execution

- `swarm.c` / `swarm.h`
- `ipc.c` / `ipc.h`

Responsibilities:

- Spawn sub-`dsco` workers
- Poll worker stdout/stderr and aggregate results
- Coordinate workers by groups
- Maintain shared SQLite state: registry, tasks, messages, scratchpad, liveness

### Rendering and UX

- `md.c` / `md.h`
- `tui.c` / `tui.h`

Responsibilities:

- Stream markdown to ANSI terminal output
- Render code blocks, tables, blockquotes, lists, and inline formatting
- Render panels/tables/spinners/status bars
- Surface real-time tool execution and performance status

### Data/Utility Subsystems

- `json_util.c` / `json_util.h`
- `semantic.c` / `semantic.h`
- `pipeline.c` / `pipeline.h`
- `ast.c` / `ast.h`
- `eval.c` / `eval.h`
- `crypto.c` / `crypto.h`
- `error.c` / `error.h`

Responsibilities:

- JSON extraction, parsing, dynamic buffers, arena allocation, schema validation
- Semantic ranking/classification for tool/context relevance
- Text transform pipeline engine with 30+ stages
- C AST introspection and dependency/call graph extraction
- Mathematical expression evaluator + bigint helpers
- Hashing, HMAC, base64, UUID, HKDF, JWT decode
- Thread-local structured error code/message/cause chain

### Observability and Setup

- `baseline.c` / `baseline.h`
- `setup.c` / `setup.h`

Responsibilities:

- Persist session/events/traces in SQLite
- Serve timeline UI and event JSON endpoint
- Manage persistent env profile file loading and bootstrap

## 3) Runtime Flow: Interactive Mode

1. `main()` resolves model/key/profile, initializes baseline/setup.
2. `agent_run()` initializes tools, renderer, MCP discovery, locks, session state.
3. User prompt enters conversation (`conv_add_user_*`).
4. Provider chosen via `provider_detect()` and `provider_create()`.
5. Request JSON assembled from conversation + tool schemas.
6. SSE stream starts:
   - text deltas -> markdown renderer
   - tool_use start -> tool status UI
   - thinking deltas (when present) -> dimmed diagnostics
7. Parsed blocks are appended to conversation.
8. If tool_use blocks exist, each tool executes and returns tool_result blocks.
9. Loop repeats until `end_turn` / no tool use.
10. Usage/cost/telemetry aggregated; baseline event logs updated.

## 4) Runtime Flow: One-Shot Mode

One-shot (`./dsco "..."`) uses the same tool loop with a compact callback path:

- Markdown text callback flushes directly to stdout
- Tool callback prints simplified tool marker
- Conversation still tracks tool_result exchanges across turns

Sub-agent mode in one-shot is enabled by `DSCO_SUBAGENT` + `DSCO_IPC_DB`:

- Process claims IPC tasks and runs each as additional turns
- On completion, writes back task result/failure

## 5) Tool Execution Flow

Execution pipeline in `tools.c`:

1. Lookup tool from `g_tool_map` (O(1) hash map)
2. Validate JSON input against schema (if configured)
3. Start watchdog thread (timeout + grace)
4. Execute built-in or external callback
5. Collect output + status + timing
6. Stop watchdog and record metrics

External tools:

- MCP tools: registered via `tools_register_external()` callback bridge
- Plugin tools: merged into tool list from dynamic libraries

## 6) Storage and Persistence

### Baseline Database (SQLite)

`baseline.c` maintains:

- `instances`: process lifecycle metadata
- `events`: chronological event log
- `trace_spans`: distributed timing spans (lazy-initialized)

Endpoints from timeline server:

- `/` timeline HTML
- `/events.json` recent events JSON
- `/health` liveness
- `/freight` static freight dashboard
- `/www/*` static file passthrough

### IPC Database (SQLite)

`ipc.c` maintains shared state for swarms:

- agent registry and heartbeat
- inbox/broadcast messages
- task queue
- scratch key-value store

## 7) Security and Guardrails

- Path traversal protection in static file serving
- Tool schema validation before execution
- Prompt injection detector with severity levels
- Timeouts and cancellation for long-running tools
- Output truncation and degeneration detection in LLM stream handling

## 8) Extension Points

### MCP

- Config file: `~/.dsco/mcp.json`
- Transport: JSON-RPC over stdio
- Dynamic registration at startup or `/mcp reload`

### Plugins

- Directory: `~/.dsco/plugins`
- ABI: exported `dsco_plugin_*` symbols
- Hot reload support

### Providers

- Built-in: `anthropic`, `openai` + OpenAI-compatible endpoints
- Auto-detection by model name and key prefix

## 9) Concurrency Model

- Core interactive loop is single-threaded orchestration
- Concurrency used for:
  - watchdog timers
  - UI spinners/status threads
  - optional swarm subprocess parallelism
- Shared state protected by lock bundle in `dsco_locks_t`

## 10) Failure Modes and Recovery

- API/HTTP errors -> surfaced with status and baseline event
- Stream interruption -> checkpoint structures and robust block finalization
- Tool timeout -> watchdog cancellation and metrics timeout count
- IPC stale agents -> heartbeat-based liveness checks
- Session recovery -> autosave/load of conversation JSON
