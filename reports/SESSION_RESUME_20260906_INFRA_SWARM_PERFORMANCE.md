# Session resume — infrastructure, swarms, observability and harness performance

Written 2026-09-06. Principal: Arthur Colle / Distributed Systems, Inc.
Canonical checkout: `/Users/arthurcolle/dsco/dsco-cli` (case-insensitive alias `~/Dsco/dsco-cli`). Branch observed: `perf/workspace-catalog-chronicle-index`, HEAD `29390f9`. Working tree is heavily dirty, with changes occurring during this session: re-read before editing and preserve unrelated work. Do not treat HEAD as identifying all inspected bytes.

## User objective and steering

Arthur wants to sell **GraphSub + agents.erl + Tool Management + router + harness as one integrated package**, with an easy installer, extensive telemetry, and the ability to train competitive models. The package itself is the product, NOT a single-task application replacing that vision.

Requests, in order:
1. Review the infrastructure and tell its story.
2. Stop wasting money/time. Actually run and monitor swarms. Do not always inherit `openai/gpt-5.6-luna`; consult the Hypercube's intelligence/quality, latency, throughput and price metadata for Chimera routing.
3. Fix broken tools. **Use native file-editing interfaces; do not use Python to make code changes.** Existing Python tests/read-only analysis were used, but repository changes in this session were made with `edit_file`/`write_file`.
4. Make worker cards dynamic: completed cards should minimize/roll up, preserve completed-work history, allow dismissal and reuse of display capacity. Arthur subsequently noticed scrolling exposes additional cards: four visible cards are a viewport, not a four-worker limit. This clarification does not establish completion of the lifecycle improvements.
5. Avoid minutes without visible progress. Use cheap models for occasional useful summaries, add progress indicators and actionable commands, research other tools' UX, and support drilling into exact schemas, arguments, outcomes and diffs even when omitted from the main transcript.
6. Explore performance improvements across harness and system.
7. Latest request: save this context for a fresh session and give the filename.

## Completed review artifact

`reports/infrastructure-story-20260906.md`

A substantial source-grounded architecture/product review with exact source references, integration gaps and training evidence. It was written BEFORE the later spawn-provider repair; its 'no source modified' statement describes the review phase, not the entire session.

Reviewed repositories:
- `/Users/arthurcolle/dsco/dsco-cli` — native C harness, observed HEAD `29390f9`.
- `/Users/arthurcolle/dsco/agents.erl` — Erlang/OTP, `c00e4e8`.
- `/Users/arthurcolle/dsco/dsco-router` — Python routing/training, `f31ab27`.
- `/Users/arthurcolle/dsco/dsco-graphsub-complex` — `d88ed75`; nested `dsco-graphsub` Rust repo `52ade08`.
- `/Users/arthurcolle/dsco/dsco-autobot/ToolManagement` — parent repo `f2788d5`.

### Important review findings

- Existing C adapters connect to Tool Management and dsco-router; GraphSub C client also exists. End-to-end integration is NOT established merely by adapters.
- Parent `unified-platform.sh` and `INTEGRATION_TEST.sh` describe an older composition (graphsub/agents.erl/macro/native-mcp-core/telemetry), including a stale graphsub checkout path. npm/Homebrew installer paths install dsco, not demonstrably the full requested package. No clean-machine five-component install was run.
- **GraphSub registration mismatch (NOT fixed):** C client `src/graphsub_client.c` sends `agent_id`; Rust `dsco-graphsub/src/api/agent_routes.rs` registration reads `id`, generating a UUID if missing. Heartbeat then expects the correct `agent_id`.
- **GraphSub adapter placeholders (NOT fixed):** its memory_sync/tool_result handlers return success while persistence remains TODO; route_task returns queued through placeholder logic. Do not confuse this adapter with the separate graph engine/storage implementation.
- **Erlang durability correction:** a worker incorrectly claimed ETS meant the timeline was memory-only. Coordinator verified `apps/agents/src/timeline_event_store.erl` restores records on init, appends NDJSON and calls `file:sync` before visibility, then replays on startup. Do NOT repeat the memory-only claim. Full agent checkpoint/distributed recovery remains unverified. Replay skips malformed records/some read failures. Some `intelligent_query_router.erl` learning functions are explicit no-ops.
- Chronicle records LLM/tool/trace/token-ledger data and training examples with consent fields. Capture defaults on in inspected source and may store request/response content. A consent field does not prove every exporter enforces consent.
- An older Chimera dataset labels `completion_success` from turn_done or successful HTTP completion. This is execution health, NOT independently verified customer task success.
- Training machinery is real: `dsco-cli/scripts/chimera_router/` has dataset, ensemble training, calibration, feedback and promotion evaluation. `dsco-router/scripts/train_chimera_qwen38.py`, `modal_chimera_train.py`, `gate_chimera_lm.py` provide LM/SFT/LoRA/GPU/promotion paths. No GPU training was launched or checkpoint promoted; competitive performance was not established.

