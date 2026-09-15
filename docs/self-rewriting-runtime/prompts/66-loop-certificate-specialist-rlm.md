# 66 — Train an RLM to construct checkable loop resource certificates

Train a termination-certificate specialist RLM that enables useful loops in self-generated executable cells without accepting unbounded computation on trust. Define a bounded IR fragment with explicit integer widths, loop invariants, ranking expressions, allocation operations, and input-domain predicates. Candidates carry certificates for maximum iterations, instruction count, and owned allocation volume. A small independent verifier checks these claims against the exact IR; runtime fuel remains a defense against verifier mistakes.

Store the candidate graph, obligations, and checker diagnostics in external REPL variables. The root receives only loop summaries and can inspect blocks through code or invoke child RLMs over separate loops. It retains invariant proposals and counterexamples between calls. The final environment variable contains the certificate and verified scope, including unsupported constructs, rather than a textual promise of termination.

Train a root adapter on proof-construction trajectories for nested counters, bounded traversals, and controlled recursion. Reward independently accepted sound certificates and useful admissible domains; penalize false bounds and excess checker calls. Hold out loop templates and integer widths. Perform an actual bounded update and compare accepted valid programs and rejected invalid ones against the frozen parent under equal inference limits.

Provide a minimal certificate language if absent. Falsifying test: use an unsigned loop whose apparent decrement wraps before the exit condition. The verifier must reject the incorrect finite bound or return a concrete violating execution. Test a valid nested loop and require observed steps and allocations never exceed its checked certificate across boundary inputs. Alter one instruction after certification and require a hash mismatch before admission. A learned confidence score, dimensional estimate, or test-only absence of hangs cannot replace the certificate.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `include/vm.h`, `src/prompt_program.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` symbols `WorkflowToolkit.validate_workflow`: The donor checks basic workflow structure but does not prove termination or train certificate synthesis. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
