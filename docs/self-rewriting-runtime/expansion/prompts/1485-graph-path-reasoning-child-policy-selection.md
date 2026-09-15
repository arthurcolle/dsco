# 1485 — Evidence graph path reasoner: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Evidence graph path reasoner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a graph RLM to construct valid explanatory paths while respecting edge type, direction, and scope.

External EVIDENCE_GRAPH and PATH_QUERIES contain typed nodes, revisioned edges, missing links, and permitted traversal rules. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions graph searches, recursively delegates frontier exploration, and buffers verified paths rather than textual associations. Role output: Return CHECKED_PATHS with exact edge identities, traversal contracts, missing links, and path validity. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Training uses generated typed graphs, real local execution relations, cycles, distractor hubs, and disconnected observations. An independent graph engine checks reachability and edge constraints; semantic edge assertions require separately validated provenance. Hold out graph topology, path length, and relation compositions. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A temporal-before edge must not be traversed as a causal-support edge to manufacture an explanation. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/trace_kg_store.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
