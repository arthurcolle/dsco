# C Module Reference

This document covers every C source/header module in the root of the repository.

## Conventions

- “Public API” means declarations exposed in `.h` files.
- “Internal behavior” describes major implementation concerns in `.c`.
- Line counts shown are approximate and from current tree snapshot.

## Core Entrypoints

### `main.c` (405 LOC)

Purpose:

- Process CLI args and dispatch interactive/one-shot/setup/timeline modes.

Key responsibilities:

- Parses `-m`, `-k`, `--setup*`, `--timeline-*`, `--profile`, and prompt tail.
- Loads setup profile env values and optionally bootstraps env file.
- Starts baseline DB and optional timeline HTTP server.
- In one-shot mode, executes the same tool loop used by interactive mode.
- In sub-agent mode, claims IPC tasks and runs them in-turn.

Primary dependencies:

- `agent.h`, `llm.h`, `md.h`, `setup.h`, `baseline.h`, `ipc.h`, `tools.h`.

### `agent.c` / `agent.h` (2142 LOC / 8 LOC)

Purpose:

- Interactive runtime loop and user command surface.

Public API (`agent.h`):

- `void agent_run(const char *api_key, const char *model);`

Internal behavior:

- Maintains `conversation_t`, `session_state_t`, metrics, and status UI.
- Handles slash commands for model, effort, provider, budget, telemetry, etc.
- Supports drag/drop image path ingestion and URL image injection.
- Executes streamed tool rounds until `end_turn`.
- Tracks cost budget, context pressure, and autosave handling.

## LLM and Provider Layer

### `llm.c` / `llm.h` (2680 LOC / 231 LOC)

Purpose:

- Request construction, conversation state, SSE parsing, and telemetry.

Public API highlights:

- Session state init: `session_state_init`
- Conversation ops: `conv_init`, `conv_free`, `conv_add_*`, `conv_save/load`, `conv_trim_old_results`
- Request building: `llm_build_request`, `llm_build_request_ex`
- Streaming: `llm_stream`
- Utility: `llm_count_tokens`, `llm_get_custom_system_prompt`, `llm_debug_save_request`
- Tool metrics/cache helpers
- Prompt injection detection: `detect_prompt_injection`

Internal behavior:

- Builds provider-specific message structures for tools, images, documents.
- Parses SSE events into structured content blocks.
- Handles retries/checkpoints for partial stream recovery.
- Detects output degeneration and truncates toxic repetitive output.
- Accumulates usage and latency (TTFT, throughput, total stream time).

### `provider.c` / `provider.h` (573 LOC / 58 LOC)

Purpose:

- Provider abstraction over Anthropic and OpenAI-compatible APIs.

Public API:

- `provider_create`, `provider_free`
- `provider_detect`
- `provider_resolve_api_key`

Internal behavior:

- Anthropic provider delegates to `llm.c` request/stream logic.
- OpenAI-compatible provider builds chat-completions request shape and SSE parser.
- Endpoint registry supports OpenAI-family compatible gateways.

## Tool Runtime and Integrations

### `tools.c` / `tools.h` (9191 LOC / 126 LOC)

Purpose:

- Built-in tool registry/execution engine and runtime controls.

Public API:

- `tools_init`, `tools_get_all`, `tools_builtin_count`, `tools_execute`
- Tool hash map (`tool_map_*`)
- External registration: `tools_register_external`
- Lock management: `dsco_locks_init/destroy`
- Timeout watchdog: `watchdog_start/stop`, `tool_timeout_for`
- Input validation: `tools_validate_input`

Internal behavior:

- Declares 288 built-in tools (`tool_def_t` table).
- Performs schema validation and dispatches by name.
- Applies timeout watchdog and cancellation signaling.
- Merges built-in tools with MCP and plugin extras.
- Includes many local system/file/network/git/shell/swarm/introspection utilities.

See full list: [Built-in Tool Catalog](TOOL_CATALOG.md)

### `integrations.c` / `integrations.h` (1487 LOC / 188 LOC)

Purpose:

- External API wrappers used by selected tools.

Public API:

- Web search adapters (`tool_tavily_search`, `tool_brave_search`, `tool_serpapi`, `tool_jina_read`)
- GitHub APIs (search/issues/PR/repo/actions/create_issue)
- Alpha Vantage + FRED wrappers (`tool_alpha_vantage`, specific endpoint helpers, `tool_fred_series`)
- Communication/productivity wrappers (Slack, Discord, Twilio, Notion)
- Weather/geocode/scraping and other connectors (Firecrawl, ElevenLabs, Pinecone, Stripe, Supabase, HuggingFace)

Internal behavior:

- Builds HTTP requests, headers, and JSON extraction logic per provider.
- Performs fallback handling when credentials are missing.

### `mcp.c` / `mcp.h` (445 LOC / 69 LOC)

Purpose:

- MCP client runtime over JSON-RPC stdio transport.

Public API:

- `mcp_init`, `mcp_shutdown`
- `mcp_get_tools`
- `mcp_call_tool`

Internal behavior:

