# 418 — Filesystem tree-diff algorithm designer: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Filesystem tree-diff algorithm designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn executable filesystem diff procedures that distinguish content edits, renames, mode changes, symlinks, and missing paths.

REPL variables `old_tree`, `new_tree`, and `path_events` retain immutable path/type/mode/content inventories and change histories. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions directory subtrees, delegates ambiguous rename candidates, and combines typed edit operations under path constraints. Role output: Final `TreeDiffCandidate` handle names native diff code, typed operations, source/target hashes, and reconstruction receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train from generated tree transformations and reference edit scripts, preserving failed rename and type-transition episodes. A fixed patch applicator reconstructs the target inventory and independently checks exact path types, modes, and bytes. Hold out directory shapes, case collisions, rename cycles, and mixed metadata/content changes with ancestry-linked trees grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A file-to-symlink transition is misclassified as an ordinary content edit and follows an unintended destination. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/vfs.c`, `src/workspace.c`, `src/buffer_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`FileSystemToolkitPro.read_file`). Donor baseline: Reads files through configurable modes; it does not derive verified native tree-diff algorithms. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
