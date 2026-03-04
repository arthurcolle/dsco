# Claude Code Session — Complete Tool Schema Reference

> **Generated**: 2026-03-01
> **Total tools available**: ~160+ (53 native + 50 Tool Management API remote + 107 Alpha Vantage financial + MCP resource tools)
> **MCP Backends**: 6 (4 custom distributed MCPs + Alpha Vantage + Tool Management API)

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Native Claude Code Tools (17)](#2-native-claude-code-tools)
3. [Task Management Tools (4)](#3-task-management-tools)
4. [MCP Resource Tools (2)](#4-mcp-resource-tools)
5. [Tool Management API — Orchestration Layer (26)](#5-tool-management-api--orchestration-layer)
6. [Distributed Memory MCP — 16 tools](#6-distributed-memory-mcp)
7. [Distributed Search MCP — 5 tools](#7-distributed-search-mcp)
8. [Distributed Research MCP — 8 tools](#8-distributed-research-mcp)
9. [Distributed Publish MCP — 21 tools](#9-distributed-publish-mcp)
10. [Alpha Vantage Financial MCP — 107 tools](#10-alpha-vantage-financial-mcp)
11. [Execution Policy Engine Deep Dive](#11-execution-policy-engine)
12. [Database Schema](#12-database-schema)
13. [Infrastructure: Circuit Breakers, Caching, Rate Limiting](#13-infrastructure)
14. [Workflow Composition Engine](#14-workflow-composition-engine)
15. [Recommendation Engine](#15-recommendation-engine)
16. [Follow-Up Action System](#16-follow-up-action-system)
17. [Environment Variables & Configuration](#17-configuration)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                     CLAUDE CODE SESSION (CLI)                       │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │ Native Tools │  │ Task Mgmt    │  │ MCP Protocol Layer       │  │
│  │ (17 tools)   │  │ (4 tools)    │  │                          │  │
│  │              │  │              │  │  ┌─────────────────────┐  │  │
│  │ Read/Write   │  │ TaskCreate   │  │  │ Tool Management API │  │  │
│  │ Edit/Glob    │  │ TaskGet      │  │  │ tools.distributed   │  │  │
│  │ Grep/Bash    │  │ TaskUpdate   │  │  │ .systems            │  │  │
│  │ WebFetch     │  │ TaskList     │  │  │ (26 meta-tools)     │  │  │
│  │ WebSearch    │  │              │  │  └────────┬────────────┘  │  │
│  │ Task/Agent   │  │              │  │           │               │  │
│  │ AskUser      │  │              │  │           ▼               │  │
│  │ Skill        │  │              │  │  ┌─────────────────────┐  │  │
│  │ PlanMode     │  │              │  │  │ 4 Distributed       │  │  │
│  │ Worktree     │  │              │  │  │ Backends (Modal)    │  │  │
│  │ Notebook     │  │              │  │  │ 50 remote tools     │  │  │
│  └──────────────┘  └──────────────┘  │  └─────────────────────┘  │  │
│                                       │                          │  │
│                                       │  ┌─────────────────────┐ │  │
│                                       │  │ Alpha Vantage MCP   │ │  │
│                                       │  │ 107 financial tools │ │  │
│                                       │  └─────────────────────┘ │  │
│                                       └──────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘

Tool Management API Internal Architecture:
┌──────────────────────────────────────────────────────────────────┐
│                    tools.distributed.systems                      │
│                                                                   │
│  ┌────────────┐  ┌───────────┐  ┌────────────┐  ┌────────────┐  │
│  │ Policy     │  │ Streaming │  │ Circuit    │  │ Rate       │  │
│  │ Engine     │  │ Executor  │  │ Breaker    │  │ Limiter    │  │
│  │            │  │           │  │            │  │            │  │
│  │ auto→      │  │ SSE       │  │ CLOSED→    │  │ TokenBucket│  │
│  │ sync|      │  │ WebSocket │  │ OPEN→      │  │ Sliding    │  │
│  │ stream|    │  │ Detached  │  │ HALF_OPEN  │  │ Window     │  │
│  │ job        │  │ Jobs      │  │            │  │ Per-client │  │
│  └────────────┘  └───────────┘  └────────────┘  └────────────┘  │
│                                                                   │
│  ┌────────────┐  ┌───────────┐  ┌────────────┐  ┌────────────┐  │
│  │ Workflow   │  │ Recommend │  │ Follow-Up  │  │ Cache      │  │
│  │ Composer   │  │ Engine    │  │ Generator  │  │ (LRU)      │  │
│  │            │  │           │  │            │  │            │  │
│  │ Sequential │  │ Embedding │  │ Fanout     │  │ Per-tool   │  │
│  │ Parallel   │  │ + Intent  │  │ Compaction │  │ TTL policy │  │
│  │ MapReduce  │  │ Scoring   │  │ Reducer    │  │ 10K max    │  │
│  │ Gates      │  │           │  │            │  │            │  │
│  └────────────┘  └───────────┘  └────────────┘  └────────────┘  │
│                                                                   │
│  Persistence: SQLite (executions.db) + DuckDB (tools.duckdb)     │
│  Backends: Modal-hosted distributed MCPs (4 services)             │
└──────────────────────────────────────────────────────────────────┘
```

### Runtime Stats (Live)
| Metric | Value |
|--------|-------|
| Registered remote tools | 50 |
| Active backends | 4/4 healthy |
| Total historical calls | 242 |
| Global error rate | 8.26% |
| Circuit breakers | All CLOSED (healthy) |

---

## 2. Native Claude Code Tools

### 2.1 `Read`
**Purpose**: Read files from the local filesystem. Supports text, images, PDFs, Jupyter notebooks.

```jsonc
{
  "file_path": string,    // REQUIRED — absolute path
  "offset": number,       // optional — start line (1-indexed)
  "limit": number,        // optional — max lines to read (default 2000)
  "pages": string         // optional — PDF page range "1-5" (max 20 pages/request)
}
```

**Constraints**: Lines >2000 chars truncated. PDF >10 pages MUST use `pages`. Returns `cat -n` format.

---

### 2.2 `Write`
**Purpose**: Create or overwrite a file.

```jsonc
{
  "file_path": string,    // REQUIRED — absolute path
  "content": string       // REQUIRED — full file content
}
```

**Constraints**: Must `Read` before overwriting existing files. Prefer `Edit` for modifications.

---

### 2.3 `Edit`
**Purpose**: Exact string replacement in files.

```jsonc
{
  "file_path": string,    // REQUIRED — absolute path
  "old_string": string,   // REQUIRED — text to find (must be unique unless replace_all)
  "new_string": string,   // REQUIRED — replacement text (must differ from old_string)
  "replace_all": boolean  // optional — default false. Replace all occurrences.
}
```

**Constraints**: Fails if `old_string` not unique and `replace_all=false`. Must `Read` file first.

---

### 2.4 `Glob`
**Purpose**: Fast file pattern matching across any codebase size.

```jsonc
{
  "pattern": string,      // REQUIRED — glob pattern ("**/*.js", "src/**/*.ts")
  "path": string          // optional — directory to search (defaults to cwd)
}
```

**Returns**: Matching paths sorted by modification time.

---

### 2.5 `Grep`
**Purpose**: Content search using ripgrep engine.

```jsonc
{
  "pattern": string,       // REQUIRED — regex pattern (ripgrep syntax)
  "path": string,          // optional — file or directory to search
  "glob": string,          // optional — file glob filter ("*.js", "*.{ts,tsx}")
  "type": string,          // optional — file type ("js", "py", "rust", "go")
  "output_mode": enum,     // optional — "files_with_matches" | "content" | "count"
  "-A": number,            // optional — lines after match (content mode only)
  "-B": number,            // optional — lines before match (content mode only)
  "-C": number,            // optional — context lines (alias for context)
  "context": number,       // optional — lines before+after match
  "-i": boolean,           // optional — case insensitive
  "-n": boolean,           // optional — show line numbers (default true, content mode)
  "multiline": boolean,    // optional — patterns span lines (default false)
  "head_limit": number,    // optional — limit output entries (0=unlimited)
  "offset": number         // optional — skip first N entries (0=none)
}
```

**Notes**: Uses ripgrep, not grep. Literal braces need escaping (`interface\{\}`).

---

### 2.6 `Bash`
**Purpose**: Execute shell commands.

```jsonc
{
  "command": string,                   // REQUIRED
  "description": string,              // optional — human-readable description
  "timeout": number,                  // optional — ms, max 600000, default 120000
  "run_in_background": boolean,       // optional — run async, get task_id
  "dangerouslyDisableSandbox": boolean // optional — bypass sandbox
}
```

**Constraints**: Sandboxed by default. Background tasks return task_id for `TaskOutput`.

---

### 2.7 `Task` (Agent Launcher)
**Purpose**: Launch autonomous sub-agents for complex multi-step work.

```jsonc
{
  "description": string,    // REQUIRED — 3-5 word summary
  "prompt": string,         // REQUIRED — detailed task description
  "subagent_type": enum,    // REQUIRED — see agent types below
  "model": enum,            // optional — "sonnet" | "opus" | "haiku"
  "resume": string,         // optional — agent ID to resume
  "run_in_background": boolean, // optional
  "isolation": enum,        // optional — "worktree" for git isolation
  "max_turns": integer      // optional — max API round-trips (>0)
}
```

**Agent Types & Their Tool Access**:

| Agent Type | Tools Available | Use Case |
|------------|----------------|----------|
| `general-purpose` | All tools (*) | Complex multi-step tasks |
| `Explore` | All except Task, ExitPlanMode, Edit, Write, NotebookEdit | Codebase exploration, search |
| `Plan` | All except Task, ExitPlanMode, Edit, Write, NotebookEdit | Architecture planning |
| `claude-code-guide` | Glob, Grep, Read, WebFetch, WebSearch | Claude Code documentation |
| `statusline-setup` | Read, Edit | Status line configuration |

---

### 2.8 `TaskOutput`
**Purpose**: Retrieve output from background tasks.

```jsonc
{
  "task_id": string,     // REQUIRED
  "block": boolean,      // REQUIRED — default true (wait for completion)
  "timeout": number      // REQUIRED — default 30000, max 600000 ms
}
```

---

### 2.9 `TaskStop`
**Purpose**: Terminate a running background task.

```jsonc
{
  "task_id": string,     // optional — background task ID
  "shell_id": string     // DEPRECATED — use task_id
}
```

---

### 2.10 `WebFetch`
**Purpose**: Fetch URL content, convert to markdown, process with AI.

```jsonc
{
  "url": string,         // REQUIRED — fully-formed URL (http→https auto-upgrade)
  "prompt": string       // REQUIRED — extraction/analysis instructions
}
```

**Constraints**: Fails on authenticated URLs. 15-min cache. Reports redirects.

---

### 2.11 `WebSearch`
**Purpose**: Web search with result blocks and markdown links.

```jsonc
{
  "query": string,            // REQUIRED — min 2 chars
  "allowed_domains": [string], // optional — whitelist
  "blocked_domains": [string]  // optional — blacklist
}
```

---

### 2.12 `NotebookEdit`
**Purpose**: Edit Jupyter notebook cells.

```jsonc
{
  "notebook_path": string,   // REQUIRED — absolute path to .ipynb
  "new_source": string,      // REQUIRED — new cell content
  "cell_id": string,         // optional — target cell ID
  "cell_type": enum,         // optional — "code" | "markdown"
  "edit_mode": enum          // optional — "replace" | "insert" | "delete"
}
```

---

### 2.13 `AskUserQuestion`
**Purpose**: Interactive user prompts with selectable options.

```jsonc
{
  "questions": [{                          // REQUIRED — 1-4 questions
    "question": string,                    // REQUIRED — ends with ?
    "header": string,                      // REQUIRED — max 12 chars
    "options": [{                          // REQUIRED — 2-4 options
      "label": string,                     // REQUIRED — 1-5 words
      "description": string,              // REQUIRED — explanation
      "markdown": string                  // optional — preview content (monospace)
    }],
    "multiSelect": boolean                // REQUIRED — allow multiple selections
  }],
  "answers": { [question]: string },      // optional — pre-collected
  "annotations": { [question]: {          // optional
    "markdown": string,
    "notes": string
  }},
  "metadata": { "source": string }       // optional — tracking
}
```

---

### 2.14 `Skill`
**Purpose**: Invoke registered skills (slash commands).

```jsonc
{
  "skill": string,      // REQUIRED — skill name ("commit", "review-pr")
  "args": string         // optional — arguments
}
```

**Available Skills**: `keybindings-help`, `claude-developer-platform`, `frontend-design`

---

### 2.15 `EnterPlanMode`
**Purpose**: Enter plan mode for implementation design. No parameters.

### 2.16 `ExitPlanMode`
**Purpose**: Signal plan completion for user approval.

```jsonc
{
  "allowedPrompts": [{        // optional — permissions needed
    "tool": "Bash",           // REQUIRED — only "Bash" supported
    "prompt": string          // REQUIRED — semantic description ("run tests")
  }]
}
```

### 2.17 `EnterWorktree`
**Purpose**: Create isolated git worktree for the session.

```jsonc
{
  "name": string             // optional — worktree name (random if omitted)
}
```

---

## 3. Task Management Tools

### 3.1 `TaskCreate`

```jsonc
{
  "subject": string,         // REQUIRED — imperative title ("Fix auth bug")
  "description": string,     // REQUIRED — detailed requirements
  "activeForm": string,      // optional — spinner text ("Fixing auth bug")
  "metadata": object         // optional — arbitrary key-value
}
```

### 3.2 `TaskGet`

```jsonc
{
  "taskId": string           // REQUIRED
}
```

### 3.3 `TaskUpdate`

```jsonc
{
  "taskId": string,          // REQUIRED
  "status": enum,            // optional — "pending" | "in_progress" | "completed" | "deleted"
  "subject": string,         // optional
  "description": string,     // optional
  "activeForm": string,      // optional
  "owner": string,           // optional
  "metadata": object,        // optional — merge (set key to null to delete)
  "addBlocks": [string],     // optional — task IDs this blocks
  "addBlockedBy": [string]   // optional — task IDs blocking this
}
```

### 3.4 `TaskList`
No parameters. Returns all tasks with id, subject, status, owner, blockedBy.

---

## 4. MCP Resource Tools

### 4.1 `ListMcpResourcesTool`

```jsonc
{
  "server": string           // optional — filter by MCP server name
}
```

### 4.2 `ReadMcpResourceTool`

```jsonc
{
  "server": string,          // REQUIRED — MCP server name
  "uri": string              // REQUIRED — resource URI
}
```

---

## 5. Tool Management API — Orchestration Layer

**Source**: `~/dsco/dsco-autobot/ToolManagement`
**Deployed to**: `tools.distributed.systems`
**Persistence**: SQLite (`executions.db`) + DuckDB (`tools.duckdb`)

### 5.1 `search_tools`
**Purpose**: Semantic search for tools by description/capability.

```jsonc
{
  "query": string,           // REQUIRED — natural language query
  "limit": integer,          // optional — default 5
  "min_score": number        // optional — default 0.3 (0-1)
}
```

### 5.2 `get_tool`
**Purpose**: Full schema for a specific tool.

```jsonc
{
  "tool_name": string        // REQUIRED — exact tool name
}
```

### 5.3 `execute_tool`
**Purpose**: Execute any registered tool with full execution control.

```jsonc
{
  "tool_name": string,              // REQUIRED
  "arguments": object,              // REQUIRED — tool-specific args
  "execution_mode": enum,           // optional — "auto" | "sync" | "stream" | "job" (default "auto")
  "stream": boolean,                // optional — request SSE streaming (default false)
  "timeout_ms": integer,            // optional — default 30000
  "sync_timeout_ms": integer,       // optional — auto-mode sync budget before escalation (default 5000)
  "priority": string,               // optional — "p0"|"p1"|"p2"|"p3" or "critical"|"high"|"normal"|"background"
  "dedupe_key": string,             // optional — explicit dedup key
  "dedupe_window_seconds": integer, // optional — default 60
  "idempotency_key": string,        // optional — prevent duplicate writes
  "max_output_chars": integer,      // optional — default 32000
  "max_links": integer,             // optional — default 50
  "early_stop": {                   // optional — streaming early-stop policy
    "enabled": boolean,
    "min_chunks": integer,
    "max_wait_ms": integer,
    "quality_threshold": number,
    "cost_budget": number
  },
  "policy_constraints": {           // optional — adaptive execution constraints
    "latency_budget_ms": number,
    "cost_budget": number,
    "reliability_target": number,   // 0-1
    "prefer_streaming": boolean,
    "allow_sync": boolean,
    "allow_stream": boolean,
    "allow_job": boolean
  }
}
```

**Extended execute_tool fields** (from source code, not exposed in MCP schema but accepted by the API):

```jsonc
{
  "convergence_key": string,        // semantic coalescing key
  "fanout_budget": integer,         // max parallel fanout
  "group_reducer": enum,            // "append_unique"|"append"|"latest"|"top_k"|"windowed"|"score_weighted"
  "group_reducer_config": object,   // reducer-specific config
  "callback_url": string,           // webhook for completion
  "callback_events": [string],      // "progress"|"complete"|"error"|"cancelled"|"all"
  "trace_id": string,               // distributed tracing
  "session_id": string,
  "parent_execution_id": string,
  "estimated_cost_cents": integer,
  "max_cost_cents": integer
}
```

### 5.4 `batch_execute`
**Purpose**: Execute multiple independent tool calls with preflight dedup and fanout control.

```jsonc
{
  "calls": [{                        // REQUIRED — array of tool calls
    "tool": string,                  // tool name (alias for tool_name)
    "tool_name": string,             // tool name
    "arguments": object,
    "execution_mode": enum,          // default "auto"
    "priority": string,              // default "p1"
    "dedupe_key": string,
    "dedupe_window_seconds": integer, // default 60
    "idempotency_key": string
  }],
  "execution": enum,                 // optional — "parallel" | "sequential" (default "parallel")
  "max_parallel": integer,           // optional — default 16
  "dedupe_calls": boolean,           // optional — deduplicate within batch (default true)
  "dedupe_on_tool": boolean,         // optional — include tool in dedup fingerprint (default true)
  "max_canonical_calls": integer,    // optional — cap unique calls after dedup
  "overflow_policy": enum,           // optional — "reject" | "truncate" (default "reject")
  "auto_follow_up": boolean,         // optional — persist follow-ups on risky fanout (default true)
  "session_id": string,
  "trace_id": string
}
```

### 5.5 `compose_workflow`
**Purpose**: Compose tools into multi-step workflows.

```jsonc
{
  "workflow_type": enum,             // REQUIRED — "sequential"|"parallel"|"map"|"reduce"|"map_reduce"|"segment_process"|"pipe"
  "tools": [string],                 // REQUIRED — tool names in order
  "inputs": object,                  // REQUIRED — initial workflow inputs
  "arguments": [object]              // optional — per-tool arguments
}
```

### 5.6 `submit_job`
**Purpose**: Submit long-running tool as background job. Returns job_id immediately.

```jsonc
{
  "tool_name": string,               // REQUIRED
  "arguments": object,               // REQUIRED
  "timeout": number,                 // optional — max seconds (default 300)
  "priority": string,                // optional — default "p1"
  "dedupe_key": string,
  "dedupe_window_seconds": integer   // default 60
}
```

### 5.7 `get_job_status`
**Purpose**: Poll job status and results.

```jsonc
{
  "job_id": string                   // REQUIRED
}
```

**Returns**: `{ status, progress, partial_results, final_result }`

### 5.8 `cancel_job`

```jsonc
{
  "job_id": string                   // REQUIRED
}
```

### 5.9 Registry & Discovery Tools

#### `list_tool_categories`
No parameters. Returns backends with name, tool_count, health, URL.

#### `get_registry_stats`
No parameters. Returns tool_count, backend_count, total_calls, error_rate.

#### `list_tools_paginated`
```jsonc
{ "offset": integer, "limit": integer, "category": string }
// defaults: offset=0, limit=20
```

#### `list_tool_names`
```jsonc
{ "category": string }  // optional filter
```

#### `batch_get_tools`
```jsonc
{
  "names": [string],       // REQUIRED — exact tool names
  "compact": boolean       // optional — compact signatures (default true)
}
```

#### `query_tools`
```jsonc
{
  "search": string,        // text search
  "category": string,
  "tags": string,          // comma-separated
  "backend_id": string,
  "sort_by": enum,         // "name"|"call_count"|"updated_at"|"avg_latency_ms"|"error_count"
  "sort_order": enum,      // "asc"|"desc"
  "offset": integer,       // default 0
  "limit": integer         // default 20
}
```

#### `export_tool_catalog`
```jsonc
{
  "format": enum,          // "mcp"|"openapi"|"schemas" (default "mcp")
  "limit": integer         // default 500
}
```

### 5.10 Observability & Metrics Tools

#### `get_usage_stats`
```jsonc
{ "hours": integer }  // default 24
```

#### `get_backend_health`
```jsonc
{ "hours": integer }  // default 24
```
Returns per-backend: calls, successes, failures, error_rate, p50/p95/p99 latency, circuit_breaker state.

#### `get_analytics_overview`
```jsonc
{ "hours": integer, "bucket_minutes": integer, "chain_limit": integer }
// defaults: 24, 60, 20
```

#### `get_tool_metrics`
```jsonc
{
  "tool_name": string,    // REQUIRED
  "hours": integer         // default 24
}
```
Returns latency percentiles, reliability, most recent error.

#### `get_tool_usage_timeseries`
```jsonc
{ "tool_name": string, "hours": integer, "bucket_minutes": integer }
// defaults: —, 24, 60
```

### 5.11 Dependency & Policy Tools

#### `get_tool_dependencies`
```jsonc
{
  "tool_name": string,
  "dependency_type": string,
  "min_confidence": number,  // default 0
  "limit": integer           // default 200
}
```

#### `infer_tool_dependencies`
```jsonc
{
  "hours": integer,          // default 24
  "min_support": integer,    // default 2
  "persist": boolean,        // default true
  "limit": integer           // default 100
}
```

#### `get_execution_policy_decisions`
```jsonc
{ "tool_name": string, "trace_id": string, "hours": integer, "limit": integer }
// defaults: —, —, 24, 200
```

#### `get_execution_policy_analytics`
```jsonc
{ "hours": integer, "limit": integer }
// defaults: 24, 500
```

### 5.12 Recommendation Tools

#### `recommend_tools`
```jsonc
{
  "query": string,
  "category": string,
  "intent": string,           // "query"|"store"|"publish"|"analyze"|"manage"
  "tags": [string],
  "backend_id": string,
  "max_latency_ms": number,
  "require_streaming": boolean, // default false
  "include_dependencies": boolean, // default true
  "limit": integer              // default 8
}
```

#### `record_recommendation_feedback`
```jsonc
{
  "recommendation_id": string,  // REQUIRED
  "accepted": boolean,          // default false
  "chosen_tool": string,
  "recommendation": object,
  "outcome_status": string,     // "success"|"failure"|"partial"
  "latency_ms": number,
  "error": string,
  "context": object,
  "metadata": object,
  "session_id": string,
  "trace_id": string
}
```

#### `get_recommendation_feedback_analytics`
```jsonc
{ "hours": integer, "limit": integer }  // defaults: 24, 25
```

### 5.13 Workflow Gate Tools

#### `record_workflow_gate_decision`
```jsonc
{
  "execution_id": string,    // REQUIRED
  "step_key": string,
  "gate_type": string,       // default "agent_approval" — also: "auto"|"quality_check"|"branch"
  "action": string,          // default "proceed" — also: "skip"|"abort"|"fork"|"rewind"
  "status": string,          // default "accepted"
  "actor": string,           // default "agent"
  "reason": string,
  "payload": object,
  "session_id": string,
  "trace_id": string
}
```

#### `list_workflow_gate_decisions`
```jsonc
{ "execution_id": string, "trace_id": string, "limit": integer }
// default limit: 500
```

### 5.14 Follow-Up Action Tools

#### `generate_execution_follow_ups`
```jsonc
{
  "hours": integer,                    // default 24
  "auto_persist": boolean,            // default true
  "session_id": string,
  "trace_id": string,
  "merge_pressure_threshold": number, // default 2
  "items_threshold": integer,         // default 200
  "max_items_target": integer,        // default 120
  "dedupe_window_seconds": integer,   // default 3600
  "limit": integer,                   // default 50
  "include_fanout_actions": boolean,  // default true
  "include_reducer_actions": boolean, // default true
  "include_compaction_actions": boolean // default true
}
```

#### `list_execution_follow_ups`
```jsonc
{
  "status": string,          // "open"|"in_progress"|"completed"|"dismissed"|"failed"
  "priority": string,        // "normal"|"high"|"low"
  "action_type": string,
  "source_type": string,
  "session_id": string,
  "trace_id": string,
  "limit": integer           // default 200
}
```

#### `get_execution_follow_up_analytics`
```jsonc
{ "hours": integer, "limit": integer }  // defaults: 24, 25
```

#### `update_execution_follow_up_status`
```jsonc
{
  "follow_up_id": string,   // REQUIRED
  "status": string,          // default "completed"
  "metadata": object
}
```

---

## 6. Distributed Memory MCP

**Backend**: `distributed-memory-mcp`
**URL**: `https://arthurcolle--distributed-memory-mcp-web.modal.run`
**Tools**: 16

### Memory Architecture
```
┌──────────────────────────────────────────────┐
│              Memory Tiers                     │
│                                               │
│  ┌─────────┐  ┌──────────┐  ┌────────────┐  │
│  │ Working  │  │ Episodic │  │ Semantic   │  │
│  │ Memory   │  │ Memory   │  │ Memory     │  │
│  │          │  │          │  │            │  │
│  │ focus()  │  │ record   │  │ store      │  │
│  │ expand   │  │ recall   │  │ query      │  │
│  │ context  │  │ episodes │  │ beliefs    │  │
│  │          │  │          │  │ (SPO       │  │
│  │ High     │  │ Time-    │  │  triples)  │  │
│  │ activation│ │ indexed  │  │            │  │
│  └─────────┘  └──────────┘  └────────────┘  │
│                                               │
│  ┌──────────────────────────────────────┐    │
│  │ Procedural Memory (Skills)           │    │
│  │ learn_skill() / apply_skill()        │    │
│  │ Executable patterns with pre/post    │    │
│  └──────────────────────────────────────┘    │
│                                               │
│  Cross-cutting: consolidate(), forget(),     │
│  search_all(), semantic_search()              │
└──────────────────────────────────────────────┘
```

### 6.1 `focus`
```jsonc
{
  "item_ids": [string],      // optional — existing items to boost
  "content": string,         // optional — new content to add
  "importance": number,      // optional — 0-1, default 0.7
  "tags": [string]           // optional
}
```

### 6.2 `expand_context`
```jsonc
{
  "query": string,           // REQUIRED
  "limit": integer,          // optional — default 10
  "include_episodic": boolean, // optional — default true
  "include_semantic": boolean  // optional — default true
}
```

### 6.3 `record_episode`
```jsonc
{
  "event_type": string,      // REQUIRED — e.g. "task_completed", "error", "interaction"
  "content": object,         // REQUIRED — event details
  "context": object,         // optional
  "participants": [string],  // optional — entities involved
  "outcome": string,         // optional
  "importance": number       // optional — 0-1, default 0.5
}
```

### 6.4 `recall_episodes`
```jsonc
{
  "query": string,           // optional — similarity search
  "event_type": string,      // optional — filter
  "participant": string,     // optional — filter
  "start_time": number,      // optional — unix timestamp
  "end_time": number,        // optional — unix timestamp
  "limit": integer           // optional — default 20
}
```

### 6.5 `store_belief`
```jsonc
{
  "subject": string,         // REQUIRED — e.g. "Python"
  "predicate": string,       // REQUIRED — e.g. "is_used_for"
  "object": string,          // REQUIRED — e.g. "web development"
  "confidence": number,      // optional — 0-1, default 1.0
  "source": string           // optional
}
```

### 6.6 `query_beliefs`
```jsonc
{
  "subject": string,         // optional — match subject
  "predicate": string,       // optional — match predicate
  "object": string,          // optional — match object
  "min_confidence": number,  // optional — default 0.0
  "infer": boolean           // optional — enable transitive inference (default false)
}
```

### 6.7 `find_related_beliefs`
```jsonc
{
  "entity": string,          // REQUIRED
  "limit": integer           // optional — default 20
}
```

### 6.8 `semantic_search_beliefs`
```jsonc
{
  "query": string,           // REQUIRED — natural language
  "limit": integer,          // optional — default 10
  "min_similarity": number   // optional — cosine threshold, default 0.3
}
```

### 6.9 `learn_skill`
```jsonc
{
  "name": string,            // REQUIRED
  "description": string,     // REQUIRED
  "pattern": object,         // REQUIRED — executable pattern (steps, params)
  "preconditions": [string], // optional
  "postconditions": [string],// optional
  "tags": [string]           // optional
}
```

### 6.10 `apply_skill`
```jsonc
{
  "name": string,            // optional — skill name
  "skill_id": string,        // optional — alternative to name
  "context": object,         // optional — current context
  "execute": boolean,        // optional — actually execute (default false)
  "record_result": boolean,  // optional — record outcome (default true)
  "success": boolean,        // optional — execution succeeded?
  "execution_time": number   // optional — seconds
}
```

### 6.11 `consolidate`
```jsonc
{
  "working_to_episodic": boolean,    // optional — default true
  "importance_threshold": number,    // optional — default 0.6
  "prune_working": boolean           // optional — default true
}
```

### 6.12 `forget`
```jsonc
{
  "tier": enum,              // REQUIRED — "working"|"episodic"|"semantic"|"procedural"
  "id": string,              // optional — single item
  "item_ids": [string],      // optional — multiple items
  "older_than": number,      // optional — seconds ago
  "below_importance": number // optional — importance threshold
}
```

### 6.13 `search_all`
```jsonc
{
  "query": string,           // REQUIRED
  "tiers": [enum],           // optional — ["working","episodic","semantic","procedural"]
  "limit_per_tier": integer  // optional — default 5
}
```

### 6.14 `semantic_search`
```jsonc
{
  "query": string,           // REQUIRED
  "tiers": [enum],           // optional — ["episodic","working"]
  "limit": integer,          // optional — default 10
  "min_similarity": number   // optional — default 0.3
}
```

### 6.15 `get_memory_stats`
No parameters.

### 6.16 `_connectivity_test`
No parameters.

---

## 7. Distributed Search MCP

**Backend**: `distributed-search-mcp`
**URL**: `https://arthurcolle--distributed-search-mcp-web.modal.run`
**Tools**: 5 | **Avg latency**: ~38.5s | **Circuit breaker**: CLOSED

### 7.1 `search`
```jsonc
{
  "query": string,           // REQUIRED
  "num_results": integer,    // optional — 1-20, default 10
  "max_output_chars": integer, // optional — default 32000
  "max_links": integer       // optional — default 50
}
```

### 7.2 `read`
```jsonc
{
  "url": string,             // REQUIRED
  "include_images": boolean, // optional — default false
  "max_output_chars": integer // optional — default 32000
}
```

### 7.3 `embed`
```jsonc
{
  "text": string,            // REQUIRED
  "model": string            // optional — default "jina-embeddings-v3"
}
```

### 7.4 `rerank`
```jsonc
{
  "query": string,           // REQUIRED
  "documents": [string],     // REQUIRED — documents to rerank
  "top_k": integer           // optional — default 5
}
```

### 7.5 `verify`
```jsonc
{
  "statement": string        // REQUIRED — claim to fact-check
}
```

---

## 8. Distributed Research MCP

**Backend**: `distributed-research-mcp`
**URL**: `https://arthurcolle--distributed-research-mcp-web.modal.run`
**Tools**: 8

### 8.1 `deep_search`
```jsonc
{
  "objective": string,       // REQUIRED — research question
  "depth": enum,             // optional — "quick"|"standard"|"thorough" (default "standard")
  "max_sources": integer     // optional — default 10
}
```

### 8.2 `run_task`
```jsonc
{
  "input": string,           // REQUIRED — task prompt
  "processor": enum,         // optional — "lite"|"turbo"|"premium" (default "lite")
  "output_schema": object,   // optional — JSON schema for structured output
  "timeout": integer         // optional — seconds, default 300
}
```

### 8.3 `create_task_group`
```jsonc
{
  "name": string,            // optional
  "tasks": [{                // REQUIRED
    "input": string,
    "processor": string
  }],
  "strategy": enum           // optional — "parallel"|"sequential"|"adaptive" (default "parallel")
}
```

### 8.4 `extract_data`
```jsonc
{
  "url": string,             // REQUIRED
  "schema": object,          // optional — JSON schema
  "instructions": string     // optional
}
```

### 8.5 `find_all`
```jsonc
{
  "objective": string,       // REQUIRED — "AI startups in San Francisco"
  "schema": object,          // optional — entity schema
  "max_results": integer     // optional — default 50
}
```

### 8.6 `synthesize`
```jsonc
{
  "sources": [string],       // REQUIRED — texts or URLs
  "objective": string,       // optional — focus area
  "format": enum             // optional — "summary"|"bullets"|"report"|"analysis" (default "summary")
}
```

### 8.7 `get_task`
```jsonc
{ "task_id": string }        // REQUIRED
```

### 8.8 `list_tasks`
```jsonc
{
  "status": enum,            // optional — "pending"|"running"|"completed"|"failed"|"all" (default "all")
  "limit": integer           // optional — default 20
}
```

---

## 9. Distributed Publish MCP

**Backend**: `distributed-publish-mcp`
**URL**: `https://arthurcolle--distributed-publish-mcp-web.modal.run`
**Tools**: 21

### Event System

#### 9.1 `publish_event`
```jsonc
{
  "type": string,            // REQUIRED — e.g. "content.published"
  "data": object,            // REQUIRED — payload
  "source": string           // optional — source identifier
}
```

#### 9.2 `list_events`
```jsonc
{ "type": string, "limit": integer }  // defaults: —, 50
```

### Webhook Management

#### 9.3 `create_webhook`
```jsonc
{
  "name": string,            // REQUIRED
  "url": string,             // REQUIRED — target URL
  "events": [string],        // optional — default ["*"]
  "secret": string           // optional — payload signing secret
}
```

#### 9.4 `list_webhooks`
```jsonc
{ "status": enum }           // optional — "active"|"paused"|"failed"|"all" (default "all")
```

#### 9.5 `delete_webhook`
```jsonc
{ "webhook_id": string }     // REQUIRED
```

#### 9.6 `trigger_webhook`
```jsonc
{
  "webhook_id": string,      // REQUIRED
  "payload": object           // REQUIRED
}
```

### Message Queue

#### 9.7 `queue_message`
```jsonc
{
  "channel": string,         // REQUIRED
  "payload": object           // REQUIRED
}
```

#### 9.8 `get_queue_messages`
```jsonc
{
  "channel": string,         // REQUIRED
  "limit": integer,          // optional — default 10
  "acknowledge": boolean     // optional — mark as delivered (default false)
}
```

### Notifications

#### 9.9 `send_notification`
```jsonc
{
  "title": string,           // REQUIRED
  "body": string,            // REQUIRED
  "targets": [string],       // optional — channels or webhook IDs
  "priority": enum           // optional — "low"|"normal"|"high"|"urgent" (default "normal")
}
```

### Content Syndication

#### 9.10 `syndicate_content`
```jsonc
{
  "content": {               // REQUIRED
    "title": string,
    "body": string,
    "url": string,
    "metadata": object
  },
  "channels": [string]       // REQUIRED — target channels
}
```

### Substack Integration (11 tools)

#### 9.11 `substack_create_draft`
```jsonc
{
  "title": string,           // REQUIRED
  "subtitle": string,        // optional
  "body": string,            // optional — markdown
  "audience": enum           // optional — "everyone"|"only_paid"|"founding" (default "everyone")
}
```

#### 9.12 `substack_update_draft`
```jsonc
{
  "draft_id": integer,       // REQUIRED
  "title": string,           // optional
  "subtitle": string,        // optional
  "body": string             // optional — replaces existing
}
```

#### 9.13 `substack_append_to_draft`
```jsonc
{
  "draft_id": integer,       // REQUIRED
  "content": string,         // REQUIRED — markdown to append
  "section_title": string,   // optional
  "add_timestamp": boolean   // optional — default true
}
```

#### 9.14 `substack_add_image`
```jsonc
{
  "draft_id": integer,       // REQUIRED
  "url": string,             // REQUIRED — image URL
  "caption": string,         // optional
  "alt": string              // optional — accessibility text
}
```

#### 9.15 `substack_add_code_block`
```jsonc
{
  "draft_id": integer,       // REQUIRED
  "code": string,            // REQUIRED
  "language": string,        // optional
  "filename": string,        // optional
  "caption": string          // optional
}
```

#### 9.16 `substack_publish`
```jsonc
{
  "draft_id": integer,       // REQUIRED
  "send_email": boolean      // optional — default false
}
```

#### 9.17 `substack_get_drafts`
No parameters.

#### 9.18 `substack_get_posts`
```jsonc
{ "limit": integer }         // optional — default 10
```

#### 9.19 `substack_post_note`
```jsonc
{
  "text": string,            // REQUIRED
  "link_url": string         // optional
}
```

#### 9.20 `substack_live_blog_start`
```jsonc
{
  "title": string,           // REQUIRED
  "subtitle": string         // optional
}
```

#### 9.21 `substack_live_blog_end`
```jsonc
{ "publish": boolean }       // optional — default false
```

---

## 10. Alpha Vantage Financial MCP

**Protocol**: TOOL_LIST → TOOL_GET → TOOL_CALL
**Total tools**: 107 (equities, options, fundamentals, forex, crypto, commodities, economic indicators, technical indicators)

### Common Parameters (present on most tools)

| Parameter | Type | Description |
|-----------|------|-------------|
| `entitlement` | string | `"delayed"` (15-min) or `"realtime"` |
| `return_full_data` | boolean | Return full response without truncation |
| `datatype` | string | `"csv"` (default) or `"json"` |

### 10.1 Equity Time Series (7 tools)

#### `TIME_SERIES_INTRADAY`
```jsonc
{
  "symbol": string,          // REQUIRED — e.g. "IBM"
  "interval": string,        // REQUIRED — "1min"|"5min"|"15min"|"30min"|"60min"
  "adjusted": boolean,       // optional — default true
  "extended_hours": boolean, // optional — default true
  "month": string,           // optional — "YYYY-MM" format
  "outputsize": string,      // optional — "compact" (100pts) | "full" (30 days)
  "datatype": string,
  "entitlement": string,
  "return_full_data": boolean
}
```

#### `TIME_SERIES_DAILY` / `TIME_SERIES_DAILY_ADJUSTED`
```jsonc
{ "symbol": string, "outputsize": string, "datatype": string, "entitlement": string, "return_full_data": boolean }
// symbol REQUIRED
```

#### `TIME_SERIES_WEEKLY` / `WEEKLY_ADJUSTED` / `MONTHLY` / `MONTHLY_ADJUSTED`
```jsonc
{ "symbol": string, "datatype": string, "entitlement": string, "return_full_data": boolean }
// symbol REQUIRED
```

### 10.2 Quote & Market Data (4 tools)

#### `GLOBAL_QUOTE`
```jsonc
{ "symbol": string, "datatype": string, "entitlement": string, "return_full_data": boolean }
```

#### `REALTIME_BULK_QUOTES`
```jsonc
{ "symbol": string, "datatype": string, "entitlement": string, "return_full_data": boolean }
// symbol: comma-separated, up to 100
```

#### `SYMBOL_SEARCH`
```jsonc
{ "keywords": string, "datatype": string, "return_full_data": boolean }
```

#### `MARKET_STATUS`
No required parameters.

### 10.3 Options (2 tools)

#### `REALTIME_OPTIONS`
```jsonc
{
  "symbol": string,          // REQUIRED
  "require_greeks": boolean, // optional — default false (enables IV, delta, gamma, theta, vega, rho)
  "contract": string,        // optional — specific contract ID
  "datatype": string,
  "entitlement": string,
  "return_full_data": boolean
}
```

#### `HISTORICAL_OPTIONS`
```jsonc
{
  "symbol": string,          // REQUIRED
  "date": string,            // optional — specific date
  "datatype": string,
  "return_full_data": boolean
}
```

### 10.4 News & Sentiment (1 tool)

#### `NEWS_SENTIMENT`
```jsonc
{
  "tickers": string,         // optional — comma-separated
  "topics": string,          // optional
  "time_from": string,       // optional — YYYYMMDDTHHMM
  "time_to": string,         // optional
  "sort": string,            // optional — "LATEST"|"EARLIEST"|"RELEVANCE"
  "limit": integer,          // optional — default 50
  "return_full_data": boolean
}
```

### 10.5 Fundamentals (12 tools)

`COMPANY_OVERVIEW`, `ETF_PROFILE`, `INCOME_STATEMENT`, `BALANCE_SHEET`, `CASH_FLOW`, `EARNINGS`, `EARNINGS_ESTIMATES`, `DIVIDENDS`, `SPLITS`, `INSIDER_TRANSACTIONS`, `INSTITUTIONAL_HOLDINGS`, `EARNINGS_CALL_TRANSCRIPT`

Common pattern:
```jsonc
{ "symbol": string, "return_full_data": boolean }
// symbol REQUIRED
```

`EARNINGS_CALL_TRANSCRIPT` additionally requires `quarter` and `year`.

### 10.6 Analytics (2 tools)

#### `ANALYTICS_FIXED_WINDOW`
```jsonc
{
  "symbols": string,         // REQUIRED — comma-separated (max 5 free, 50 premium)
  "range_param": string,     // REQUIRED — date range
  "interval": string,        // REQUIRED — "1min"|...|"DAILY"|"WEEKLY"|"MONTHLY"
  "calculations": string,    // REQUIRED — comma-separated metrics
  "ohlc": string,            // optional — "open"|"high"|"low"|"close" (default "close")
  "return_full_data": boolean
}
```

#### `ANALYTICS_SLIDING_WINDOW`
Same structure with additional `window_size` parameter.

### 10.7 Commodities (12 tools)

`WTI`, `BRENT`, `NATURAL_GAS`, `COPPER`, `ALUMINUM`, `WHEAT`, `CORN`, `COTTON`, `SUGAR`, `COFFEE`, `ALL_COMMODITIES`, `GOLD_SILVER_SPOT`, `GOLD_SILVER_HISTORY`

Common pattern:
```jsonc
{ "interval": string, "datatype": string, "return_full_data": boolean }
// interval: "daily"|"weekly"|"monthly"|"quarterly"|"annual" (varies by commodity)
```

### 10.8 Forex (4 tools)

#### `FX_INTRADAY`
```jsonc
{
  "from_symbol": string,     // REQUIRED — e.g. "EUR"
  "to_symbol": string,       // REQUIRED — e.g. "USD"
  "interval": string,        // REQUIRED — "1min"|"5min"|"15min"|"30min"|"60min"
  "outputsize": string,
  "datatype": string,
  "return_full_data": boolean
}
```

#### `FX_DAILY` / `FX_WEEKLY` / `FX_MONTHLY`
```jsonc
{ "from_symbol": string, "to_symbol": string, "outputsize": string, "datatype": string, "return_full_data": boolean }
```

#### `CURRENCY_EXCHANGE_RATE`
```jsonc
{ "from_currency": string, "to_currency": string, "return_full_data": boolean }
```

### 10.9 Crypto (4 tools)

#### `CRYPTO_INTRADAY`
```jsonc
{
  "symbol": string,          // REQUIRED — e.g. "BTC"
  "market": string,          // REQUIRED — e.g. "USD"
  "interval": string,        // REQUIRED
  "outputsize": string,
  "datatype": string,
  "return_full_data": boolean
}
```

#### `DIGITAL_CURRENCY_DAILY` / `WEEKLY` / `MONTHLY`
```jsonc
{ "symbol": string, "market": string, "return_full_data": boolean }
```

### 10.10 Economic Indicators (9 tools)

`REAL_GDP`, `REAL_GDP_PER_CAPITA`, `TREASURY_YIELD`, `FEDERAL_FUNDS_RATE`, `CPI`, `INFLATION`, `RETAIL_SALES`, `DURABLES`, `UNEMPLOYMENT`, `NONFARM_PAYROLL`

Common pattern:
```jsonc
{ "interval": string, "maturity": string, "datatype": string, "return_full_data": boolean }
// interval: "daily"|"weekly"|"monthly"|"quarterly"|"annual" (varies)
// maturity: only for TREASURY_YIELD — "3month"|"2year"|"5year"|"7year"|"10year"|"30year"
```

### 10.11 Technical Indicators (54 tools)

**Moving Averages** (10): `SMA`, `EMA`, `WMA`, `DEMA`, `TEMA`, `TRIMA`, `KAMA`, `MAMA`, `VWAP`, `T3`

Standard moving average schema:
```jsonc
{
  "symbol": string,          // REQUIRED
  "interval": string,        // REQUIRED — "1min"|...|"daily"|"weekly"|"monthly"
  "time_period": integer,    // REQUIRED — e.g. 60
  "series_type": string,     // REQUIRED — "close"|"open"|"high"|"low"
  "month": string,           // optional — "YYYY-MM" for intraday
  "datatype": string,
  "entitlement": string,
  "return_full_data": boolean
}
```

**MACD Family** (2): `MACD`, `MACDEXT`
```jsonc
{
  "symbol": string,          // REQUIRED
  "interval": string,        // REQUIRED
  "series_type": string,     // REQUIRED
  "fastperiod": integer,     // optional — default 12
  "slowperiod": integer,     // optional — default 26
  "signalperiod": integer,   // optional — default 9
  "month": string,
  "datatype": string,
  "entitlement": string,
  "return_full_data": boolean
}
```

**Oscillators** (20+): `STOCH`, `STOCHF`, `RSI`, `STOCHRSI`, `WILLR`, `ADX`, `ADXR`, `APO`, `PPO`, `MOM`, `BOP`, `CCI`, `CMO`, `ROC`, `ROCR`, `AROON`, `AROONOSC`, `MFI`, `TRIX`, `ULTOSC`, `DX`, `MINUS_DI`, `PLUS_DI`, `MINUS_DM`, `PLUS_DM`

**Volatility/Bands** (6): `BBANDS`, `MIDPOINT`, `MIDPRICE`, `SAR`, `TRANGE`, `ATR`, `NATR`

**Volume** (3): `AD`, `ADOSC`, `OBV`

**Hilbert Transform** (6): `HT_TRENDLINE`, `HT_SINE`, `HT_TRENDMODE`, `HT_DCPERIOD`, `HT_DCPHASE`, `HT_PHASOR`

### 10.12 Utility Tools (4)

#### `SEARCH`
```jsonc
{
  "query": string,           // REQUIRED — natural language
  "return_full_data": boolean
}
```

#### `FETCH`
```jsonc
{
  "id": string,              // REQUIRED — API function name from SEARCH results
  "return_full_data": boolean
}
```

#### `PING`
No parameters. Health check.

#### `ADD_TWO_NUMBERS`
```jsonc
{ "a": number, "b": number }  // REQUIRED
```

### 10.13 Calendars & Listings (3 tools)

#### `LISTING_STATUS`
```jsonc
{ "date": string, "state": string, "return_full_data": boolean }
// state: "active"|"delisted"
```

#### `EARNINGS_CALENDAR`
```jsonc
{ "symbol": string, "horizon": string, "return_full_data": boolean }
// horizon: "3month"|"6month"|"12month"
```

#### `IPO_CALENDAR`
No required parameters.

---

## 11. Execution Policy Engine

Source: `~/dsco/dsco-autobot/ToolManagement/core/policy_engine.py`

### Decision Algorithm

```
1. FIXED MODE CHECK
   If caller specifies exact mode → use it (confidence=1.0)

2. LATENCY CLASSIFICATION
   p95 ≤ 0       → "unknown"
   p95 < 2000ms  → "fast"
   p95 ≤ 30000ms → "medium"
   p95 > 30000ms → "slow"

3. BASE SCORING
   ┌──────────┬───────┬────────┬──────┐
   │ Class    │ sync  │ stream │ job  │
   ├──────────┼───────┼────────┼──────┤
   │ fast     │ +2.0  │ +0.8   │ -0.6 │
   │ medium   │ +0.6  │ +2.0   │ +0.5 │
   │ slow     │ -1.8  │ +1.0   │ +2.2 │
   │ unknown  │ +0.8  │ +1.2   │  0   │
   └──────────┴───────┴────────┴──────┘

4. CONSTRAINT ADJUSTMENTS
   • Latency budget met?     → sync +1.2
   • Latency budget breached → sync -1.4, stream +0.7, job +0.9
   • Reliability below target → sync -1.0, stream +0.5, job +0.8
   • Tight cost budget (≤$0.50) → sync +0.4, job -0.5
   • prefer_streaming → stream +1.0
   • Disallowed modes → score = -∞

5. SELECTION
   selected = argmax(scores)
   separation = top_score - second_score
   history_weight = min(1.0, call_count / 20)
   confidence = min(1.0, 0.4 + separation/3.0 + 0.4 * history_weight)
```

---

## 12. Database Schema

### SQLite: `data/executions.db` (14 tables)

| Table | Primary Key | Purpose |
|-------|-------------|---------|
| `executions` | execution_id (UUID) | Core execution tracking |
| `execution_events` | id (AUTO) | Event stream per execution |
| `workflow_executions` | execution_id (UUID) | Workflow invocations |
| `workflow_definitions` | workflow_id | Stored workflow templates |
| `workflow_steps` | id (AUTO), UNIQUE(exec_id, step_key) | Step-level tracking |
| `workflow_events` | id (AUTO) | Workflow event stream |
| `workflow_artifacts` | id (AUTO), UNIQUE(exec, step, type, hash) | Output artifacts with dedup |
| `workflow_side_effects` | id (AUTO), UNIQUE(exec, step, key) | Write operation journal |
| `execution_policy_decisions` | id (AUTO) | Policy engine audit trail |
| `workflow_gate_decisions` | id (AUTO) | Gate approval records |
| `recommendation_feedback` | id (AUTO) | Recommendation tracking |
| `follow_up_actions` | follow_up_id (TEXT) | Autonomous follow-up tasks |
| `execution_group_states` | convergence_key (TEXT) | Semantic coalescing state |
| `execution_group_members` | id (AUTO), UNIQUE(key, exec_id) | Group membership |

### DuckDB: `data/tools.duckdb` (4 tables)

| Table | Primary Key | Purpose |
|-------|-------------|---------|
| `tools` | tool_id (VARCHAR) | Tool catalog with schemas, metrics |
| `backends` | backend_id (VARCHAR) | Backend registration & health |
| `idempotency_records` | (key, tool_id) | Deduplication cache with TTL |
| `tool_call_history` | id (BIGINT) | Per-call latency/error telemetry |

---

## 13. Infrastructure

### Circuit Breaker
```
State machine: CLOSED ──[5 failures/60s]──→ OPEN ──[30s cooldown]──→ HALF_OPEN
                  ↑                                                      │
                  └────────────[success]───────────────────────────────────┘
                                          [failure] → OPEN
```

Configurable via env:
- `BACKEND_CIRCUIT_FAILURE_THRESHOLD` (default 5)
- `BACKEND_CIRCUIT_WINDOW_SECONDS` (default 60)
- `BACKEND_CIRCUIT_COOLDOWN_SECONDS` (default 30)

### Rate Limiter (Multi-level)

| Level | Mechanism | Default |
|-------|-----------|---------|
| Per-second | Token bucket | 10 req/s |
| Per-minute | Sliding window | 100 req/min |
| Per-hour | Counter | 1000 req/hr |
| Per-day | Counter | 10000 req/day |
| Concurrent | Semaphore | 10 |
| Per-tool | Per-minute limit | Configurable |

### Cache (LRU)

| Setting | Default |
|---------|---------|
| Max entries | 10,000 |
| Default TTL | 3,600s (1hr) |
| Cleanup interval | 60s |
| Key derivation | SHA256(tool_name + normalized_inputs) |
| Policies | ALWAYS, NEVER, IDEMPOTENT, TTL, STALE_WHILE_REVALIDATE |

### Backpressure
- `MAX_INFLIGHT_REQUESTS`: 500 (returns 429 if exceeded)
- Health checks exempt

### Streaming Fanout Limits

| Limit | Default |
|-------|---------|
| Max pending | 4,096 |
| Max inflight | 8,192 |
| Per-trace active | 256 |
| Per-session active | 512 |
| Per-parent active | 256 |

---

## 14. Workflow Composition Engine

### Supported Workflow Types

| Type | Description |
|------|-------------|
| `sequential` | Steps execute in order, output→input chaining |
| `parallel` | All steps execute simultaneously |
| `map` | Apply one tool to each element of input array |
| `reduce` | Aggregate results from multiple inputs |
| `map_reduce` | Map then reduce |
| `segment_process` | Split input, process segments, merge |
| `pipe` | Unix-style pipe: tool1 | tool2 | tool3 |

### Step Execution Modes

| Mode | Description |
|------|-------------|
| `passthrough` | Direct output→input (no transformation) |
| `transform` | LLM-mediated text→structured output |
| `map` | Apply to each element |
| `reduce` | Aggregate results |
| `filter` | Filter elements by condition |
| `segment` | Split into parts |
| `classify` | Classify each element |
| `embed` | Generate embeddings |
| `compare` | Compare pairs |
| `merge` | Merge multiple inputs |

### Gate Types

| Gate | Description | Default Action |
|------|-------------|----------------|
| `auto` | Automatic pass-through | proceed |
| `agent_approval` | Agent must approve | proceed |
| `quality_check` | Quality validation | proceed |
| `branch` | Conditional branching | proceed |

### Gate Actions
`proceed` | `skip` | `abort` | `fork` | `rewind`

### Side Effect Tracking
Tools in `WORKFLOW_SIDE_EFFECT_TOOLS` (default: `store_belief,publish_event,substack_create_draft`) get idempotency-keyed journaling with replay support.

### Cleanup TTLs

| Resource | Default TTL |
|----------|-------------|
| Job executions | 1 hour |
| Workflow executions | 24 hours |
| Artifacts | = workflow TTL |
| Side effects | = workflow TTL |
| Definitions | Never (0) |

---

## 15. Recommendation Engine

### Intent→Tool Defaults

| Intent | Default Tools | Confidence Floor |
|--------|---------------|------------------|
| RESEARCH | search, read, verify | 0.85, 0.80, 0.75 |
| REMEMBER | record_episode, store_belief | 0.85, 0.80 |
| NOTIFY | publish_event | 0.85 |
| ANALYZE | search, deep_search | 0.85, 0.80 |
| PUBLISH | publish_event | 0.85 |

### Scoring Pipeline
1. Embedding-based semantic search (Jina v3) → top 10 by cosine similarity
2. Intent default boosting (confidence floor applied)
3. Deduplication + sort by confidence descending
4. Input template generation from tool schemas
5. Return top N (default 5) with confidence, reasoning, input templates

---

## 16. Follow-Up Action System

### Action Types
`cost_analysis` | `latency_optimization` | `retry_recommendation` | `alternative_tool` | `artifact_processing` | `alert_escalation` | `feedback_collection`

### Priority Levels
`low` | `normal` (default) | `high`

### Status Workflow
```
open → in_progress → completed
  ↘        ↘
   dismissed  failed
```

### Deduplication
Same `dedupe_key` + `status=open` → considered duplicate, not re-created.

### Group Reducers (for execution coalescing)
`append_unique` | `append` | `latest` | `top_k` | `windowed` | `score_weighted`

---

## 17. Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HOST` | `0.0.0.0` | Server bind address |
| `PORT` | `2016` | Server port |
| `DATA_DIR` | `data` | Persistence directory |
| `AUTO_START_QUEUE` | `1` | Start task queue on boot |
| `ENABLE_DURABLE_EXECUTIONS` | `1` | Enable SQLite persistence |
| `ENABLE_BACKEND_WARM_POOL` | `1` | Keep backends warm |
| `BACKEND_WARM_POOL_INTERVAL_SECONDS` | `240` | Warm pool ping interval |
| `MAX_INFLIGHT_REQUESTS` | `500` | Backpressure threshold |
| `STREAMING_MAX_PENDING` | `4096` | Max queued executions |
| `STREAMING_MAX_INFLIGHT` | `8192` | Max concurrent executions |
| `CLEANUP_INTERVAL_SECONDS` | `300` | Cleanup cycle interval |
| `JOB_EXECUTION_TTL_SECONDS` | `3600` | Job result retention |
| `WORKFLOW_EXECUTION_TTL_SECONDS` | `86400` | Workflow retention |
| `REQUIRE_BEARER_AUTH` | `0` | Enforce auth |
| `CORS_ORIGINS` | `*` | Allowed origins |
| `WORKFLOW_SIDE_EFFECT_TOOLS` | `store_belief,...` | Tools with side-effect journaling |

### Backend URLs (env vars)
`PARALLEL_AI_URL`, `JINA_MCP_URL`, `SEARCH_MCP_URL`, `RESEARCH_MCP_URL`, `PUBLISH_MCP_URL`, `MEMORY_MCP_URL`

### Core Enums Reference

| Enum | Values |
|------|--------|
| ExecutionMode | AUTO, SYNC, STREAM, JOB, ASYNC, BATCH |
| ExecutionPriority | CRITICAL, HIGH, NORMAL, LOW, BACKGROUND |
| ToolStatus | ACTIVE, DEPRECATED, DISABLED, DRAFT |
| ToolVisibility | PUBLIC, PRIVATE, ORGANIZATION |
| BudgetType | TOKENS, TIME, COST, CALLS, DEPTH, MEMORY, BANDWIDTH |
| CachePolicy | ALWAYS, NEVER, IDEMPOTENT, TTL, STALE_WHILE_REVALIDATE |
| RoutingStrategy | ROUND_ROBIN, LEAST_CONNECTIONS, WEIGHTED, LATENCY, COST, ADAPTIVE |
| BackendStatus | HEALTHY, DEGRADED, UNHEALTHY, UNKNOWN |
| EventType | STARTED, PROGRESS, PARTIAL, SIGNAL, LOG, PAUSED, RESUMED, DETACHED, COMPLETED, ERROR, CANCELLED, HEARTBEAT |
| SchemaType | PYDANTIC, JSON_SCHEMA, OPENAPI |

---

## Tool Count Summary

| Source | Category | Count |
|--------|----------|-------|
| Claude Code Native | File I/O, Search, Execution, UX | 17 |
| Claude Code | Task Management | 4 |
| Claude Code | MCP Resources | 2 |
| Tool Management API | Orchestration, Analytics, Workflows | 26 |
| distributed-memory-mcp | Multi-tier Memory System | 16 |
| distributed-search-mcp | Search, Embed, Rerank, Verify | 5 |
| distributed-research-mcp | Deep Search, Tasks, Synthesis | 8 |
| distributed-publish-mcp | Events, Webhooks, Queues, Substack | 21 |
| Alpha Vantage | Financial Data & Technical Analysis | 107 |
| **Total** | | **~206** |