- Parses `~/.dsco/mcp.json` server config.
- Spawns subprocess servers and performs MCP initialize/discovery.
- Registers discovered tools for runtime execution bridge.

### `plugin.c` / `plugin.h` (230 LOC / 71 LOC)

Purpose:

- Dynamic plugin loading from shared libraries.

Public API:

- `plugin_init`, `plugin_reload`, `plugin_cleanup`
- `plugin_load`, `plugin_unload`
- `plugin_get_tools`, `plugin_list`

Internal behavior:

- Uses `dlopen`/`dlsym` to find `dsco_plugin_*` symbols.
- Flattens plugin tool defs into runtime-accessible array.

## Swarm and IPC

### `swarm.c` / `swarm.h` (556 LOC / 117 LOC)

Purpose:

- Spawn and supervise child `dsco` processes.

Public API:

- Lifecycle: `swarm_init`, `swarm_destroy`
- Spawn: `swarm_spawn`, `swarm_spawn_in_group`
- Grouping: `swarm_group_create`, `swarm_group_dispatch`, `swarm_group_complete`
- Polling/streaming: `swarm_poll`, `swarm_poll_stream`
- Control: `swarm_kill`, `swarm_group_kill`
- Output/status formatters: `swarm_status_json`, `swarm_child_output`, `swarm_group_status_json`

Internal behavior:

- Passes environment inheritance (`DSCO_SUBAGENT`, `DSCO_SWARM_DEPTH`, parent instance IDs).
- Captures stdout/stderr via pipes and tracks per-child status and timing.

### `ipc.c` / `ipc.h` (781 LOC / 186 LOC)

Purpose:

- SQLite-backed inter-agent coordination layer.

Public API areas:

- Lifecycle: `ipc_init`, `ipc_shutdown`, `ipc_self_id`, `ipc_db_path`
- Agent registry: `ipc_register`, `ipc_set_status`, `ipc_heartbeat`, `ipc_list_agents`, `ipc_get_agent`, `ipc_agent_alive`
- Messaging: `ipc_send`, `ipc_recv`, `ipc_recv_topic`, `ipc_unread_count`
- Tasks: `ipc_task_submit`, `ipc_task_claim`, `ipc_task_start`, `ipc_task_complete`, `ipc_task_fail`, `ipc_task_list`, `ipc_task_pending_count`
- Scratchpad KV: `ipc_scratch_put/get/del/keys`
- Poll/status: `ipc_poll`, `ipc_status_json`

Internal behavior:

- Uses WAL mode for multi-process concurrency.
- Implements stale-agent detection via heartbeat timestamps.

## Rendering and UX

### `md.c` / `md.h` (2235 LOC / 172 LOC)

Purpose:

- Streaming markdown renderer to ANSI terminal output.

Public API:

- `md_init`, `md_feed`, `md_feed_str`, `md_flush`, `md_reset`

Internal behavior:

- Supports CommonMark + selected GFM + LaTeX + HTML subset.
- Handles streaming partial-line echo and block state transitions.
- Renders code blocks with syntax-aware output.
- Includes adjacent duplicate suppression for repeated long paragraph lines.

### `tui.c` / `tui.h` (1241 LOC / 291 LOC)

Purpose:

- Terminal UI toolkit (colors, layout, spinners, status bar, stream wrappers).

Public API areas:

- Terminal control and box/panel/table rendering
- Styled status helpers (`tui_info/success/warning/error`)
- Stream helper hooks (`tui_stream_*`)
- Truecolor and gradients
- Async and batch spinners
- Live status bar and swarm panels

Internal behavior:

- ANSI escape heavy implementation optimized for interactive feedback.

## Observability and Setup

### `baseline.c` / `baseline.h` (1052 LOC / 54 LOC)

Purpose:

- Timeline/event/tracing persistence plus timeline server.

Public API:

- Baseline lifecycle and event logging:
  - `baseline_start`, `baseline_stop`, `baseline_log`
  - `baseline_instance_id`, `baseline_db_path`
  - `baseline_serve_http`
- Trace API:
  - `trace_new_id`, `trace_span_begin`, `trace_span_end`, `trace_query_recent`, `trace_print_waterfall`

Internal behavior:

- Initializes SQLite schema (`instances`, `events`, `trace_spans`).
- Exposes local HTTP endpoints (`/`, `/events.json`, `/health`, `/freight`, `/www/*`).
- Emits lifecycle events for server and session boundaries.

### `setup.c` / `setup.h` (700 LOC / 28 LOC)

Purpose:

- Persistent profile/env bootstrap and reporting.

Public API:

- `dsco_setup_profile_name`
- `dsco_setup_load_saved_env`
- `dsco_setup_autopopulate`
- `dsco_setup_bootstrap_from_env`
- `dsco_setup_report`
- `dsco_setup_env_path`

Internal behavior:

- Supports alias mapping (e.g., `GH_TOKEN` -> `GITHUB_TOKEN`).
- Captures known keys + generic `_API_KEY`/`_TOKEN` patterns.
- Writes profile-specific env files under `~/.dsco`.

## Semantic/Analysis Engines

### `semantic.c` / `semantic.h` (532 LOC / 131 LOC)

