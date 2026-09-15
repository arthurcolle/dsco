# 63 — Train an RLM for reversible debugging of pure native cells

Train a reversible-debugging specialist RLM that investigates a resident code cell by stepping backward through its logical execution. For a bounded pure instruction subset, record value changes or periodic snapshots plus deterministic forward replay. Debugging operates on owned inputs and state; external effects are observation boundaries, never something an inverse instruction pretends to undo. Preserve the code generation and source mapping for every frame.

Keep instruction traces, snapshots, and comparison outputs in external REPL variables. The root receives sizes and error metadata, issues code to jump to a checkpoint or inspect selected values, and delegates branch-local analysis to child RLMs. Retain child diagnoses and replay states across iterations. Its final variable contains a reproducible sequence locating the earliest wrong state, rather than an unverified narrative.

Train the root model or adapter on injected arithmetic, branch, and indexing defects with known first-divergence locations. Supervise useful checkpoint and slice actions; independently reward correct localization with the fewest executed replay steps and inspected values. Hold out different control-flow patterns and trace lengths. Run an actual bounded update, then compare the trained checkpoint against its unchanged parent on hidden bugs.

Implement a minimal pure-cell interpreter and native/reference comparison if needed. Falsifying test: rewind before a branch, replay the same generation and input, and require identical logical state hashes and output. Switch the active resident generation while a debug frame is pinned; the frame must still inspect its original code. An attempted rewind past a governed file write must report an effect boundary. Wrong-generation values, invented inverse effects, or localization based only on line-name guessing fail. Show native bug reproduction separately from interpreter-only debugging mechanics.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/plugin.c`, `src/event_stream.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/hooks.py` symbols `TelemetryHook.handle`, `TelemetryHook.get_metrics`: The donor collects operation telemetry but provides no instruction-state rewind or trained reversible-debugging policy. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
