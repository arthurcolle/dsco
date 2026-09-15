# 2416 — Provider failure classifier: Learned asynchronous recursion

Implement the learned asynchronous recursion feature for the Provider failure classifier RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Classify inference failures precisely enough to choose retry, credential repair, alternate routing, or abstention without duplicate effects.

Store redacted status codes, headers, stream milestones, provider identity, and request deadlines as external REPL objects. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare transport and protocol evidence, distinguish accepted requests from pre-dispatch failures, and propose bounded remedies. Role output: Produce a provider-triage root adapter, typed failure diagnosis, permitted next action, and request-level outcome evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose bounded futures for recursive calls and train submit, await, batch, and cancel decisions on controlled completion distributions. Reservations span all descendants. Keep completed result identities stable and ensure training and inference scheduling never block the terminal input handler.

Train role roots on controlled disconnects, quota resets, authentication failures, malformed streams, and incompatible-model responses. A local provider fixture independently labels causes and counts actual retry attempts and duplicate downstream actions. Hold out provider families, error phrasings, and combinations of transport and application failures. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A stream interrupted after a committed tool call must not be classified safe for complete replay. Delay a required child indefinitely while delivering other results out of order. The system must remain responsive, preserve partial work, and report unresolved completeness; dropping the slow child cannot manufacture a successful fast result.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider.c`, `src/provider_pool.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` (`_sanitize_child_error`). Donor baseline: Sanitizes child exception details before exposing errors to parent results. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
