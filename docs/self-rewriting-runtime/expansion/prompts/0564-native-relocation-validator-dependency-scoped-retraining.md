# 564 — Native relocation validator: Dependency-scoped retraining

Implement the dependency-scoped retraining feature for the Native relocation validator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn relocation decoding and validation procedures for an explicit native object-format subset before executable admission.

External `object_sections`, `relocation_entries`, and `target_layouts` retain bytes, symbol mappings, and candidate load addresses. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code groups relocation kinds, delegates range equations, and generates bounded patch calculations with checked arithmetic. Role output: Final `RelocationValidator` handle supplies native validation code, supported format subset, patch equations, and rejecting witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Bind training episodes and learned procedures to content and environment dependencies. When one dependency changes, select the affected development slices and train a bounded role update with replay from unaffected families. Version the dependency graph and resulting checkpoint separately.

Train from synthetic relocatable objects, validated load layouts, malformed offsets, and reference-labeled relocation action traces. A frozen object-format checker verifies relocation width, target identity, arithmetic bounds, and resulting reference addresses. Hold out relocation kinds, section arrangements, displacement extremes, and malformed-entry families with object ancestry grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A negative displacement truncates during range checking and redirects a branch outside the admitted executable section. Editing content at the same path must reopen dependent learning claims. Retraining only on the changed slice must not hide regressions elsewhere, and unrelated episodes must not be relabeled stale merely to improve reported coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plugin.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.register_code`). Donor baseline: Registers executable Python functions; it has no native relocation validation or machine-address contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
