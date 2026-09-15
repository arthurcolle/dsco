# 2102 — Cross-repository contract discoverer: Teacher rejection distillation

Implement the teacher rejection distillation feature for the Cross-repository contract discoverer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a contract-discovery RLM to locate and check semantic boundaries between services maintained in separate repositories.

External REPOSITORY_SNAPSHOTS and CALL_RECEIPTS retain API schemas, client payloads, server handlers, versions, and integration tests. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs producer-consumer joins, recursively delegates repository-local inspection, and buffers boundary contracts with executable mismatch witnesses. Role output: Return CROSS_REPO_CONTRACTS with producer-consumer bindings, version constraints, mismatch tests, and unresolved assumptions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect multiple executable teacher trajectories per development task. Replay their programs against the fixed oracle, reject incorrect or incomplete episodes, and deduplicate shared derivations before extracting root action targets. Distill accepted trajectories into the role policy with balanced family sampling.

Training includes paired local services, renamed fields, nesting mismatches, status semantics, version skew, and incomplete documentation. Owned client-server fixtures and independent schema checks verify controlled contracts; inferred real-service behavior remains explicitly unverified. Hold out service families, schema evolution patterns, and repository layouts. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A client emitting flat tool_id must not satisfy a handler expecting request.tool_id merely because both names appear. A fluent teacher episode containing the expected answer but an incorrect executable result must be excluded. Compare the trained root against its initial checkpoint on untouched families.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/mcp.c`, `src/tools.c`, `src/context_fabric.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/integration_hooks.py` (`HookEvent`, `HookManager.emit`). Donor baseline: Ordered hook events carry request IDs but do not establish cross-service semantic evidence contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
