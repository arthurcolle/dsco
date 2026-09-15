# 3119 — Interoperability conformance examiner: Capability-constrained program learning

Implement the capability-constrained program learning feature for the Interoperability conformance examiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Check that role runtimes honor negotiated protocol contracts across clients without mistaking permissive parsing for compatibility.

Expose protocol versions, message traces, capability negotiations, transport framing, and expected state transitions in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively enumerate boundary cases, generate protocol probes, and compare actual behavior against explicit conformance rules. Role output: Produce an interoperability root adapter, executable conformance suite, minimal failing traces, and supported-contract matrix. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to generate useful programs within a declared host-service contract, using structured action schemas and explicit capability denials as observations. Constraints shape valid proposals, but the host still authorizes every effect independently. Evaluate task completion under different legitimate grant sets.

Train roots on valid exchanges, malformed envelopes, reordered notifications, cancellation races, and unsupported-version responses. An independent state-machine harness scores framing, identity, lifecycle, and effect semantics against controlled clients. Hold out client implementations, transport fragmentation patterns, and version-transition combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A successful initialization followed by tool calls using another session's identity must fail conformance. A recursive child requesting broader authority cannot obtain it from generated code or a rewritten schema. The root must select an allowed alternative or report inability, and a denied action cannot be recorded as completed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/agent_interop.c`, `src/mcp_server.c`, `src/mcp_response.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`ToolRouterClient._unwrap_tools`). Donor baseline: Normalizes tool lists from supported response-envelope shapes. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
