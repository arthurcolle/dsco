# 112 — Measure role RLM length generalization

Show whether a trained role can apply its recursive procedure to substantially longer problems, not just memorize the training layout. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/core.py`'s `format_history()`, which truncates conversation text. Build a length-generalization experiment for the external-context contract described in the [RLM paper](https://arxiv.org/html/2512.24601v3), with independently varied information density and semantic work.

Create generated role task families whose required work is constant, linear, and pairwise in input size. Separate distractor length from number of relevant records, and vary record order, identifier distribution, and location of required evidence. Train a root adapter only on declared short-context distributions. Freeze the resulting checkpoint and evaluate progressively longer contexts with new seeds and changed layouts, including lengths above its neural window. Return final artifacts from environment variables so output length does not secretly cap the benchmark. Track failure due to correctness, budget, protocol, and memory separately.

Implement a benchmark adapter near `src/agent_interop.c` and `src/agent.c`, returning candidate-role results to `src/self_improve.c`. If no role RLM exists, include a minimal trainable open-model harness and deterministic task generator so the experiment is standalone. The trained output must contain actual root weight or adapter changes verified against the initial checkpoint. Keep leaf policy, task oracle, and tokenizer fixed across lengths; report resource scaling alongside accuracy.

Acceptance runs the real binary with the frozen trained artifact and baseline on held-out task families and untouched lengths. An independent generator computes the exact selected records, aggregate, or pair set; model grading cannot establish correctness. Test fixed-position memorization by shuffling relevant records, test keyword shortcuts with semantic distractors, and reject duplicate task identities crossing the train/eval boundary. Demonstrate full external input access without copying all text into root history. Produce accuracy-versus-length and cost-versus-work tables with seeds and confidence intervals. A small supported device run may establish mechanics, but label its scale honestly and never extrapolate the paper's gains to DSCO.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Verify changed trainable tensors against the initial artifact.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.
