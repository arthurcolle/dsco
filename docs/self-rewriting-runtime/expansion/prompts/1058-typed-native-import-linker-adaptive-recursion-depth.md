# 1058 — Typed native import linker: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Typed native import linker RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native linking plans that resolve only permitted typed host services and reject incompatible or undeclared imports.

REPL variables `candidate_imports`, `host_exports`, and `abi_descriptors` retain complete signatures, capabilities, and symbol versions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code groups import obligations, delegates signature matching, and constructs explicit binding tables with rejection explanations. Role output: Final `NativeLinkPlan` handle identifies verified bindings, rejected imports, ABI hashes, and executable compatibility receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Train from synthetic object/import pairs, ABI mismatches, accidental privilege requests, and independently checked linker outcomes. A frozen ABI checker and import allowlist verify exact signatures, ownership conventions, and permitted service identities. Hold out signature shapes, service namespaces, ABI revisions, and capability combinations with renamed-equivalent interfaces grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A correctly named import has the wrong buffer ownership convention and frees host memory with an incompatible allocator. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plugin.c`, `src/service_boundary.c`, `src/capability.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/decorators.py` (`operation`). Donor baseline: Builds operation metadata from Python signatures; native linker import contracts require separate implementation. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
