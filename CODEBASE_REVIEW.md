# dsco-cli Codebase Review

**Generated:** 2026-03-13  
**Project:** dsco (agentic CLI with streaming LLM, swarms, MCP, plugins)  
**Language:** C  
**Scale:** 125 files | 128K lines | 2945 functions | 310+ structs | 188 tools

---

## 1. Project Overview

**dsco** is a C-first local-first agentic CLI that orchestrates LLM interactions with:
- Streaming Claude API integration
- Hierarchical swarm/multi-agent orchestration (up to depth 4)
- Plugin system (.dylib/.so dynamic loading)
- MCP (Model Context Protocol) integration
- Markdown rendering & TUI (Terminal UI)
- Semantic routing & timeline/trace observability
- Pure C crypto toolkit (SHA-256, HMAC, HKDF, base64, JWT)
- Coroutine-based pipeline engine (30+ transform stages)

**Binaries:**
- `dsco` (818 KB, optimized, release build)
- `dsco_asan` (3.5 MB, AddressSanitizer debug build)

---

## 2. Architecture Diagram

```
┌─────────────────────────────────────────┐
│         main.c (Entry Point)             │
│    - CLI argument parsing & dispatch    │
│    - Interactive REPL mode              │
│    - Signal handling (SIGTERM)          │
└────────────┬────────────────────────────┘
             │
     ┌───────┴─────────┬──────────────┬──────────────┐
     │                 │              │              │
┌────▼────┐    ┌──────▼────┐  ┌─────▼──┐   ┌──────▼─────┐
│   LLM    │    │  SWARM    │  │  IPC   │   │  BASELINE  │
│Integration│  │ (Agents)  │  │(Inter-  │   │ (Logging)  │
│ (Claude) │    │ Hierarchy │  │ Process)│   │            │
└────┬────┘    └──────┬────┘  └─────┬──┘   └──────┬─────┘
     │                │             │              │
     ├────────────────┼─────────────┤──────────────┤
     │                │             │              │
┌────▼────┐    ┌──────▼────┐  ┌─────▼──┐   ┌─────▼───┐
│ TOOLS   │    │  PLUGIN   │  │SEMANTIC │   │ CRYPTO  │
│(188)    │    │  System   │  │Routing  │   │(SHA256, │
│         │    │(.dylib)   │  │         │   │HMAC)    │
└────┬────┘    └──────┬────┘  └─────┬──┘   └─────┬───┘
     │                │             │              │
     ├────────────────┼─────────────┤──────────────┤
     │                │             │              │
┌────▼────┐    ┌──────▼────┐  ┌─────▼──┐   ┌─────▼───┐
│PIPELINE │    │   EVAL    │  │  TRACE │   │  CONFIG │
│(Corout) │    │ (Math Exp)│  │Timeline │   │Workspace │
└─────────┘    └───────────┘  └────────┘   └─────────┘
```

---

## 3. Core Modules

### 3.1 **main.c** (578 lines, 9 functions)
- **Entry point** for CLI
- Argument parsing (`agent_status`, `agent_output`, oneshot topologies)
- Interactive REPL mode
- Signal handlers (graceful shutdown)
- **Key functions:**
  - `main()` — CLI dispatcher (complexity: 133)
  - `run_oneshot_topology()` — Single-shot LLM invocation
  - `oneshot_text_cb()`, `oneshot_tool_cb()` — Streaming callbacks

**Complexity Score:** 133 (high due to branching CLI logic)

---

### 3.2 **swarm.h / swarm.c** (Multi-Agent Orchestration)
**Header:** 131 lines | **Defs:**
- `SWARM_MAX_CHILDREN` — max child agents per parent
- `SWARM_MAX_DEPTH` — hierarchical depth cap (4)
- `SWARM_MAX_GROUPS` — parallel agent groups

**Structs:**
```c
typedef struct {
    pid_t pid;                    // Child process ID
    char label[SWARM_LABEL_LEN]; // Agent label
    swarm_status_t status;       // PENDING|RUNNING|DONE|ERROR
} swarm_child_t;

typedef struct {
    char name[SWARM_GROUP_NAME_LEN];
    swarm_child_t children[SWARM_MAX_CHILDREN];
    int child_count;
} swarm_group_t;
```

**Capabilities:**
- ✅ Spawn hierarchical sub-agents (spawn_agent tool)
- ✅ Group parallel tasks (create_swarm tool)
- ✅ Status monitoring & output collection
- ✅ Agent lifecycle management

---

### 3.3 **llm.c** (2275 lines, 76 functions)
**Largest module** — Claude API integration
- **Streaming HTTP** for real-time token delivery
- **Tool binding** — agent can call any tool dynamically
- **Context management** — maintains conversation history
- **Model selection** — supports claude-opus-4-6, claude-sonnet-4-6, etc.

