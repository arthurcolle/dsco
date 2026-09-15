# 724 — Weighted queue-service policy designer: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Weighted queue-service policy designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn executable queue-service policies that balance declared weights, deadlines, and starvation bounds across competing task classes.

External `arrival_traces`, `service_costs`, and `queue_contracts` variables retain complete workloads and per-class obligations. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions busy periods, delegates starvation analysis, and builds deterministic admission and service transitions. Role output: Final `QueuePolicyCandidate` handle contains native selection code, fairness assumptions, service traces, and starvation witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Train from generated bursty workloads and fixed-reference fairness violations, retaining root choices and actual completed-work costs. A frozen discrete-event simulator checks service order, capacity, waiting bounds, and declared weighted-share conditions. Hold out burst correlations, weight ratios, job-size distributions, and deadline mixtures with related arrival processes grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An endless stream of short high-priority tasks prevents one lower-weight long task from ever receiving service. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/scheduler.c`, `src/ipc.c`, `src/goal_queue.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`QueueToolkit.publish`). Donor baseline: Inserts messages by priority; it does not establish weighted fairness guarantees. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
