# Built-in Tool Catalog

This catalog is generated from the static `src/tools.c` built-in tool registry.

- Source: `src/tools.c`
- Total built-in tools: 172
- Core tools: 39
- Read-only tools: 100
- Concurrent tools: 98
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
| <code>Agent</code> |  |  |  |  | Claude-compatible sub-agent task alias. |
| <code>agent</code> | yes |  |  |  | Agent management: spawn, status, output, wait, race, kill. |
| <code>agent_wait</code> |  |  |  |  | Wait for agent(s) to complete. |
| <code>agentic_commerce</code> |  | yes | yes |  | Agentic commerce protocol registry: list/status/coverage/plan for ACP, UCP, AP2, x402, Stripe MPP/SPT, Visa, Mastercard, and clearing watchlist protocols. |
| <code>alpha_vantage</code> |  | yes | yes |  | Alpha Vantage financial data API. Supports 100+ functions: time series (TIME_SERIES_DAILY, TIME_SERIES_INTRADAY), technical indicators (SMA, EMA, RSI, MACD, BBANDS, STOCH, ADX, CCI, OBV, ATR, VWAP), fundamentals (OVERVIEW, INCOME_STATEMENT, BALANCE_SHEET, EARNINGS), macro (CPI, REAL_GDP, UNEMPLOYMENT, TREASURY_YIELD), commodities (WTI, BRENT, NATURAL_GAS, GOLD_SILVER_SPOT), forex (CURRENCY_EXCHANGE_RATE, FX_DAILY), crypto (DIGITAL_CURRENCY_DAILY), options (REALTIME_OPTIONS), news (NEWS_SENTIMENT). |
| <code>append_file</code> |  |  |  |  | Append content, fsync it, and verify appended bytes on disk. |
| <code>AskUserQuestion</code> | yes |  |  | yes | Show an interactive multi-question dialog to collect structured input from the user. Use when a response merits clarification. Supports option lists with descriptions, conditional branching (show_if), computed options (options_cmd), free-text + 'chat about this' escape hatches, and reopen-by-id to append follow-up questions while preserving prior answers. Returns {status:submit\|cancel\|chat\|no_tty, answers:[{id,header,value,selected[],custom,answered}]}. |
| <code>avian</code> |  |  |  |  | Bird-inspired Wings mechanisms: nesting workspaces, brooding incubation, fledging promotion, roosting cooldown, molting refresh. |
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
| <code>delete_file</code> |  |  |  |  | Delete a file or empty directory. |
| <code>dependency_graph</code> |  | yes | yes |  | Build a C/C header dependency graph for a project directory. |
| <code>diff</code> |  | yes | yes |  | Compare two files or strings. |
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
| <code>eval</code> |  | yes | yes |  | Evaluate a math expression. |
| <code>ExitPlanMode</code> |  |  |  |  | Exit Claude-compatible advisory plan mode. |
| <code>file_hash</code> |  | yes | yes |  | Compute SHA-256 hash of a file on disk. Returns hash, path, and file size. |
| <code>file_info</code> |  | yes | yes |  | Get file metadata (size, permissions, timestamps). |
| <code>file_tree</code> |  | yes | yes |  | Recursive directory tree with file sizes and types. Controls depth with max_depth. |
| <code>find_files</code> | yes | yes | yes |  | Find files by name pattern (glob). |
| <code>git</code> | yes |  |  |  | Git operations: status, diff, log, commit, add, branch, stash, clone, push, pull. |
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
| <code>inspect_file</code> |  | yes | yes |  | AST summary for one C/C header source file. |
| <code>ipc</code> |  |  |  |  | Inter-process communication: send, recv, agents, scratch_put, scratch_get, task_submit, task_list, set_role. |
| <code>jina_embed</code> |  | yes | yes |  | Compute embeddings via Jina v4 API. Returns 1024d float vectors for semantic similarity. |
| <code>jina_search</code> |  | yes | yes |  | AI-powered web search via Jina AI. Returns structured results with titles, URLs, and descriptions. |
| <code>jq</code> | yes | yes | yes |  | Process JSON with jq expressions. |
| <code>json_format</code> |  | yes | yes |  | Pretty-print or minify JSON. Pass raw JSON string, get formatted output. |
| <code>jwt_decode</code> |  | yes | yes |  | Decode a JWT header and payload without verifying the signature. |
| <code>kalshi</code> |  |  |  |  | Kalshi prediction market. Actions: markets, events, search, orderbook, trades, series, candlesticks, weather, snapshot, event_detail, daily (read); positions, balance, portfolio, fills, open_orders (account); create_order, batch_create, cancel_order, cancel_all, amend_order (trade); historical_markets, historical_trades, historical_cutoff (history). |
| <code>killswitch</code> |  |  |  |  | Kill switch control: trigger, resolve, status. |
| <code>knowledge_base</code> |  |  |  |  | KB operations: ingest, search, deep_search, list, get, delete, arxiv_search, arxiv_ingest. |
| <code>learned_cost</code> |  |  |  |  | Learned k-NN cost model (Priority 3): predict/record/stats. action=predict needs {task,topology}; action=record needs {task,topology,tokens,cost}; action=stats returns DB summary. |
| <code>legion</code> |  |  |  |  | Legion agent system: spawn, status, find. |
| <code>list_directory</code> | yes | yes | yes |  | List directory contents with file info. |
| <code>load_tools</code> | yes |  |  |  | Dynamically load tools into the active register file. Provide at least one of: names (comma-separated), tools (array), or category. |
| <code>LoopConstructStatus</code> | yes | yes |  |  | Inspect the live recursive MetaConstruct stack, parsed continue/break expressions, counters, override flags, ontology metadata, mutable graph nodes/edges, traversal state, dyads, MapReduce jobs, SRM/metrology and catalog/order state, effects, reward dynamics, learning signals, policies, decisions, attractors, prompt games, refinement rules, and schema rewrite rules. |
| <code>md5</code> |  | yes | yes |  | Compute MD5 hash of text. |
| <code>memory_tier</code> |  |  |  |  | Three-tier memory: store, recall, promote, forget, status. |
| <code>meta_optimize</code> |  |  |  |  | Meta-optimization of the agent's own execution. action=analyze recommends tuning from observed signals (failure/redundancy/cost) + self-improve suggestions; action=apply auto-tunes strategy weights and publishes the config to the IPC scratchpad (meta.recommended_config) for worker processes; action=tune sets a specific weight (param: parallel\|cache\|cost_sensitivity\|timeout_aggression\|compaction_thresh\|batch, value 0..1). |
| <code>mkdir</code> |  |  |  |  | Create directory (with parents). |
| <code>move_file</code> |  |  |  |  | Move or rename a file/directory. |
| <code>net</code> |  |  |  |  | Native networking: mesh P2P (libsodium encrypted), HTTP/TLS server/client (mbedTLS), bridge fleet ops, remote tool invocation. Actions: mesh/status, mesh/peers, mesh/send, mesh/broadcast, mesh/connect, http/post, http/status, bridge/fleet, bridge/exec, bridge/send, bridge/bus_put, bridge/bus_get, remote. |
| <code>network</code> |  | yes | yes |  | Network diagnostics: dns, ping, port_check, port_scan, netstat, cert, traceroute, whois, interfaces, websocket. |
| <code>nws</code> |  | yes | yes |  | NWS API: forecast (lat/lon), hourly, station_obs (METAR station), alerts (by state), stations (near lat/lon), discussion (NWS office AFD). Free, no auth. |
| <code>ol_call</code> | yes | yes | yes |  | Call an already-running local OpenAI-compatible model server (LM Studio/Ollama/MLX) without spawning CLI chat commands. Prefer this over bash lms chat; for LM Studio, start the server with `lms server start`. |
| <code>ooda</code> |  |  |  |  | OODA loop discipline (Talons): begin, observe, orient, decide, complete, status. |
| <code>openrouter_models</code> |  | yes | yes |  | Fetch and filter OpenRouter model metadata by search, context, price, and free/chat-only constraints. |
| <code>page_file</code> |  | yes | yes |  | Page through a large file. |
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
| <code>privacy_filter</code> |  | yes | yes |  | Redact obvious email addresses and phone-like tokens from text. |
| <code>process_tree</code> |  | yes | yes |  | Show process parent/child rows, optionally filtered. |
| <code>ps</code> |  | yes | yes |  | List running processes. |
| <code>python</code> | yes |  |  |  | Run Python code. |
| <code>random_bytes</code> |  | yes | yes |  | Generate random bytes (hex). |
| <code>Read</code> | yes | yes | yes |  | Claude-compatible alias for read_file. |
| <code>read_file</code> | yes | yes | yes |  | Read file with line numbers. Use offset/limit for large files. |
| <code>recovery</code> |  |  |  |  | Failure recovery (Priority 7): retry/fallback/backtrack. action=retry\|fallback\|backtrack\|log_dump. |
| <code>regex_match</code> |  | yes | yes |  | Run an extended regular expression over text and return matches. |
| <code>research_compare</code> |  | yes | yes |  | Compare two text snippets with token overlap and Jaccard similarity. |
| <code>research_probe</code> |  | yes | yes |  | Deep research probe on a topic. |
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
| <code>slack_post</code> |  |  |  |  | Post message to Slack. |
| <code>sqlite</code> |  |  |  |  | Execute SQLite queries. |
| <code>ssh_command</code> |  |  |  |  | Run command on remote host via SSH. |
| <code>StartOfLoopConstruct</code> | yes |  |  |  | Start a live recursive agent loop construct. Accepts a bounded MetaConstruct/OORL DSL: continue/break expressions, max controls, DEFINE/GOAL/TASK/BELIEF/INFER/DECIDE/LEARN metadata, mutable ontology graph nodes/edges, dyad object interactions, reward objects, valence/intensity, causal/message links, stochastic exploration, pruning, credit assignment, attractors, prompt games, basin hopping, effect weights, traversal/find/balance operations, MapReduce map/shuffle/reduce job state, SRM catalog/store search, availability/orderability, licensed distributors, order policies, shipping restrictions, standard reference material records, certificates/reports/SDS, metrological traceability, calibration measurements, uncertainty budgets, one-shot REFINE rules, and bounded schema_rewrite rules. Example: define(sensor,state); reward_object success valence 0.8 intensity 0.5 target state; causal_link state -> action weight 0.7; schema_rewrite add_edge state -> policy relation optimized weight 0.9 when credit >= 0.8; continue when rewrites_applied >= 1. Expressions support loop variables plus meta_count, belief_count, goal_count, task_count, dyad_count, reward_object_count, causal_link_count, message_count, node_count, edge_count, graph_density, traverse_hits, mapreduce_count, map_count, shuffle_count, reduce_count, partition_count, rewrite_count, rewrites_applied, srm_count, current_certificate_count, sds_count, traceability_count, measurement_count, calibration_count, uncertainty_budget_count, mean_uncertainty, max_uncertainty, available_count, orderable_count, product_search_count, catalog_count, annual_catalog_count, licensed_distributor_count, order_policy_count, paper_checks_blocked, shipping_block_count, price_total, effect.tool, effect.world, effect.meta, reward, valence, intensity, exploration_rate, credit, curiosity, empowerment, confidence, uncertainty, learning_rate, pruning_threshold, basin_temperature. |
| <code>strategy</code> |  | yes | yes |  | Trading strategies: completeness, binary_fade, stale_snipe, kelly, spread_scan. |
| <code>string_transform</code> |  | yes | yes |  | String transformations: upper, lower, trim, reverse, slugify, capitalize. |
| <code>swarm</code> | yes |  |  |  | Swarm orchestration: create, map_reduce, status, collect, budget, spawn_executor, spawn_provider, create_executor_swarm, executor_status, topology_list, topology_run, task_profile. map_reduce fans out 'tasks' as parallel workers then spawns a 'coordinator' sub-agent that synthesizes their outputs into one result (hierarchical map→reduce; workers may recurse). Each spawned agent is an INDEPENDENT OS process wrapping a model instance; action=create accepts per-agent effort/temperature/system_prompt/tool_choice so workers can run as distinct instances in parallel, interoperating via IPC. |
| <code>synoptic</code> |  | yes | yes |  | Synoptic Data real-time weather station observations (ASOS/METAR). Actions: latest (current obs), timeseries (historical), nearesttime, metadata, precip, kalshi_stations (all 29 Kalshi cities). Requires SYNOPTIC_API_TOKEN. |
| <code>sysinfo</code> |  | yes | yes |  | System info: CPU, memory, OS. |
| <code>system_profiler</code> |  | yes | yes |  | Summarize local CPU, disk, network, or load information. |
| <code>systematic</code> |  |  |  |  | Systematic trading: ingest_polymarket, ingest_kalshi, analytics, signals. |
| <code>talons</code> |  |  |  |  | Competitive execution (Talons): goal, advance, depend, tick, tournament, recommend, status. Strategies (36, military-history canon): direct, flanking, escalation, divide, ambush, attrition, pincer, blitz, siege, feint, opportunistic, envelopment, encirclement, guerrilla, scorched_earth, fabian, defense_in_depth, oblique, infiltration, interior_lines, defeat_in_detail, turning_movement, breakthrough, shock, decapitation, blockade, raid, indirect, tempo, deterrence, counterattack, maneuver, hedgehog, screen, asymmetric, tournament. Or omit strategy and use action=recommend. depend gates a goal on a prerequisite (dep_goal_id); tick runs the deadline/dependency engine. |
| <code>Task</code> |  |  |  |  | Claude-compatible task agent alias. |
| <code>TaskList</code> |  | yes | yes |  | Return the Claude-compatible todo list state. |
| <code>tavily_search</code> |  | yes | yes |  | Web search via Tavily. |
| <code>template_render</code> |  | yes | yes |  | Render a simple {{name}} template from a JSON-object string of variables. |
| <code>text_diff</code> |  | yes | yes |  | Compute a unified diff between two text strings. |
| <code>timestamp</code> |  | yes | yes |  | Current time: epoch, iso (ISO 8601), local, date, or relative (time until a target ISO timestamp). |
| <code>TodoWrite</code> |  |  |  |  | Claude-compatible todo list state writer. |
| <code>token_audit</code> |  | yes | yes |  | Audit token usage across conversation. |
| <code>trading</code> |  |  |  |  | Trading ops: arb_execute, arb_monitor, portfolio, risk_check, risk_configure. |
| <code>url_parse</code> |  | yes | yes |  | Parse a URL into components. |
| <code>uuid</code> |  | yes | yes |  | Generate a UUID v4. |
| <code>view_image</code> |  |  |  |  | Prepare a local image file for model-side vision analysis. |
| <code>view_pdf</code> |  |  |  |  | Prepare a local PDF file for model-side document analysis. |
| <code>vos_status</code> |  | yes | yes |  | Virtual OS subsystem status. |
| <code>weather</code> |  | yes | yes |  | Get weather data for a location. |
| <code>WebFetch</code> | yes | yes | yes |  | Claude-compatible URL fetch/extract. |
| <code>WebSearch</code> | yes | yes | yes |  | Claude-compatible web search alias. |
| <code>wings_talons_status</code> |  | yes | yes |  | Unified Wings+Talons+Immune system status. |
| <code>word_count</code> |  | yes | yes |  | Count words, lines, and characters in a file or text string. |
| <code>workflow</code> |  |  |  |  | Workflow management: plan, status, checkpoint, resume, heartbeat, dead-letter, reprocess, validate, smoke. |
| <code>Write</code> | yes |  |  |  | Claude-compatible alias for verified atomic write_file. |
| <code>write_file</code> | yes |  |  |  | Create/overwrite a file atomically, fsync it, and verify bytes on disk. Creates parent dirs. |
| <code>xml_extract</code> |  | yes | yes |  | Extract tag contents or attribute values from XML/HTML text or a file. |
