# 2984 — Input latency investigator: Contradiction-guided program revision

Implement the contradiction-guided program revision feature for the Input latency investigator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Find why terminal input stalls during tool discovery, training, or recursive execution without confusing rendering delay with admission delay.

Expose PTY timestamps, event-loop spans, lock waits, network stalls, and input-buffer states in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively isolate latency intervals, compare controlled interventions, and propose the smallest change removing the blocking dependency. Role output: Produce an input-latency root adapter, causal blocking diagnosis, focused patch candidate, and measured PTY regression evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Generate episodes where two independently sourced intermediate results conflict. Train the root to localize the conflicting assumptions, recursively investigate discriminating evidence, and revise dependent program steps. Retain both original claims and the evidence that resolved or preserved the disagreement.

Train roots on stalled catalogs, mutex contention, rendering bursts, background training, and healthy slow-output cases. An independent PTY harness measures keystroke admission, command response, and preservation of partially typed input. Hold out stall durations, terminal sizes, and concurrent background-task combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A cosmetic spinner that appears responsive while input remains blocked must fail the actual latency check. Several copied supporting reports must not overwhelm one valid counterexample. If the conflict remains unresolved, the output must preserve that state rather than select whichever child answered last.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/agent.c`, `src/tui.c`, `src/mcp.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/streaming.py` (`stream_swarm`). Donor baseline: Exposes asynchronous swarm status, token, and prediction events to callers. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