## Hypercube and provider observations

Read implementation: `/Users/arthurcolle/dsco/dsco-router/src/dsco_router/routing/hypercube.py`.
Database: `/Users/arthurcolle/dsco/dsco-router/artifacts/hypercube/live/openrouter.duckdb`.

Snapshot metadata observed:
- fetched_at `2026-08-23T22:51:19.185700+00:00`, **stale** at review time.
- 422 models, 1,535 endpoints.
- rows_sha256 `b177bef2338ee878f5e4292054bb01eacc8fb5498cc091b875d3820052295404`.

`query_hypercube` supports objectives balanced/quality/cost/latency/throughput/reliability; task profiles coding/agentic/etc.; tools/structured-output/context/provider/uptime constraints; observed performance filtering; Pareto filtering and weighted ranking. Coding uses coding_index; agentic/research uses agentic_index. It reports stale metadata rather than automatically refreshing it.
`sync_openrouter_hypercube.py` requires a nonempty OPENROUTER_API_KEY for authenticated provider performance statistics. That environment variable was empty in this session. No refresh was performed. Old endpoint metadata was used only as historical evidence for bounded lane attempts; do not present it as live pricing/performance.

Native workers actually attempted:
- Cerebras / `gpt-oss-120b`: HTTP **402 payment_required**.
- Anthropic / `claude-sonnet-5`: HTTP **401**, OAuth access token revoked. Note `swarm_spawn_provider` deliberately lets Anthropic workers discover durable OAuth rather than exporting a possibly stale parent API key. Do not bypass that policy casually.
- Together / `openai/gpt-oss-120b`: HTTP **402 credit limit exceeded**.
- dsco-router / `deepseek/deepseek-v4-flash-0731`: HTTP **400**, 'the connection is lost'.

Earlier direct read-only /models requests to Cerebras and Together returned HTTP 403. Native inference failures above are more specific evidence. No payments, credential repairs or production changes were authorized/performed. Usage for failed requests was reported unknown (zero input/output observed); do not assert $0 actual billed cost.

## Swarm accounting / lessons

First review group: `infrastructure-story-review`, group 0, run id `swarm_1788720384_g0_create_infrastructure-story-review`.
Three native DSCO workers inherited `openai-codex` / `openai/gpt-5.6-luna` because the coordinator did not explicitly pin providers. This was poor routing, explicitly criticized by Arthur.
- Worker 0 GraphSub: stopped after ~492 seconds; no final brief.
- Worker 1 agents.erl: final brief after ~370 seconds; contained the incorrect ETS inference corrected above.
- Worker 2 Tool Management/router: stopped after ~492 seconds; no final brief.
- Total estimated input tokens ~792,734 across those three attempts.
- Reported per-worker estimated inference costs: ~0.0576972, 0.04596596, 0.08666012 USD, subscription-included; actual billed cost unknown. Do not extrapolate these to total session cost.
- Final collection reported complete, 0 active, 1 done, 2 killed.

Later provider workers IDs 3–6 all ended in the explicit errors above. Last process check found none of their PIDs remaining. **No workers intentionally left running.** IDs/group IDs are session-local; don't reuse them blindly after restart.

Tool interface lessons:
- `swarm action=kill` is NOT supported; that attempt returned 'unknown swarm action: kill'. Use **`agent {action:"kill", id:...}`**.
- `agent action=output` exposes errors/progress; `swarm action=collect` collects group results. `spawn_provider` returning running proves only a launched process, not successful model work.
- Tool discovery queries for 'hypercube', 'agent_send', and 'agent' returned unrelated remote matches. No Hypercube tool was located; coordinator inspected/called the local Python query implementation read-only. Do not invent a registered Hypercube tool.
- Bound worker prompts/read scope tightly. Broad reads produced enormous ANSI-colored tool output and excessive inherited context. Stop or narrow unproductive workers promptly.

## Actual source changes made in this session

### `src/tools.c` — small spawn_provider repair

