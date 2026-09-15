# 79 — Train an RLM for budgeted native compilation reuse

Train a compilation-reuse specialist RLM that accelerates repeated self-rewriting under a finite native-code memory and artifact budget. Cache compiled units by normalized semantics, target ISA, ABI, imports, compiler configuration, and qualification scope. Separate reusable disk artifacts, mapped native generations, and currently pinned frames. The learned policy may choose admission or eviction priorities, but a deterministic lifetime manager prohibits eviction from reclaiming active code.

Store compilation histories, reuse distances, code sizes, and pinned-generation metadata in external REPL variables. The root receives a bounded cache summary, queries workload slices through code, and invokes child RLMs to analyze candidate reuse clusters. Retain policy decisions and outcomes. Return an environment handle containing the proposed cache policy and independently measured resource tradeoffs.

Train a root adapter on compilation/reuse episodes with reward for saved compilation time after I/O and mapping cost, subject to hard memory and correctness limits. Include expensive never-reused objects and small frequent kernels. Hold out workload phase changes, ABI revisions, and object-size distributions. Run a bounded actual parameter update and compare against the frozen parent, LRU, and size-aware baselines at identical budgets.

Provide a minimal artifact-cache protocol if absent. Falsifying test: apply memory pressure while a retired generation still has a paused native frame; the policy may evict its disk copy but must not unmap that live code. Change an imported ABI or compiler semantic flag and require a cache miss despite matching source text. Demonstrate measured compilation reuse on repeated native candidates, then correct reclamation after leases drain. A policy that improves hit rate by reusing unqualified bytes or exceeds the stated budget fails.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/plugin.c`, `src/vm.c`, `src/arena_alloc.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/base.py` symbols `BaseAgent._get_cache_key`, `BaseAgent._check_cache`: The donor caches operation results by arguments and TTL, not executable artifacts with semantic identities and pinned generations. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
