# 78 — Train an RLM to optimize native precision under error contracts

Train a numerical-precision specialist RLM that improves native scoring or reduction kernels without silently changing their accuracy contract. Search bounded choices such as pairwise versus compensated summation, accumulator width, vector reduction trees, and guarded stable fallbacks. Define allowed absolute/relative error, exceptional-value behavior, reproducibility requirements, and input range before optimization. Avoid claiming floating-point reassociation is exact integer-style equivalence.

Keep kernel source, input distributions, high-precision reference results, and candidate error profiles in external REPL variables. The root receives compact statistics, writes code selecting difficult numeric slices, and delegates independent stability analyses to child RLMs. Retain error witnesses and candidate artifacts. Its final environment handle names a qualified native kernel and the exact error domain where it may run.

Train the root checkpoint or adapter on algorithm-choice trajectories with external high-precision or exact-reference rewards. Accuracy and exceptional-value constraints are hard gates; only surviving candidates receive speed or memory rewards. Hold out cancellation patterns, dynamic ranges, and distribution shifts. Execute a bounded real training update and compare the trained policy against its frozen parent plus fixed stable and fast baselines.

Provide a minimal evaluator and native loader if absent. Falsifying test: give a candidate ordinary positive inputs where it looks excellent, then hidden alternating-magnitude inputs exposing catastrophic cancellation. It must fail qualification or take its verified stable fallback. Test NaNs, infinities, signed zero, and empty reductions according to the declared contract. Activate a surviving native implementation with heap/session continuity and audited absence of resident exec. Report full-call performance and worst observed error separately; averaged error cannot excuse a violated per-case limit.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/math_fastpath.c`, `src/vm.c`, `src/cost_frontier.c`, `include/simd.h`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` symbols `MathToolkit`: The donor supplies mathematical operations but does not learn native precision or reduction order under independently checked numerical-error limits. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