In `tool_spawn_provider`, before calling `swarm_spawn_provider`:
- Parse effort, install a one-shot instance spec with `max_worker_turns`.
- Apply `swarm_set_next_budget_usd(budget)` BEFORE fork.
- Apply `swarm_set_next_max_tokens` from `max_tokens` BEFORE fork.

Previously the handler only assigned `c->budget_usd` after spawning. Workers visibly retained their $5 default despite requested $0.10/$0.20 budgets. The parent accounting cap existed, but the child agent-loop cap did not receive the requested value.

Also changed the response hint to the actual `agent action=status` / `action=output` interface; updated swarm schema descriptions for spawn_provider budget/turn limits and exposed max_tokens.

Do not sweep-rewrite tools.c. It already had thousands of unrelated changed lines, and concurrent changes shifted line numbers while working. Locate functions/text, not old line offsets.

### Added regression files

- `tests/test_spawn_provider_limits.c`
- `tests/test_spawn_provider_limits.sh`

The shell extracts the CURRENT real handler into a temporary header and compiles it with recording mocks. It verifies budget/turn/token/effort are installed before spawn, defaults do not leak, admission failures do not spawn, and the returned monitoring hint uses the real interface. This is not a paid inference test. Existing `tests/test.c` already covers actual provider-pinned fork/environment propagation (`test_swarm_provider_spawn_honors_bounded_instance_policy`).

Run: `sh tests/test_spawn_provider_limits.sh`.

Final hashes:
- C fixture: `ec4d05572f6f073f2bcc8ca9f537b8dded6f5146231467d30be9ce61482ea67a`
- shell runner: `5a2047bd984ac12f6da919f851b2ae2c7c7dbee264f572cf983395074fad6bb0`

No UI lifecycle changes, provider credential repairs, GraphSub fixes or global 'all tools fixed' result have been delivered. No commit was made.

## Verification completed

Review phase:
- Trace-KG export/store: 12 tests passed.
- Chimera dataset/feedback/fusion/planner/routing/model: 60 tests passed using existing Homebrew Python/NumPy.
- Router Chimera LM/gate/corpus/scaling: 12 tests passed using router .venv.
- Router Hypercube: 2 tests passed.
- Initial Chimera interpreter lacked NumPy; changed to an already-installed interpreter, did not install dependencies.

Repair phase:
- New spawn_provider limits regression passed.
- `make -j4 dsco test_runner` completed; actual binary strings included the new monitoring hint. Existing deprecation warning for posix_spawn_file_actions_addchdir_np observed.
- `make test-gate-claims`: **9 passed, 0 failed, 0 skipped**, against the live binary. No gate policy was changed.
- `make test_tui_swarm_dock`: lifecycle/layout/input/UTF-8/sanitization/concurrency tests passed.
- Existing `test_swarm_health.py`, `test_live_swarm_accounting.py`, `test_toolmgmt_parallel.py` passed.
- `git diff --check -- src/tools.c` passed.
- Full test_runner was BUILT but **not run**. Do not report the full test suite as passing.

## Current UI implementation / exploration

Relevant files:
- `src/tui_swarm_dock.c`, `include/tui_swarm_dock.h`, `tests/test_tui_swarm_dock.c`.
- `src/tui.c` composer input/render loop.
- `src/swarm.c` lifecycle/stream updates.
- `src/swarm_telemetry.c` health snapshots.
- `src/kitty_agent_windows.c` separate Kitty window integration.

At inspection, the three dock files were pre-existing UNTRACKED files. Do not mistake them for files authored by this session.

Observed behavior:
- Retained dock has 64 worker slots, 8 KiB text tail per worker.
- Layout generally shows 2x2 cards, with selection/paging across more workers.
- Ctrl+G focuses dock; Tab selects; z zooms; r arranges; Shift+arrows move; Ctrl+arrows resize; wheel scrolls selected output.
- All-success unfocused dock already compacts to six rows; failed/manual/focused/zoomed cards preserve expanded space.
- Existing progress notices are deterministic: start, 15-second active pulse, completion. Counts distinguish active/finished/failed; output-serial change is evidence of output, not proof of task advancement.
- `tui_swarm_dock_progress` is called from the composer in tui.c, not visibly from the busy parent model loop in the inspected references. Investigate why updates stop during long model/tool waits before adding duplicate timers.
- Retained rendering already emits changed rows rather than repainting unchanged frames.
- No dismiss key currently exists. Input mapping in tui.c recognizes only existing dock keys; a new action must be wired there as well.
- At capacity, worker_index currently shifts away the oldest retained worker without selecting terminal workers first. ID reuse resets output on terminal-to-active transitions or byte-count decrease. Resource reaping and card history are separate concerns.

