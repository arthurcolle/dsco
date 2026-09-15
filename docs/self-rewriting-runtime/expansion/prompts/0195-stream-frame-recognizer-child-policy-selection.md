# 195 — Incremental stream-frame recognizer: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Incremental stream-frame recognizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn executable stream recognizers that distinguish complete messages, partial frames, terminal markers, and malformed framing without guessing.

External variables `wire_chunks`, `frame_contract`, and `frame_states` preserve complete segmented protocol transcripts and parser checkpoints. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs test delimiter hypotheses, recurse over ambiguous boundary windows, and assemble a bounded framing automaton. Role output: Final `FrameRecognizer` handle names executable states, buffering bounds, completion predicates, and minimal framing counterexamples. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Train on generated fragmentation/coalescing episodes with action traces and labels from a separately fixed frame recognizer. A deterministic reference automaton checks emitted frame bytes, exact order, terminal status, and unconsumed trailing data. Hold out delimiter grammars, escape patterns, chunk lengths, and close-before-completion cases across entire transcript families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A terminal-looking marker inside escaped payload text prematurely closes the stream and discards a later valid message. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider.c`, `src/mcp.c`, `src/event_stream.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_openrouter_streaming.py` (`_safe_stream`). Donor baseline: Consumes DSPy stream chunks and final predictions; protocol frame recognition is not implemented there. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
