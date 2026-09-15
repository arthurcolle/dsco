# 1772 — Protocol state inferencer: Teacher rejection distillation

Implement the teacher rejection distillation feature for the Protocol state inferencer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a protocol RLM to infer hidden interaction states through bounded probes and distinguishing response sequences.

External OBSERVATION_TABLE and PROTOCOL_ORACLE retain reset receipts, action histories, outputs, counterexamples, and nondeterminism markers. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs membership queries, recursively delegates distinguishing suffixes, and buffers refined transition models with uncertainty. Role output: Return PROTOCOL_MODEL with executable transitions, query receipts, unsupported states, and reset assumptions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect multiple executable teacher trajectories per development task. Replay their programs against the fixed oracle, reject incorrect or incomplete episodes, and deduplicate shared derivations before extracting root action targets. Distill accepted trajectories into the role policy with balanced family sampling.

Training uses owned resettable protocol fixtures containing authentication, token consumption, retries, cancellation, and delayed failures. A separate conformance oracle checks withheld action sequences against the controlled service; uncertain resets block deterministic claims. Hold out protocol topologies, alphabets, and delayed-error structures. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Identical retry responses must remain distinct when one consumes a token needed for a later commit. A fluent teacher episode containing the expected answer but an incorrect executable result must be excluded. Compare the trained root against its initial checkpoint on untouched families.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/mcp.c`, `src/pty_session.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`InductiveReasoningSignature`). Donor baseline: Induction requests generalizations and possible counterexamples without constructing an executable learned model. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
