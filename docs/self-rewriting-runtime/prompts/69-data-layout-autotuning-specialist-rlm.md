# 69 — Train an RLM to autotune resident data layouts

Train a data-layout specialist RLM that improves a resident cell by selecting and compiling a better physical representation of its owned records. Search a bounded set of array-of-structures, structure-of-arrays, and blocked layouts with explicit field offsets, alignment, accessors, and conversion routines. Keep the logical schema stable while changing actual native load/store paths; a renamed struct alone is not layout optimization.

Store schemas, access profiles, generated layouts, and benchmark records in REPL variables. The root receives bounded summaries, inspects hot field groups through code, and recursively assigns layout and conversion subproblems to child RLMs. Persist their proposed mappings and evaluation results. The final environment handle contains a native layout candidate plus its measured applicability conditions.

Train a root adapter on profile-to-layout trajectories using independent output parity, peak-memory, and whole-workload timing rewards. Include conversion cost and layout preparation in the objective. Hold out record widths and access distributions; run a real bounded parameter update and compare with the frozen parent and original layout. Report cases where retaining the current layout wins.

Implement minimal logical accessors and generation ownership if absent. Migrate a copied state under writer quiescence or an explicit reconciled delta protocol; snapshotting while late writes disappear is unacceptable. Falsifying test: inject a write between snapshot and publication and require it to survive or publication to abort. A qualified new layout must execute changed native accessors with unchanged heap/session continuity and audited no resident exec. Test round-trip logical equality, padding assumptions, empty datasets, and skewed access. Speed gains that depend on omitted fields or broken external handles fail.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/arena_alloc.c`, `src/vecstore.c`, `src/plugin.c`, `src/vm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/agents/duckdb.py` symbols `DuckDBAgent.create_table`, `DuckDBAgent.aggregate`: The donor uses Python column dictionaries for simplified analytics, not a native layout optimizer or a real DuckDB engine. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
