# 74 — Train an RLM for generation-aware native source debugging

Train a native-debugging specialist RLM that can inspect the generated procedure actually running, set bounded source-level stop points, and identify a repairable defect. Preserve source/IR locations, native address ranges, inline frames, and variable-location descriptions for each executable generation. Prefer compiler metadata or explicit emitter maps; the current source-summary AST cannot by itself recover optimized machine state.

Keep debug maps, source, stack snapshots, and observed values in external REPL variables. The root receives only stop-reason and generation metadata, queries selected locations through code, and calls child RLMs on relevant frames or source slices. Persist inspected state and competing diagnoses. The final environment handle contains a replayable debugging action sequence, implicated code span, and validated defect evidence.

Train the root adapter on seeded native defects with known origins, supervising useful breakpoint placement and variable inspections. Independent reward requires correct localized fault and a reproducing input, penalizing intrusive stops and unnecessary memory inspection. Hold out source layouts, inlining shapes, and defect families. Run a bounded real parameter update, then compare the trained policy against the frozen parent under the same debugging budget.

Provide an owned test-process debugging surface if missing; do not attach to unrelated applications. Falsifying test: suspend a generation-A frame, publish B with different line mappings, and inspect A. Its source and locals must remain A-specific while new calls use B. An optimized-out value must be marked unavailable, never guessed. Prove heap/session continuity and no resident exec across the replacement. A correct-looking source line from the wrong generation fails even if the final explanation sounds plausible; verify actual machine-address ownership.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/ast.c`, `src/plugin.c`, `src/native_trace.c`, `src/vm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer._extract_structure`, `MetaToolRegistry.get_source`: The donor stores source and line-oriented structure but has no live native breakpoint mapping or trained debugging policy. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
