# 2952 — User correction prioritizer: Verified long-output assembly

Implement the verified long-output assembly feature for the User correction prioritizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Convert user corrections into precise objective changes while retaining authorized work that the correction does not supersede.

Keep task hierarchy, original requirements, chronological corrections, current outputs, and authority boundaries in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively identify affected obligations, update priorities, and invalidate only decisions contradicted by the new instruction. Role output: Produce a correction-prioritization root adapter, revised obligation graph, preserved commitments, and an explicit scope-change explanation. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to assemble semantically required role outputs exceeding one generation, using an expected coverage manifest and externally checked fragments. Return the typed result handle and digest with streaming backpressure. Padding cannot satisfy output length; validate the whole artifact against domain obligations after assembly.

Train roots on narrow clarifications, cancellations, incompatible objective changes, and messages containing mere status questions. An independently authored requirement-state oracle checks intended scope changes and preservation of unaffected commitments. Hold out paraphrase families, correction orderings, and ambiguous references requiring abstention. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Asking for a progress update must not cancel ongoing work or authorize unrelated deployment. A correct prefix followed by silent truncation must fail completeness. Reordering asynchronous fragments or repeating one fragment must be caught before final publication, without copying the whole output into the root prompt.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/agent.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._apply_steering`). Donor baseline: Applies queued cancellation or reprioritization to eligible goals and records resulting state changes. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
