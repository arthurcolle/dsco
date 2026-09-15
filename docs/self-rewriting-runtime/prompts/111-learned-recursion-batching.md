# 111 — Learn recursion depth and batching as root behavior

Teach the role root when recursion is useful and how much independent work belongs in one child call. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, start from `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/config.py`'s `SwarmSettings`, whose fanout and concurrency are static limits. The [RLM paper](https://arxiv.org/html/2512.24601v3) distinguishes depth-zero external reasoning, leaf subcalls, and recursive sub-RLMs. Implement training that chooses among them while preserving host-enforced ceilings.

Generate role tasks spanning local arithmetic, independent semantic classification, hierarchical aggregation, and cross-partition dependencies. Collect verified root programs with different chunk sizes, batches, and recursion choices. Labels or terminal rewards must come from task correctness and measured resource use, not “more recursion is better.” Train the role's root adapter to emit bounded programmatic decisions using context size, task structure, observed uncertainty, and remaining budget. The host remains authoritative for maximum depth, calls, memory, and concurrency; generated code cannot redefine them. Distinguish a large input from intrinsically quadratic work.

Add a training/measurement adapter around `src/topology.c`, `src/swarm_scale.c`, and the root loop in `src/agent.c`. If no RLM environment exists, provide a minimal persistent context plus recursive-call primitive for the generated curriculum. A proposed `dsco role-train recursion-policy` command should declare the trainable root parameters, frozen leaf revision, and exact reward or supervised-label contract. Record both chosen and actually admitted recursion structures.

Acceptance requires a bounded real adapter update and reloaded DSCO role inference on unseen task shapes. An independent oracle checks all outputs; record correctness, depth, batching, calls, latency, and tokens rather than only aggregate reward. Include a trivial task where unnecessary recursion wastes budget, an aggregation task where leaf calls help, and a pairwise task where combining dependent items incorrectly loses answers. Inject an attempted depth-limit override and prove denial survives training. Compare the learned root to fixed depth and fixed batch baselines under equal budgets, with no performance claim unsupported by held-out results.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Verify changed trainable tensors against the initial artifact.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.
