# 408 — Filesystem tree-diff algorithm designer: Active example acquisition

Implement the active example acquisition feature for the Filesystem tree-diff algorithm designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn executable filesystem diff procedures that distinguish content edits, renames, mode changes, symlinks, and missing paths.

REPL variables `old_tree`, `new_tree`, and `path_events` retain immutable path/type/mode/content inventories and change histories. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions directory subtrees, delegates ambiguous rename candidates, and combines typed edit operations under path constraints. Role output: Final `TreeDiffCandidate` handle names native diff code, typed operations, source/target hashes, and reconstruction receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to select the next development example or oracle query using uncertainty, task importance, and expected decision benefit. Record selection probabilities and the population from which examples were chosen. Keep a fixed independent evaluation distribution for measuring actual improvement.

Train from generated tree transformations and reference edit scripts, preserving failed rename and type-transition episodes. A fixed patch applicator reconstructs the target inventory and independently checks exact path types, modes, and bytes. Hold out directory shapes, case collisions, rename cycles, and mixed metadata/content changes with ancestry-linked trees grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A file-to-symlink transition is misclassified as an ordinary content edit and follows an unintended destination. A policy that repeatedly samples easy successes must lose coverage credit. A highly uncertain but irrelevant example must not outrank a decisive missing case merely because it produces a larger confidence change.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/vfs.c`, `src/workspace.c`, `src/buffer_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`FileSystemToolkitPro.read_file`). Donor baseline: Reads files through configurable modes; it does not derive verified native tree-diff algorithms. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
