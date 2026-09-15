# 2305 — API version migrator: Checkpoint recovery policy training

Implement the checkpoint recovery policy training feature for the API version migrator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Migrate integration calls across explicit API contracts while identifying semantic changes that require caller decisions.

Load old and new schemas, recorded requests, responses, and compatibility requirements into external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively map endpoints and fields, generate translation candidates, and test changed pagination, error, and idempotency behavior. Role output: Produce an API-migration root adapter, translation patch, compatibility matrix, and explicit unsupported-operation report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create interrupted role episodes with committed buffers, pending child calls, and explicit effect receipts. Train the root to reconstruct logical state, reconcile pending work, and continue from a compatible model/environment tuple. Treat unavailable state as a recoverable diagnostic rather than invented memory.

Use paired API fixtures, migration patches, replayed requests, and known silent semantic incompatibilities as role data. A reference compatibility harness compares actual observable effects and responses under both versions. Hold out version transitions, service families, and combinations of backward-incompatible changes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Renaming an identifier field while dropping its account scope must fail despite successful HTTP responses. Resume after an effect completed but before its acknowledgement was recorded. Reconcile the receipt instead of repeating the effect; incompatible state schemas or changed model identities must prevent a misleading continuation claim.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/mcp.c`, `src/toolmgmt.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`anthropic_tool_schema_from_router`). Donor baseline: Converts router tool definitions into Anthropic-compatible tool schema records. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
