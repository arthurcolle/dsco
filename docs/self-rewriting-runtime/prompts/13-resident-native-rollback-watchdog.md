# 13 — Resident rollback with a bounded behavioral watchdog

Let a resident DSCO process automatically withdraw a newly activated native implementation when independently measured behavior regresses. Define a probation policy over accepted outputs, invariant failures, and bounded latency measurements, tied to a candidate generation and a retained champion. The watchdog samples evidence produced outside the candidate's self-reporting path and atomically redirects future calls to the compatible champion when a declared condition is met.

Scope the first version to admitted pure or cooperatively bounded callbacks whose state and lifetime contracts are explicit. Do not claim that a watchdog can safely recover arbitrary in-process memory corruption, SIGSEGV, or an uninterruptible native loop. Pre-qualify such faults in a child process; resident rollback addresses observed wrong results, contract violations, or cooperative deadline breaches. Document this boundary plainly.

Retain champion code and compatible state until probation ends, and ensure rollback never frees an in-flight candidate frame. Distinguish dispatch rollback from reversal of external effects: completed writes cannot be undone by changing a function pointer. Include hysteresis or a declared observation count to avoid oscillation, and require a fresh qualified generation before the withdrawn candidate can return. Expose the exact evidence and transition responsible for rollback.

Falsifying test: activate a native scorer that passes ordinary inputs but returns a wrong result for a hidden boundary case. Feed that case during probation and require the next invocation to use the retained champion in the same PID. Pause an earlier candidate call and verify its image survives until return. Inject a noisy slow sample below the policy threshold and ensure it does not trigger rollback. Any unsupported crash recovery claim, repeated effects, premature unload, or silent state mismatch fails.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `src/event_loop.c`, `src/supervisor.c`, `src/execution_kernel.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/scaling.py` symbols `ScaledSwarm._run_replica`, `ScaledSwarm._select_best`. Observed donor behavior: The donor contains failed replicas and chooses a survivor; it does not roll back resident executable generations.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