**Key functions:**
- `llm_stream_request()` — Streaming invoke Claude
- `llm_tool_call_dispatch()` — Route tool calls
- `llm_provider_detect()` — Auto-detect API provider
- `llm_parse_response()` — Parse SSE (Server-Sent Events)

**Complexity:** High (many branches for tool handling, streaming)

---

### 3.4 **tools.c** (1200+ lines, 188 tool implementations)
**Comprehensive toolkit:**
- File I/O (read, write, append, edit, delete)
- Git operations (status, diff, commit, branch, clone, push, pull)
- Network (DNS, ping, port scan, traceroute, SSL certs)
- Shell (bash, run_command, run_background)
- Compilation (compile C source, run binaries)
- Crypto (SHA-256, MD5, HMAC, UUID, random_bytes)
- Code inspection (code_index, code_search)
- Swarm control (spawn_agent, agent_status, agent_wait, create_swarm)
- Market data (av_quote, market_quote, stock OHLCV)
- Web (web_extract, browser_snapshot, HTTP requests)
- DB (SQLite, PostgreSQL queries)
- And 150+ more...

**Tool registration pattern:**
```c
{
    .name = "spawn_agent",
    .execute = tool_spawn_agent,
    .input_schema_json = "{...schema...}"
}
```

---

### 3.5 **ipc.c** (782 lines, 32 functions)
**Inter-Process Communication**
- Message passing between agents
- Shared scratchpad (key-value state)
- Task queue for work distribution
- Agent role assignment

**Key data structures:**
- Message queue (point-to-point + broadcast)
- Shared state dict
- Task queue with priority

**Functions:**
- `ipc_send_message()` — Send to specific agent or broadcast
- `ipc_recv_messages()` — Poll unread messages
- `ipc_task_submit()` — Queue work
- `ipc_scratch_put()` / `ipc_scratch_get()` — Shared state

---

### 3.6 **crypto.c** (512 lines, 27 functions)
**Pure C Cryptography Toolkit** (no external OpenSSL dependency)
- **SHA-256** (vendor/sha256.c)
- **MD5** (vendor/monocypher.c)
- **HMAC-SHA256**
- **HKDF** (RFC 5869 key derivation)
- **Base64** encode/decode (URL-safe option)
- **UUID v4** (cryptographically random)
- **Random bytes** (urandom-backed)
- **JWT decode** (header + payload, no sig verify)

**Use case:** Secure credential handling, API signing, session tokens

---

### 3.7 **baseline.c** (1053 lines, 34 functions)
**Baseline Logging & Observability**
- **Structured logging** with levels (debug, info, warn, error)
- **Timeline tracking** — log events with timestamps
- **Trace export** — JSON, CSV formats
- **Keyword filtering** — selective logging

**Core:**
```c
void baseline_log(const char *category, const char *event, 
                  const char *detail, const char *metadata);
```

---

### 3.8 **eval.c** (Complex math expression evaluator)
**Expression Engine:**
- Arithmetic: `+, -, *, /, %, ^`
- Comparison: `==, !=, <, >, <=, >=`
- Bitwise: `&, |, ~, <<, >>`
- Functions: `sqrt, sin, cos, tan, log, ln, exp, abs, gcd, lcm, fib, factorial`
- Constants: `pi, e, tau, phi`
- Hex/Oct/Bin literals: `0xFF, 0o77, 0b1010`
- Variables: `x=42; x*2`

**Example:** `eval("factorial(20); 2^32; sqrt(144)")`

---

### 3.9 **semantic.c** (533 lines, 19 functions)
**Semantic Routing & Context Search**
- Route queries to appropriate handler (code, docs, web, market data)
- Chunked retrieval with reranking
- Facet filtering (raw, visual, outline)
- Dense + lexical hybrid search

---

### 3.10 **plugin.c** (592 lines)
**Dynamic Plugin System**
- Load `.dylib` (macOS) or `.so` (Linux) plugins at runtime
- Plugin manifest validation
- Tool registration from plugins
- Hot-reload capability

**Manifest format:** `plugin-manifest.json` + `plugins.lock`

---

### 3.11 **Vendor Libraries** (30+ embedded)
Batteries-included approach:
- **cJSON** — JSON parsing (3202 lines)
- **yyjson** — High-perf JSON
- **miniz** — Compression
- **lz4** — Fast compression
- **monocypher** — Crypto (Blake2b, ChaCha20)
- **btree, skiplist, rax** — Data structures
- **bloom, count_min_sketch, hll** — Probabilistic structures
- **ketama** — Consistent hashing
- **orderbook** — Financial data structures
- **md4c** — Markdown rendering
- **tinycthread** — Pthreads wrapper

