# Built-in Tool Catalog

This catalog is generated from `src/tools.c` tool registrations (`.name` / `.description` pairs).

- Source: `src/tools.c`
- Total built-in tools: 126

Regeneration:

```bash
./scripts/gen_tool_catalog.sh
```

| Tool | Description |
|---|---|
| <code>agent</code> | Agent management: spawn, status, output, wait, race, kill. |
| <code>Agent</code> | Claude-compatible sub-agent task alias. |
| <code>agent_wait</code> | Wait for agent(s) to complete. |
| <code>alpha_vantage</code> | Alpha Vantage financial data API. Supports 100+ functions: time series  |
| <code>append_file</code> | Append content, fsync it, and verify appended bytes on disk. |
| <code>avian</code> | Bird-inspired Wings mechanisms: nesting workspaces, brooding incubation, fledging promotion, roosting cooldown, molting refresh. |
| <code>base64</code> | Base64 encode/decode (legacy). |
| <code>base64_tool</code> | Base64 encode/decode. |
| <code>bash</code> | Run a shell command. Use write_file/append_file for durable artifacts;  |
| <code>Bash</code> | Claude-compatible shell runner. Use Write/write_file for durable artifacts;  |
| <code>bg_learn</code> | Control the realtime background learner that consolidates  |
| <code>browser</code> | Browser operations: snapshot, extract, viewport, outline. |
| <code>calc</code> | Evaluate math expressions. |
| <code>code_search</code> | Search codebase by symbol or pattern. |
| <code>compile</code> | Compile source code. |
| <code>context_compact</code> | Compress old conversation history to reclaim tokens. |
| <code>context_recall</code> | Retrieve persisted tool results. No args = list available keys. |
| <code>context_status</code> | Context window self-awareness: tokens, schema overhead, recommendations. |
| <code>contract_landscape</code> | Contract database summary: total/open/settled counts, breakdown by underlying  |
| <code>contract_new_issues</code> | Detect NEW contracts not yet in contracts.db. Fetches current open events,  |
| <code>control_flow</code> | Conditional branching (Priority 6): if/while/for/switch/try on steps.  |
| <code>copy_file</code> | Copy a file or directory. |
| <code>cron_parse</code> | Parse a cron expression. |
| <code>curl_raw</code> | Raw curl command execution. |
| <code>cwd</code> | Get current working directory. |
| <code>date</code> | Get current date/time or parse dates. |
| <code>delete_file</code> | Delete a file or empty directory. |
| <code>diff</code> | Compare two files or strings. |
| <code>discover_integrations</code> | Discover cached, installed, connected, live, inaccessible, stale, OAuth-gated, mutating, and sync-capable external integrations from the Codex app directory plus live MCP tools. |
| <code>discover_tools</code> | List available tools by category or search. |
| <code>disk_usage</code> | Disk usage for a path. |
| <code>docker</code> | Docker operations. |
| <code>download_file</code> | Download a file from URL. |
| <code>dsco_doctor_integrations</code> | Diagnose integration catalog/cache health: stale connector IDs, missing auth/install state, dangerous mutating connectors, and control-plane governance tools. |
| <code>Edit</code> | Claude-compatible alias for edit_file. |
| <code>edit_file</code> | Edit file by replacing old_string with new_string. |
| <code>EnterPlanMode</code> | Enter Claude-compatible advisory plan mode. |
| <code>env_get</code> | Get environment variable. |
| <code>eval</code> | Evaluate a math expression. |
| <code>ExitPlanMode</code> | Exit Claude-compatible advisory plan mode. |
| <code>file_hash</code> | Compute SHA-256 hash of a file on disk. Returns hash, path, and file size. |
| <code>file_info</code> | Get file metadata (size, permissions, timestamps). |
| <code>find_files</code> | Find files by name pattern (glob). |
| <code>github_search</code> | Search GitHub repos, code, issues. |
| <code>Glob</code> | Claude-compatible file glob search. |
| <code>Grep</code> | Claude-compatible content search with glob/output_mode/head_limit support. |
| <code>grep_files</code> | Search file contents with regex. |
| <code>head_tail</code> | Read first or last N lines of a file. action=head (default) or tail. |
| <code>hmac</code> | Compute HMAC-SHA256. |
| <code>hostname</code> | DNS lookup: resolve hostname to IPs (action=resolve) or reverse DNS from IP  |
| <code>http_request</code> | Make HTTP requests (GET/POST/PUT/DELETE). |
| <code>ipc</code> | Inter-process communication: send, recv, agents, scratch_put, scratch_get,  |
| <code>jina_search</code> | AI-powered web search via Jina AI. Returns structured results with titles,  |
| <code>jq</code> | Process JSON with jq expressions. |
| <code>json_format</code> | Pretty-print or minify JSON. Pass raw JSON string, get formatted output. |
| <code>kalshi</code> | Kalshi prediction market. Actions: markets, events, search, orderbook,  |
| <code>killswitch</code> | Kill switch control: trigger, resolve, status. |
| <code>knowledge_base</code> | KB operations: ingest, search, deep_search, list, get, delete, arxiv_search,  |
| <code>learned_cost</code> | Learned k-NN cost model (Priority 3): predict/record/stats. action=predict  |
| <code>legion</code> | Legion agent system: spawn, status, find. |
| <code>list_directory</code> | List directory contents with file info. |
| <code>load_tools</code> | Dynamically load tools into the active register file. Provide at least one  |
| <code>md5</code> | Compute MD5 hash of text. |
| <code>memory_tier</code> | Three-tier memory: store, recall, promote, forget, status. |
| <code>mkdir</code> | Create directory (with parents). |
| <code>move_file</code> | Move or rename a file/directory. |
| <code>network</code> | Network diagnostics: dns, ping, port_check, port_scan, netstat, cert,  |
| <code>nws</code> | NWS API: forecast (lat/lon), hourly, station_obs (METAR station), alerts (by  |
| <code>ol_call</code> | Call an already-running local OpenAI-compatible model server  |
| <code>page_file</code> | Page through a large file. |
| <code>parallel_search</code> | Fan out web search to multiple providers (Jina, Tavily, Brave) concurrently.  |
| <code>pets</code> | Companion sprites for background agents. action=roster shows live  |
| <code>pheromone</code> | Pheromone coordination (Wings): deposit, sense, status. |
| <code>pipeline</code> | Pipeline execution and chaining. |
| <code>plan_state</code> | Stateful atom execution (Priority 2): init/run/rollback/get/set. action=init  |
| <code>playbook</code> | ACE playbook: read, add, tag, remove, search, gc, inject. |
| <code>playbook_add</code> | Add entry to ACE playbook. |
| <code>playbook_search</code> | Search ACE playbook. |
| <code>plot</code> | Render data as a Unicode chart (returns ANSI/Unicode art). Types: line, bar,  |
| <code>polymarket</code> | Polymarket prediction market. Actions: markets, events, categories, prices,  |
| <code>port_check</code> | Check if a TCP port is open on a host. 3-second timeout. |
| <code>prediction</code> | Cross-platform prediction market ops: scan, weather, snapshot, arb,  |
| <code>ps</code> | List running processes. |
| <code>python</code> | Run Python code. |
| <code>random_bytes</code> | Generate random bytes (hex). |
| <code>Read</code> | Claude-compatible alias for read_file. |
| <code>read_file</code> | Read file with line numbers. Use offset/limit for large files. |
| <code>recovery</code> | Failure recovery (Priority 7): retry/fallback/backtrack.  |
| <code>research_probe</code> | Deep research probe on a topic. |
| <code>run_command</code> | Run a shell command with optional artifact verification. |
| <code>sandbox_run</code> | Run command in sandboxed container. |
| <code>scratchpad</code> | Read/write scratchpad for temporary data. |
| <code>self_assess</code> | Quick self-evaluation of current session performance. Returns efficiency  |
| <code>self_exit</code> | Gracefully exit the agent loop. |
| <code>self_exiting</code> | Legacy alias for self_exit. |
| <code>self_improve</code> | Run the self-improvement loop and RSI safety curriculum: summary,  |
| <code>semver_compare</code> | Compare two semantic versions. |
| <code>session_memory</code> | Persistent session KV memory (Priority 5): remember/recall/status.  |
| <code>sha256</code> | Compute SHA-256 hash of text. |
| <code>slack_post</code> | Post message to Slack. |
| <code>sqlite</code> | Execute SQLite queries. |
| <code>ssh_command</code> | Run command on remote host via SSH. |
| <code>StartOfLoopConstruct</code> | Start a live recursive agent loop construct. Accepts a bounded  |
| <code>string_transform</code> | String transformations: upper, lower, trim, reverse, slugify, capitalize. |
| <code>swarm</code> | Swarm orchestration: create, map_reduce, status, collect, budget,  |
| <code>synoptic</code> | Synoptic Data real-time weather station observations (ASOS/METAR). Actions:  |
| <code>sysinfo</code> | System info: CPU, memory, OS. |
| <code>systematic</code> | Systematic trading: ingest_polymarket, ingest_kalshi, analytics, signals. |
| <code>Task</code> | Claude-compatible task agent alias. |
| <code>TaskList</code> | Return the Claude-compatible todo list state. |
| <code>tavily_search</code> | Web search via Tavily. |
| <code>timestamp</code> | Current time: epoch, iso (ISO 8601), local, date, or relative (time until a  |
| <code>TodoWrite</code> | Claude-compatible todo list state writer. |
| <code>token_audit</code> | Audit token usage across conversation. |
| <code>trading</code> | Trading ops: arb_execute, arb_monitor, portfolio, risk_check, risk_configure. |
| <code>url_parse</code> | Parse a URL into components. |
| <code>uuid</code> | Generate a UUID v4. |
| <code>vos_status</code> | Virtual OS subsystem status. |
| <code>weather</code> | Get weather data for a location. |
| <code>WebFetch</code> | Claude-compatible URL fetch/extract. |
| <code>WebSearch</code> | Claude-compatible web search alias. |
| <code>wings_talons_status</code> | Unified Wings+Talons+Immune system status. |
| <code>word_count</code> | Count words, lines, and characters in a file or text string. |
| <code>workflow</code> | Workflow management: plan, status, checkpoint, resume, heartbeat,  |
| <code>Write</code> | Claude-compatible alias for verified atomic write_file. |
| <code>write_file</code> | Create/overwrite a file atomically, fsync it, and verify bytes on disk.  |
