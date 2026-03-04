# Built-in Tool Catalog

This catalog is generated from `tools.c` tool registrations (`.name` / `.description` pairs).

- Source: `tools.c`
- Total built-in tools: 171

Regeneration:

```bash
./scripts/gen_tool_catalog.sh
```

| Tool | Description |
|---|---|
| <code>agent_kill</code> | Kill a running sub-agent by ID. |
| <code>agent_output</code> | Get the accumulated output from a specific sub-agent. Polls for latest data. Returns the agent's streamed stdout including Claude's responses and tool usage. |
| <code>agent_status</code> | Check status of all spawned sub-agents. Shows running/done/error state, elapsed time, output size. Use to monitor progress of spawned agents. |
| <code>agent_wait</code> | Wait for a specific agent or all agents to complete. Streams status updates while waiting. Returns final output when done. |
| <code>append_file</code> | Append content to the end of a file. |
| <code>awk</code> | Run an awk program on a file. |
| <code>base64</code> | Base64 encode or decode data or a file. |
| <code>base64_tool</code> | Base64 encode or decode text. |
| <code>bash</code> | Execute a bash command and return stdout+stderr. Supports multi-line scripts, pipes, redirections. Preferred for all shell operations. |
| <code>big_factorial</code> | Compute exact factorial of large numbers using arbitrary-precision arithmetic.  |
| <code>brave_search</code> | Search the web using Brave Search API. Privacy-focused, returns web results with titles, URLs, and snippets. Requires BRAVE_API_KEY. |
| <code>browser_extract</code> | Query indexed browser snapshots with optional snapshot_id/facet filters (visual facet default). |
| <code>browser_outline</code> | Return the structural DOM outline (headings + key links + layout stats) for a snapshot. |
| <code>browser_snapshot</code> | Fetch and index a browser-grade snapshot with adaptive multi-pass fallbacks: raw HTML + visual text + DOM outline. |
| <code>browser_viewport</code> | Scroll a visual snapshot like a browser viewport (line offset + window size). |
| <code>calc</code> | Evaluate a mathematical expression using bc or python. |
| <code>call_graph</code> | Show what functions a given function calls (from body analysis). Useful for understanding control flow and planning refactors. |
| <code>cert_info</code> | Show TLS certificate information for a host. |
| <code>chmod</code> | Change file permissions. |
| <code>clipboard</code> | Read from or write to the system clipboard. |
| <code>code_index</code> | Index repository files into retrieval context for semantic code search. |
| <code>code_search</code> | Search indexed code chunks using retrieval + reranking. |
| <code>compile</code> | Compile a C source file. Uses cc (system compiler). |
| <code>context_fuse</code> | Fuse multiple retrieval queries via reciprocal-rank fusion (RRF) before packing/get. |
| <code>context_gc</code> | Garbage-collect retrieval context by max_chunks/max_bytes while preserving pinned and recent chunks. |
| <code>context_get</code> | Fetch full text of a previously indexed retrieval chunk by chunk_id. |
| <code>context_pack</code> | Pack retrieval evidence into a strict character/token budget with chunk citations. |
| <code>context_pin</code> | Pin or unpin a context chunk to protect it from context_gc pruning. |
| <code>context_search</code> | Search the chunked retrieval context (dense+lexical reranking) with optional source/facet metadata filters. |
| <code>context_stats</code> | Show retrieval context store stats and chunk counts by source tool. |
| <code>context_summarize</code> | Build a compact evidence summary from retrieval hits for a query (with chunk citations). |
| <code>copy_file</code> | Copy a file or directory (recursive). |
| <code>create_swarm</code> | Create a named group of sub-agents and dispatch multiple tasks to them simultaneously. Each task gets its own agent. Returns group ID for monitoring. Use for parallel work: e.g. create_swarm with tasks=['write backend API', 'write frontend UI', 'write tests']. |
| <code>crontab</code> | List or add crontab entries. |
| <code>curl_raw</code> | Run curl with arbitrary arguments. |
| <code>cwd</code> | Get or change the current working directory. |
| <code>date</code> | Get current date/time, optionally in a specific timezone or format. |
| <code>delete_file</code> | Delete a file or directory. |
| <code>dependency_graph</code> | Show the include dependency graph between source files. Returns which files depend on which headers. |
| <code>diff</code> | Show unified diff between two files. |
| <code>discord_post</code> | Post a message to Discord via webhook URL or bot token. For webhooks, provide webhook_url. For bot mode, provide channel_id and set DISCORD_TOKEN. |
| <code>disk_usage</code> | Show disk usage for a path. |
| <code>dns_lookup</code> | Resolve a hostname to IP addresses. |
| <code>docker</code> | Run a docker command (e.g. ps, images, run, build, exec). |
| <code>docker_compose</code> | Run docker compose commands. |
| <code>download_file</code> | Download a URL to a local file. Follows redirects. |
| <code>edit_file</code> | Edit a file by replacing old_string with new_string. The old_string must uniquely match unless replace_all is true. |
| <code>elevenlabs_tts</code> | Convert text to speech audio using ElevenLabs. Saves MP3 file. Requires ELEVENLABS_API_KEY. |
| <code>env_get</code> | Get environment variable(s). Without name, lists all. |
| <code>env_set</code> | Set an environment variable for this session. |
| <code>eval</code> | Evaluate mathematical expressions. Supports: arithmetic (+,-,*,/,%,^/**),  |
| <code>file_info</code> | Get file metadata (size, type, permissions). |
| <code>find_files</code> | Recursively find files matching a name pattern (glob). |
| <code>firecrawl</code> | Scrape a web page and extract structured content as markdown. Better than raw HTTP for complex pages with JS rendering. Requires FIRECRAWL_API_KEY. |
| <code>fred_series</code> | Get economic data from FRED (Federal Reserve). Series IDs: GDP, UNRATE (unemployment), CPIAUCSL (CPI), DFF (fed funds rate), T10Y2Y (yield curve), VIXCLS (VIX), DGS10 (10yr treasury), MORTGAGE30US. Requires FRED_API_KEY. |
| <code>git_add</code> | Stage files for git commit. |
| <code>git_branch</code> | List, create, or switch git branches. |
| <code>git_clone</code> | Clone a git repository. |
| <code>git_commit</code> | Create a git commit with a message. |
| <code>git_diff</code> | Show git diff. Pass args like '--staged' or 'HEAD~1'. |
| <code>git_log</code> | Show recent git commits (oneline format). |
| <code>git_pull</code> | Pull changes from remote. |
| <code>git_push</code> | Push commits to remote. |
| <code>git_stash</code> | Git stash operations: push, pop, list, drop. |
| <code>git_status</code> | Show git status (short format with branch info). |
| <code>github_actions</code> | View GitHub Actions workflow runs and workflows for a repository. Requires GITHUB_TOKEN. |
| <code>github_create_issue</code> | Create a new GitHub issue. Requires GITHUB_TOKEN with repo write access. |
| <code>github_issue</code> | Get GitHub issues for a repository. Without number, lists open issues. With number, gets specific issue details. Requires GITHUB_TOKEN. |
| <code>github_pr</code> | Get GitHub pull requests for a repository. Without number, lists open PRs. With number, gets specific PR details. Requires GITHUB_TOKEN. |
| <code>github_repo</code> | Get GitHub repository information including stars, forks, language, description. Requires GITHUB_TOKEN. |
| <code>github_search</code> | Search GitHub repositories, code, issues, or users. Requires GITHUB_TOKEN. |
| <code>grep_files</code> | Search for a pattern in file contents recursively. |
| <code>hash</code> | Compute hash (md5, sha1, sha256) of data or file. |
| <code>head</code> | Show the first N lines of a file. |
| <code>hkdf</code> | Derive keys using HKDF-SHA256 (RFC 5869). |
| <code>hmac</code> | Compute HMAC-SHA256 of a message with a given key. |
| <code>http_headers</code> | Fetch only HTTP response headers for a URL. |
| <code>http_request</code> | Make an HTTP request. Supports GET, POST, PUT, DELETE, PATCH with custom headers and body. |
| <code>huggingface</code> | Run inference on a Hugging Face model. Supports text classification, generation, NER, summarization. Requires HF_TOKEN. |
| <code>inspect_file</code> | Deep AST analysis of a single C/H file. Returns every function (with params, return type, complexity, line range), every struct/typedef/enum, includes, defines. Use for targeted code understanding. |
| <code>ipc_agents</code> | List all agents in the hierarchy with their status, role, depth, and liveness. Shows the full agent tree. |
| <code>ipc_recv</code> | Read unread messages from other agents. Returns messages addressed to this agent plus broadcasts. |
| <code>ipc_scratch_get</code> | Read a value from the shared scratchpad by key. Returns the value and which agent wrote it. |
| <code>ipc_scratch_put</code> | Write a key-value pair to the shared scratchpad. All agents in the hierarchy can read/write this shared state. |
| <code>ipc_send</code> | Send a message to another agent (point-to-point or broadcast). For inter-agent coordination, task delegation, and status sharing. |
| <code>ipc_set_role</code> | Set this agent's role/specialization (e.g., 'researcher', 'coder', 'reviewer'). Visible to other agents. |
| <code>ipc_task_list</code> | List tasks in the shared queue with status, priority, and results. Filter by assigned agent optionally. |
| <code>ipc_task_submit</code> | Submit a task to the shared task queue. Any idle agent can claim and execute it. Use for work distribution. |
| <code>jina_read</code> | Extract readable content from any URL using Jina AI Reader. Returns clean markdown text, stripping navigation and ads. Optionally uses JINA_API_KEY for higher limits. |
| <code>jq</code> | Process JSON with jq. Provide input as string or file. |
| <code>json_api</code> | Call a JSON API with Content-Type and Accept set to application/json. |
| <code>jwt_decode</code> | Decode a JWT token (header + payload, no signature verification). |
| <code>kill_process</code> | Send a signal to a process. |
| <code>list_directory</code> | List files and subdirectories in a directory. |
| <code>mapbox_geocode</code> | Geocode an address or place name to coordinates using Mapbox. Requires MAPBOX_API_KEY. |
| <code>market_quote</code> | Fetch market quotes with Yahoo+Stooq fallback, richer analytics, staleness checks, and optional JSON output. |
| <code>md5</code> | Compute MD5 hash of text or file. |
| <code>mkdir</code> | Create a directory (and parents). |
| <code>move_file</code> | Move or rename a file or directory. |
| <code>net_interfaces</code> | Show network interfaces and their IP addresses. |
| <code>netstat</code> | Show active network connections and listening ports. |
| <code>node</code> | Execute JavaScript/Node.js code (inline or from file). |
| <code>notion_page</code> | Read the content blocks of a Notion page by its ID. Requires NOTION_API_KEY. |
| <code>notion_search</code> | Search across all Notion pages and databases accessible to the integration. Requires NOTION_API_KEY. |
| <code>npm</code> | Run npm commands (install, list, init, etc.). |
| <code>page_file</code> | Page through a file. Returns one page at a time with line numbers and page info. |
| <code>patch</code> | Apply a unified diff patch to a file. |
| <code>pinecone_query</code> | Query a Pinecone vector database index. Returns top-k nearest neighbors with metadata. Requires PINECONE_API_KEY. |
| <code>ping</code> | Ping a host to check connectivity and latency. |
| <code>pip</code> | Run pip3 commands (install, list, freeze, etc.). |
| <code>pipeline</code> | Run a streaming data pipeline on text. Chains transform stages using coroutines.  |
| <code>pkg</code> | Run a package manager command (auto-detects brew/apt/yum). |
| <code>plugin_list</code> | List loaded plugins from ~/.dsco/plugins/. Shows plugin names, versions,  |
| <code>plugin_load</code> | Load a specific plugin from a file path. |
| <code>plugin_reload</code> | Hot-reload all plugins from ~/.dsco/plugins/. |
| <code>port_check</code> | Check if a TCP port is open on a host. |
| <code>port_scan</code> | Scan common ports on a host (or specify custom ports). |
| <code>privacy_filter</code> | Redact common PII patterns (email and phone-like tokens) from text. |
| <code>ps</code> | List running processes. Optionally filter by name. |
| <code>psql</code> | Execute a SQL query against PostgreSQL. |
| <code>python</code> | Execute Python code (inline or from file). |
| <code>random_bytes</code> | Generate cryptographically random bytes (hex-encoded). |
| <code>read_file</code> | Read a file with line numbers. Use offset/limit for large files. |
| <code>research_compare</code> | Compare two passages and report lexical overlap metrics. |
| <code>research_probe</code> | Fetch and index a source, then optionally run focused retrieval against that source. |
| <code>risk_gate</code> | Score action/content risk and return allow/review/deny decision. |
| <code>run_background</code> | Run a command in the background. Returns the PID. |
| <code>run_command</code> | Execute a shell command and return stdout+stderr. Use for running compiled programs, scripts, or any system command. |
| <code>sandbox_run</code> | Run a command in a constrained environment (Docker no-network if available, otherwise minimal env shell). |
| <code>scp</code> | Copy files over SSH (local->remote or remote->local). |
| <code>screenshot</code> | Take a screenshot on macOS. Saves as PNG. Use full_screen=false for interactive region selection. |
| <code>secret_scan</code> | Scan text or file content for common secret/key leakage patterns. |
| <code>sed</code> | Run a sed expression on a file. |
| <code>self_inspect</code> | Introspect dsco's own source code. Returns AST-level analysis: files, functions, structs, tools, line counts, complexity scores, and dependency graph. Use this to understand dsco's architecture before modifying it. |
| <code>serpapi</code> | Search Google via SerpAPI. Returns organic results with titles, snippets, links. Requires SERPAPI_API_KEY. |
| <code>sha256</code> | Compute SHA-256 hash of text or file. Pure C implementation — no external dependencies. |
| <code>slack_post</code> | Post a message to a Slack channel. Requires SLACK_BOT_TOKEN with chat:write scope. |
| <code>sort_uniq</code> | Sort a file, optionally unique or with counts. |
| <code>spawn_agent</code> | Spawn a sub-dsco agent to work on a task autonomously. The agent runs as a separate process with its own conversation with Claude. Returns immediately with an agent ID — use agent_status/agent_output to monitor. Great for parallelizing: spawn multiple agents for different subtasks. |
| <code>sqlite</code> | Execute a SQL query against a SQLite database. |
| <code>ssh_command</code> | Execute a command on a remote host via SSH. |
| <code>stripe</code> | Access Stripe payment data — charges, customers, balance, invoices. Requires STRIPE_API_KEY. |
| <code>supabase_query</code> | Query a Supabase table using PostgREST. Requires SUPABASE_API_KEY and SUPABASE_URL. |
| <code>swarm_collect</code> | Wait for all agents in a swarm to complete and collect their outputs. Returns all results aggregated. |
| <code>swarm_status</code> | Check status of a swarm group. Shows each agent's status and output preview. |
| <code>symlink</code> | Create a symbolic link. |
| <code>sysinfo</code> | Show system information (OS, disk, uptime). |
| <code>tail</code> | Show the last N lines of a file. |
| <code>tar</code> | Create, extract, or list tar.gz archives. |
| <code>tavily_search</code> | Search the web using Tavily API with AI-generated answers and source attribution. Returns relevant results with snippets. Requires TAVILY_API_KEY. |
| <code>token_audit</code> | Report token-efficiency metrics from large-result offloading and retrieval context. |
| <code>traceroute</code> | Trace the network route to a host. |
| <code>tree</code> | Show directory tree structure. |
| <code>twilio_sms</code> | Send an SMS via Twilio. Requires TWILIO_AUTH_TOKEN, TWILIO_ACCOUNT_SID, TWILIO_FROM_NUMBER. |
| <code>upload_file</code> | Upload a file to a URL via multipart POST. |
| <code>uuid</code> | Generate a UUID v4 (cryptographically random). |
| <code>view_image</code> | Read and encode an image file for vision analysis. Supports PNG, JPEG, GIF, WebP. The image will be included in the next API call for Claude to analyze. |
| <code>view_pdf</code> | Read and encode a PDF file for analysis. The PDF will be included in the next API call for Claude to read and analyze. |
| <code>wc</code> | Count lines, words, and bytes in a file. |
| <code>weather</code> | Get current weather for any location worldwide. Returns temperature, conditions, humidity, wind. Requires OPENWEATHERMAP_API_KEY. |
| <code>web_extract</code> | Fetch a URL and extract readable text content, stripping HTML tags. Much more token-efficient than raw HTTP. Good for reading articles, docs, search results. |
| <code>websocket_test</code> | Test a WebSocket connection. |
| <code>which</code> | Find a command's location and version. |
| <code>whois_lookup</code> | Look up WHOIS information for a domain. |
| <code>workflow_checkpoint</code> | Update a workflow step status with an optional note. |
| <code>workflow_plan</code> | Create a workflow plan with newline/semicolon-separated steps. |
| <code>workflow_resume</code> | Return the next actionable workflow step. |
| <code>workflow_status</code> | Show workflow status (one workflow by id, or all). |
| <code>write_file</code> | Create or overwrite a file with the given content. Creates parent directories automatically. |
| <code>xattr</code> | List or clear extended attributes on a file (macOS). |
| <code>zip</code> | Create, extract, or list zip archives. |