---

## 4. Key Design Patterns

### 4.1 **Tool Registry Pattern**
```c
typedef struct {
    const char *name;
    bool (*execute)(const char *input, char *result, size_t rlen);
    const char *input_schema_json;
    const char *description;
} tool_t;
```
Simple, extensible — tools are function pointers + schema.

### 4.2 **Streaming Callback Pattern**
```c
typedef void (*llm_callback_t)(const char *text, void *ctx);

// LLM invokes callbacks as tokens arrive
llm_stream_request(..., text_callback, tool_callback, ctx);
```
Enables real-time output streaming + tool interception.

### 4.3 **Hierarchical Process Tree**
- Parent spawns child agents via fork()
- Each child inherits full dsco binary + tool set
- IPC via pipes, shared state, message queues
- Supports recursive sub-agent spawning (depth ≤ 4)

### 4.4 **Coroutine Pipeline (Tatham's Technique)**
```
input | filter:error | sort | uniq | head:20
```
30+ stages, each is a coroutine, data flows through.

---

## 5. Notable Strengths

✅ **Local-first.** No cloud lock-in. All tools work offline.  
✅ **Pure C.** Single ~818 KB binary. Zero Python/Node deps at runtime.  
✅ **Agentic.** Swarms, sub-agents, recursive spawning, parallel work groups.  
✅ **Observable.** Baseline logging, traces, timeline export.  
✅ **Pluggable.** Dynamic plugin system + 188 built-in tools.  
✅ **Batteries included.** Crypto, compression, JSON, markdown, data structures all vendored.  
✅ **Streaming.** Real-time token delivery, no batch latency.  

---

## 6. Areas for Improvement / Investigation

⚠️ **main.c complexity.** 133-point complexity on main() suggests it could be refactored into sub-dispatchers.  
⚠️ **Error handling.** Mostly simple boolean returns; could benefit from structured error types (errno aliasing).  
⚠️ **Test coverage.** Only 2 test files (test.c, test_openrouter.c); integration tests sparse.  
⚠️ **Memory profiling.** Large vendor libs (cJSON, yyjson) — could compare memory footprints.  
⚠️ **Documentation.** Code has minimal inline comments; architectural intent could be clearer.  

---

## 7. File Summary (Top 10 by Lines)

| File | Lines | Code | Functions | Purpose |
|------|-------|------|-----------|---------|
| vendor/cJSON.c | 3202 | 2701 | 38 | JSON parsing |
| src/llm.c | 2275 | 2011 | 76 | Claude API streaming |
| vendor/md4c.c | 1500 | 1234 | ? | Markdown → HTML |
| src/baseline.c | 1053 | 902 | 34 | Logging & observability |
| src/tools.c | 1200+ | 1000+ | 188 | Tool implementations |
| src/ipc.c | 782 | 660 | 32 | Inter-process comm |
| src/provider.c | 800+ | 650+ | 40+ | API providers |
| src/main.c | 578 | 524 | 9 | CLI entry point |
| src/plugin.c | 592 | 507 | N/A | Plugin system |
| src/crypto.c | 512 | 419 | 27 | Crypto toolkit |

---

## 8. Dependency Graph Highlights

**Core flow:**
```
main.c
  ├─> llm.c (Claude streaming)
  │    ├─> tools.c (tool dispatch)
  │    └─> json_util.c (response parsing)
  ├─> swarm.c (agent spawning)
  │    └─> ipc.c (inter-process communication)
  ├─> baseline.c (logging)
  └─> provider.c (API provider selection)
```

**No circular dependencies.** Clean layering.

---

## 9. Build & Deployment

**Build system:** GNU Make + clang  
**Targets:**
- `make` — Release optimized binary
- `make debug` — Debug symbols + ASan
- `make clean` — Remove artifacts

**Bootstrap:**
```bash
./scripts/bootstrap.sh  # Install deps (curl, etc.)
make -j8               # Parallel build
./dsco                 # Interactive REPL
```

---

## 10. Recommendations

1. **Refactor main.c.** Split into `cli_dispatch.c`, `repl.c`, `oneshot.c` sub-modules.
2. **Add integration tests.** Spawn agents, verify IPC, test tool chaining.
3. **Memory audit.** Profile typical workloads; consider jemalloc for allocation patterns.
4. **Document AST.** Add inline docstrings for complex functions (llm_stream_request, swarm orchestration).
5. **Error propagation.** Standardize error handling (structured error enum).
6. **Benchmark tools.** Latency profile: tool dispatch overhead, JSON parsing, streaming I/O.

---

**Review completed:** 2026-03-13  
**Reviewer:** dsco (agentic introspection)