Purpose:

- Lightweight semantic ranking/classification primitives.

Public API:

- Tokenization: `sem_tokenize`
- TF-IDF: `sem_tfidf_init/add_doc/finalize/vectorize`
- Similarity: `sem_cosine_sim`
- BM25 ranking: `sem_bm25_rank`
- Tool ranking: `sem_tools_index_build`, `sem_tools_rank`
- Query classification: `sem_classify`, `sem_category_name`
- Message relevance scoring: `sem_score_messages`

### `ast.c` / `ast.h` (813 LOC / 79 LOC)

Purpose:

- C source structural introspection for functions/types/includes/graphs.

Public API:

- Parse/free file: `ast_parse_file`, `ast_free_file`
- Queries: `ast_find_function`, `ast_count_type`
- Project introspection: `ast_introspect`, `ast_free_project`
- JSON summaries: `ast_summary_json`, `ast_file_summary_json`, `ast_function_list_json`
- Graphs: `ast_dependency_graph`, `ast_call_graph`

### `eval.c` / `eval.h` (686 LOC / 81 LOC)

Purpose:

- Expression evaluator and bigint arithmetic helpers.

Public API:

- Eval context/value functions: `eval_init`, `eval_expr`, `eval_format`, `eval_multi`, `eval_set_var`, `eval_get_var`
- Bigint ops: `bigint_from_str`, `bigint_to_str`, `bigint_add`, `bigint_mul`, `bigint_factorial`, `bigint_is_prime`

### `pipeline.c` / `pipeline.h` (727 LOC / 98 LOC)

Purpose:

- Multi-stage text transform pipeline (filter/map/sort/uniq/etc.).

Public API:

- `pipeline_create/free`
- `pipeline_add_stage`, `pipeline_add_stage_n`
- `pipeline_execute`
- `pipeline_parse`
- `pipeline_run`

Internal behavior:

- Operates primarily on line arrays with stage-by-stage transforms.
- Supports 30+ stage enums including JSON/CSV extraction and stats.

## Foundation Utilities

### `json_util.c` / `json_util.h` (795 LOC / 95 LOC)

Purpose:

- JSON extraction helpers, dynamic string buffer, arena allocator.

Public API:

- `safe_malloc/realloc/strdup`
- `jbuf_*` builders
- `json_get_*` extractors
- `json_parse_response` (+ arena variant)
- `json_validate_schema`
- `json_array_foreach`
- `arena_*`

### `crypto.c` / `crypto.h` (511 LOC / 80 LOC)

Purpose:

- Pure-C crypto/encoding utility surface.

Public API includes:

- SHA-256, MD5, HMAC-SHA256
- Base64/base64url, hex encode/decode
- UUID v4 generation
- Random bytes/hex
- HKDF-SHA256
- JWT decode (parse only)
- Constant-time equality

### `error.c` / `error.h` (83 LOC / 75 LOC)

Purpose:

- Thread-local structured error state with code/message/cause.

Public API:

- `dsco_err_last`, `dsco_err_code`, `dsco_err_msg`, `dsco_err_clear`
- `dsco_err_set`, `dsco_err_wrap`, `dsco_err_code_str`
- Macros: `DSCO_SET_ERR`, `DSCO_WRAP_ERR`, `DSCO_TRY`, `DSCO_TRY_MSG`

### `config.h` (164 LOC)

Purpose:

- Build constants, model registry, defaults, and system prompt text.

Key exports:

- `DSCO_VERSION`
- buffer/token/context constants
- model registry + helper lookup/resolution inline functions
- default system prompt string

### `coroutine.h` (234 LOC)

Purpose:

- Macro-based stackless coroutine primitives and scheduler helpers.

Key exports:

- Static coroutine macros (`scr_*`)
- Context coroutine macros (`ccr_*`)
- Generator aliases (`gen_*`)
- `coro_scheduler_t` and inline round-robin scheduler helpers

## Testing and Build

### `test.c` (695 LOC)

Purpose:

- In-process test runner for utility + request-building + tool behavior.

Coverage includes:

- JSON helpers and escaping
- Conversation save/load/pop
- SHA-256/base64 utilities
- Eval parser
- Model alias/context helpers
- Request-build regressions for server/web tool result shapes
- Tool execution sanity and validation paths

### `Makefile` (51 LOC)

Purpose:

- Build/test/install orchestration.

Targets:

- `all` (default) -> `dsco`
- `test` -> `test_runner`
- `clean`
- `install` / `uninstall`

Notable details:

- Embeds `BUILD_DATE` and `GIT_HASH` via `CFLAGS`
- Auto-detects readline support

## Header-Only Module Inventory

All headers present and covered by paired module sections or utility sections:

- `agent.h`, `ast.h`, `baseline.h`, `config.h`, `coroutine.h`, `crypto.h`, `error.h`, `eval.h`, `integrations.h`, `ipc.h`, `json_util.h`, `llm.h`, `mcp.h`, `md.h`, `pipeline.h`, `plugin.h`, `provider.h`, `semantic.h`, `setup.h`, `swarm.h`, `tools.h`, `tui.h`
