# dsco Quick Reference Guide

## Build & Run

```bash
# Bootstrap dependencies
./scripts/bootstrap.sh

# Build release binary
make -j8

# Build with debug symbols + ASan
make debug

# Run interactive REPL
./dsco

# Run one-shot with API key
export ANTHROPIC_API_KEY=sk-...
./dsco "What files are in /tmp?"

# List available topologies
./dsco --list-topologies

# Run specific topology
./dsco --topology=bash --auto-topology "list files in /tmp"
```

## Core Modules @ A Glance

| Module | Lines | Purpose | Key Exports |
|--------|-------|---------|-------------|
| **main.c** | 578 | CLI entry, arg parsing, REPL | `main()` |
| **llm.c** | 2275 | Claude API streaming | `llm_stream_request()` |
| **tools.c** | 1200+ | 188 tool implementations | `tool_execute()` |
| **swarm.c** | ~400 | Multi-agent orchestration | `swarm_spawn()` |
| **ipc.c** | 782 | Inter-process communication | `ipc_send()`, `ipc_recv()` |
| **crypto.c** | 512 | Pure C crypto (SHA256, HMAC) | `sha256()`, `hmac_sha256()` |
| **baseline.c** | 1053 | Structured logging | `baseline_log()` |
| **eval.c** | ~400 | Math expression evaluator | `eval_expr()` |
| **semantic.c** | 533 | Context search & routing | `semantic_route()` |
| **plugin.c** | 592 | Dynamic plugin system | `plugin_load()` |

## File Organization

```
dsco-cli/
├── src/              # Source files (.c)
│   ├── main.c        # Entry point
│   ├── llm.c         # LLM integration
│   ├── tools.c       # Tool implementations
│   ├── swarm.c       # Agent orchestration
│   ├── ipc.c         # IPC
│   ├── crypto.c      # Crypto primitives
│   └── [22 more].c
│
├── include/          # Headers (.h)
│   ├── agent.h       # Agent data structures
│   ├── llm.h         # LLM interface
│   ├── tools.h       # Tool registry
│   ├── swarm.h       # Swarm types
│   └── [22 more].h
│
├── vendor/           # Vendored libraries
│   ├── cJSON.c/.h    # JSON parsing
│   ├── yyjson.c/.h   # Fast JSON
│   ├── md4c.c/.h     # Markdown → HTML
│   ├── miniz.c       # Compression
│   ├── monocypher/   # Crypto (Blake2b, ChaCha)
│   └── [40+ more]
│
├── test/             # Test files
├── docs/             # Documentation
├── scripts/          # Build/bootstrap scripts
├── Makefile          # Build configuration
└── README.md         # Project overview
```

## Tools (188 total)

### File I/O (11)
```
read_file, write_file, append_file, edit_file, delete_file,
move_file, copy_file, mkdir, chmod, list_directory, find_files
```

### Git (13)
```
git_status, git_diff, git_log, git_add, git_commit,
git_branch, git_stash, git_clone, git_push, git_pull
```

### Network (12)
```
http_request, json_api, dns_lookup, ping, port_check,
port_scan, netstat, cert_info, traceroute, whois_lookup,
http_headers, websocket_test
```

### Shell & Execution (5)
```
bash, run_command, run_background, ssh_command, compile
```

### Swarm & Agent Control (10)
```
spawn_agent, agent_status, agent_output, agent_wait, agent_kill,
create_swarm, swarm_status, swarm_collect,
ipc_send, ipc_recv, ipc_scratch_put, ipc_scratch_get, ipc_task_submit
```

### Crypto (10)
```
sha256, md5, hmac, uuid, random_bytes, base64_tool,
jwt_decode, hkdf, hash (generic)
```

### Data/Code (8)
```
code_index, code_search, jq, pipeline, eval, calc,
sqlite, psql
```

### Web & Market Data (40+)
```
web_extract, browser_snapshot, browser_extract, browser_viewport,
firecrawl, jina_read, web_search, serpapi,
av_quote, av_daily, av_intraday, av_balance, av_income,
av_cashflow, av_earnings, av_news, [+ 20 more]
```

### Integrations (40+)
```
slack_post, discord_post, twilio_sms,
notion_search, notion_page,
github_search, github_issue, github_pr, github_repo,
stripe, supabase_query, pinecone_query, huggingface
```

### Media (5)
```
screenshot, view_image, view_pdf, elevenlabs_tts, weather
```

