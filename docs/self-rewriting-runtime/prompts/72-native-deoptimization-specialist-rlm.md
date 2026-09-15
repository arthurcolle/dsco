# 72 — Train an RLM to reconstruct deoptimization state

Train a deoptimization specialist RLM that lets an optimized native cell fall back into its baseline IR at a declared safepoint while preserving computation. Define maps from native registers, stack slots, and constant-folded values to logical locals, continuation labels, and owned-object handles. Recover eliminated values using bounded pure reconstruction expressions. Unsupported safepoints remain pinned to native completion rather than pretending arbitrary machine stacks are resumable.

Store native code metadata, baseline IR, safepoint maps, and differential traces in REPL variables. The root receives map sizes and failing-point identifiers, queries relevant live-value slices through code, and invokes child RLMs for independent reconstruction obligations. Keep proposed maps and verifier diagnostics across calls. The final environment handle names the checked mapping artifact and its coverage.

Train the root checkpoint or adapter on optimization/deoptimization trajectories, rewarding independently correct resumed suffix outputs and minimal reconstruction overhead. Include wrong register assignments and stale schema assumptions as negative examples. Hold out optimization combinations and control-flow shapes; perform an actual bounded update and compare against the frozen parent at equal qualification budgets.

Provide a small native-cell and baseline interpreter interface if absent. Falsifying test: force deoptimization at every supported safepoint of a qualified native scorer, including after a branch and after a recorded observation. The resumed result must equal uninterrupted baseline execution, with no repeated effect. Corrupt one live-value location and require map rejection before activation. Demonstrate fallback within the resident address space using heap-sentinel/session continuity and audited no exec. A VM-to-VM version switch alone does not satisfy native state reconstruction; retain machine-location evidence for actual native frames.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `include/vm.h`, `src/plugin.c`, `src/lingo_workflow.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_sequential`: The donor forwards prior results between calls, but cannot reconstruct a suspended native frame into an interpreter continuation. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
