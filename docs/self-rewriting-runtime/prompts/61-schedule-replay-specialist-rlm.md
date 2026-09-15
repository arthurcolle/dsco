# 61 — Train an RLM to reproduce decisive native schedules

Train a schedule-replay specialist RLM that makes concurrency failures in self-rewriting native cells reproducible. Instrument a bounded cooperative execution domain at cell acquisition, safepoint, publication, and release. Record thread identities, synchronization dependencies, input hashes, and generation decisions. Replay selects recorded runnable participants rather than reproducing wall-clock delays. Explicitly exclude arbitrary unsynchronized machine instructions from the initial determinism claim.

Keep the full schedule in an external REPL variable `trace`; the root receives only its schema, length, and bounded summary. Root-generated code selects intervals and invokes child RLMs over relevant slices, retaining their results in environment variables. Bound recursion depth, total subcalls, and trace materialization. The final answer references an environment variable containing the replay program and reproduced failure evidence, not a pasted transcript.

Create training episodes from controlled races between generation acquisition, activation, and reclamation. Train the role's root checkpoint or adapter to choose informative cuts, request useful child analyses, and construct replay schedules. Reward only independently reproduced terminal state and causal-event order, penalizing unnecessary inspected bytes and child calls. Hold out entire race families and thread-count configurations; compare the updated parameters with the frozen parent at matched resource limits.

Build a minimal scheduler/replay interface if absent. Falsifying test: an old-generation call racing publication must reproduce the original failure with deliberately changed sleeps; removing a necessary ordering edge must make replay fail its outcome oracle. Corrupt a participant or generation reference and require rejection before replay. Demonstrate one bounded real training update and rerun hidden schedules through the resulting checkpoint. Record replay coverage boundaries; matching log text without matching controlled execution does not count.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/event_loop.c`, `src/event_stream.c`, `src/vm.c`, `src/swarm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_structured`: The donor dispatches dependency-ready waves but does not record thread interleavings or train a replay specialist. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