### Utility (15+)
```
date, clipboard, pkg (brew/apt), pip, npm,
crontab, diff, patch, sed, awk, sort_uniq,
wc, head, tail, xattr
```

## Common Usage Patterns

### 1. One-Shot Query
```bash
./dsco "Find the top 10 lines by word count in README.md"
```
Claude will:
1. Call `read_file("README.md")`
2. Process with `pipeline` or `awk`
3. Return results

### 2. Multi-Turn Conversation (REPL)
```bash
./dsco
> What's my current git status?
> Show me the last 5 commits
> Let me stash my changes
> (REPL maintains conversation history, context carries over)
```

### 3. Spawn Sub-Agent
```bash
# In REPL or prompt:
> I need you to analyze the codebase in /Users/me/project while I work on docs.
> Use spawn_agent to parallelize.

# Claude calls:
spawn_agent({
  "task": "Analyze /Users/me/project, output summary"
})

# Returns agent_id=123
# Then monitor:
agent_status --id 123
agent_output --id 123
```

### 4. Create Swarm (Parallel Group)
```bash
# Claude calls:
create_swarm({
  "name": "batch_analysis",
  "tasks": [
    "Analyze file A",
    "Analyze file B",
    "Analyze file C"
  ]
})

# Three agents spawn in parallel
# Then collect:
swarm_collect("batch_analysis")
```

### 5. Code Search & Understand
```bash
./dsco "In the codebase, find all calls to llm_stream_request()"

# Claude will:
code_index(".")           # Index all .c/.h files
code_search("llm_stream_request")  # Find usages
```

## Data Structures

### Agent Registration
```c
typedef struct {
    int id;
    pid_t pid;
    char role[64];      // "researcher", "coder", etc.
    bool alive;
} agent_t;
```

### Tool
```c
typedef struct {
    const char *name;
    bool (*execute)(const char *input, char *result, size_t rlen);
    const char *input_schema_json;
    const char *description;
} tool_t;
```

### IPC Message
```c
typedef struct {
    int from_agent_id;
    char topic[64];
    char body[4096];
    uint64_t timestamp;
} ipc_message_t;
```

### Swarm Group
```c
typedef struct {
    char name[64];
    pid_t children[SWARM_MAX_CHILDREN];
    int child_count;
    swarm_status_t status;
} swarm_group_t;
```

## Environment Variables

```bash
# Required
ANTHROPIC_API_KEY=sk-ant-...        # Claude API key

# Optional
CLAUDE_MODEL=claude-opus-4-6        # Default model (def: opus-4-6)
WORKSPACE_DIR=/path/to/workspace    # Workspace root
DEBUG=1                              # Verbose logging
DSCO_LOG_LEVEL=debug|info|warn|error
```

## Performance Tips

1. **Use batch processing** for multiple API calls
2. **Enable prompt caching** for large context windows
3. **Spawn agents** for parallel tasks (swarms)
4. **Stream responses** — don't buffer entire output
5. **Index code once** with `code_index()`, reuse with `code_search()`
6. **Use pipelines** for data transformation instead of individual tools

## Debugging

```bash
# Build with AddressSanitizer
make debug

# Run with verbose logging
DEBUG=1 ./dsco "your query"

# Check last session trace
cat ~/.dsco/trace.json

# Inspect baseline logs
grep "ERROR" ~/.dsco/baseline.log
```

## Extension Points

### Add a New Tool
1. Implement `bool my_tool(const char *input, char *result, size_t rlen)`
2. Define JSON schema for input
3. Add to `tool_registry[]` in tools.c
4. Recompile: `make clean && make`

### Load a Plugin
1. Create `.dylib` or `.so` with compatible symbols
2. Place in `~/.dsco/plugins/`
3. Create `plugin-manifest.json`
4. `plugin_reload` at runtime

### Custom Topology
1. Add `.md` description + JSON spec to `~/.dsco/topologies/`
2. Reference with `--topology=name`

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "API key not set" | Export `ANTHROPIC_API_KEY=sk-...` |
| Swarm children don't spawn | Check `~/.dsco/` directory exists + writable |
| IPC messages lost | Verify named pipes created; check `netstat` |
| Memory spike on large files | Use `pipeline` for streaming, not `read_file` |
| Plugin won't load | Ensure `.dylib`/`.so` has correct arch (x86_64/arm64) |

---

**Last Updated:** 2026-03-13  
**Version:** dsco-cli (local-first agentic CLI)
