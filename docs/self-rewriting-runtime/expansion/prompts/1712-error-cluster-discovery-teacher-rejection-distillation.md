# 1712 — Error cluster discoverer: Teacher rejection distillation

Implement the teacher rejection distillation feature for the Error cluster discoverer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an error-analysis RLM to discover actionable failure clusters without letting duplicate wording overwhelm underlying mechanisms.

External FAILURE_EPISODES and FEATURE_TABLES retain minimal inputs, symptoms, procedure identities, observed mechanisms, and uncertain labels. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs feature extraction, recursively challenges cluster boundaries, and buffers representatives with evidence-backed merge or split decisions. Role output: Return ERROR_PARTITION with representatives, mechanism hypotheses, uncertain memberships, and cluster-specific counterexamples. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect multiple executable teacher trajectories per development task. Replay their programs against the fixed oracle, reject incorrect or incomplete episodes, and deduplicate shared derivations before extracting root action targets. Distill accepted trajectories into the role policy with balanced family sampling.

Training contains seeded C bugs, randomized messages, duplicate floods, interacting faults, and reviewed incident groups. Known fixture mechanisms validate controlled clusters; independent reviewers assess natural cluster usefulness with ambiguity preserved. Hold out fault families, message templates, and duplication patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Thousands of duplicated timeout messages must not hide a small cluster caused by a distinct ownership defect. A fluent teacher episode containing the expected answer but an incorrect executable result must be excluded. Compare the trained root against its initial checkpoint on untouched families.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`, `MetacognitiveEngine._evaluate_strategy_adjustment`). Donor baseline: Strategy selection uses fixed confidence weights and coarse success-rate thresholds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