## Performance baseline and proposed direction

Single warm samples, not distributions:
- `make bench-tool`: direct cwd tool ~0.01 s wall time; in-process perf markers ~0.04 ms total (not equivalent to end-to-end time).
- `--version`: ~0.00 s at timer resolution.
- `--models-json`: ~0.01 s.

Existing Makefile targets: bench-startup, bench-tool, bench-local, bench-agent-loop, bench-sota, bench-ttft, bench-worker, bench-size. Network benchmarks can incur cost; don't run them indiscriminately. bench-ttft has DSCO_RUN_NETWORK_BENCH opt-in.

Priority suggested to Arthur:
1. Compact worker task packets/progressive schema disclosure; avoid inheriting huge parent briefings and dumping entire megafiles.
2. Fresh Hypercube + functioning credential admission, task-specific quality/price/latency filtering. Missing/stale data remains unknown.
3. Event-driven progress independent of parent model turns. Deterministic UI first; cheap-model summaries only for meaningful new evidence, tightly bounded and deduplicated.
4. Bounded worker lifecycle, cancellation and safe reuse. Dismiss cards without deleting execution records.
5. Profile connection reuse, schema caching, serialization, subprocess startup and allocation before assuming low-level C is the bottleneck. Existing http_pool/json_fast modules mean reuse first, not duplicate implementation.
6. Train routing/recovery policies on independently verified outcomes; evaluate quality/cost/latency on held-out workloads.

Desired operator drill-down: task -> model/provider/version -> exact tool schema and arguments -> result/error -> artifacts/diffs -> verified outcome/cost. Preserve raw detail securely even when the primary UI shows a summary; don't leak secrets through telemetry.

## Unfinished work / resumption discipline

- 'Fix all broken tools' is NOT complete. Only the concrete spawn-provider limit propagation/hint/schema repair above is implemented and verified. Continue with a bounded failure inventory rather than claiming all thousands of tools work or invoking mutating remote tools indiscriminately.
- UI dismissal/history/progress enhancements remain requests, not delivered changes.
- UX research/data-mining other tools has NOT been performed.
- Hypercube remains stale; provider lanes above have concrete external failures. Don't repeatedly retry them unchanged or silently route everything back to Luna.
- Full install/integration, GraphSub mismatches/placeholders and model competitiveness remain unverified/unfixed.
- No append-only production receipt was published to the durable bus in this session. The review mentioned a companion hash receipt, but none was produced; the hashes here cover only the new regression files. Do not claim that missing artifact exists.
- Preserve native DSCO delegation, explicit provider pins, bounded budgets and real progress checks. Never use Codex/Claude Code external executors for workers.
- Before further changes, load current repository instructions and inspect current disk state. Use native `read_file`, `edit_file`, `write_file`; shell for builds/tests/read-only profiling. Keep source changes minimal and verification proportional.

## Follow-up — dock lifecycle verified (2026-09-06)

Completed mixed-run success roll-up, H history/live toggle, X terminal-only dismissal, retained output, active-preserving capacity reclamation, and native killed-status handling. Focused C, sanitizer, real-composer PTY, snapshot and build checks passed. Session-only patch, exact baseline backups and scratch rollback verification: `reports/dock-lifecycle-20260906/`. See `RESULT.md` there for evidence and limitations. Native worker progress freshness remains unresolved: the composer repaints but does not own synchronized pipe/status polling. One native Luna audit worker completed and was collected; billed/subscription economics remain unknown. Durable receipt readback is in `bus-readback.json`. No commits or governance changes.

## Follow-up — exact-work swarm scaling verified

Implemented opt-in `swarm action=scale`: pure exact descriptors share one native worker/result reference. Live 64 logical tasks -> one successful collected worker; 4096 logical/64-unique admission family verified without claiming billed-cost savings for arbitrary unique work. Seven-file patch, scratch rollback, review resolution, sanitizer/MCP/gate evidence and durable receipt: `reports/swarm-scale-20260906/RESULT.md`. Prior 16-lane receipt is now closed as historical evidence in `reports/progress-16-20260906/receipt-historical.json`, not as certification of subsequently changed source. The active operator was not hot-patched.
