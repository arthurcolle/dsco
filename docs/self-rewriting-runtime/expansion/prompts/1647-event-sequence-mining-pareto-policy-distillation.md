# 1647 — Event sequence miner: Constrained Pareto policy distillation

Implement the constrained pareto policy distillation feature for the Event sequence miner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a sequence RLM to discover recurring runtime behavior patterns with explicit ordering and episode boundaries.

External EVENT_STREAMS and SESSION_BOUNDARIES retain causal IDs, timestamps, dropped-event markers, revisions, and concurrency metadata. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs sequence segmentation, recursively compares recurring motifs, and buffers patterns with supporting and contradicting episodes. Role output: Return SEQUENCE_PATTERNS with ordering constraints, episode provenance, support counts, and known exceptions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect independently verified root trajectories with quality, latency, memory, and cost measurements. Construct a constrained frontier and distill policies conditioned on declared operating budgets. Keep correctness and capability rules as hard requirements rather than exchangeable score terms.

Training contains generated interleavings, repeated local execution traces, missing events, retries, and renamed action labels. A separate sequence matcher checks pattern support; controlled partial-order traces supply known admissible ordering relationships. Hold out interleaving structures, motif lengths, and protocol families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Interleaved events from two requests must not become a fictional single-request failure sequence. A fast invalid trajectory must never enter the feasible frontier. Tightening a resource budget should produce a valid lower-cost policy or explicit infeasibility, not an unreported reduction in required task coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/event_stream.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
