# 2971 — Input latency investigator: Executable curriculum compiler

Implement the executable curriculum compiler feature for the Input latency investigator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Find why terminal input stalls during tool discovery, training, or recursive execution without confusing rendering delay with admission delay.

Expose PTY timestamps, event-loop spans, lock waits, network stalls, and input-buffer states in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively isolate latency intervals, compare controlled interventions, and propose the smallest change removing the blocking dependency. Role output: Produce an input-latency root adapter, causal blocking diagnosis, focused patch candidate, and measured PTY regression evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Compile a task grammar with explicit difficulty axes, valid transformations, and known failure labels. Generate positive and negative episodes, retaining their originating family identifiers. Train the role's root policy on a balanced development curriculum and expose a reproducible curriculum-build command.

Train roots on stalled catalogs, mutex contention, rendering bursts, background training, and healthy slow-output cases. An independent PTY harness measures keystroke admission, command response, and preservation of partially typed input. Hold out stall durations, terminal sizes, and concurrent background-task combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A cosmetic spinner that appears responsive while input remains blocked must fail the actual latency check. Reject a generator that changes the answer without updating its reference or produces duplicate families under different seeds. Measure valid-case coverage and trained-root success separately.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/agent.c`, `src/tui.c`, `src/mcp.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/streaming.py` (`stream_swarm`). Donor baseline: Exposes asynchronous swarm status, token, and prediction events to callers. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
