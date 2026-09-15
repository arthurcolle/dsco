# 1790 — Protocol state inferencer: Invariant-conditioned program generation

Implement the invariant-conditioned program generation feature for the Protocol state inferencer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a protocol RLM to infer hidden interaction states through bounded probes and distinguishing response sequences.

External OBSERVATION_TABLE and PROTOCOL_ORACLE retain reset receipts, action histories, outputs, counterexamples, and nondeterminism markers. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs membership queries, recursively delegates distinguishing suffixes, and buffers refined transition models with uncertainty. Role output: Return PROTOCOL_MODEL with executable transitions, query receipts, unsupported states, and reset assumptions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Attach explicit executable invariants to development tasks and train the root to carry them through decomposition, child-result consumption, and final assembly. Check invariants outside the policy, distinguishing proven bounded conditions from sampled checks and unresolved assumptions.

Training uses owned resettable protocol fixtures containing authentication, token consumption, retries, cancellation, and delayed failures. A separate conformance oracle checks withheld action sequences against the controlled service; uncertain resets block deterministic claims. Hold out protocol topologies, alphabets, and delayed-error structures. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Identical retry responses must remain distinct when one consumes a token needed for a later commit. A candidate that satisfies visible examples while violating a required invariant on a boundary case must fail. Removing the check or replacing it with the root's assurance must not change the verdict.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/mcp.c`, `src/pty_session.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`InductiveReasoningSignature`). Donor baseline: Induction requests generalizations and possible counterexamples without constructing an executable learned model. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
