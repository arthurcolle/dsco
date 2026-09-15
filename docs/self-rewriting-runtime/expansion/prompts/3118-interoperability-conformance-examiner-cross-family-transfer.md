# 3118 — Interoperability conformance examiner: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Interoperability conformance examiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Check that role runtimes honor negotiated protocol contracts across clients without mistaking permissive parsing for compatibility.

Expose protocol versions, message traces, capability negotiations, transport framing, and expected state transitions in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively enumerate boundary cases, generate protocol probes, and compare actual behavior against explicit conformance rules. Role output: Produce an interoperability root adapter, executable conformance suite, minimal failing traces, and supported-contract matrix. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train roots on valid exchanges, malformed envelopes, reordered notifications, cancellation races, and unsupported-version responses. An independent state-machine harness scores framing, identity, lifecycle, and effect semantics against controlled clients. Hold out client implementations, transport fragmentation patterns, and version-transition combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A successful initialization followed by tool calls using another session's identity must fail conformance. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/agent_interop.c`, `src/mcp_server.c`, `src/mcp_response.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`ToolRouterClient._unwrap_tools`). Donor baseline: Normalizes tool lists from supported response-envelope shapes. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
