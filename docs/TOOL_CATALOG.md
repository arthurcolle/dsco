# Built-in Tool Catalog

This catalog is generated from the static `src/tools.c` built-in tool registry.

- Source: `src/tools.c`
- Total built-in tools: 271
- Core tools: 40
- Read-only tools: 161
- Concurrent tools: 164
- Interactive tools: 1

Regeneration:

```bash
./scripts/gen_tool_catalog.sh
```

Flags:

- Core: always available in the active register set.
- Read-only: marked as side-effect-free for streaming execution.
- Concurrent: marked safe for parallel execution.
- Interactive: owns the terminal or user turn.

| Tool | Core | Read-only | Concurrent | Interactive | Description |
|---|---:|---:|---:|---:|---|
| <code>abi_diff</code> |  | yes |  |  | Diff exported symbols between two binaries (nm) — added/removed symbols. Catches accidental ABI breaks. |
| <code>Agent</code> |  |  |  |  | Claude-compatible sub-agent task alias. |
| <code>agent</code> | yes |  |  |  | Agent management: spawn, status, output, wait, race, kill. |
| <code>agent_wait</code> |  |  |  |  | Wait for agent(s) to complete. |
| <code>agentic_commerce</code> |  | yes | yes |  | Agentic commerce protocol registry: list/status/coverage/plan for ACP, UCP, AP2, x402, Stripe MPP/SPT, Visa, Mastercard, and clearing watchlist protocols. |
| <code>alpha_vantage</code> |  | yes | yes |  | Alpha Vantage financial data API. Supports 100+ functions: time series (TIME_SERIES_DAILY, TIME_SERIES_INTRADAY), technical indicators (SMA, EMA, RSI, MACD, BBANDS, STOCH, ADX, CCI, OBV, ATR, VWAP), fundamentals (OVERVIEW, INCOME_STATEMENT, BALANCE_SHEET, EARNINGS), macro (CPI, REAL_GDP, UNEMPLOYMENT, TREASURY_YIELD), commodities (WTI, BRENT, NATURAL_GAS, GOLD_SILVER_SPOT), forex (CURRENCY_EXCHANGE_RATE, FX_DAILY), crypto (DIGITAL_CURRENCY_DAILY), options (REALTIME_OPTIONS), news (NEWS_SENTIMENT). |
| <code>api_outline</code> |  | yes | yes |  | Public API of a file from the AST: non-static function signatures, structs, typedefs, defines. Faster than reading the whole file. |
| <code>append_file</code> |  |  |  |  | Append content, fsync it, and verify appended bytes on disk. |
| <code>apply_patch</code> |  |  |  |  | Apply a unified diff atomically across one or more files. Each hunk is matched by its context; if any hunk's context has drifted, NOTHING is written and the failing hunk is named. Safer than edit_file for multi-edit changes. Provide a standard `diff -u` patch with --- / +++ / @@ headers. |
| <code>archive</code> |  |  |  |  | Create, list, or extract tar.gz archives with quoted archive/destination paths and a constrained files list for create. |
| <code>AskUserQuestion</code> | yes |  |  | yes | Show an interactive multi-question dialog to collect structured input from the user. Use when a response merits clarification. Supports option lists with descriptions, conditional branching (show_if), computed options (options_cmd), free-text + 'chat about this' escape hatches, and reopen-by-id to append follow-up questions while preserving prior answers. Returns {status:submit\|cancel\|chat\|no_tty, answers:[{id,header,value,selected[],custom,answered}]}. |
| <code>ast_classify</code> |  | yes | yes |  | Zero-shot classify a code block against your own labels via Jina (jina-embeddings-v5-text-small). Feed a card from ast_search plus labels like ["networking","crypto","ui","governance"] to categorize it. |
| <code>ast_edit</code> |  |  |  |  | AST-aware edits. action='rename': whole-word rename a symbol across the tree (protected immune files are skipped, never modified). action='replace_function': replace a function's whole body by its AST line span. Verify with diagnostics after. |
| <code>ast_index</code> |  | yes |  |  | Index dsco's own source into an AST-block vector store (one card per function/struct/typedef). Run once before ast_search; re-run to refresh. Defaults to src/ + include/; pass comma-separated paths to narrow. |
| <code>ast_insights</code> |  | yes | yes |  | Structured aggregate analysis over dsco's source (no API, no index needed): node-type histogram, function complexity stats, complexity hotspots (the 'what needs attention' list), and per-directory rollups. Defaults to src/+include/; pass comma-separated paths to narrow. |
| <code>ast_search</code> |  | yes | yes |  | Semantic retrieval over the indexed AST: describe a symptom or capability in natural language and get the source blocks where it lives, file:line anchored. Local recall + jina-reranker-v3 cross-encoder precision. Run ast_index first. |
| <code>avian</code> |  |  |  |  | Bird-inspired Wings mechanisms: nesting workspaces, brooding incubation, fledging promotion, roosting cooldown, molting refresh. |
| <code>backtrace</code> |  | yes |  |  | Symbolicate a stack from a binary and optional core dump (lldb 'thread backtrace all'). Turns a crash into a readable stack. |
| <code>base64</code> |  | yes | yes |  | Base64 encode/decode (legacy). |
| <code>base64_tool</code> |  | yes | yes |  | Base64 encode/decode. |
| <code>Bash</code> | yes |  |  |  | Claude-compatible shell runner. Use Write/write_file for durable artifacts; declare verify_path/verify_paths when shell creates files. |
| <code>bash</code> | yes |  |  |  | Run a shell command. Use write_file/append_file for durable artifacts; declare verify_path/verify_paths when shell creates files. |
| <code>bg_learn</code> |  |  |  |  | Control the realtime background learner that consolidates self-improvement patterns and mines tool co-occurrence into auto-generated skills. action=status (default) \| on \| off \| run (force one cycle now). |
| <code>big_factorial</code> |  | yes | yes |  | Compute n! for n from 0 through 500 using the bigint engine. |
| <code>browser</code> |  | yes | yes |  | Browser operations: snapshot, extract, viewport, outline. |
| <code>calc</code> |  | yes | yes |  | Evaluate math expressions. |
| <code>call_graph</code> |  | yes | yes |  | Build a call graph rooted at a C function in a project directory. |
| <code>chmod_tool</code> |  |  |  |  | Change file permissions. Accepts octal (e.g. 755) or symbolic (e.g. u+x, go-w). |
| <code>clipboard</code> |  |  |  |  | Read or write the system clipboard via pbpaste/pbcopy with Linux fallbacks. |
| <code>code_index</code> |  |  |  |  | Index source files into the local context store for later code_search. |
| <code>code_search</code> |  | yes | yes |  | Search codebase by symbol or pattern. |
| <code>compile</code> |  |  |  |  | Compile source code. |
| <code>computer</code> |  |  |  |  | Control the local desktop like a human: screenshot, mouse_move, left_click, right_click, middle_click, double_click, triple_click, left_click_drag, key (combos like cmd+a), type, scroll, cursor_position, wait. Coordinates are display points [x,y]; a fresh screenshot is attached after each action so you can see the result. |
| <code>ConstructColorSample</code> | yes | yes |  |  | Sample deterministic named colors or export dynamic highlight colors for the entire live MetaConstruct stack. Use action=sample with name/kind/state/weight for one named color, or action=palette to return colors for loop constructs, effects, signals, graph nodes/edges, dyads, MapReduce jobs, SRM records, measurements, operations, refinements, and schema rewrites. Returns RGB, hex, ansi256, and optional ANSI escapes. |
| <code>context_compact</code> | yes |  |  |  | Compress old conversation history to reclaim tokens. |
| <code>context_recall</code> |  | yes | yes |  | Retrieve persisted tool results. No args = list available keys. |
| <code>context_status</code> | yes | yes | yes |  | Context window self-awareness: tokens, schema overhead, recommendations. |
| <code>contract_ingest</code> |  |  |  |  | Bulk-fetch all open Kalshi events+markets into contracts.db. Persists title, settlement_date, strike, underlying, YES/NO meanings, prices. Run before searching. |
| <code>contract_ingest_all</code> |  |  |  |  | Exhaustive historical ingestion: fetch ALL settled Kalshi markets via cursor pagination into contracts.db. Can take minutes for full history. Use max_pages to control depth. |
| <code>contract_landscape</code> |  | yes | yes |  | Contract database summary: total/open/settled counts, breakdown by underlying asset, settlement date distribution, newest contracts. |
| <code>contract_lookup</code> |  | yes | yes |  | Get full contract context for a ticker or all markets in an event. Returns title, YES/NO meanings, settlement_date, strike, underlying, close_time, prices. |
| <code>contract_new_issues</code> |  | yes | yes |  | Detect NEW contracts not yet in contracts.db. Fetches current open events, diffs against stored contracts, returns only new issues. Run periodically (e.g. every hour) to catch new market listings. |
| <code>contract_search</code> |  | yes | yes |  | Semantic search over persisted contracts. Natural language queries: 'Bitcoin above 90000', 'Fed rate cut March', 'Chicago temperature'. Uses FTS5 full-text search. |
| <code>control_flow</code> |  |  |  |  | Conditional branching (Priority 6): if/while/for/switch/try on steps. action=parse\|evaluate\|execute\|set\|get. |
| <code>copy_file</code> |  |  |  |  | Copy a file or directory. |
| <code>cron_parse</code> |  | yes | yes |  | Parse a cron expression. |
| <code>csv_parse</code> |  | yes | yes |  | Parse CSV text or a CSV file, optionally extracting one column. |
| <code>curl_raw</code> |  |  |  |  | Raw curl command execution. |
| <code>cwd</code> | yes | yes | yes |  | Get current working directory. |
| <code>date</code> |  | yes | yes |  | Get current date/time or parse dates. |
| <code>debugger</code> |  |  |  |  | Drive lldb in batch mode on a program: breakpoints, run, backtrace, inspect. Pass an lldb `script` (newline-separated commands; default run;bt;quit). |
| <code>delete_file</code> |  |  |  |  | Delete a file or empty directory. |
| <code>dependency_graph</code> |  | yes | yes |  | Build a C/C header dependency graph for a project directory. |
| <code>diagnostics</code> |  | yes | yes |  | Fast single-file syntax check with the project's real compile flags (from compile_commands.json). Returns structured {severity,line,col,message} diagnostics in ~200ms — the tight edit→verify loop, no full make needed. Run after editing a .c/.h file. |
| <code>diff</code> |  | yes | yes |  | Compare two files or strings. |
| <code>disasm</code> |  | yes | yes |  | Disassemble a single symbol from a built binary (objdump). See the actual emitted machine code. Defaults to the dsco binary. |
| <code>discover_integrations</code> | yes | yes | yes |  | Discover cached, installed, connected, live, inaccessible, stale, OAuth-gated, mutating, and sync-capable external integrations from the Codex app directory plus live MCP tools. |
| <code>discover_tools</code> | yes | yes | yes |  | List available tools by category or search. |
| <code>disk_usage</code> |  | yes | yes |  | Disk usage for a path. |
| <code>docker</code> |  |  |  |  | Docker operations. |
| <code>download_file</code> |  |  |  |  | Download a file from URL. |
| <code>dsco_doctor_integrations</code> | yes | yes | yes |  | Diagnose integration catalog/cache health: stale connector IDs, missing auth/install state, dangerous mutating connectors, and control-plane governance tools. |
| <code>Edit</code> | yes |  |  |  | Claude-compatible alias for edit_file. |
| <code>edit_file</code> | yes |  |  |  | Edit file by replacing old_string with new_string. |
| <code>EndOfLoopConstruct</code> | yes |  |  |  | Continue, modify, break, complete, or unwind live loop constructs. action=continue/recur can replace the active MetaConstruct DSL program; action=break/complete exits. exit_break_conditions=true resets iteration and restores done/max-turn overrides. |
| <code>EnterPlanMode</code> |  |  |  |  | Enter Claude-compatible advisory plan mode. |
| <code>env_get</code> |  | yes | yes |  | Get environment variable. |
| <code>env_scan</code> |  | yes | yes |  | Search environment variables with sensitive values redacted by default. Set reveal=true only when intentionally inspecting secrets. |
| <code>evict_tools</code> | yes |  |  |  | Unload dynamically loaded tools from the active register file. Provide names, tools, category, or all:true. |
| <code>eval</code> |  | yes | yes |  | Evaluate a math expression. |
| <code>ExitPlanMode</code> |  |  |  |  | Exit Claude-compatible advisory plan mode. |
| <code>file_hash</code> |  | yes | yes |  | Compute SHA-256 hash of a file on disk. Returns hash, path, and file size. |
| <code>file_info</code> |  | yes | yes |  | Get file metadata (size, permissions, timestamps). |
| <code>file_tree</code> |  | yes | yes |  | Recursive directory tree with file sizes and types. Controls depth with max_depth. |
| <code>find_files</code> | yes | yes | yes |  | Find files by name pattern (glob). |
| <code>format_code</code> |  |  |  |  | Run clang-format in place on a file (optionally --lines=A:B). Keeps edits style-consistent with the codebase. |
| <code>fs_watch</code> |  | yes | yes |  | Bounded filesystem watcher: snapshot a path, wait briefly, and report created/changed/deleted files. Defaults to path=. seconds=2. |
| <code>git</code> | yes |  |  |  | Git operations: status, diff, log, commit, add, branch, stash, clone, push, pull. |
| <code>git_bisect</code> |  |  |  |  | Automated git bisect: given good and bad refs and a test command, find the breaking commit. NOTE: moves HEAD across commits during the run, then resets. |
| <code>git_blame</code> |  | yes | yes |  | Line-level authorship: commit, author, when for a file (optionally a start..end line range). |
| <code>git_log_symbol</code> |  | yes | yes |  | Pickaxe history: every commit that changed the count of a symbol/identifier (git log -S). The history of a function. |
| <code>github_search</code> |  | yes | yes |  | Search GitHub repos, code, issues. |
| <code>Glob</code> | yes | yes | yes |  | Claude-compatible file glob search. |
| <code>governance</code> |  |  |  |  | Governance controls: status, curriculum, authorize, checkpoint, budget, audit, param. Curriculum exposes the safety-aware RSI skill gates and top-priority control skills. |
| <code>graphsub</code> |  |  | yes |  | GraphSub substrate client: agent registration, pheromone coordination, graph traversal, memory sync, swarm topology, fleet management. Actions: status, register, pheromone (deposit\|query\|sweep), query (traverse), memory_sync, swarm, fleet. |
| <code>Grep</code> | yes | yes | yes |  | Claude-compatible content search with glob/output_mode/head_limit support. |
| <code>grep_files</code> | yes | yes | yes |  | Search file contents with regex. |
| <code>head_tail</code> |  | yes | yes |  | Read first or last N lines of a file. action=head (default) or tail. |
| <code>hermes_agent</code> |  | yes | yes |  | Nous Hermes Agent compatibility helper: status/doctor/preset/capabilities for MCP stdio bridge (hermes mcp serve), Agent Client Protocol editor mode (hermes acp), gateway/memory/skills/scheduling/provider surfaces, and Hermes MCP config import paths. |
| <code>hkdf</code> |  | yes | yes |  | Derive bytes using HKDF-SHA256 from hex input key material. |
| <code>hmac</code> |  | yes | yes |  | Compute HMAC-SHA256. |
| <code>hostname</code> |  | yes | yes |  | DNS lookup: resolve hostname to IPs (action=resolve) or reverse DNS from IP (action=reverse). |
| <code>http_request</code> | yes | yes | yes |  | Make HTTP requests (GET/POST/PUT/DELETE). |
| <code>include_graph</code> |  | yes | yes |  | Header include tree for a translation unit (cc -H) — what a file pulls in and why edits trigger big rebuilds. |
| <code>inspect_file</code> |  | yes | yes |  | AST summary for one C/C header source file. |
| <code>ipc</code> |  |  |  |  | Inter-process communication: send, recv, agents, scratch_put, scratch_get, task_submit, task_list, set_role. |
| <code>jina_ai_batch_cancel</code> |  |  | yes |  | Cancel a native Jina Batch Embeddings job by batch_id. |
| <code>jina_ai_batch_embed_submit</code> |  |  | yes |  | Submit a native Jina Batch Embeddings job for inline arrays or a GCS JSONL input_file. String arrays are auto-wrapped into Jina custom_id/body batch request objects. |
| <code>jina_ai_batch_errors</code> |  | yes | yes |  | Download native Jina Batch Embeddings error JSONL by batch_id. |
| <code>jina_ai_batch_output</code> |  | yes | yes |  | Download native Jina Batch Embeddings output JSONL by batch_id. |
| <code>jina_ai_batch_status</code> |  | yes | yes |  | Retrieve native Jina Batch Embeddings job status by batch_id. |
| <code>jina_ai_batches_list</code> |  | yes | yes |  | List recent native Jina Batch Embeddings jobs for the authenticated user. |
| <code>jina_ai_chat</code> |  | yes | yes |  | Native Jina experimental Chat Completions API call using OpenAI-compatible messages. Defaults to jina-vlm. |
| <code>jina_ai_classifier_delete</code> |  |  | yes |  | Delete a native Jina few-shot classifier by classifier_id. |
| <code>jina_ai_classifiers_list</code> |  | yes | yes |  | List native Jina few-shot classifiers owned by the authenticated user. |
| <code>jina_ai_classify</code> |  | yes | yes |  | Native Jina Classify API call. Supports zero-shot labels with text/image inputs and few-shot classifier_id classification; accepts body/request for full passthrough. |
| <code>jina_ai_constellation</code> |  |  |  |  | High-level native Jina capability router over live_kb, research, search, reader, embed, rerank, classify, train, match, models, batches, classifiers, and chat. |
| <code>jina_ai_embed</code> |  | yes | yes |  | Native Jina Embeddings API call for text, code, image, PDF, and multimodal input arrays. Defaults to jina-embeddings-v5-text-small. |
| <code>jina_ai_live_kb</code> |  |  |  |  | Realtime Jina web-to-knowledge-base generator. Searches live web, reads sources, chunks cited evidence, optionally reranks/classifies/embeds chunks, and can persist results into the local knowledge_base with persist=true. |
| <code>jina_ai_match</code> |  | yes |  |  | Embedding similarity matcher using Jina embeddings. Embeds query and documents with task=text-matching, then returns documents ranked by cosine similarity. |
| <code>jina_ai_model_get</code> |  | yes | yes |  | Get a native Jina AI model catalog entry by short or full model_id, such as jina-embeddings-v5-text-small or jina-ai/jina-vlm. |
| <code>jina_ai_models_list</code> |  | yes | yes |  | List native Jina AI model catalog entries, including modalities, context lengths, and pricing metadata. |
| <code>jina_ai_reader</code> |  | yes | yes |  | Native Jina Reader API call. Reads one public URL into LLM-ready content with browser, selector, cache, locale, proxy, markdown, and return-format controls. |
| <code>jina_ai_rerank</code> |  | yes | yes |  | Native Jina Reranker API call. Reranks string/object/multimodal documents for a query; defaults to jina-reranker-v3. |
| <code>jina_ai_research</code> |  | yes |  |  | Compound native Jina research pipeline: search the web, read top result URLs, and rerank the extracted page text for the query. |
| <code>jina_ai_search</code> |  | yes | yes |  | Native Jina Search API call. Searches the web through s.jina.ai with SERP, site, engine, cache, locale, proxy, return-format, links, images, and favicon controls. |
| <code>jina_ai_train</code> |  |  | yes |  | Native Jina Train API call. Creates or updates a few-shot classifier from labeled text/image training items; accepts body/request for full passthrough. |
| <code>jina_embed</code> |  | yes | yes |  | Compute embeddings via Jina v4 API. Returns 1024d float vectors for semantic similarity. |
| <code>jina_read</code> |  | yes | yes |  | Fetch a URL as LLM-ready markdown via Jina Reader (r.jina.ai). The natural follow-up to jina_search: search picks URLs, jina_read fetches one. |
| <code>jina_search</code> |  | yes | yes |  | AI-powered web search via Jina AI. Returns titles, URLs, and descriptions; pass content=true for full page content per result (large — prefer jina_read on chosen URLs). |
| <code>jq</code> | yes | yes | yes |  | Process JSON with jq expressions. |
| <code>json_format</code> |  | yes | yes |  | Pretty-print or minify JSON. Pass raw JSON string, get formatted output. |
| <code>jwt_decode</code> |  | yes | yes |  | Decode a JWT header and payload without verifying the signature. |
| <code>kalshi</code> |  |  |  |  | Kalshi prediction market. Actions: markets, events, search, orderbook, trades, series, candlesticks, weather, snapshot, event_detail, daily (read); positions, balance, portfolio, fills, open_orders (account); create_order, batch_create, cancel_order, cancel_all, amend_order (trade); historical_markets, historical_trades, historical_cutoff (history). |
| <code>killswitch</code> |  |  |  |  | Kill switch control: trigger, resolve, status. |
| <code>knowledge_base</code> |  |  |  |  | KB operations: ingest, search, deep_search, list, get, delete, arxiv_search, arxiv_ingest. |
| <code>learned_cost</code> |  |  |  |  | Learned k-NN cost model (Priority 3): predict/record/stats. action=predict needs {task,topology}; action=record needs {task,topology,tokens,cost}; action=stats returns DB summary. |
| <code>legion</code> |  |  |  |  | Legion agent system: spawn, status, find. |
| <code>lint</code> |  |  |  |  | Run clang-tidy on a file and return its warnings/errors. Uses compile_commands.json automatically. |
| <code>list_directory</code> | yes | yes | yes |  | List directory contents with file info. |
| <code>load_tools</code> | yes |  |  |  | Dynamically load tools into the active register file. Provide at least one of: names (comma-separated), tools (array), or category. |
| <code>LoopConstructStatus</code> | yes | yes |  |  | Inspect the live recursive MetaConstruct stack, parsed continue/break expressions, counters, override flags, ontology metadata, mutable graph nodes/edges, traversal state, dyads, MapReduce jobs, SRM/metrology and catalog/order state, effects, reward dynamics, learning signals, policies, decisions, attractors, prompt games, refinement rules, and schema rewrite rules. |
| <code>lsof</code> |  | yes | yes |  | List open files/sockets by pid, port, path, or a short global sample. |
| <code>make_build</code> |  |  |  |  | Build a make target and return STRUCTURED {ok,exit,errors[],warnings[],ms} — the build-side twin of test_run. Defaults to the default target. |
| <code>md5</code> |  | yes | yes |  | Compute MD5 hash of text. |
| <code>memcheck</code> |  |  |  |  | Run a command under macOS `leaks` and report leaked allocations. Pairs with the C codebase for memory-safety checks. |
| <code>memory_tier</code> |  |  |  |  | Three-tier memory: store, recall, promote, classify, review, forget, status. |
| <code>meta_optimize</code> |  |  |  |  | Meta-optimization of the agent's own execution. action=analyze recommends tuning from observed signals (failure/redundancy/cost) + self-improve suggestions; action=apply auto-tunes strategy weights and publishes the config to the IPC scratchpad (meta.recommended_config) for worker processes; action=tune sets a specific weight (param: parallel\|cache\|cost_sensitivity\|timeout_aggression\|compaction_thresh\|batch, value 0..1). |
| <code>mkdir</code> |  |  |  |  | Create directory (with parents). |
| <code>move_file</code> |  |  |  |  | Move or rename a file/directory. |
| <code>net</code> |  |  |  |  | Native networking: mesh P2P (libsodium encrypted), HTTP/TLS server/client (mbedTLS), bridge fleet ops, remote tool invocation. Actions: mesh/status, mesh/peers, mesh/send, mesh/broadcast, mesh/connect, http/post, http/status, bridge/fleet, bridge/exec, bridge/send, bridge/bus_put, bridge/bus_get, remote. |
| <code>net_probe</code> |  |  | yes |  | Active network probe: ping a host or TCP-connect to host:port with a short timeout. |
| <code>network</code> |  | yes | yes |  | Network diagnostics: dns, ping, port_check, port_scan, netstat, cert, traceroute, whois, interfaces, websocket. |
| <code>notify</code> |  |  |  |  | Send a desktop notification (osascript on macOS, printf fallback elsewhere). |
| <code>nws</code> |  | yes | yes |  | NWS API: forecast (lat/lon), hourly, station_obs (METAR station), alerts (by state), stations (near lat/lon), discussion (NWS office AFD). Free, no auth. |
| <code>ol_call</code> | yes | yes | yes |  | Call an already-running local OpenAI-compatible model server (LM Studio/Ollama/MLX) without spawning CLI chat commands. Prefer this over bash lms chat; for LM Studio, start the server with `lms server start`. |
| <code>ooda</code> |  |  |  |  | OODA loop discipline (Talons): begin, observe, orient, decide, complete, status. |
| <code>openrouter_models</code> |  | yes | yes |  | Fetch and filter OpenRouter model metadata by search, context, price, and free/chat-only constraints. |
| <code>page_file</code> |  | yes | yes |  | Page through a large file. |
| <code>parallel_ai_chat</code> |  | yes | yes |  | Native Parallel.ai Chat Completions API call for low-latency web research chat. Accepts OpenAI-style messages or a prompt. |
| <code>parallel_ai_constellation</code> |  |  | yes |  | High-level Parallel.ai capability router over research, live_kb, wait, jobs, Search, Extract, Task, Task Group, FindAll, Monitor, and Chat APIs. |
| <code>parallel_ai_extract</code> |  | yes | yes |  | Native Parallel.ai Extract API call. Extracts relevant LLM-ready content from public URLs. |
| <code>parallel_ai_findall_cancel</code> |  |  |  |  | Cancel a Parallel.ai FindAll run by findall_id, or the tracked latest FindAll run when omitted. |
| <code>parallel_ai_findall_create</code> |  |  | yes |  | Create and track an async Parallel.ai FindAll run with match_conditions. Defaults to generator=base and minimum match_limit=5. |
| <code>parallel_ai_findall_enrich</code> |  |  |  |  | Add an enrichment to a Parallel.ai FindAll run. Defaults to the tracked latest FindAll run and processor=core. |
| <code>parallel_ai_findall_entity_search</code> |  | yes | yes |  | Run Parallel.ai FindAll fast entity search for people or companies. |
| <code>parallel_ai_findall_events</code> |  | yes | yes |  | Retrieve Parallel.ai FindAll events with optional last_event_id and timeout. Uses the tracked latest FindAll run when findall_id is omitted. |
| <code>parallel_ai_findall_extend</code> |  |  |  |  | Extend a Parallel.ai FindAll run with additional_match_limit. Defaults to the tracked latest FindAll run when findall_id is omitted. |
| <code>parallel_ai_findall_ingest</code> |  | yes | yes |  | Ingest a natural-language objective into a structured Parallel.ai FindAll schema without starting a run. |
| <code>parallel_ai_findall_result</code> |  | yes | yes |  | Retrieve a Parallel.ai FindAll run result snapshot by findall_id, or the tracked latest FindAll run when omitted. |
| <code>parallel_ai_findall_schema</code> |  | yes | yes |  | Retrieve the schema for a Parallel.ai FindAll run, defaulting to the tracked latest FindAll run. |
| <code>parallel_ai_findall_status</code> |  | yes | yes |  | Retrieve a Parallel.ai FindAll run status by findall_id, or the tracked latest FindAll run when omitted. |
| <code>parallel_ai_jobs</code> |  | yes | yes |  | List locally tracked Parallel.ai job ids from create/status/result calls. Kinds include task_run, task_group, findall, and monitor. |
| <code>parallel_ai_live_kb</code> |  |  | yes |  | Realtime Parallel.ai knowledge-base generator: Search, Extract sources, chunk cited evidence, and optionally persist extracted documents into the local knowledge_base. |
| <code>parallel_ai_monitor_cancel</code> |  |  |  |  | Cancel a Parallel.ai monitor, stopping future executions. Uses the tracked latest monitor when monitor_id is omitted. |
| <code>parallel_ai_monitor_create</code> |  |  |  |  | Create a Parallel.ai Monitor API event_stream or snapshot monitor. This creates a recurring web monitor. |
| <code>parallel_ai_monitor_events</code> |  | yes | yes |  | List detected events for a Parallel.ai monitor, including cursor and event_group_id filters. Uses the tracked latest monitor when omitted. |
| <code>parallel_ai_monitor_get</code> |  | yes | yes |  | Retrieve a Parallel.ai monitor by monitor_id, or the tracked latest monitor when monitor_id is omitted. |
| <code>parallel_ai_monitor_list</code> |  | yes | yes |  | List Parallel.ai monitors for the current API key. |
| <code>parallel_ai_monitor_trigger</code> |  |  |  |  | Trigger an immediate Parallel.ai monitor run. Uses the tracked latest monitor when monitor_id is omitted. |
| <code>parallel_ai_monitor_update</code> |  |  |  |  | Update a Parallel.ai monitor. Uses the tracked latest monitor when monitor_id is omitted. |
| <code>parallel_ai_research</code> |  |  | yes |  | Composed Parallel.ai research pipeline: Search, Extract top URLs, and optionally create/poll a Task API synthesis with locally tracked job ids. |
| <code>parallel_ai_search</code> |  | yes | yes |  | Native Parallel.ai Search API call. Searches the web with objective, search_queries, mode, session_id, and advanced_settings support. Defaults to mode=turbo when omitted. |
| <code>parallel_ai_task_create</code> |  |  | yes |  | Create and track a Parallel.ai Task API run for web research or enrichment. Defaults to the lowest task processor, base. Use parallel_ai_task_result to long-poll for output; later task tools can omit run_id and use the tracked latest run. |
| <code>parallel_ai_task_events</code> |  | yes | yes |  | Retrieve Parallel.ai Task run progress events by run_id, or the tracked latest task run when run_id is omitted. Set beta=true for the v1beta events route. |
| <code>parallel_ai_task_group_add_runs</code> |  |  | yes |  | Add one or many Task runs to a tracked Parallel.ai Task Group. Single-input convenience defaults to processor=base. |
| <code>parallel_ai_task_group_create</code> |  |  | yes |  | Create and track a Parallel.ai Task Group for coordinating many Task runs. |
| <code>parallel_ai_task_group_events</code> |  | yes | yes |  | Retrieve Parallel.ai Task Group events with optional last_event_id and timeout. Uses the tracked latest group when taskgroup_id is omitted. |
| <code>parallel_ai_task_group_get</code> |  | yes | yes |  | Retrieve a Parallel.ai Task Group by taskgroup_id, or the tracked latest group when omitted. |
| <code>parallel_ai_task_group_run_get</code> |  | yes | yes |  | Retrieve one run inside a Parallel.ai Task Group. Falls back to the tracked latest task_group and task_run ids when omitted. |
| <code>parallel_ai_task_group_runs</code> |  | yes | yes |  | Fetch Task runs in a Parallel.ai Task Group, optionally including inputs and outputs. Uses the tracked latest group when taskgroup_id is omitted. |
| <code>parallel_ai_task_input</code> |  | yes | yes |  | Retrieve the original input for a Parallel.ai Task run by run_id, or the tracked latest task run when run_id is omitted. |
| <code>parallel_ai_task_result</code> |  | yes | yes |  | Long-poll a Parallel.ai Task API run result by run_id, or the tracked latest task run when run_id is omitted. |
| <code>parallel_ai_task_status</code> |  | yes | yes |  | Retrieve a Parallel.ai Task API run status by run_id, or the tracked latest task run when run_id is omitted. |
| <code>parallel_ai_wait</code> |  | yes |  |  | Poll a tracked Parallel.ai async job until terminal status, then fetch the result/runs when available. Supports task_run, task_group, findall, and monitor; ids default to the locally tracked latest job for the kind. |
| <code>parallel_search</code> | yes | yes | yes |  | Fan out web search to multiple providers (Jina, Tavily, Brave) concurrently. Returns merged results from all available providers. |
| <code>pets</code> |  | yes | yes |  | Companion sprites for background agents. action=roster shows live background-agent pets (face, status, cost, activity sparkline); gallery shows a species sampler; roll shows a single deterministic pet for a seed string. Each agent deterministically hatches the same pet from its id/task. |
| <code>pheromone</code> |  |  |  |  | Pheromone coordination (Wings): deposit, sense, status. |
| <code>pipeline</code> |  |  |  |  | Pipeline execution and chaining. |
| <code>plan_state</code> |  |  |  |  | Stateful atom execution (Priority 2): init/run/rollback/get/set. action=init needs {plan_id}; action=run needs {atom_id}; action=rollback needs {steps}; action=get/set needs {atom_id}. |
| <code>playbook</code> |  |  |  |  | ACE playbook: read, add, tag, remove, search, gc, inject. |
| <code>playbook_add</code> |  |  |  |  | Add entry to ACE playbook. |
| <code>playbook_search</code> |  | yes | yes |  | Search ACE playbook. |
| <code>plot</code> |  | yes | yes |  | Render data as a Unicode chart (returns ANSI/Unicode art). Types: line, bar, column, area, scatter, hist, heatmap, box, candlestick, gauge, sparkline, pie, waterfall, bullet, lollipop, slope, ecdf, calendar, ridgeline, violin, bignum, attractor, mandelbrot, julia. Uses subpixel Braille (2x4 dots/cell) for line/scatter/area/ridgeline/attractor, eighth-block bars, and 256-color heatmaps/calendars. 'attractor' traces a 2-D chaotic map in phase space (kind=dejong\|clifford, coeffs a/b/c/d, iters) with viridis density shading. 'mandelbrot'/'julia' render escape-time fractals in truecolor half-blocks (cx/cy center, zoom, iters; julia adds jx/jy) — no data needed for any of these. Inline-printable and usable as a display artifact. |
| <code>plugin_validate</code> |  | yes | yes |  | Validate a plugin manifest and optional lockfile. |
| <code>polymarket</code> |  |  |  |  | Polymarket prediction market. Actions: markets, events, categories, prices, book, trades, search, resolved, resolved_events, whale_trades, leaderboard, history (read); balance, positions, open_orders, api_keys, derive_api_key (account); create_order, cancel_order, cancel_all (trade); relayer_deploy, relayer_approve, relayer_execute, relayer_status (relayer). |
| <code>port_check</code> |  | yes | yes |  | Check if a TCP port is open on a host. 3-second timeout. |
| <code>prediction</code> |  | yes | yes |  | Cross-platform prediction market ops: scan, weather, snapshot, arb, semantic_match, cross_delta, movers, cache_refresh, cache_query, historical. |
| <code>preprocess</code> |  | yes | yes |  | Run the C preprocessor (cc -E) on a file with the project's real flags — see fully expanded macros. Essential in a macro-heavy codebase. |
| <code>privacy_filter</code> |  | yes | yes |  | Redact obvious email addresses and phone-like tokens from text. |
| <code>process_tree</code> |  | yes | yes |  | Show process parent/child rows, optionally filtered. |
| <code>ps</code> |  | yes | yes |  | List running processes. |
| <code>python</code> | yes |  |  |  | Run Python code or a Python file. |
| <code>random_bytes</code> |  | yes | yes |  | Generate random bytes (hex). |
| <code>Read</code> | yes | yes | yes |  | Claude-compatible alias for read_file. |
| <code>read_file</code> | yes | yes | yes |  | Read file with line numbers. Use offset/limit for large files. |
| <code>recovery</code> |  |  |  |  | Failure recovery (Priority 7): retry/fallback/backtrack. action=retry\|fallback\|backtrack\|log_dump. |
| <code>regex_match</code> |  | yes | yes |  | Run an extended regular expression over text and return matches. |
| <code>research_compare</code> |  | yes | yes |  | Compare two text snippets with token overlap and Jaccard similarity. |
| <code>research_probe</code> |  | yes | yes |  | Deep research probe on a topic. |
| <code>review_diff</code> |  | yes | yes |  | Structured working diff: {files:[{path,added,removed}], file_count, added, removed}. Reason over your own changes before committing. |
| <code>risk_gate</code> |  | yes | yes |  | Score an action/content pair for destructive, privileged, secret, or PII risk. |
| <code>run_command</code> | yes |  |  |  | Run a shell command with optional artifact verification. |
| <code>sandbox_run</code> |  |  |  |  | Run command in sandboxed container. |
| <code>scratchpad</code> | yes |  |  |  | Read/write scratchpad for temporary data. |
| <code>secret_scan</code> |  | yes | yes |  | Scan text or one file for obvious secret patterns. |
| <code>self_analyze</code> |  | yes | yes |  | Deep self-analysis: per-tool efficiency (success rate, latency, efficiency score), session economy (turns/cost/failures/redundancy), adaptive strategy weights, and the live independent agent PROCESSES interoperating via IPC (pid, role, status, task). |
| <code>self_assess</code> |  | yes | yes |  | Quick self-evaluation of current session performance. Returns efficiency score, top issues, and recommendations. No input required. |
| <code>self_exit</code> | yes |  |  |  | Gracefully exit the agent loop. |
| <code>self_exiting</code> | yes |  |  |  | Legacy alias for self_exit. |
| <code>self_improve</code> |  | yes |  |  | Run the self-improvement loop and RSI safety curriculum: summary, consolidate, acknowledge, history, save, curriculum, skill, promotion_gate. |
| <code>self_inspect</code> |  | yes | yes |  | AST summary for a C/C header project directory. |
| <code>semver_compare</code> |  | yes | yes |  | Compare two semantic versions. |
| <code>session_memory</code> |  |  |  |  | Persistent session KV memory (Priority 5): remember/recall/status. action=remember needs {key,value,ttl(seconds, 0=permanent)}; action=recall needs {key}; action=status returns counts. |
| <code>sha256</code> |  | yes | yes |  | Compute SHA-256 hash of text. |
| <code>signal_process</code> |  |  |  |  | Send a signal to a process by pid (default TERM). Process mutation is governance-gated. |
| <code>slack_post</code> |  |  |  |  | Post message to Slack. |
| <code>spawn_bg</code> |  |  |  |  | Start a background shell command with nohup, returning {pid,log}. Exec-gated because it launches arbitrary work. |
| <code>sqlite</code> |  |  |  |  | Execute SQLite queries. |
| <code>ssh_command</code> |  |  |  |  | Run command on remote host via SSH. |
| <code>stage_hunk</code> |  |  |  |  | Stage a patch to the git index (git apply --cached) without touching the working tree — build tight, single-concern commits. |
| <code>StartOfLoopConstruct</code> | yes |  |  |  | Start a live recursive agent loop construct. Accepts a bounded MetaConstruct/OORL DSL: continue/break expressions, max controls, DEFINE/GOAL/TASK/BELIEF/INFER/DECIDE/LEARN metadata, mutable ontology graph nodes/edges, dyad object interactions, reward objects, valence/intensity, causal/message links, stochastic exploration, pruning, credit assignment, attractors, prompt games, basin hopping, effect weights, traversal/find/balance operations, MapReduce map/shuffle/reduce job state, SRM catalog/store search, availability/orderability, licensed distributors, order policies, shipping restrictions, standard reference material records, certificates/reports/SDS, metrological traceability, calibration measurements, uncertainty budgets, one-shot REFINE rules, and bounded schema_rewrite rules. Example: define(sensor,state); reward_object success valence 0.8 intensity 0.5 target state; causal_link state -> action weight 0.7; schema_rewrite add_edge state -> policy relation optimized weight 0.9 when credit >= 0.8; continue when rewrites_applied >= 1. Expressions support loop variables plus meta_count, belief_count, goal_count, task_count, dyad_count, reward_object_count, causal_link_count, message_count, node_count, edge_count, graph_density, traverse_hits, mapreduce_count, map_count, shuffle_count, reduce_count, partition_count, rewrite_count, rewrites_applied, srm_count, current_certificate_count, sds_count, traceability_count, measurement_count, calibration_count, uncertainty_budget_count, mean_uncertainty, max_uncertainty, available_count, orderable_count, product_search_count, catalog_count, annual_catalog_count, licensed_distributor_count, order_policy_count, paper_checks_blocked, shipping_block_count, price_total, effect.tool, effect.world, effect.meta, reward, valence, intensity, exploration_rate, credit, curiosity, empowerment, confidence, uncertainty, learning_rate, pruning_threshold, basin_temperature. |
| <code>strategy</code> |  | yes | yes |  | Trading strategies: completeness, binary_fade, stale_snipe, kelly, spread_scan. |
| <code>string_transform</code> |  | yes | yes |  | String transformations: upper, lower, trim, reverse, slugify, capitalize. |
| <code>swarm</code> | yes |  |  |  | Swarm orchestration: create, map_reduce, status, collect, inspect, budget, spawn_executor, spawn_provider, provider_fabric, create_executor_swarm, executor_status, topology_list, topology_run, task_profile. provider_fabric saturates available subscription/provider lanes (Fugu weighted first) by spawning independent provider-pinned dsco worker processes; it defaults to race/speculative execution, returning the first successful lane and killing slower losers; mode=collect waits for all, and mode=spawn returns the live group. map_reduce fans out 'tasks' as parallel workers then spawns a 'coordinator' sub-agent that synthesizes their outputs into one result. Each spawned agent is an INDEPENDENT OS process wrapping a model instance; action=create accepts per-agent effort/temperature/system_prompt/tool_choice so workers can run as distinct instances in parallel, interoperating via IPC. |
| <code>symbol_def</code> |  | yes | yes |  | Exact go-to-definition over the AST: find where a function/struct/typedef/enum named <name> is defined, with file:line and signature. Precise complement to the semantic ast_search. |
| <code>symbol_refs</code> |  | yes | yes |  | Find-references: every whole-word use of an identifier across the source, file:line anchored with the source line. Use before renaming or to trace impact. |
| <code>synoptic</code> |  | yes | yes |  | Synoptic Data real-time weather station observations (ASOS/METAR). Actions: latest (current obs), timeseries (historical), nearesttime, metadata, precip, kalshi_stations (all 29 Kalshi cities). Requires SYNOPTIC_API_TOKEN. |
| <code>syscall_trace</code> |  |  |  |  | Trace the syscalls a command makes (dtruss; may require elevated privileges). Ground truth for what a program actually touches. |
| <code>sysinfo</code> |  | yes | yes |  | System info: CPU, memory, OS. |
| <code>system_profiler</code> |  | yes | yes |  | Summarize local CPU, disk, network, or load information. |
| <code>systematic</code> |  |  |  |  | Systematic trading: ingest_polymarket, ingest_kalshi, analytics, signals. |
| <code>tail_follow</code> |  | yes | yes |  | Tail a file, optionally following for a bounded number of seconds. Use for logs without hanging the agent. |
| <code>talons</code> |  |  |  |  | Competitive execution (Talons): goal, advance, depend, tick, tournament, recommend, status. Strategies (36, military-history canon): direct, flanking, escalation, divide, ambush, attrition, pincer, blitz, siege, feint, opportunistic, envelopment, encirclement, guerrilla, scorched_earth, fabian, defense_in_depth, oblique, infiltration, interior_lines, defeat_in_detail, turning_movement, breakthrough, shock, decapitation, blockade, raid, indirect, tempo, deterrence, counterattack, maneuver, hedgehog, screen, asymmetric, tournament. Or omit strategy and use action=recommend. depend gates a goal on a prerequisite (dep_goal_id); tick runs the deadline/dependency engine. |
| <code>Task</code> |  |  |  |  | Claude-compatible task agent alias. |
| <code>TaskList</code> |  | yes | yes |  | Return the Claude-compatible todo list state. |
| <code>tavily_search</code> |  | yes | yes |  | Web search via Tavily. |
| <code>template_render</code> |  | yes | yes |  | Render a simple {{name}} template from a JSON-object string of variables. |
| <code>test_run</code> |  | yes |  |  | Run a make target and return STRUCTURED results: {passed,total,failed,failing[],exit,ms}. Parses the 'N tests: M passed' summary and collects FAIL/error lines. Use instead of `bash make test` when you need to act on the outcome. Defaults to target 'test'. |
| <code>text_diff</code> |  | yes | yes |  | Compute a unified diff between two text strings. |
| <code>timestamp</code> |  | yes | yes |  | Current time: epoch, iso (ISO 8601), local, date, or relative (time until a target ISO timestamp). |
| <code>TodoWrite</code> |  |  |  |  | Claude-compatible todo list state writer. |
| <code>token_audit</code> |  | yes | yes |  | Audit token usage across conversation. |
| <code>trading</code> |  |  |  |  | Trading ops: arb_execute, arb_monitor, portfolio, risk_check, risk_configure. |
| <code>type_at</code> |  | yes | yes |  | Clang AST nodes at file:line — the resolved types of what appears on that line. Precise type info the semantic search can't give. |
| <code>unused_symbols</code> |  | yes | yes |  | Find static functions that are referenced only at their own definition — dead code safe to remove. AST-based, file-scope-correct. |
| <code>url_parse</code> |  | yes | yes |  | Parse a URL into components. |
| <code>uuid</code> |  | yes | yes |  | Generate a UUID v4. |
| <code>view_image</code> |  |  |  |  | Prepare a local image file for model-side vision analysis. |
| <code>view_pdf</code> |  |  |  |  | Prepare a local PDF file for model-side document analysis. |
| <code>vos_status</code> |  | yes | yes |  | Virtual OS subsystem status. |
| <code>watch_run</code> |  |  |  |  | Run a command and return {exit, wall_ms, peak_rss_bytes} — structured resource accounting via /usr/bin/time -l. |
| <code>weather</code> |  | yes | yes |  | Get weather data for a location. |
| <code>WebFetch</code> | yes | yes | yes |  | Claude-compatible URL fetch/extract. |
| <code>WebSearch</code> | yes | yes | yes |  | Claude-compatible web search alias. |
| <code>wings_talons_status</code> |  | yes | yes |  | Unified Wings+Talons+Immune system status. |
| <code>word_count</code> |  | yes | yes |  | Count words, lines, and characters in a file or text string. |
| <code>workflow</code> |  |  |  |  | Workflow management: plan, status, checkpoint, resume, heartbeat, dead-letter, reprocess, validate, smoke. |
| <code>Write</code> | yes |  |  |  | Claude-compatible alias for verified atomic write_file. |
| <code>write_file</code> | yes |  |  |  | Create/overwrite a file atomically, fsync it, and verify bytes on disk. Creates parent dirs. |
| <code>xml_extract</code> |  | yes | yes |  | Extract tag contents or attribute values from XML/HTML text or a file. |
