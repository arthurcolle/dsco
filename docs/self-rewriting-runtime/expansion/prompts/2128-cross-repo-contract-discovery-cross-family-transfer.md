# 2128 — Cross-repository contract discoverer: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Cross-repository contract discoverer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a contract-discovery RLM to locate and check semantic boundaries between services maintained in separate repositories.

External REPOSITORY_SNAPSHOTS and CALL_RECEIPTS retain API schemas, client payloads, server handlers, versions, and integration tests. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs producer-consumer joins, recursively delegates repository-local inspection, and buffers boundary contracts with executable mismatch witnesses. Role output: Return CROSS_REPO_CONTRACTS with producer-consumer bindings, version constraints, mismatch tests, and unresolved assumptions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training includes paired local services, renamed fields, nesting mismatches, status semantics, version skew, and incomplete documentation. Owned client-server fixtures and independent schema checks verify controlled contracts; inferred real-service behavior remains explicitly unverified. Hold out service families, schema evolution patterns, and repository layouts. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A client emitting flat tool_id must not satisfy a handler expecting request.tool_id merely because both names appear. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/mcp.c`, `src/tools.c`, `src/context_fabric.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/integration_hooks.py` (`HookEvent`, `HookManager.emit`). Donor baseline: Ordered hook events carry request IDs but do not establish cross-service semantic evidence contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
