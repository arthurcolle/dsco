# 60 new prompts: each role trains an RLM

Prompts 61–120. Copy one complete numbered prompt into an independent implementation session. These are specifications, not completed training runs.

# 61 — Train an RLM to reproduce decisive native schedules

Train a schedule-replay specialist RLM that makes concurrency failures in self-rewriting native cells reproducible. Instrument a bounded cooperative execution domain at cell acquisition, safepoint, publication, and release. Record thread identities, synchronization dependencies, input hashes, and generation decisions. Replay selects recorded runnable participants rather than reproducing wall-clock delays. Explicitly exclude arbitrary unsynchronized machine instructions from the initial determinism claim.

Keep the full schedule in an external REPL variable `trace`; the root receives only its schema, length, and bounded summary. Root-generated code selects intervals and invokes child RLMs over relevant slices, retaining their results in environment variables. Bound recursion depth, total subcalls, and trace materialization. The final answer references an environment variable containing the replay program and reproduced failure evidence, not a pasted transcript.

Create training episodes from controlled races between generation acquisition, activation, and reclamation. Train the role's root checkpoint or adapter to choose informative cuts, request useful child analyses, and construct replay schedules. Reward only independently reproduced terminal state and causal-event order, penalizing unnecessary inspected bytes and child calls. Hold out entire race families and thread-count configurations; compare the updated parameters with the frozen parent at matched resource limits.

Build a minimal scheduler/replay interface if absent. Falsifying test: an old-generation call racing publication must reproduce the original failure with deliberately changed sleeps; removing a necessary ordering edge must make replay fail its outcome oracle. Corrupt a participant or generation reference and require rejection before replay. Demonstrate one bounded real training update and rerun hidden schedules through the resulting checkpoint. Record replay coverage boundaries; matching log text without matching controlled execution does not count.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/event_loop.c`, `src/event_stream.c`, `src/vm.c`, `src/swarm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_structured`: The donor dispatches dependency-ready waves but does not record thread interleavings or train a replay specialist. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 62 — Train an RLM to prove bounded executable equivalence

Train an executable-equivalence specialist RLM that decides whether a proposed native rewrite preserves a declared bounded semantics. Support a precise initial fragment: fixed-width integers, bounded records, branches, and statically bounded loops. Translate original and candidate IR into symbolic constraints with explicit overflow and memory-access rules. An independent bounded solver or exhaustive checker establishes equivalence within the declared domain or returns a concrete counterexample; timeout means unknown.

Store both programs, specifications, and solver traces in REPL variables. The root sees metadata and accesses selected blocks through code; child RLMs can analyze separate path obligations, while their results remain keyed by obligation identity. The final environment handle contains checked obligations, domain bounds, and executable counterexamples. A child saying equivalent is merely a proposal until the deterministic checker validates it.

Train a role-owned root adapter on successful decomposition, solver-query construction, and counterexample interpretation trajectories. Reward independently discharged coverage and valid counterexamples, subtracting solver cost and redundant recursion. Include incorrect proofs as negatives. Hold out operator combinations and control-flow shapes, not just random inputs from known programs; load the trained checkpoint and compare proof coverage with the frozen parent under equal budgets.

Provide a small IR/checker interface if none exists; do not treat the current source-summary AST as a complete C semantics. Falsifying test: submit a rewrite valid over mathematical integers but wrong under the declared overflow behavior. Require a runnable distinguishing input and reject activation. For a genuinely equivalent bounded rewrite, verify every admitted path or return unknown. A fabricated certificate, omitted branch, or solver timeout must never become approval. Record a real parameter update and the separate executable validation results.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/ast.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`: The donor transforms ASTs without proving output equivalence or training a proof-decomposition policy. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 63 — Train an RLM for reversible debugging of pure native cells

Train a reversible-debugging specialist RLM that investigates a resident code cell by stepping backward through its logical execution. For a bounded pure instruction subset, record value changes or periodic snapshots plus deterministic forward replay. Debugging operates on owned inputs and state; external effects are observation boundaries, never something an inverse instruction pretends to undo. Preserve the code generation and source mapping for every frame.

Keep instruction traces, snapshots, and comparison outputs in external REPL variables. The root receives sizes and error metadata, issues code to jump to a checkpoint or inspect selected values, and delegates branch-local analysis to child RLMs. Retain child diagnoses and replay states across iterations. Its final variable contains a reproducible sequence locating the earliest wrong state, rather than an unverified narrative.

Train the root model or adapter on injected arithmetic, branch, and indexing defects with known first-divergence locations. Supervise useful checkpoint and slice actions; independently reward correct localization with the fewest executed replay steps and inspected values. Hold out different control-flow patterns and trace lengths. Run an actual bounded update, then compare the trained checkpoint against its unchanged parent on hidden bugs.

Implement a minimal pure-cell interpreter and native/reference comparison if needed. Falsifying test: rewind before a branch, replay the same generation and input, and require identical logical state hashes and output. Switch the active resident generation while a debug frame is pinned; the frame must still inspect its original code. An attempted rewind past a governed file write must report an effect boundary. Wrong-generation values, invented inverse effects, or localization based only on line-name guessing fail. Show native bug reproduction separately from interpreter-only debugging mechanics.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/plugin.c`, `src/event_stream.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/hooks.py` symbols `TelemetryHook.handle`, `TelemetryHook.get_metrics`: The donor collects operation telemetry but provides no instruction-state rewind or trained reversible-debugging policy. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 64 — Train an RLM to minimize native counterexamples structurally

Train a counterexample-minimization specialist RLM that turns a large native failure input into a small explanatory witness. Go beyond deleting arbitrary bytes: represent structured records, program fragments, and schedule segments with grammar-aware reductions. Every proposed reduction must preserve the declared failure identity, including oracle mismatch class, implicated generation, and necessary preconditions. A different crash is not a successful minimization.

Place the original input, grammar, candidate history, and execution receipts in external REPL variables. The root sees bounded metadata and writes code selecting subtrees or field groups for reduction. Child RLMs inspect disjoint structural regions; the root retains their proposals and executes reductions through the independent oracle. Return a final environment variable holding the minimized input, exact reproduction invocation, and surviving failure evidence.

Train a root adapter on reduction trajectories from seeded native parser and scorer defects. Reward verified size reduction and stable failure identity; penalize invalid syntax, oracle calls, and switching to easier unrelated failures. Use family-disjoint holdouts containing unseen grammars and defect locations. Demonstrate a bounded parameter update and compare trained versus frozen-root reduction quality at the same oracle-call budget.

Provide a standalone reducer/oracle protocol if absent, allowing deterministic caching only by candidate bytes, oracle version, and executable generation. Falsifying test: start with an input exposing an overflow bug and offer a much smaller input causing an out-of-bounds crash. The second must be rejected because the failure changed. Require the final witness to reproduce the original defect repeatedly and reach declared one-step minimality under the supported transformations. Labels such as minimal must state that transformation set; global minimality must not be inferred from exhausted search time.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/autoresearch.c`, `src/ast.c`, `src/execution_kernel.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.parse`, `ASTAnalyzer._extract_structure`: The donor extracts code structure but does not minimize behavioral witnesses while preserving a failure predicate. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 65 — Train an RLM for executable change-impact slicing

Train a change-impact specialist RLM that determines which executable behaviors must be requalified after a proposed self-rewrite. Build bounded dependency records for definitions, uses, control predicates, state schemas, imported contracts, and specialization assumptions. Compute a conservative mechanical slice first; let the RLM propose justified refinements and missing dependencies. Evidence-claim invalidation and lexical file matching are not substitutes for executable dataflow.

Keep source, typed IR, dependency graphs, and test-coverage observations in REPL variables. The root receives graph statistics and a change identifier, then uses code to query slices and recursively delegates independent subgraphs. Child results retain stable node identities. The final environment handle names affected native cells, test obligations, and unresolved dependencies; unknown indirect calls expand the slice rather than disappearing.

Train the role's root checkpoint or adapter on program mutations with independently measured downstream outcome changes. Reward full affected-behavior recall before rewarding slice reduction or fewer qualification runs. Penalize missed ABI, alias, and control dependencies heavily. Hold out mutation types and call-graph shapes, perform a real bounded update, and compare the trained root against the frozen parent and conservative baseline.

Implement the minimal graph interface locally if needed; the existing source AST offers anchors but not complete C semantics. Falsifying test: change a branch guard that controls a shared value while leaving function names and call edges untouched. Every dependent contract must enter the qualification set. Change an unrelated pure cell and require unaffected cells to remain reusable when dependencies are known. Missing analysis coverage must produce conservative expansion or unknown, never a falsely precise green result. Measure actual avoided qualification work on valid slices.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/ast.c`, `src/vm.c`, `src/lingo_workflow.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` symbols `WorkflowToolkit.analyze_dependencies`: The donor analyzes workflow dependencies, not native data/control dependencies or learned change-impact slicing. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 66 — Train an RLM to construct checkable loop resource certificates

Train a termination-certificate specialist RLM that enables useful loops in self-generated executable cells without accepting unbounded computation on trust. Define a bounded IR fragment with explicit integer widths, loop invariants, ranking expressions, allocation operations, and input-domain predicates. Candidates carry certificates for maximum iterations, instruction count, and owned allocation volume. A small independent verifier checks these claims against the exact IR; runtime fuel remains a defense against verifier mistakes.

Store the candidate graph, obligations, and checker diagnostics in external REPL variables. The root receives only loop summaries and can inspect blocks through code or invoke child RLMs over separate loops. It retains invariant proposals and counterexamples between calls. The final environment variable contains the certificate and verified scope, including unsupported constructs, rather than a textual promise of termination.

Train a root adapter on proof-construction trajectories for nested counters, bounded traversals, and controlled recursion. Reward independently accepted sound certificates and useful admissible domains; penalize false bounds and excess checker calls. Hold out loop templates and integer widths. Perform an actual bounded update and compare accepted valid programs and rejected invalid ones against the frozen parent under equal inference limits.

Provide a minimal certificate language if absent. Falsifying test: use an unsigned loop whose apparent decrement wraps before the exit condition. The verifier must reject the incorrect finite bound or return a concrete violating execution. Test a valid nested loop and require observed steps and allocations never exceed its checked certificate across boundary inputs. Alter one instruction after certification and require a hash mismatch before admission. A learned confidence score, dimensional estimate, or test-only absence of hangs cannot replace the certificate.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `include/vm.h`, `src/prompt_program.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` symbols `WorkflowToolkit.validate_workflow`: The donor checks basic workflow structure but does not prove termination or train certificate synthesis. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 67 — Train an RLM to synthesize pointer-free native host bindings

Train a host-binding specialist RLM that gives generated native procedures useful host services without persisting or exchanging raw host pointers. Define opaque handles containing namespace, slot, generation, and type identity, with explicit ownership, borrowing, revocation, and permitted operations. A checked resolver validates every access; candidate ABI records describe handles rather than accidental process addresses. This supports replaceable cells while keeping host resource ownership explicit.

Place API declarations, schemas, and lifetime traces in REPL variables. The root sees metadata, queries selected interfaces through code, and asks child RLMs to inspect ownership paths or adapter signatures. Retain proposed bindings and test outcomes in the environment. The final handle identifies generated binding code, its contract, and independent qualification receipts.

Train the role-owned root adapter on API-to-binding trajectories with validated type mappings, retain/release balance, and capability behavior. Reward usable correct adapters and rejection of invalid access; penalize unnecessary copying only after safety predicates pass. Hold out resource kinds and nested ownership patterns. Run a bounded real parameter update and compare the trained checkpoint with its frozen parent on unseen APIs.

Implement one pure buffer-view binding and one governed observation service through a minimal local handle table. Native code still executes with process authority; handles do not create memory isolation for arbitrary malicious instructions. Falsifying test: release a handle, reuse its slot for a different object, and require the old generation to fail without exposing new bytes. Reject cross-type reinterpretation and use after cell retirement. Deny the observation capability and verify generated bindings preserve the denial. Qualify actual compiled adapter functions, not only matching schema strings.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/plugin.c`, `include/plugin.h`, `src/vm.c`, `src/capability.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/decorators.py` symbols `operation`, `agent`: The donor derives operation metadata from Python signatures but does not generate checked native host-handle bindings. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 68 — Train an RLM to generate SIMD native batch kernels

Train a SIMD-codegen specialist RLM that rewrites a bounded scalar native cell into a batch implementation using the current host's supported vector ISA. Begin with integer scoring or byte classification where exact scalar parity is well defined. Generate vector-width operations, checked loads, masked or scalar tails, and a scalar fallback. Inspect emitted instructions to establish that the candidate actually vectorizes rather than merely advertising a batch interface.

Keep scalar IR, record layouts, profiles, and candidate assembly in external REPL variables. The root sees operation counts and ISA metadata, uses code to inspect hot loops, and delegates independent vectorization or tail obligations to child RLMs. Retain candidate kernels and test receipts; return the selected kernel through an environment handle. Bound recursion and compilation attempts.

Train a root adapter from executable vectorization trajectories, independently rewarding parity first and end-to-end throughput second, including packing and dispatch cost. Hold out lengths, alignment patterns, and expression structures. Perform a real bounded update and compare the trained policy with its frozen parent under equal compile and evaluation budgets; retain the scalar champion if no candidate qualifies.

Provide a minimal native-cell loader if absent. Falsifying test: test lengths around every vector boundary, guard-page adjacent buffers, misaligned input, and extreme integer values. Any unsupported ISA must select a truthful fallback without illegal instructions. Activate a qualified batch kernel inside the resident process and prove changed native bytes, heap-sentinel/session continuity, old-frame completion, and audited absence of resident exec. A fast kernel that reads past the admitted input or changes overflow semantics must never activate; report whole-call performance rather than arithmetic-only speed.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `include/simd.h`, `src/vm.c`, `src/vecstore.c`, `src/plugin.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`, `MetaToolRegistry.register_code`: The donor transforms executable Python functions but does not generate vector instructions or establish scalar parity. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 69 — Train an RLM to autotune resident data layouts

Train a data-layout specialist RLM that improves a resident cell by selecting and compiling a better physical representation of its owned records. Search a bounded set of array-of-structures, structure-of-arrays, and blocked layouts with explicit field offsets, alignment, accessors, and conversion routines. Keep the logical schema stable while changing actual native load/store paths; a renamed struct alone is not layout optimization.

Store schemas, access profiles, generated layouts, and benchmark records in REPL variables. The root receives bounded summaries, inspects hot field groups through code, and recursively assigns layout and conversion subproblems to child RLMs. Persist their proposed mappings and evaluation results. The final environment handle contains a native layout candidate plus its measured applicability conditions.

Train a root adapter on profile-to-layout trajectories using independent output parity, peak-memory, and whole-workload timing rewards. Include conversion cost and layout preparation in the objective. Hold out record widths and access distributions; run a real bounded parameter update and compare with the frozen parent and original layout. Report cases where retaining the current layout wins.

Implement minimal logical accessors and generation ownership if absent. Migrate a copied state under writer quiescence or an explicit reconciled delta protocol; snapshotting while late writes disappear is unacceptable. Falsifying test: inject a write between snapshot and publication and require it to survive or publication to abort. A qualified new layout must execute changed native accessors with unchanged heap/session continuity and audited no resident exec. Test round-trip logical equality, padding assumptions, empty datasets, and skewed access. Speed gains that depend on omitted fields or broken external handles fail.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/arena_alloc.c`, `src/vecstore.c`, `src/plugin.c`, `src/vm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/agents/duckdb.py` symbols `DuckDBAgent.create_table`, `DuckDBAgent.aggregate`: The donor uses Python column dictionaries for simplified analytics, not a native layout optimizer or a real DuckDB engine. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 70 — Train an RLM to remove allocations through escape analysis

Train an escape-analysis specialist RLM that rewrites a generated native cell to eliminate unnecessary heap allocation without changing lifetime semantics. Represent allocations, aliases, returns, callbacks, and retained-state references in a bounded ownership graph. A deterministic checker validates proposed nonescape facts before moving storage to an invocation arena, reusing a scratch slot, or replacing an allocation with scalar fields.

Keep IR, ownership graphs, and allocation traces in external REPL variables. The root receives graph sizes and hotspot metadata, queries selected alias paths through code, and delegates uncertain regions to child RLMs. Persist their proposed proofs and rejection witnesses. The final environment handle names the rewritten executable candidate and the checked lifetime facts enabling it.

Train the root checkpoint or adapter on successful allocation-elimination trajectories and adversarial escaping examples. Independent rewards require output parity and memory-valid execution before considering reduced allocations or latency. Hold out callback shapes, alias depths, and conditional escapes. Perform a bounded real parameter update; compare the trained policy with the frozen parent and a conservative no-rewrite baseline under equal verifier budgets.

Use explicit per-invocation arenas rather than assuming existing global scratch storage is thread-safe. Supply a minimal ownership IR if current source summaries cannot express required facts. Falsifying test: a returned closure or registered callback retains a reference beyond the call; the proposed temporary allocation must be rejected. A genuinely local temporary should be eliminated and show lower measured allocation counts. Activate the qualified native version with heap-sentinel/session continuity and audited no resident exec; concurrent old frames retain valid storage. Include cancellation and multiple arena chunks so a superficial head-offset rewind cannot establish full lifetime correctness.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/arena_alloc.c`, `include/arena_alloc.h`, `src/ast.c`, `src/vm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer._extract_structure`: The donor identifies structural syntax but does not prove lifetimes or optimize allocation placement. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 71 — Train an RLM to adapt native algorithms to allocation pressure

Train an allocation-pressure specialist RLM that chooses and produces different native algorithms for the same logical operation as available memory changes. Start with a bounded transform having a fast materializing implementation and a lower-memory streaming implementation. Capture measured allocation peaks, input size, and concurrency; learn guarded applicability rather than merely changing an allocator limit. Each variant must satisfy the same externally checked output contract.

Keep pressure traces, code variants, and outcome records in REPL variables. The root sees bounded statistics, uses code to inspect pressure windows, and delegates implementation or guard analysis to child RLMs. Retain candidate decisions and counterexamples in the environment. Return a final variable identifying qualified native variants, switching predicates, and measured tradeoffs.

Train a root adapter on episodes where workload size and concurrent activity change the best algorithm. Reward verified completion under declared memory ceilings, then latency and compilation cost; account for unsuccessful attempts. Hold out workload mixtures and pressure transitions. Run a bounded actual update and compare its policy with the frozen parent and always-materialize/always-stream baselines at matched resource budgets.

Define a minimal guarded cell interface if absent. Switch only at invocation boundaries, pin in-flight calls, and include hysteresis so noisy readings cannot cause compilation thrash. Falsifying test: introduce allocator pressure during a burst and require bounded-memory completion with exact reference outputs; relieve pressure and verify the declared policy adapts without oscillating on every sample. Prove actual native variant selection, resident heap/session continuity, and no host exec. A candidate that avoids allocation by dropping records or whose estimates contradict measured peaks must be rejected.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/arena_alloc.c`, `src/cost_frontier.c`, `src/vm.c`, `src/self_improve.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/base.py` symbols `AgentStats.record_call`: The donor records operation timings and failures but does not learn native algorithm choices from memory-pressure feedback. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 72 — Train an RLM to reconstruct deoptimization state

Train a deoptimization specialist RLM that lets an optimized native cell fall back into its baseline IR at a declared safepoint while preserving computation. Define maps from native registers, stack slots, and constant-folded values to logical locals, continuation labels, and owned-object handles. Recover eliminated values using bounded pure reconstruction expressions. Unsupported safepoints remain pinned to native completion rather than pretending arbitrary machine stacks are resumable.

Store native code metadata, baseline IR, safepoint maps, and differential traces in REPL variables. The root receives map sizes and failing-point identifiers, queries relevant live-value slices through code, and invokes child RLMs for independent reconstruction obligations. Keep proposed maps and verifier diagnostics across calls. The final environment handle names the checked mapping artifact and its coverage.

Train the root checkpoint or adapter on optimization/deoptimization trajectories, rewarding independently correct resumed suffix outputs and minimal reconstruction overhead. Include wrong register assignments and stale schema assumptions as negative examples. Hold out optimization combinations and control-flow shapes; perform an actual bounded update and compare against the frozen parent at equal qualification budgets.

Provide a small native-cell and baseline interpreter interface if absent. Falsifying test: force deoptimization at every supported safepoint of a qualified native scorer, including after a branch and after a recorded observation. The resumed result must equal uninterrupted baseline execution, with no repeated effect. Corrupt one live-value location and require map rejection before activation. Demonstrate fallback within the resident address space using heap-sentinel/session continuity and audited no exec. A VM-to-VM version switch alone does not satisfy native state reconstruction; retain machine-location evidence for actual native frames.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `include/vm.h`, `src/plugin.c`, `src/lingo_workflow.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_sequential`: The donor forwards prior results between calls, but cannot reconstruct a suspended native frame into an interpreter continuation. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 73 — Train an RLM for semantic incremental native compilation

Train an incremental-compilation specialist RLM that makes resident self-rewrites cheap by rebuilding only semantically affected native units. Define content identities over normalized IR, ABI, imported semantic contracts, target ISA, and compiler configuration. Partition bounded programs into reusable compilation units; distinguish source spelling changes from changes that affect behavior or generated assumptions. Reuse cannot be decided from a filename timestamp or function name.

Keep program versions, dependency graphs, build receipts, and unit artifacts in external REPL variables. The root receives change summaries, inspects semantic neighborhoods through code, and delegates independent partition or dependency questions to child RLMs. Retain compiled-unit handles and reuse justifications. The final environment variable identifies a complete linkable candidate and its reuse proof obligations.

Train a root adapter on edit/build trajectories, independently rewarding semantic parity with clean rebuilds before saved compilation work. Penalize missed dependency changes and overfragmentation overhead. Hold out edit classes, module shapes, and compiler settings. Perform a bounded real parameter update and compare the trained checkpoint with its frozen parent and always-rebuild baseline at equal workloads.

Implement a minimal isolated cache/link interface if needed. Falsifying test: change an inline constant or imported record layout without renaming its symbol; every affected specialization must rebuild. A comment-only change may reuse code if normalized semantics and debugging policy allow it. Compare incremental and clean artifacts by executable behavior across hidden cases, not byte identity alone. Activate a qualified linked candidate with heap/session continuity and audited absence of resident exec. Deliberately substitute an object built for another target and require rejection before linking or publication.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/ast.c`, `src/plugin.c`, `src/vm.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.get_ast`, `MetaToolRegistry.transform_tool`: The donor caches AST descriptions and transforms whole registered tools, not semantic native dependency units. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 74 — Train an RLM for generation-aware native source debugging

Train a native-debugging specialist RLM that can inspect the generated procedure actually running, set bounded source-level stop points, and identify a repairable defect. Preserve source/IR locations, native address ranges, inline frames, and variable-location descriptions for each executable generation. Prefer compiler metadata or explicit emitter maps; the current source-summary AST cannot by itself recover optimized machine state.

Keep debug maps, source, stack snapshots, and observed values in external REPL variables. The root receives only stop-reason and generation metadata, queries selected locations through code, and calls child RLMs on relevant frames or source slices. Persist inspected state and competing diagnoses. The final environment handle contains a replayable debugging action sequence, implicated code span, and validated defect evidence.

Train the root adapter on seeded native defects with known origins, supervising useful breakpoint placement and variable inspections. Independent reward requires correct localized fault and a reproducing input, penalizing intrusive stops and unnecessary memory inspection. Hold out source layouts, inlining shapes, and defect families. Run a bounded real parameter update, then compare the trained policy against the frozen parent under the same debugging budget.

Provide an owned test-process debugging surface if missing; do not attach to unrelated applications. Falsifying test: suspend a generation-A frame, publish B with different line mappings, and inspect A. Its source and locals must remain A-specific while new calls use B. An optimized-out value must be marked unavailable, never guessed. Prove heap/session continuity and no resident exec across the replacement. A correct-looking source line from the wrong generation fails even if the final explanation sounds plausible; verify actual machine-address ownership.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/ast.c`, `src/plugin.c`, `src/native_trace.c`, `src/vm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer._extract_structure`, `MetaToolRegistry.get_source`: The donor stores source and line-oriented structure but has no live native breakpoint mapping or trained debugging policy. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 75 — Train an RLM for cross-compiler native qualification

Train a cross-compiler specialist RLM that qualifies self-generated native code through independent toolchains and optimization settings. Compile the same bounded candidate with explicitly identified compiler versions, targets, ABI flags, and semantic modes. Run each executable against an external reference oracle and compare outcomes, sanitizer diagnostics where supported, and resource measurements. Compiler majority agreement is not a correctness proof.

Store candidate source, build configurations, diagnostics, and run traces in REPL variables. The root receives an inventory and bounded failure summaries, generates code selecting the next informative configuration, and delegates particular diagnostic or language-rule slices to child RLMs. Retain all results. Return a final environment variable containing qualification scope and unresolved discrepancies, not a synthesized vote.

Train a role-owned root adapter on known undefined-behavior, optimization-sensitivity, and ABI defect families. Reward independently reproduced distinguishing cases and valid qualified candidates; penalize redundant compiler runs and unsupported portability claims. Hold out compiler-version combinations and bug patterns. Perform a bounded actual parameter update and compare its experiment policy with the frozen parent and fixed-matrix baseline.

Provide a minimal shell-free build/run adapter if absent. Falsifying test: seed signed-overflow or aliasing assumptions whose behavior changes under an admitted configuration. The role must produce a concrete discrepancy and block qualification until the contract or source is corrected. Supply two agreeing wrong binaries and require the external oracle to reject both. If only one compiler is installed, report the missing cross-compiler evidence instead of relabeling two optimization flags as independent toolchains. The output must identify exact executable hashes; source-only inspection cannot satisfy runtime qualification.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/autoresearch.c`, `src/plugin.c`, `src/ast.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/scaling.py` symbols `ScaledSwarm._select_best`: The donor selects answer replicas by judge or heuristics, not native executable behavior across independent compilers. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 76 — Train an RLM to search native optimization phase orders

Train an optimization-order specialist RLM that learns when known executable transformations help or interfere with one another. Search sequences of already-defined passes such as constant folding, fusion, vectorization, loop unrolling, and allocation elimination over a bounded IR. Each pass declares preconditions and invalidated analyses; the search regenerates those analyses rather than applying stale facts. This feature learns pass ordering, not new rewrite rules or prompt topology.

Keep IR generations, pass histories, analysis caches, and measured outcomes in REPL variables. The root sees compact structural features, uses code to branch candidate sequences, and invokes child RLMs on independent sequence prefixes. Retain losing traces as well as winners. The final environment handle names a fully qualified pass sequence and resulting native candidate.

Train a root adapter on executable optimization trajectories. Reward external semantic equivalence first, then end-to-end latency or throughput including code-size and compilation penalties. Charge every explored candidate to the search budget. Hold out program families and pass-interaction patterns; run a bounded real parameter update and compare the trained policy with its frozen parent, fixed orders, and budget-matched random search.

Implement a small pass pipeline if none exists, preserving a generic champion. Falsifying test: provide a pair of passes where one order exposes vectorization and the other inflates code enough to slow the complete workload. The learned policy must be evaluated on untouched variants, not selected training timings. A semantically invalid order must fail before promotion regardless of speed. Activate a qualifying native result with heap/session continuity and audited absence of resident exec. Report failures to improve honestly rather than equating a changed phase list with learned optimization.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/autoresearch.c`, `src/vm.c`, `src/ast.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`: The donor applies transformations in supplied list order without learning interactions or checking optimization outcomes. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 77 — Train an RLM for checked speculative native parallelization

Train a speculative-parallelization specialist RLM that extracts parallelism from bounded native computations whose independence depends on runtime input. Partition an owned-state computation into speculative regions with explicit read/write sets or validated versioned handles. Execute against private state, validate conflicts, and commit in declared serial order only when equivalent. On conflict, discard speculation and run the serial champion. External effects are forbidden inside speculative regions.

Store IR, dependence traces, candidate partitions, and conflict records in external REPL variables. The root receives bounded statistics, selects regions through code, and recursively delegates uncertain dependency slices to child RLMs. Retain proposed guards and observed conflicts. Return the partition and executable candidate through an environment handle with exact applicability conditions.

Train a root adapter on partition/guard trajectories using independent serial-equivalence rewards and whole-workload speed after rollback overhead. Penalize missed conflicts, excessive copying, and wasted worker time. Hold out aliasing patterns and contention distributions. Perform a bounded real parameter update and compare the trained policy with its frozen parent and always-serial baseline at matched resource limits.

Provide a minimal versioned owned-state interpreter/native wrapper if absent. Falsifying test: supply apparently disjoint records that alias through one hidden handle. Validation must detect the conflict, discard every speculative write, and return the exact serial result. A governed counter effect requested within speculation must be rejected before execution, not replayed after rollback. For a nonconflicting batch, demonstrate actual concurrent native execution and improved complete-call timing. If publishing an optimized generation, prove heap/session continuity and audited no resident exec; scheduling two unsafe callbacks concurrently is insufficient.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/lingo_workflow.c`, `src/swarm.c`, `src/execution_kernel.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_parallel`, `ToolExecutor._execute_structured`: The donor runs declared parallel or dependency-ready calls but does not speculate over unknown native data dependencies. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 78 — Train an RLM to optimize native precision under error contracts

Train a numerical-precision specialist RLM that improves native scoring or reduction kernels without silently changing their accuracy contract. Search bounded choices such as pairwise versus compensated summation, accumulator width, vector reduction trees, and guarded stable fallbacks. Define allowed absolute/relative error, exceptional-value behavior, reproducibility requirements, and input range before optimization. Avoid claiming floating-point reassociation is exact integer-style equivalence.

Keep kernel source, input distributions, high-precision reference results, and candidate error profiles in external REPL variables. The root receives compact statistics, writes code selecting difficult numeric slices, and delegates independent stability analyses to child RLMs. Retain error witnesses and candidate artifacts. Its final environment handle names a qualified native kernel and the exact error domain where it may run.

Train the root checkpoint or adapter on algorithm-choice trajectories with external high-precision or exact-reference rewards. Accuracy and exceptional-value constraints are hard gates; only surviving candidates receive speed or memory rewards. Hold out cancellation patterns, dynamic ranges, and distribution shifts. Execute a bounded real training update and compare the trained policy against its frozen parent plus fixed stable and fast baselines.

Provide a minimal evaluator and native loader if absent. Falsifying test: give a candidate ordinary positive inputs where it looks excellent, then hidden alternating-magnitude inputs exposing catastrophic cancellation. It must fail qualification or take its verified stable fallback. Test NaNs, infinities, signed zero, and empty reductions according to the declared contract. Activate a surviving native implementation with heap/session continuity and audited absence of resident exec. Report full-call performance and worst observed error separately; averaged error cannot excuse a violated per-case limit.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/math_fastpath.c`, `src/vm.c`, `src/cost_frontier.c`, `include/simd.h`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` symbols `MathToolkit`: The donor supplies mathematical operations but does not learn native precision or reduction order under independently checked numerical-error limits. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 79 — Train an RLM for budgeted native compilation reuse

Train a compilation-reuse specialist RLM that accelerates repeated self-rewriting under a finite native-code memory and artifact budget. Cache compiled units by normalized semantics, target ISA, ABI, imports, compiler configuration, and qualification scope. Separate reusable disk artifacts, mapped native generations, and currently pinned frames. The learned policy may choose admission or eviction priorities, but a deterministic lifetime manager prohibits eviction from reclaiming active code.

Store compilation histories, reuse distances, code sizes, and pinned-generation metadata in external REPL variables. The root receives a bounded cache summary, queries workload slices through code, and invokes child RLMs to analyze candidate reuse clusters. Retain policy decisions and outcomes. Return an environment handle containing the proposed cache policy and independently measured resource tradeoffs.

Train a root adapter on compilation/reuse episodes with reward for saved compilation time after I/O and mapping cost, subject to hard memory and correctness limits. Include expensive never-reused objects and small frequent kernels. Hold out workload phase changes, ABI revisions, and object-size distributions. Run a bounded actual parameter update and compare against the frozen parent, LRU, and size-aware baselines at identical budgets.

Provide a minimal artifact-cache protocol if absent. Falsifying test: apply memory pressure while a retired generation still has a paused native frame; the policy may evict its disk copy but must not unmap that live code. Change an imported ABI or compiler semantic flag and require a cache miss despite matching source text. Demonstrate measured compilation reuse on repeated native candidates, then correct reclamation after leases drain. A policy that improves hit rate by reusing unqualified bytes or exceeds the stated budget fails.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/plugin.c`, `src/vm.c`, `src/arena_alloc.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/base.py` symbols `BaseAgent._get_cache_key`, `BaseAgent._check_cache`: The donor caches operation results by arguments and TTL, not executable artifacts with semantic identities and pinned generations. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 80 — Train an RLM for lossless logical code-cell checkpoints

Train a checkpoint specialist RLM that preserves the logical state of a self-rewriting code-cell computation across suspension or recovery. Serialize typed values, stable continuation labels, code/ABI/schema hashes, owned-object identities, and completed-effect references. Never serialize raw instruction pointers, stack addresses, or borrowed host pointers as portable state. Reconstruct executable mappings through verified artifacts and resolve resources through explicit typed handles.

Keep live-value schemas, checkpoint candidates, continuation graphs, and restoration traces in external REPL variables. The root receives bounded state metadata, inspects selected ownership slices through code, and delegates reconstruction obligations to child RLMs. Retain proposed serializers and their rejected cases. The final environment handle identifies a checked checkpoint/reconstruction program with its supported state domain.

Train the role-owned root adapter on checkpoint-construction trajectories from bounded native/IR computations. Reward independent suffix-equivalence and complete reachable-state coverage before checkpoint size or restoration latency. Hold out object-graph shapes, cycles, and continuation locations. Perform a bounded real parameter update and compare the trained checkpoint with its frozen parent at matched checker budgets.

Implement a minimal logical continuation interface if absent. Falsifying test: checkpoint after a governed observation, restore into a deliberately different allocation layout, and require identical remaining outputs with no repeated external effect. Reject stale code hashes, unknown schema versions, missing objects, and pointer-looking values declared as handles. Checkpoint an old generation while a new one is active and restore only to a verified compatible continuation. Recovery may use a new process; label it recovery, not evidence of live same-address-space rewriting. Separately demonstrate lossless pause/resume within the resident process, preserving unrelated live session state.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/recovery.c`, `src/execution_recovery.c`, `src/arena_alloc.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `CodeSandbox.execute`: The donor retains Python locals in memory but does not serialize typed executable continuations or validate their restoration. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.

---

# 81 — Train a trace-to-state-machine induction RLM

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`InductiveReasoningSignature`): Induction requests a textual generalization and possible counterexamples; it does not infer executable state machines from traces.

This role learns the hidden behavioral states of resident procedures from event traces, producing a runnable transition table useful for checking replacements. Define observations, action labels, terminal states, reset boundaries, and missing-event markers. The root must select trace partitions, ask children for candidate distinguishing suffixes, and execute deterministic transition checks before choosing state merges. Unknown behavior remains unknown rather than an invented self-loop.

Generate training tasks from small deterministic machines with observable outputs, including queues, parsers, and retry protocols. Hide the generating graph; provide only trace buffers and a bounded replay interface. Target root actions include partitioning, comparing suffixes, splitting inconsistent states, and writing a serialized executable machine. Training reward measures development-validation sequence agreement, rejection of invalid transitions, machine size, and actual query cost. Reject a smaller machine that achieves compression by accepting forbidden behavior.

Hold out entire graph topologies, alphabet renamings, and longer sequences, not merely random traces from the same machine. Never use this final split for training rewards or checkpoint selection. A challenge has two states with identical prefixes but different close/reset behavior: the trained root must request a distinguishing suffix and preserve the split. In the live binary execute its learned table on withheld sequences and compare with the owned oracle. Report state accuracy and query count separately from leaf accuracy; retain inferred-machine provenance by procedure revision.

Expose `TRACE_BATCHES` and `QUERY_HISTORY` as external variables; the root RLM sees counts and handles. Its REPL partitions histories and recursively calls child RLMs for distinguishing suffixes, buffers verified splits, and returns a `MACHINE_TABLE` handle. Train root weights or an adapter on independently accepted partition/query/refinement programs. Mask child and environment tokens; keep child checkpoints fixed. Save the trained checkpoint and corpus lineage, and compare hidden-sequence accuracy with the initial root through a standalone replay entrypoint.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/trace_kg_store.c`, `src/event_stream.c`, `src/vm.c` through proposed `src/rlm_state_induction.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 82 — Train an active protocol-learning RLM

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`MultiHopQueryGenerator`, `UniversalMultiHopRAG.forward`): The donor generates retrieval queries over notes; it has no membership-query or equivalence-counterexample protocol-learning loop.

Train this specialist to discover the legal interaction language of an opaque tool adapter before the resident runtime synthesizes a client for it. Supply an owned resettable service, action alphabet, observations, and explicit probe budget. The root maintains an observation table, proposes distinguishing suffixes, recursively delegates candidate-model checks, and consumes counterexamples from a bounded conformance oracle. Probes are effects and must use the ordinary governed execution path.

Training tasks contain authenticated session, transaction, pagination, and retry state machines with randomized labels. Record reset, membership-query, table-refinement, and candidate-submission root actions. Reward conformance on independently generated interaction sequences, economical probing, and honest unknown-state reporting; do not reward a plausible verbal protocol summary. Impure reset or nondeterminism must produce a scoped model or refusal, not a deterministic fiction.

Hold out whole protocol families and delayed-error patterns. Never use this final split for training rewards or checkpoint selection. Challenge the role with two identical retry responses where one consumes a one-use token; only a later commit distinguishes the states. The learned root must discover the difference without sending an unbounded probe storm. Exercise its client plan against a real local fixture through the built binary, including denied probes, cancellation, and reset failure. Compare trained and initial roots under identical service snapshots and fixed child checkpoints; save the learned automaton and actual interaction receipts.

Keep `PROTOCOL_ORACLE`, `OBSERVATION_TABLE`, and reset receipts outside the root context; expose bounded metadata. The root RLM generates REPL membership queries and recursively asks child RLMs to challenge candidate states, buffering responses before returning `PROTOCOL_MODEL`. Rejection-distill independently accepted query/refinement trajectories into a real root adapter or weight checkpoint. Exclude child/environment tokens from loss and freeze child models. Preserve dataset lineage and report trained-versus-initial conformance on the sealed protocol holdout through an independently runnable learner.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/mcp.c`, `src/execution_kernel.c`, `src/pty_session.c` through proposed `src/rlm_protocol_learning.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 83 — Train an executable API-contract mining RLM

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`): AST inspection extracts declarations and arguments; registration validates syntax but does not infer behavioral preconditions or postconditions.

This role mines candidate executable contracts for APIs the resident binary may replace. Inputs include revisioned source, schemas, successful and failed invocations, and an owned implementation oracle. The root separates syntactic constraints from semantic invariants, delegates function-level inspection, synthesizes bounded predicate programs, and searches for counterexamples. Each condition retains source or execution evidence and an applicability scope; observed regularity is not automatically a universal guarantee.

Train on generated record transforms and local API fixtures with hidden contracts covering nullability, ordering, identity preservation, length relations, error atomicity, and side effects. Root trajectories should expose probe selection, predicate proposal, falsification, and contract narrowing. Terminal reward combines hidden positive/negative classification with predicate execution cost and coverage; trivial always-false contracts cannot win by rejecting every input.

Hold out schema combinations and implementation families. Never use this final split for training rewards or checkpoint selection. The main challenge returns nonempty records with a wrong request ID and one out-of-order pair. A contract inferred from superficial successful responses must fail the hidden verifier; the trained root should refine identity and ordering predicates. Run accepted contracts against the actual local fixture through the binary and demonstrate a proposed replacement violating one clause is rejected. Export a minimal versioned contract format usable without another prompt's runtime, and distinguish inferred assumptions from independently established obligations.

Place `SOURCE_SNAPSHOTS`, `INVOCATIONS`, and `PREDICATES` in the external environment, exposing only summaries and handles to the root RLM. Its REPL recursively delegates function inspection and counterexample search to child RLMs, stores checked clauses, and returns an `API_CONTRACT` handle. Update root weights or an adapter from independently accepted probe/propose/falsify programs; mask environment and child tokens. Freeze children, save the actual checkpoint and data lineage, and replay hidden contract tasks against the initial root using a minimal standalone interface.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/ast.c`, `src/tools.c`, `src/execution_kernel.c` through proposed `src/rlm_contract_mining.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 84 — Train minimal causal-feature discovery for failures

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`CausalReasoningSignature`, `ScientificReasoningWorkflow.forward`): Causality is requested as generated text; the workflow leaves causal analysis unimplemented rather than discovering causal features through interventions.

Teach this RLM to find the smallest useful causal feature set behind a class of resident failures. Present trace tables with candidate features, event identities, workload factors, and an owned intervention runner. The root proposes feature partitions, delegates evidence comparisons, runs permitted interventions, and minimizes a sufficient explanation while checking omitted-variable counterexamples. Represent observational association and interventional evidence separately.

Generate training tasks from bounded causal simulators and deterministic C failure fixtures with nuisance variables, interactions, redundant features, and hidden confounding. Supervise successful root sequences that choose informative interventions and eliminate spurious predictors. Training reward uses development-validation intervention prediction, parsimony, and experiment cost; reward cannot be based on training classification alone. Unsupported identifiability must return an explicit ambiguity set.

Hold out causal graph structures and nuisance-feature encodings. Never use this final split for training rewards or checkpoint selection. Challenge a fixture where a monotonic timestamp predicts every observed crash but only shared ownership causes failure. Intervene on ownership while preserving the timestamp distribution; the learned root must drop the proxy and retain the causal interaction. Add two individually harmless features whose conjunction fails to prevent overaggressive minimization. Through the live binary show the chosen intervention receipts and explanation's predictions on hidden cases. The output guides candidate-generation constraints; it does not automatically authorize source mutation or collapse association into a proven mechanism.

The root RLM receives metadata for external `TRACE_TABLE`, `INTERVENTIONS`, and `HYPOTHESES` variables. Generated REPL programs partition candidate features and recursively delegate contrast analysis to child RLMs; buffered intervention results produce a final `CAUSAL_FEATURE_SET` handle with ambiguity retained. Train real root weights or an adapter on independently verified elimination trajectories. Mask observed results and child tokens, freeze children, and save checkpoint plus task lineage. A standalone replay must compare trained and initial intervention predictions on the protected causal-family split.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/trace_kg_store.c`, `src/self_improve.c`, `src/execution_kernel.c` through proposed `src/rlm_causal_features.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 85 — Train quality-diversity search over rewrite behaviors

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer.transform`, `MetaProgrammingAgent.run`): The donor selects individual define, compose, inspect or transform actions; it maintains no behavior-space archive or learned diversity-seeking search policy.

Train a rewrite-search role whose objective is a useful portfolio of behaviors, not merely another best-score optimizer. Define archive descriptors from measured properties such as memory regime, supported input shape, latency sensitivity, and branch behavior. The root inspects underfilled niches, delegates candidate generation, selects real evaluations, and updates an archive only from independent correctness and descriptor measurements. Candidate self-reported novelty is ignored.

Build training families of bounded pure transforms with multiple valid algorithmic strategies and workload regimes. Root targets include niche selection, novelty comparison, duplicate rejection, and budget allocation. Terminal reward measures qualified archive coverage and best verified utility per niche under a frozen descriptor definition; invalid or untested candidates contribute zero. Keep training search artifacts separate from protected evaluation workloads.

Hold out algorithm families and shifted workload mixtures. Never use this final split for training rewards or checkpoint selection. Challenge a population where the fastest uniform-input implementation collapses on skewed inputs, while a slower general implementation remains robust. The learned root should retain both justified niches rather than overwrite everything with the latest champion. Compare archive coverage at matched execution budgets against random and greedy search. In the built binary query the archive with unseen workload descriptors and execute the selected candidate on owned hidden tests. Report coverage, correctness, and selection regret separately; this feature must not claim that diversity alone establishes safer live activation.

Expose `NICHE_ARCHIVE`, `CANDIDATES`, and `EVALUATIONS` externally; the root RLM sees bounded archive descriptors. Its REPL selects underfilled cells, recursively calls child RLM generators, buffers independent measurements, and returns `QUALIFIED_PORTFOLIO`. Train root weights or an adapter on accepted niche-selection and evaluation-allocation programs, preserving explicit invalid-candidate penalties in terminal scoring. Mask child/environment observations and hold child checkpoints fixed. Save the actual trained checkpoint and archive lineage; a standalone budget-matched replay compares held-out portfolio utility with the initial root.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/autoresearch.c`, `src/cost_frontier.c`, `src/ast.c` through proposed `src/rlm_diversity_search.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 86 — Train constrained Bayesian compiler-search programs

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`): Strategy selection multiplies fixed confidence values by hand-written mode weights; it does not learn acquisition decisions from compiler measurements.

This specialist learns to search bounded compiler settings for a candidate procedure using actual measurements and explicit feasibility constraints. Supply a finite or mixed configuration space, compiler identity, workload partition, correctness oracle, memory limit, and wall-clock budget. The root writes code to fit a real probabilistic surrogate, inspect uncertainty, choose constrained acquisition candidates, delegate compilation, and update from observed outcomes. Numerical optimization runs in deterministic libraries or checked local code, not model-invented posterior arithmetic.

Train on diverse small C kernels and synthetic optimization surfaces with recorded compilation failures, noise, and feasibility boundaries. Root action targets cover initialization design, fitting, acquisition evaluation, retries, and stopping. Terminal reward is verified feasible improvement minus actual evaluation cost, compared under matched budgets. Save model choices and seeds; unmeasured settings remain predictions.

Hold out entire kernel families and compiler versions. Never use this final split for training rewards or checkpoint selection. Challenge a flag combination that removes required signed-overflow semantics or exceeds memory while appearing fastest. The trained root must exclude it and choose or retain a verified feasible candidate. Include heteroscedastic timing and repeated evaluations so lucky noise does not win. Run a bounded real compiler-search episode through the binary in an isolated directory, with no install or production activation. Report training versus initial-root regret, feasibility violations, and confidence coverage without promising a speedup when the incumbent already dominates.

Store `CONFIG_SPACE`, `MEASUREMENTS`, and fitted `SURROGATE` objects outside the root RLM's prompt; supply bounded metadata. Generated REPL code evaluates numerical acquisitions and recursively asks child RLMs to inspect constraint failures, buffering results into a final `FEASIBLE_CONFIGURATION` handle. Distill independently accepted fit/acquire/stop root trajectories into actual weights or an adapter. Mask child and environment tokens, freeze leaf checkpoints, and save training lineage. Reproduce the trained-versus-initial search comparison through a standalone driver with sealed kernel holdouts.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/autoresearch.c`, `src/cost_model.c`, `src/cost_frontier.c` through proposed `src/rlm_compiler_search.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 87 — Train propensity-aware contextual exploration

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._generate_strategies`, `MetacognitiveEngine._select_strategy`): Strategies use hand-assigned confidences and fixed weights; there are no logged action propensities or counterfactual policy estimates.

Train this role to allocate bounded rewrite or diagnostic strategies under partial feedback. Each decision has a pre-action context, allowed action set, selected action, exact selection probability, policy revision, delayed reward, and outcome availability. The root executes a supported contextual-bandit algorithm, delegates reward-evidence checking, logs propensities before effects, and computes inspectable off-policy estimates. Exploration cannot include actions excluded by capability or correctness constraints.

Training episodes use controlled contextual environments and owned local procedure tasks with selection bias, delayed outcomes, and nonuniform logging policies. Root targets include exploration distribution construction, propensity validation, reward joins, and confidence-aware policy comparison. Terminal reward measures achieved constrained utility and valid estimation, penalizing missing support and fabricated probabilities. Specify clipping, effective sample size, and unsupported-action handling; inverse weighting is not magic when overlap is absent.

Hold out context generators and behavior-policy mixtures. Never use this final split for training rewards or checkpoint selection. Challenge a log where one strategy was chosen only on easy tasks; naive average reward prefers it. The root must expose selection bias and decline unsupported conclusions or produce the correctly qualified weighted estimate. Replay the trained program through the live binary using frozen logs and then run bounded local exploration with real outcomes. Compare against the initial root with identical child checkpoints. Export the learned root checkpoint separately from bandit coefficients, and verify no post-outcome context leaks into training decisions.

Keep `CONTEXT_LOG`, `POLICY_STATE`, and `PROPENSITY_LEDGER` in external variables; the root RLM sees bounded schema metadata. Its REPL constructs action distributions and recursively delegates outcome-evidence checks to child RLMs, then returns a buffered `POLICY_EVALUATION` handle. Train a real root adapter or weights on independently accepted exploration/logging/estimation trajectories, masking environment and child tokens. Freeze child checkpoints and retain policy/data lineage. A standalone replay compares trained and initial roots on protected biased-log generators, including unsupported-action rejection.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/self_improve.c`, `src/cost_model.c`, `src/event_stream.c` through proposed `src/rlm_bandit_policy.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 88 — Train pairwise ranking of rewrite candidates

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`StrategyOption`, `MetacognitiveEngine._select_strategy`): The selector sorts fixed confidence-weight products rather than learning rankings from controlled pairwise outcomes.

Build a specialist that learns how to compare candidate rewrites before spending the next verification budget. Inputs are structured candidate contracts, code or IR handles, measured pairwise outcomes, workload identities, and unresolved evidence. The root retrieves comparable pairs, recursively checks decisive differences, and emits a ranking with ties, incomparability, and required follow-up measurements. Keep correctness constraints ahead of preference ranking.

Training data must come from independent paired evaluations, not candidate-authored explanations. Include wins, losses, ties, noisy reversals, and pairs that differ in permitted tradeoffs. Train root trajectories by rejection-distillation and, if used, a separate pairwise ranking objective whose labels preserve workload scope. Training reward uses development-validation selection regret and invalid-candidate avoidance, not agreement with a model judge's prose taste. Candidate text length and source prestige should be nuisance variables.

Hold out code lineages and transformation families so near-duplicate patches cannot cross splits. Never use this final split for training rewards or checkpoint selection. Challenge a verbose rationale supporting an unmeasured candidate against a concise candidate with verified benefit; the trained root must not reward verbosity. Add nontransitive workload preferences and require context-specific ranking rather than forced universal order. Through the real binary select among locally compiled candidates and execute the selected one on hidden matched tests. Report ranking quality, abstention, and actual selected utility separately, and compare trained versus initial roots with fixed leaf evaluators.

External `PAIR_OUTCOMES`, `CANDIDATE_CONTRACTS`, and `WORKLOADS` hold the full corpus; the root RLM sees bounded indexes. Generated REPL programs retrieve comparable pairs and recursively call child RLMs for decisive checks, buffering findings into `CONTEXTUAL_RANKING`. Train root weights or an adapter from independently accepted compare/abstain/probe trajectories and scoped pairwise labels. Mask child/environment output tokens and freeze leaf checkpoints. Save model and pair-lineage artifacts; the standalone evaluator measures held-out selection regret against the initial root, not stylistic agreement.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/cost_frontier.c`, `src/plan_optimizer.c`, `src/self_improve.c` through proposed `src/rlm_rewrite_ranker.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 89 — Train compositional task generation from capability grammars

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.compose`): Composition chains named callables but does not generate typed challenge distributions or verify task solvability from capability grammars.

Train this role to manufacture useful learning tasks for a resident runtime from a typed capability grammar. Productions describe input/output types, effects, resource bounds, preconditions, and oracle construction. The root chooses a compositional structure, delegates local oracle synthesis, samples witnesses, and stores both a public task and a sealed solution or property checker. Generated tasks are not allowed to broaden capabilities.

Construct training episodes from bounded parser, arithmetic, record-transform, and stateful-protocol primitives. Root action targets include type checking, witness generation, negative-case creation, difficulty estimation, and duplicate filtering. Terminal reward combines independent solvability, discriminating power against known defective candidates, and structural novelty. Reject tasks with leaked answers, impossible constraints, or difficulty caused only by enormous inputs.

Hold out grammar production combinations and task templates rather than just random seeds. Never use this final split for training rewards or checkpoint selection. Challenge a generator that composes a filesystem write with a read-only capability envelope, and another requiring a sorted permutation with a contradictory fixed first element. The learned root must reject both before adding them to a curriculum. A valid unseen composition should produce executable positive and negative oracles, then reveal a planted defect in the live binary's local fixture. Report generated-task validity and defect discrimination under a fixed candidate pool. This role generates training environments; it does not replace the independent referee or use its own subjective difficulty rating as terminal truth.

Put `CAPABILITY_GRAMMAR`, `TASK_POOL`, and sealed `ORACLES` in the external environment. With only bounded metadata, the root RLM programs REPL grammar expansion and recursively calls child RLMs for witness and checker construction, buffering validated tasks before returning `TASK_BATCH`. Train actual root weights or an adapter on independently accepted generate/check/reject trajectories. Mask environment and child tokens; freeze children and protect oracle holdouts. Save the checkpoint and grammar lineage, then compare trained versus initial task validity through a standalone generation/evaluation driver.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/prompt_program.c`, `src/plan_dag.c`, `src/tools.c` through proposed `src/rlm_task_grammar.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 90 — Train adversarial coevolution with a protected referee

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`DebuggedReasoningSignature`, `HybridReasoningPipeline.forward`): Bias critique is generated text and final synthesis remains the original multi-strategy synthesis; no adversary learns tests against an independent executable referee.

Train a counterexample-generating RLM role that coevolves with candidate procedures while the acceptance oracle remains fixed. The root inspects a candidate's public contract and bounded behavior samples, delegates attack-family exploration, constructs executable test inputs, and submits them to an independent referee. The referee determines input validity, reference behavior, candidate behavior, and whether a genuine contract violation occurred.

Training episodes pair evolving parser, queue, and numeric candidates with historical defect families. Root trajectories include hypothesis selection, input construction, shrinking, and duplicate-counterexample rejection. Reward valid distinct violations and compact reproducible witnesses, subtract execution cost, and assign no reward to undefined inputs or harness failures. Keep referee code and hidden acceptance cases outside both candidate and adversary training contexts; recursive children receive only task-scoped subsets.

Hold out defect mechanisms and candidate lineages. Never use this final split for training rewards or checkpoint selection. Challenge an adversary that exploits timeout settings, malformed oracle inputs, or redefines expected output to claim a win. Those attempts must be rejected even when the candidate visibly fails. Then require discovery and minimization of a valid overflow or cancellation counterexample in a real locally compiled fixture through the binary. Compare discovery rate at equal budgets with the initial adversary root and fixed leaves. Preserve a non-adaptive final holdout so reciprocal overfitting between generator and candidate is measured rather than celebrated as progress.

Expose external `PUBLIC_CONTRACT`, `CANDIDATE_SAMPLES`, and `ATTACK_BUFFER` handles; never expose sealed referee internals. The root RLM generates REPL attack programs, recursively delegates attack families to child RLMs, and returns a verified `COUNTEREXAMPLE_SET` handle. Train root weights or an adapter on independently rewarded construct/test/shrink trajectories. Referee outcomes enter observations, not supervised output targets; mask child/environment tokens and freeze leaves. Save checkpoints and candidate-lineage splits. A standalone held-out contest compares initial and trained adversaries without allowing either to rewrite the referee.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/autoresearch.c`, `src/blackboard.c`, `src/execution_kernel.c` through proposed `src/rlm_adversary.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 91 — Train executable invariant discovery with held-out checking

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`InductiveReasoningSignature`, `DeductiveReasoningSignature`): The signatures request laws and validity labels from an LM; they do not discover executable predicates and verify them against unseen transitions.

This role learns candidate invariants for resident procedures from state-transition traces and a bounded transition runner. Specify an expression language with typed variables, integer widths, interval relations, equality, and selected linear constraints. The root partitions traces, delegates predicate proposals, executes initiation and preservation checks, and refines predicates using actual counterexamples. Distinguish empirical invariants from properties proved within an explicitly bounded model.

Generate training tasks from loops, parsers, and resource-state machines with known hidden invariants and deliberate near-invariants. Supervise root actions that propose, test, weaken, or split scope conditions. Terminal reward requires usefulness for rejecting invalid rewrites plus soundness on independent transition checks; tautologies and always-false predicates receive no useful-invariant credit. Record overflow and alias assumptions in every artifact.

Hold out program templates and state-range boundaries. Never use this final split for training rewards or checkpoint selection. A counter increases monotonically on training traces but wraps under fixed-width arithmetic; the learned root must find the boundary probe and reject unrestricted monotonicity. A second fixture has a valid bounded invariant requiring a precondition, which should be retained with that scope. Run the inferred predicate and transition checker through the actual binary, including an unseen replacement violating it. Save counterexamples and executable predicates with revision identities. Compare initial and trained roots under equal probe budgets; do not advertise formal proof when validation only sampled states.

The external context contains `STATE_TRACES`, `TRANSITION_RUNNER`, and `PREDICATE_POOL`; root-RLM metadata stays bounded. REPL programs recursively assign predicate families to child RLMs, buffer initiation/preservation checks, and return `CHECKED_INVARIANTS`. Rejection-distill independently accepted propose/falsify/weaken programs into actual root weights or an adapter. Mask child and environment tokens, freeze child checkpoints, and preserve counterexample lineage with the trained checkpoint. The standalone evaluation compares useful invariant discovery against the initial root on sealed templates, separating empirical support from bounded proof.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/ast.c`, `src/vm.c`, `src/execution_kernel.c` through proposed `src/rlm_invariant_discovery.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 92 — Train error-cluster curriculum scheduling

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._evaluate_strategy_adjustment`): Adjustment checks a tool success-rate threshold and emits generic alternatives; it does not learn curriculum choices from structured error clusters.

Train a curriculum-specialist RLM to choose the next bounded learning batch for resident procedure improvement. Its external dataset contains failure signatures, minimal counterexamples, workload families, candidate revisions, prior interventions, retention checks, and observed post-training changes. The root clusters by executable failure characteristics, delegates representative-case analysis, estimates which cluster is learnable now, and selects examples under explicit severity and retention constraints.

Generate episodes where cluster frequency, severity, novelty, and learning responsiveness differ. Root targets cover duplicate suppression, cluster refinement, representative selection, and evaluation-budget allocation. Terminal reward is independently measured improvement across a separate development-validation task distribution after the selected batch, minus regressions and consumed compute. Do not train solely against an internal “learning progress” score or allow the scheduler to redefine the evaluation population.

Hold out error taxonomies and curriculum orderings. Never use this final split for training rewards or checkpoint selection. Challenge a corpus with thousands of duplicates of one easy issue and a small severe ownership-bug cluster. The trained root must avoid frequency-only allocation while retaining coverage of old capabilities. Include an unlearnable noisy cluster so endless repetition is penalized. Through the live binary select a real local batch, apply a bounded learning update to the owned target fixture, and evaluate withheld clusters. Log scheduler-root actions separately from target-model updates; report any negative learning outcome honestly. This extends simple staged curricula with learned error-conditioned scheduling, not another static checklist.

Maintain external `ERROR_CLUSTERS`, `TRAINING_HISTORY`, and `RETENTION_RESULTS` variables; expose counts and handles to the root RLM. Its REPL recursively delegates representative-case analysis to child RLMs and buffers selections into `NEXT_LEARNING_BATCH`. Train actual scheduler-root weights or an adapter from independently evaluated select/update/measure episodes, with target-learner updates logged separately. Mask child/environment tokens, freeze interpretation leaves, and save both checkpoint identities and batch lineage. A standalone held-out curriculum replay compares trained and initial schedulers at matched target-training budgets.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/self_improve.c`, `src/trace_kg_store.c`, `src/autoresearch.c` through proposed `src/rlm_error_curriculum.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 93 — Train abstract-interpretation-guided candidate bounding

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer.parse`, `ASTAnalyzer._extract_structure`): The AST analyzer extracts declarations and syntax structure; it does not compute abstract states or bound candidate execution.

Build a specialist root RLM that learns to direct a trusted abstract interpreter over a bounded candidate IR. The interpreter, not the model, computes interval or constant domains, transfer functions, joins, widening, and explicit unknown results. The root chooses entry assumptions, splits problematic paths, asks children to inspect candidate refinements, and submits bounded analysis jobs. Its final handle contains checked state bounds, unsupported operations, and the exact assumptions needed for soundness.

Train on generated integer loops, buffer indexes, and state machines with reference reachable-state sets for small instances. Root action targets include domain selection, path partitioning, widening thresholds, and counterexample-guided refinement. Terminal reward balances independently checked soundness, useful precision, and analysis cost. Unsound bounds incur failure regardless of speed; returning unknown everywhere cannot earn precision credit.

Hold out loop shapes and arithmetic edge cases. Never use this final split for training rewards or checkpoint selection. Challenge a branch that multiplies a counter only under a rare input, plus fixed-width overflow that invalidates an intuitive range. The trained root must return a sound bound or explicit unknown, never infer safety from successful traces. Execute the abstract checker through the built binary and compare bounds against exhaustive small-state enumeration. Then reject a candidate buffer access outside its established range. Freeze analysis semantics during root training and keep initial-versus-trained comparisons at identical child and interpreter versions; learning improves analysis orchestration, not the definition of correctness.

Expose `CANDIDATE_IR`, `ABSTRACT_STATES`, and `CHECKER_RESULTS` externally, with bounded metadata for the root RLM. Generated REPL programs recursively ask child RLMs for path refinements, then call the trusted numerical interpreter and return a `BOUNDS_CERTIFICATE` handle. Train actual root weights or an adapter on independently sound domain/split/refine trajectories. Mask checker and child observations; freeze interpreter semantics and child checkpoints. Save model/data lineage and use a standalone held-out replay to compare precision and cost against the initial root without weakening soundness.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/ast.c`, `src/vm.c`, `src/wasm_core.c` through proposed `src/rlm_abstract_bounds.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 94 — Train semantic indexing by executable behavior

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.list_tools`, `MetaToolRegistry.get_ast`): Registry discovery exposes names, descriptions and AST metadata; it does not index procedures by independently observed input/output behavior.

Train this RLM to find reusable procedures by their actual behavior rather than source vocabulary. Store revisioned procedures, typed input generators, output observations, effect summaries, and probe coverage in an external corpus. The root chooses discriminating probes, recursively compares promising families, builds measured behavioral signatures, and retrieves candidates satisfying a target contract. Approximate matches retain explicit uncertainty and unresolved distinguishing tests.

Create training tasks with renamed equivalent implementations, deceptively similar names, boundary-only differences, and stateful versus pure procedures. Root targets include signature construction, probe reuse, candidate elimination, and exact evidence retrieval. Terminal reward measures correct retrieval on hidden behavioral distinctions and minimized execution cost, penalizing false equivalence. A behavioral fingerprint is an index key, not a proof of full semantic equivalence.

Hold out algorithm families, names, and input-generator combinations. Never use this final split for training rewards or checkpoint selection. Challenge two “normalize” procedures that agree on ordinary records but differ on empty input and mutate caller state differently. The trained root must choose boundary and effect probes before proposing substitution. A renamed genuinely equivalent procedure should still be found. Through the live binary ingest locally executed observations, answer a target-contract query, and run the remaining checks on the retrieved candidate. Compare lexical retrieval with trained-root behavioral retrieval under a fixed probe budget. Return procedure and evidence handles rather than generated descriptions, and never replace the active implementation based only on index similarity.

Keep `PROCEDURE_CORPUS`, `PROBE_RESULTS`, and `BEHAVIOR_INDEX` outside the root prompt. The root RLM sees bounded descriptors, writes REPL discriminating probes, and recursively asks child RLMs to compare candidate families; checked results accumulate behind a final `MATCH_SET` handle. Train root weights or an adapter on independently accepted probe/eliminate/retrieve programs, masking environment and child tokens. Freeze leaf checkpoints and retain probe lineage. Save the trained checkpoint and reproduce held-out retrieval against the initial root through a standalone behavioral-index driver.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/skill_index.c`, `src/semantic.c`, `src/context_fabric.c` through proposed `src/rlm_behavior_index.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 95 — Train uncertainty-aware surrogate performance reasoning

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`ProbabilisticReasoningSignature`, `ScientificReasoningWorkflow.forward`): The workflow passes fixed prior 0.5 and placeholder likelihood 0.8; it does not fit performance-response models or evaluate their uncertainty on observations.

Train a performance-modeling RLM role that builds actual conditional surrogates from candidate measurements. External tables include code descriptors, input size, distribution, concurrency, hardware identity, compilation flags, latency samples, memory, and failures. The root chooses comparable records, fits a supported numerical model, delegates residual diagnosis, and produces predictions with tested uncertainty and an explicit extrapolation flag. Generated numerical confidence is not a substitute for fitting and validation.

Training episodes should vary heteroscedastic noise, discontinuities, missing outcomes, and hardware shifts. Root targets include feature construction, model fitting, residual partitioning, interval checks, and requests for informative measurements. Training reward uses independently scored development-validation prediction error, interval coverage and sharpness, plus measurement cost. Include failed candidates in the appropriate outcome model instead of fitting only successful runs.

Hold out kernel families and size regimes. Never use this final split for training rewards or checkpoint selection. Challenge a smooth small-array fit confronted with a cache-thrashing threshold; the trained root must widen uncertainty or request measurement rather than confidently extrapolate a speedup. Add a hardware change and censored timeout observations. Through the binary run a bounded local measurement fixture, fit the surrogate, and compare hidden predictions with actual timings. Report initial-root versus trained-root modeling quality with frozen leaves. This is conditional response modeling and learned diagnostic orchestration, distinct from a reference-class average or a complete Bayesian optimization controller.

Use external `MEASUREMENT_TABLE`, `FIT_OBJECTS`, and `RESIDUALS` variables rather than inline performance histories. The root RLM receives bounded schemas, writes REPL fitting code, recursively delegates residual partitioning to child RLMs, and returns a buffered `PREDICTIVE_MODEL` handle. Train actual root weights or an adapter on independently scored select/fit/diagnose/measure trajectories. Mask child/environment tokens and freeze leaf checkpoints. Preserve trained-model and numerical-surrogate identities separately; a standalone held-out driver compares interval quality and measurement cost against the initial root with full data lineage.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/cost_model.c`, `src/learned_cost.c`, `src/cost_frontier.c` through proposed `src/rlm_performance_surrogate.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 96 — Train amortized experiment-selection policies

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._plan_immediate_actions`, `MetacognitiveEngine._plan_next_steps`): Immediate actions take the first few selected tools and next steps are generic strings; no policy is learned from experiment outcomes.

Train a specialist that amortizes the choice of useful self-investigation experiments across prior episodes. Inputs are competing hypotheses, available safe probe contracts, accumulated observations, costs, and a decision objective. The root inspects relevant episode buffers, delegates uncertain outcome interpretation, and selects a probe or stop action. Learn decomposition and probe choice from consequences; do not merely wrap an analytic value-of-information calculation in another prompt.

Construct episodic training environments with known latent faults and real local diagnostic fixtures. Record state before selection, admissible probes, action probabilities when stochastic, outcomes, and final diagnosis quality. Root trajectory targets favor experiments that distinguish remaining hypotheses. Terminal reward combines correct final decision, avoided unnecessary probes, and actual budget usage, with failures for unauthorized actions or unsupported certainty. Use independently accepted episodes for root SFT; optional verifiable reinforcement learning may optimize the same terminal task reward.

Hold out fault mechanisms, probe names, and cost schedules. Never use this final split for training rewards or checkpoint selection. Challenge a cheap probe that was historically useful but is conditionally uninformative for the current hypothesis pair; the trained root should select a different discriminant or stop. Include an expensive perfectly informative probe whose cost exceeds benefit. Execute selections against an owned fixture through the live binary and compare total diagnostic regret with the initial root and an analytic baseline. Keep child interpretation fixed to attribute gains to learned root action policy, and save failed episodes without converting them into positive targets.

The root RLM operates on external `EPISODES`, `HYPOTHESIS_STATE`, and `PROBE_MENU` variables through bounded metadata. Its generated REPL code recursively asks child RLMs to interpret selected observations and buffers updates before returning `EXPERIMENT_POLICY_RESULT`. Train real root weights or an adapter on independently successful select/observe/stop trajectories; optional terminal-reward updates must preserve the same oracle. Mask child/environment tokens, freeze leaf checkpoints, and save propensity/data lineage. Compare the trained checkpoint with the initial root through a standalone held-out diagnostic environment.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/plan_optimizer.c`, `src/self_improve.c`, `src/execution_kernel.c` through proposed `src/rlm_probe_policy.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 97 — Train hierarchical skill-option induction

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack`, `CognitiveArchitecture._action_executor`): Actions have dependencies and completion status, but there is no learned initiation set, reusable subpolicy or termination condition for a temporal option.

Train an RLM role to discover temporally extended skills from successful and failed reasoning-action episodes. An option contains an initiation predicate, typed arguments, bounded subpolicy, termination predicate, failure exits, and effect requirements. The root segments trajectories, delegates boundary comparisons, proposes shared subpolicies, and executes option candidates on changed tasks. Preserve outcome identity and distinguish a useful option from a merely frequent action subsequence.

Training tasks should include diagnostics, parsing repairs, and bounded protocol workflows with alternative paths and misleading repeated prefixes. Root targets cover segmentation, parameter extraction, initiation refinement, and termination testing. Terminal reward measures compositional task success, reuse across development-validation instances, and reduced decision overhead while penalizing unsafe initiation and nontermination. Options remain candidate executable procedures; training does not grant new capabilities.

Hold out task compositions and episode lineages. Never use this final split for training rewards or checkpoint selection. Challenge an apparent “retry until success” segment where retries consume a finite token and the caller can cancel. The learned option must include a retry bound, token precondition, and cancellation exit. A superficially matching task with non-idempotent effects must reject initiation. Through the live binary execute an induced option on an unseen owned workflow and inspect actual initiation, child calls, and termination receipts. Compare against a flat baseline with identical leaf checkpoints and tool observations. Store the learned root checkpoint separately from the emitted option library and report whether the learned abstraction transfers rather than only replaying its training sequence.

Expose external `EPISODE_CORPUS`, `OPTION_CANDIDATES`, and `EXECUTION_RESULTS` handles to the root RLM. Its REPL recursively delegates boundary comparisons to child RLMs, tests initiation and termination, buffers qualified options, and returns `OPTION_LIBRARY`. Train root weights or an adapter from independently accepted segment/parameterize/test trajectories. Mask child and environment tokens, freeze leaf checkpoints, and save model lineage separately from emitted options. A standalone held-out composition evaluator compares the trained root with its initial checkpoint under equal execution budgets.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/plan.c`, `src/plan_dag.c`, `src/skill_candidate.c` through proposed `src/rlm_skill_options.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 98 — Train online concept-drift segmentation

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._evaluate_strategy_adjustment`): Strategy adjustment uses aggregate tool success-rate thresholds; it does not infer changing regimes or distinguish drift from isolated failures.

Train a drift-specialist RLM for resident learning systems whose behavior changes without a file or dependency update. Feed time-ordered performance and error observations, workload covariates, missingness flags, and policy revisions as external buffers. The root chooses bounded windows, runs real change-point or sequential-test calculations, delegates segment explanations, and proposes retain, split-regime, collect-more-data, or relearn actions. Separate changed input mix from changed conditional behavior.

Generate training streams with abrupt and gradual drift, recurring regimes, seasonal patterns, censoring, and isolated incidents. Root trajectory targets include window selection, test calibration, covariate conditioning, and delayed confirmation. Terminal reward uses development-validation change detection delay, false alarms, segment predictive quality, and cost of unnecessary relearning. The root cannot retroactively revise timestamps or use future observations during an online decision.

Hold out regime generators and change schedules. Never use this final split for training rewards or checkpoint selection. Challenge a single huge latency spike followed by recovery, then a sustained workload shift that alters the preferred procedure. The trained root must avoid the first false alarm and detect the second with evidence. Add a pure covariate shift where the conditional model still holds and require a different response. Replay the stream chronologically through the real binary, asserting identical causal decisions after checkpoint restart. Report drift detection separately from dependency-expiry logic and confidence calibration. Freeze children and numerical tests while comparing trained and initial roots; any adaptation proposal remains scoped to the affected regime.

External `OBSERVATION_STREAM`, `SEGMENT_STATE`, and `TEST_RESULTS` hold the ordered history; the root RLM receives bounded current metadata only. REPL programs run sequential tests and recursively delegate segment explanations to child RLMs, buffering causal decisions into `REGIME_MAP`. Train actual root weights or an adapter on independently scored window/test/confirm trajectories, masking child/environment observations. Freeze test semantics and leaf checkpoints; retain time-split lineage. A standalone chronological holdout replay compares the trained checkpoint with the initial root without exposing future samples.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/self_improve.c`, `src/cost_model.c`, `src/event_stream.c` through proposed `src/rlm_drift_segmentation.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 99 — Train compression-driven reusable procedure libraries

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.compose`, `ASTAnalyzer.transform`): Composition joins named callables and transformations are hand-authored; the donor does not extract a reusable library by jointly compressing many programs.

Train this role to extract a reusable code library from many verified reasoning procedures, rather than distilling one successful episode. The external corpus contains typed program graphs, execution contracts, examples, and failure cases. The root identifies repeated structure, delegates parameterization candidates, rewrites programs against proposed abstractions, and checks equivalence before scoring compression. Preserve variable binding, ownership, effect order, numeric semantics, and applicability conditions.

Define a transparent minimum-description-length objective including library definitions, call sites, exception cases, and validation cost. Training tasks use generated program families with genuine shared combinators, accidental textual repetition, and misleading constants. Root targets cover anti-unification, abstraction selection, counterexample handling, and library pruning. Terminal reward combines verified corpus compression and development-validation reuse; changing program behavior or hiding data inside oversized constants invalidates a candidate.

Hold out whole program families and renamed structural variants. Never use this final split for training rewards or checkpoint selection. Challenge a library that compresses visible examples by baking in their answers, plus a shared operation that incorrectly reorders an effectful read. Both must fail independent checking. A valid abstraction should reduce description length across unseen compatible procedures and preserve outputs on hidden inputs. Execute rewritten programs through the live binary's bounded procedure interface and compare with originals. Report compression and runtime overhead separately. Save the trained root checkpoint, learned library, reconstruction manifest, and evidence hashes independently so a useful library is not mistaken for proof that the model's weights improved.

Place `PROGRAM_CORPUS`, `LIBRARY_CANDIDATES`, and `EQUIVALENCE_RESULTS` outside the root-RLM prompt; show bounded indexes. Its REPL recursively delegates anti-unification candidates to child RLMs, checks reconstruction, buffers qualified abstractions, and returns `COMPRESSED_LIBRARY`. Train real root weights or an adapter on independently accepted propose/rewrite/check/prune trajectories, masking child/environment tokens. Keep leaf checkpoints fixed and save model, library, and reconstruction lineage separately. A standalone held-out evaluator compares trained and initial compression at equal checking budgets, with semantic failures excluded from reward.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/ast.c`, `src/prompt_program.c`, `src/skill_index.c` through proposed `src/rlm_library_compression.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 100 — Train transfer attribution and negative-transfer prevention

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`AnalogicalReasoningSignature`): Analogy requests similarities and inferred features from an LM; it does not measure whether transferred learning improves independent target tasks.

Train a transfer-specialist RLM to decide which learned procedures, examples, or adapters should influence a new resident task. Inputs include source-task contracts, target preconditions, candidate transferable components, training histories, and a budgeted target evaluation interface. The root proposes a transfer, delegates mismatch analysis, runs paired target experiments with and without the component, and attributes benefit or harm under controlled training and execution conditions.

Construct training episodes with positive transfer, irrelevant transfer, and negative transfer across parser, numerical, and stateful task families. Root targets include source selection, ablation design, matched seed allocation, outcome comparison, and selective rejection. Terminal reward measures independent development-family target improvement while retaining source capabilities and controlling added cost. Prevent leakage by splitting task lineages before generating transfer pairs; similarity labels cannot substitute for measured transfer effects.

Hold out source-target family pairings. Never use this final split for training rewards or checkpoint selection. Challenge a caching skill that improves pure deterministic transforms but corrupts a stateful counter despite matching input/output schemas. The trained root must design the stateful ablation, identify the negative transfer, and exclude that component rather than discarding all source knowledge. Include a beneficial component obscured by a harmful companion to test component-level attribution. Through the real binary run a bounded target fixture under matched transferred and baseline conditions, report uncertainty and regressions, and verify initial-versus-trained root comparisons use fixed leaves. Emit an applicability decision and evidence handles; do not automatically merge adapters or activate a resident rewrite from analogy alone.

Keep `TRANSFER_COMPONENTS`, `TARGET_EPISODES`, and `ABLATION_RESULTS` in external variables. The root RLM sees bounded contract metadata, writes REPL paired experiments, recursively delegates mismatch analysis to child RLMs, and returns a buffered `TRANSFER_APPLICABILITY` handle. Train actual root weights or an adapter on independently evaluated select/ablate/attribute/reject trajectories. Mask child/environment tokens and freeze leaves; save model checkpoints, seeds, and source-target lineage. A standalone protected-family evaluation compares trained and initial transfer decisions without letting source similarity count as measured benefit.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree; integrate `src/self_improve.c`, `src/skill_index.c`, `src/autoresearch.c` through proposed `src/rlm_transfer_attribution.c`. Preserve dirty work. Tool effects use `tools_execute_for_tier()`. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install.

---

# 101 — Give every role an independently trained RLM identity

Every DSCO role should resolve to a particular trained recursive policy, not merely a role name in a prompt. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, evolve the static role idea in `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/config.py`'s `ModelSpec` and `MODEL_SPECS`. Those objects select model settings; they do not establish that any role learned new weights. Use the [RLM paper](https://arxiv.org/html/2512.24601v3) to distinguish the role's root policy from its recursive leaves.

Create a bounded role-model registry whose entry binds role contract, immutable base revision, tokenizer/renderer digest, root checkpoint or adapter tensor digest, frozen leaf policy, REPL protocol version, curriculum split, training recipe, and evaluation result. Keep training state, candidate state, and active serving identity distinct. Shared base weights are allowed, but each trained role needs its own adapter lineage and compatibility checks. Registering a file cannot itself certify training or measured improvement. Verify tensor metadata, parent relationships, and an optimizer-step receipt against the actual produced artifact.

Implement new registry logic with small hooks in `src/agent_profile.c`, `src/provider_profiles.c`, and `src/self_improve.c`. A proposed `dsco role-model inspect/register` interface must work independently; define a minimal manifest and local checkpoint-inspection adapter if no trainer integration exists. Register a role trained to emit external-context inspection and finalization code from executable task trajectories. The trainable output is its root LoRA or equivalent real parameter delta, while prompts remain fixed during the comparison.

Acceptance requires a bounded local training witness or already authorized device run: demonstrate nonzero finite tensor deltas, unchanged frozen base/leaf tensors, and a reloadable role artifact. An independent held-out generated-task oracle checks execution results before and after training; report no gain honestly. Reject a renamed untouched base, mismatched tokenizer, cyclic lineage, and an adapter belonging to another role. Through the real DSCO binary, resolve two roles concurrently and prove each request carries the correct immutable root identity. Include exact training budget, artifact checks, and evaluation partition in the closeout.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 102 — Implement the external-context RLM execution contract

An RLM role must manipulate its input as an external object even when that input is larger than its neural context. Implement the [paper's Algorithm 1 contract](https://arxiv.org/html/2512.24601v3) inside `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree: persistent context variable, executable recursive calls from generated code, bounded root observations, and final output returned from environment state. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/core.py`'s `CognitiveAgentProgram` and `format_history()` instead pass rendered text into the model; preserve that mode separately.

Define a role session containing the full immutable input, mutable intermediate values, a finalization slot, and host-mediated recursive-call functions. The root initially sees metadata and a bounded preview. Each generated program executes with explicit resource limits and returns bounded stdout metadata; intermediate arrays and long child outputs remain addressable in the environment. Finalization must return the selected variable's complete bounded artifact without forcing the model to autoregressively repeat it. Specify exact behavior for undefined variables, exceptions, cancellation, and repeated finalization.

Add a narrow runtime bridge near `src/agent_interop.c` and `src/agent.c`; use `src/vm.c` only if its semantics satisfy the contract, otherwise implement a controlled subprocess REPL adapter. Every effectful host call crosses `src/tools.c`. A minimal local protocol and context-handle implementation must make this independently runnable. Generated Python is not a sandbox; execution must respect the outer worker isolation.

Collect root code/observation trajectories for deterministic corpus transformations, then train a small role adapter to inspect slices, retain intermediate values, and assign the final result. Require an actual optimizer update and reload proof, not a prompt-only demonstration. Through the real binary, solve a fixture exceeding the root context, produce a final variable longer than one model output budget, and verify exact bytes with an independent oracle. Inject enormous stdout, an invalid final handle, and recursion attempts exceeding policy. Report root-visible token growth, full-output integrity, training tensor changes, and whether held-out execution improved.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 103 — Train programmatic recursion within role boundaries

Teach each role to express recursive work as executable programs while the host owns authority and lifecycle. In `/Users/arthurcolle/Dsco/dsco-cli` or its worktree, inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py`'s `FourModelHierarchicalSwarm._route()` and `_run_child()`. They route predefined subtask objects; the new capability follows [RLM symbolic recursion](https://arxiv.org/html/2512.24601v3), where generated code can loop over context slices and invoke recursive policies directly.

Define a recursive request containing parent session, role contract, selected context transformation, root/leaf model revisions, depth, and inherited resource/effect limits. The host assigns child identity and rejects any widening of role authority. Calls return values or futures to the REPL, not raw control over another agent's session. Preserve deterministic mapping from programmatic call index to child result across out-of-order completion. A failed child is a typed observation, never an empty successful answer. Explicitly distinguish a leaf LM from a child RLM with its own environment.

Implement a new recursion broker using small integration hooks in `src/swarm.c`, `src/agent_profile.c`, and `src/agent.c`, with governed effects through `src/tools.c`. If the external-context interface is absent, include a minimal session variable store and one host-call primitive sufficient to run a loop; do not require another feature prompt. Training data consists of verified decomposition programs plus actual child observations. Train a role-specific root adapter on those programs while freezing the child policy and role rules.

Exercise the actual binary on a generated repository-indexing task whose correct result requires inspecting every partition. Demonstrate the trained adapter issuing calls from a loop and aggregating actual child results; provide finite tensor deltas and reloaded checkpoint identity. An independent index oracle checks completeness and duplicates. Inject a child requesting a more privileged role, swapped child results, depth overflow, and one timed-out partition. None may become a successful complete index. Compare a held-out partition layout before and after the update, recording correctness, call count, and role-boundary denials without rewarding needless recursion.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 104 — Export faithful root-turn training trajectories

Training a role's RLM requires examples of what its root actually saw and generated at each decision. Build a faithful exporter in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/decomposition.py`'s `_inference_receipt()` records usage evidence; it does not produce replayable learning samples. The [RLM paper's training appendix](https://arxiv.org/html/2512.24601v3) motivates root-turn examples, with recursive child work treated as environment observations.

Export one sample per selected root generation: the exact preceding model-visible history as input and only that generation as the supervised target. Preserve role/model/tokenizer identity, sampling settings, code bytes, bounded stdout observations, recursive-call references, terminal oracle result, and environment revision. Never reconstruct absent content from summaries. Child generations are separately attributable and excluded from root targets unless an explicitly different leaf-training recipe is selected. Failed, censored, incomplete, and accepted trajectories require distinct labels. Exclude future observations and post-hoc corrections from earlier prefixes.

Add a new exporter module with small hooks around `src/agent.c`, `src/provider_events.c`, and existing `src/chronicle.c` events. Do not duplicate long-context evidence storage; consume immutable handles. A proposed `dsco role-data export` command must accept a minimal explicit trajectory JSON format when existing events lack required fields, and report missing fidelity instead of guessing. Produce prompt/completion records that downstream trainers can render without accidentally re-training every prior assistant turn.

Prove the data path with a tiny real root-adapter SFT run on exported executable corpus-transformation trajectories. Record actual trainable tensor changes and reload the artifact; this witness verifies usability, not broad learning efficacy. Through the worktree binary, export a nested episode with two roots and several child calls, then compare every prefix and target to captured requests byte-for-byte. An independent replay oracle verifies final results. Reject a future-result leak, a child message relabeled root, and a truncated generation claimed complete. Report exported counts by disposition, exact target ownership, and a held-out execution score from the trained role.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 105 — Distill only executable teacher trajectories

A stronger model can teach a role to operate its REPL, but its confident transcript is not sufficient supervision. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, implement executable teacher rejection distillation. Contrast `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py`'s `swarm_metric()`, which can reward an answer substring, with the [RLM paper's filtered root-trajectory training](https://arxiv.org/html/2512.24601v3). Adopt the mechanism, not its reported gains as a promise.

Generate a bounded number of teacher root trajectories per role task with a fixed leaf model, environment, and resource ceiling. Replay the generated programs in fresh task environments and score complete outputs using an oracle that the teacher cannot edit. Classify correct execution, partial credit under a declared rubric, incorrect result, invalid program, and infrastructure failure separately. Deduplicate near-identical roots and retain diverse successful decompositions rather than the most verbose samples. Keep rejected trajectories available for analysis but out of positive SFT targets; never invent missing execution evidence.

Implement a collection/filtering adapter beside `src/agent.c` and `src/provider.c`, then attach an explicit dataset-to-role-training handoff near `src/self_improve.c`. External training can use a small local Python worker under a defined protocol; the C runtime remains responsible for role and artifact identity. If no RLM collector exists, provide a minimal external-context harness with programmatic subcalls for a deterministic task family. The trained output must be a new root checkpoint or adapter with actual updated tensors; keep the teacher and leaf frozen.

Acceptance runs the real binary over generated record-selection tasks with independently computed expected records. Include a teacher response that prints the right answer but never sets the required final variable, code that depends on contaminated state, and forged child observations. Reject all three under the declared contract. Train on the accepted subset within a stated device/step budget, reload the resulting role model, and evaluate held-out task families, new seeds, and longer layouts. Report tensor deltas, acceptance rates, actual executable accuracy, and comparison to unfiltered distillation. Do not claim success merely because training loss decreases.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 106 — Align root SFT targets with actual token boundaries

Incorrect loss masks can train a role to imitate tool output or repeatedly memorize earlier root turns. Implement token-faithful root SFT preparation in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py`'s `save_program()`/`load_program()` persist DSPy artifacts; they do not establish neural training semantics. Follow the root-turn distinction in the [RLM paper](https://arxiv.org/html/2512.24601v3) and inspect the pinned trainer's renderer contract before coding.

Take each exact root history and single generated target through the target model's official tokenizer and renderer. Record token IDs, attention boundaries, target span, ignored positions, stop tokens, and renderer revision. Use prompt/completion records when that is how the pinned trainer masks prior history; avoid also supplying messages if messages takes precedence. Tool observations, child answers, system text, and earlier root generations must have zero loss for this sample. Templates that transform earlier reasoning text require renderer-owned alignment, not string offsets patched after tokenization.

Add a new dataset-validation adapter with integration points at `src/provider_events.c`, `src/agent.c`, and `src/agent_interop.c`. If no event exporter exists, accept a minimal local sample carrying explicit root/child ownership. Expose a proposed `dsco role-data inspect-tokens` surface showing redacted token attribution and validation errors. Training output is a role-root adapter updated on the selected code tokens; a program JSON file or changed prompt is not an acceptable substitute.

Use the actual DSCO binary to validate samples containing Unicode, escaped code, tool JSON with literal assistant markers, an empty finalization, and a multi-turn recursive episode. Compare masks with an independent token-attribution oracle using a small reference renderer for the chosen supported template. Run a bounded actual optimizer step and inspect gradients: intended adapter parameters change, while tokens outside the target contribute no supervised loss and frozen tensors remain unchanged. Inject a one-token shift and duplicate earlier targets; both must fail before training. Reload the artifact and test executable finalization on unseen examples, reporting exact mask counts and parameter-update evidence.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 107 — Expose each role as a verifiable RLM environment

Make each role trainable against the same executable contract used to judge its deployed behavior. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, build a `verifiers` environment/harness adapter for a role RLM. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py`'s `evaluate()` calls a prediction metric; the new environment evaluates real recursive execution. Consult the [RLM training implementation](https://github.com/alexzhang13/rlm/tree/main/training) and [paper](https://arxiv.org/html/2512.24601v3), pinning mutually compatible upstream revisions rather than assuming current packages interoperate.

Define a task with role contract, external context payload, allowed operations, deterministic seed, and hidden expected result. The harness launches the DSCO role session, supplies the payload as environment state, drives root generations and recursive calls, and returns terminal outcome, usage, and the exact trainable root trajectory. The oracle runs outside generated code's writable environment. Separate invalid protocol, infrastructure failure, budget exhaustion, and incorrect answer from success. Include an explicit task-family version and split identity so a trainer cannot silently substitute another scoring contract.

Implement the C bridge around `src/agent_interop.c`, exposing governed role effects through `src/mcp_server.c` and `src/tools.c`. Keep Python framework glue in a bounded adapter package rather than importing the training stack into C. If DSCO lacks ACP or RLM hooks sufficient for this task, provide a minimal subprocess protocol and context-variable harness. A proposed `dsco role-env validate/run` interface must exercise one episode without requiring a training service.

Acceptance uses a generated aggregation task with a deterministic independent oracle and an intentionally incorrect role policy. Verify pass/fail outcomes, cancellation cleanup, isolated task state, and zero unauthorized grader access through the actual binary. Then perform a bounded real root-adapter update from these environment trajectories and reload it; preserve base/leaf tensors and record optimizer-step evidence. Evaluate held-out seeds with the same environment contract. Attempt reward-file modification, forged terminal messages, and dropped failed episodes; none may improve reported performance. Report exact pinned interfaces, training artifact identity, and oracle results without uploading private trajectories by default.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 108 — Bridge role root datasets to actual Prime SFT jobs

A role is not trained when its prompt optimizer finishes; it is trained when a declared neural update produces a usable checkpoint. Build the bridge in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py`'s `compile_with_mipro()` and `compile_with_gepa()` optimize DSPy programs. The new path follows root-policy SFT from the [RLM paper](https://arxiv.org/html/2512.24601v3), using the actual [Prime training contract](https://github.com/PrimeIntellect-ai/prime-rl/blob/main/docs/training.md).

Create a training manifest binding role, open base checkpoint, adapter configuration or declared full-update parameters, tokenizer/renderer, root prompt/completion dataset, frozen leaf model, optimizer settings, seed, device allocation, and hard step/token/time ceilings. Resolve it to the pinned `prime-rl` SFT configuration and validate supported fields before submission. Batch units must match that revision; never copy a paper's example count into a token-batch field. Capture resolved configuration, start/finish state, tensor artifact, optimizer progress, and failure reason. Do not treat configuration validation or an exit-zero wrapper as proof of a model update.

Add a small job adapter near `src/agent_interop.c` with explicit candidate-artifact reporting to `src/self_improve.c` and serving identity checks near `src/provider.c`. If no dataset producer exists, include a minimal generated role curriculum whose root code has an executable oracle. The proposed `dsco role-train sft` command should use a local trainer or already authorized device allocation; it must not silently provision paid capacity. Pin compatible trainer, renderer, and model revisions.

Acceptance executes a genuinely bounded SFT job, reloads its root adapter, and proves finite nonzero trainable tensor changes while frozen weights are identical. Compare oracle-verified RLM execution before and after on held-out task families and seeds, honestly allowing no measured gain. Inject incompatible tokenizer metadata, a mixed messages column, an exhausted device budget, and a trainer that returns no changed checkpoint. Show exact rejection or failure state and no deployment. The final evidence must connect dataset digest to optimizer steps, artifact tensors, and a real DSCO inference request using that artifact.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 109 — Train role roots with terminal verifiable rewards

Train a role's recursive decisions from outcomes that a program can verify. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, replace the scoring idea in `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/eval_bias_reduction.py`'s `evaluate_test_case()`, where heuristic coherence and generated score strings stand in for correctness. Implement root-policy RL with terminal verifiable rewards, drawing on the [RLM paper](https://arxiv.org/html/2512.24601v3) and a pinned [Prime RL training interface](https://github.com/PrimeIntellect-ai/prime-rl/blob/main/docs/training.md).

Define an initial task family with deterministic expected outputs, external context, recursive helper calls, and a finite horizon. The policy being updated is the selected role's root checkpoint or adapter; keep leaf weights and environment behavior frozen. Capture actual sampled root token IDs, behavior-policy version, masks, terminal reward, and truncation reason. Use a supported RL objective with its required group/advantage information; never synthesize missing log probabilities. Correctness reward dominates any bounded efficiency shaping, so cheap wrong answers cannot beat valid solutions. Infrastructure failure is separately labeled, not silently transformed into a positive or excluded result.

Build a bounded orchestration adapter through `src/agent_interop.c`, report candidate models to `src/self_improve.c`, and resolve exact serving versions in `src/provider.c`. If no environment framework exists, provide a minimal local verifiable task adapter and use a compatible pinned trainer; this prompt must not depend on another feature. A proposed `dsco role-train rlvr` interface should state trainable parameters and compute ceilings before execution.

Acceptance runs real optimizer steps on a small supported open model or adapter, records nonzero finite tensor updates, and reloads the policy through DSCO. Evaluate held-out task families, untouched seeds, and longer contexts with the independent oracle, alongside the frozen baseline. Inject an attempted grader edit, a final answer containing the expected substring but wrong structure, an empty rollout, and a timeout after child dispatch. Verify exact reward and masking behavior for each. Report success rate, reward components, policy-version staleness, total cost, and whether the trained role improved; a changed reward log alone is insufficient.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 110 — Separate root and leaf learning credit experimentally

Determine whether a role learned better recursive control or merely received a better helper model. Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` separates `FourModelHierarchicalSwarm` and `LeafWorker`, but does not isolate neural learning credit. The [RLM paper](https://arxiv.org/html/2512.24601v3) motivates training the root while treating leaf calls as a distinct component; turn that distinction into an executable experiment.

Define four pinned conditions: old root/old leaf, trained root/old leaf, old root/trained leaf, and trained root/trained leaf. Hold task instances, context exposure, sampling, budgets, and REPL semantics fixed. Pair evaluations on common held-out task-family seeds while retaining stochastic repeat identifiers. Exclude evaluation families from both training sets. Record root program quality, leaf answer quality, terminal correctness, recursive call count, and cost, then estimate root effect, leaf effect, and their interaction with uncertainty. Do not infer causality from comparing different curricula or model families. Allow the scientifically useful result that the root update did not help.

Implement a comparison controller with hooks in `src/provider.c`, `src/swarm_accounting.c`, and `src/self_improve.c`. It should accept explicit model artifacts and a minimal role-environment callback if no full training registry exists. Training data consists of root execution trajectories for the root update and separately labeled leaf subproblems for the leaf update. The output must include two real parameter artifacts or a clearly declared frozen-leaf-only experiment; prompt changes alone cannot fill a trained condition.

Through the actual binary, train a bounded root adapter and, when testing all four conditions, a separate bounded leaf adapter. Prove which tensors changed in each arm. Use an independent corpus-join oracle with tasks requiring both correct partitioning and correct local classification. Deliberately swap the leaf serving revision in one arm and verify the controller rejects the comparison. Test missing results, contaminated task seeds, and identical artifacts mislabeled trained. Produce paired outcome tables and artifact identities, showing whether improvement belongs to root control, leaf skill, interaction, or neither.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 111 — Learn recursion depth and batching as root behavior

Teach the role root when recursion is useful and how much independent work belongs in one child call. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, start from `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/config.py`'s `SwarmSettings`, whose fanout and concurrency are static limits. The [RLM paper](https://arxiv.org/html/2512.24601v3) distinguishes depth-zero external reasoning, leaf subcalls, and recursive sub-RLMs. Implement training that chooses among them while preserving host-enforced ceilings.

Generate role tasks spanning local arithmetic, independent semantic classification, hierarchical aggregation, and cross-partition dependencies. Collect verified root programs with different chunk sizes, batches, and recursion choices. Labels or terminal rewards must come from task correctness and measured resource use, not “more recursion is better.” Train the role's root adapter to emit bounded programmatic decisions using context size, task structure, observed uncertainty, and remaining budget. The host remains authoritative for maximum depth, calls, memory, and concurrency; generated code cannot redefine them. Distinguish a large input from intrinsically quadratic work.

Add a training/measurement adapter around `src/topology.c`, `src/swarm_scale.c`, and the root loop in `src/agent.c`. If no RLM environment exists, provide a minimal persistent context plus recursive-call primitive for the generated curriculum. A proposed `dsco role-train recursion-policy` command should declare the trainable root parameters, frozen leaf revision, and exact reward or supervised-label contract. Record both chosen and actually admitted recursion structures.

Acceptance requires a bounded real adapter update and reloaded DSCO role inference on unseen task shapes. An independent oracle checks all outputs; record correctness, depth, batching, calls, latency, and tokens rather than only aggregate reward. Include a trivial task where unnecessary recursion wastes budget, an aggregation task where leaf calls help, and a pairwise task where combining dependent items incorrectly loses answers. Inject an attempted depth-limit override and prove denial survives training. Compare the learned root to fixed depth and fixed batch baselines under equal budgets, with no performance claim unsupported by held-out results.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Verify changed trainable tensors against the initial artifact.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 112 — Measure role RLM length generalization

Show whether a trained role can apply its recursive procedure to substantially longer problems, not just memorize the training layout. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/core.py`'s `format_history()`, which truncates conversation text. Build a length-generalization experiment for the external-context contract described in the [RLM paper](https://arxiv.org/html/2512.24601v3), with independently varied information density and semantic work.

Create generated role task families whose required work is constant, linear, and pairwise in input size. Separate distractor length from number of relevant records, and vary record order, identifier distribution, and location of required evidence. Train a root adapter only on declared short-context distributions. Freeze the resulting checkpoint and evaluate progressively longer contexts with new seeds and changed layouts, including lengths above its neural window. Return final artifacts from environment variables so output length does not secretly cap the benchmark. Track failure due to correctness, budget, protocol, and memory separately.

Implement a benchmark adapter near `src/agent_interop.c` and `src/agent.c`, returning candidate-role results to `src/self_improve.c`. If no role RLM exists, include a minimal trainable open-model harness and deterministic task generator so the experiment is standalone. The trained output must contain actual root weight or adapter changes verified against the initial checkpoint. Keep leaf policy, task oracle, and tokenizer fixed across lengths; report resource scaling alongside accuracy.

Acceptance runs the real binary with the frozen trained artifact and baseline on held-out task families and untouched lengths. An independent generator computes the exact selected records, aggregate, or pair set; model grading cannot establish correctness. Test fixed-position memorization by shuffling relevant records, test keyword shortcuts with semantic distractors, and reject duplicate task identities crossing the train/eval boundary. Demonstrate full external input access without copying all text into root history. Produce accuracy-versus-length and cost-versus-work tables with seeds and confidence intervals. A small supported device run may establish mechanics, but label its scale honestly and never extrapolate the paper's gains to DSCO.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Verify changed trainable tensors against the initial artifact.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 113 — Serve isolated role adapters by immutable version

Once every role has a trained RLM, serving the wrong adapter becomes a behavioral correctness bug. Work in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/config.py` supplies `ModelSpec` and `normalize_model_id()`, but no immutable adapter-serving boundary. Implement one for role roots, preserving the [RLM paper's root/leaf distinction](https://arxiv.org/html/2512.24601v3).

Bind a request tree to base checkpoint, root adapter tensor digest, tokenizer/renderer revision, role contract, and frozen leaf selector at admission. Loading a newer role adapter affects only newly admitted trees unless an explicit supported migration exists. Reject shape, rank, base-revision, or tokenizer incompatibility before serving. Cache keys must include role and adapter identity; a shared base does not imply shared behavioral state. Require the backend's actual loaded-version receipt or local artifact verification, not just the model string requested by the client.

Add a small serving-snapshot module around `src/provider.c`, `src/provider_pool.c`, and `src/agent_profile.c`. A proposed `dsco role-model serve/verify` surface may use a pinned local inference backend; inspect its real adapter API rather than inventing load endpoints. If no role registry exists, accept a minimal manifest with explicit immutable versions. Produce two distinct root adapters by real bounded training on different executable role curricula, keeping the base and leaf frozen; these are the artifacts whose isolation must be demonstrated.

Use the actual binary to issue concurrent recursive tasks for both roles while loading a new version of one adapter. Capture backend identity receipts and compare outputs to independent oracles on held-out role task families. No existing tree may mix root generations, and role B must retain its original adapter. Reject an adapter for the wrong base, a renamed unchanged checkpoint claimed trained, and a backend acknowledging the requested name while loading a different digest. Verify cache separation with identical prompt text under different roles. Report tensor-update evidence, version transitions, oracle results, and the concrete guarantees supported by the backend, without claiming unsupported hot-swapping behavior.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 114 — Train interrole protocols against frozen partners

Role RLMs should learn to ask and answer useful subproblems without changing the communication contract or colluding with a moving partner. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, use `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py`'s `Subtask`, `ChildResult`, and `FinalSynthesis` as actual typed-message prior art. Extend them into weight training for programmatic role interactions under the [RLM execution model](https://arxiv.org/html/2512.24601v3).

Choose two roles with complementary responsibilities, such as partition planner and local verifier. Freeze one role's checkpoint, prompt, and protocol implementation while updating the other's root adapter from complete interactions. Train on messages that preserve task identity, required evidence, uncertainty, and executable result references. Use terminal task correctness and protocol-validity checks as independent targets; do not reward a partner's praise. Alternate updates only at explicit generation boundaries, retaining old partner versions for cross-play evaluation. Hidden answer keys and grading metadata must never enter either role's observed messages.

Implement the interaction-data and training adapter with small hooks in `src/swarm.c`, `src/agent_profile.c`, and `src/self_improve.c`. If no message transport supports these records, provide a minimal bounded request/result protocol and local two-role RLM harness. A proposed `dsco role-train protocol` operation declares the one trainable adapter, frozen partner revisions, and task split. The output is a real role parameter update, not a rewritten message template.

Acceptance runs bounded actual training for one role, then reloads that adapter through the real DSCO binary. Verify the partner tensors remain identical. Test it against both the training partner and an unseen compatible partner on generated tasks with an exact oracle. Include malformed child replies, omitted uncertainty, a partner that repeats an incorrect answer, and a planted label-leak field. The trained role must not gain reward from blindly trusting or echoing its partner. Report cross-play correctness, schema violations, call cost, tensor deltas, and any negative transfer before considering another alternating update.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 115 — Split role training data by repository ancestry

A role that remembers yesterday's patch is not necessarily learning a reusable recursive procedure. Build leakage-resistant dataset partitioning in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` exposes `compile_with_mipro()`, `compile_with_gepa()`, and `evaluate()` with caller-supplied splits; it does not enforce repository ancestry isolation. Apply that missing discipline to actual role-root weight training inspired by the [RLM paper](https://arxiv.org/html/2512.24601v3).

Group episodes by repository lineage, source snapshot, patch family, task-generation seed family, and shared external context before allocating train, selection, and sealed holdout partitions. Detect exact duplicates and configurable structural near-duplicates, including renamed code and paraphrased questions over the same answer-bearing context. Track what the root and its leaves could access, not only the final sample text. Preserve chronological boundaries where evaluating future tasks. Repeated optimization on a holdout converts it into selection data; record and enforce that transition rather than retaining the label.

Implement a bounded split-planning adapter using references from `src/chronicle.c`, external data preparation through `src/agent_interop.c`, and training admission near `src/self_improve.c`. Consume existing episode artifacts instead of creating a second evidence store. A proposed `dsco role-data split/audit` command accepts a minimal manifest when ancestry metadata is otherwise unavailable and reports unsupported isolation explicitly. Its output includes immutable partitions and reasons for grouped or rejected records.

Acceptance uses generated repositories with copied functions, adjacent commits, renamed bugs, and wholly separate task families. The actual binary must reject cross-split contamination and prevent the trainer from reading sealed answer keys. Run a bounded real adapter SFT witness using the accepted training partition, proving tensor changes and exact dataset identity, then evaluate with an independent execution oracle on holdout only once. Compare against a deliberately leaked split to demonstrate why its apparent result is invalid, without treating that run as a valid benchmark. Report unresolved provenance, excluded records, sample support, and honest held-out behavior of the trained role.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 116 — Train recovery from REPL syntax and finalization errors

Each role needs to learn how to recover its recursive program after a real execution error. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, build a replayable correction curriculum grounded in the [RLM paper's program-execution setting](https://arxiv.org/html/2512.24601v3). `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py`'s `DeepCognitiveAgent.stream()` only assembles a final object when a final prediction arrives; it does not supply a learned recovery policy.

Generate or collect root episodes containing syntax errors, missing variables, wrong child-result shapes, interrupted batches, and invalid finalization. Snapshot the exact environment before the failing root action and retain the actual error observation. Produce corrected root targets only when replay from that state completes the original task under its unchanged contract. Train the role-root adapter on error-conditioned corrective programs, masking environment messages and failed code from positive SFT targets while preserving them in context. Do not “repair” an episode by removing difficult records, lowering requirements, or fabricating child results.

Add an error-episode adapter near `src/agent.c`, `src/provider_events.c`, and `src/agent_interop.c`; use a minimal persistent REPL subprocess if necessary for independent operation. A proposed `dsco role-data corrections` surface must validate replayability and then support a bounded actual root-adapter update through a local trainer protocol. Declare which error classes are trainable and which infrastructure failures require host recovery rather than model improvisation. The output includes changed parameter tensors and the exact correction dataset identity.

Acceptance drives the real binary through controlled errors, trains on one subset, and reloads the adapter for unseen variants. An independent task oracle verifies full results, not merely valid syntax or the absence of exceptions. Include a correction that sets Final to a constant expected for one training example, one that suppresses an exception and drops a partition, and one that re-executes an already committed mutation. Reject them as successful demonstrations. Report recovery rate, newly introduced errors, finalization integrity, actual tensor deltas, and non-recovery behavior when the environment cannot safely resume.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 117 — Train asynchronous recursion policies without blocking input

A role's learned recursion policy must use concurrency without freezing the user's input or evading unfinished work. Work in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_async.py`'s `AsyncComprehensiveDebiasedReasoning.aforward()` runs two fixed gather waves. Implement a training environment for asynchronous RLM submission, awaiting, and cancellation while retaining the [paper's programmatic recursion contract](https://arxiv.org/html/2512.24601v3).

Expose opaque futures inside the root REPL and let generated programs submit independent slices, await selected results, batch useful work, and cancel no-longer-needed calls. The host enforces aggregate token reservations, concurrency, depth, deadlines, and role permissions. Training observations include queue state and resolved outcomes, but not hidden future completion times. Rewards require the full task result and charge actual resource use; cancelling a required slow branch cannot improve apparent correctness. Label timeout, policy cancellation, infrastructure failure, and valid partial progress distinctly.

Implement a thin environment/training adapter around `src/swarm_reactor.c`, `src/swarm_accounting.c`, and `src/agent.c`. If no recursive futures API exists, supply a minimal local broker and external-context store. The proposed `dsco role-train async-policy` command trains a real root adapter on controlled latency/task distributions, with a fixed leaf model. Training and collection run outside the terminal input path; input admission must remain available while a job or child waits. Prompt retuning does not count as the model update.

Acceptance executes a bounded actual optimizer run, reloads the resulting artifact, and evaluates unseen latency distributions through the DSCO binary. An independent oracle requires every relevant partition exactly once. Test out-of-order completion, an indefinitely stalled branch, cancellation after partial work, duplicate future results, and budget exhaustion. Verify response to terminal input within a stated measured bound during the stalled case. Show nonzero finite adapter deltas, unchanged frozen tensors, complete outcome accounting, and correctness/cost/latency versus a fixed scheduler. If learned scheduling harms accuracy, retain the incumbent and report the failure rather than rewarding throughput alone.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 118 — Distill across roles without hiding negative transfer

Transfer a useful recursive procedure between roles without assuming all roles should learn the same behavior. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, extend the shared-method idea in `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py`'s `REASONING_METHODOLOGIES` and `MethodologySignature`. The donor shares instructions; this feature distills actual root-policy behavior, using the [RLM paper](https://arxiv.org/html/2512.24601v3) as the execution/training model.

Choose a source role, target role, and explicit transferable mechanism such as context partitioning or evidence aggregation. Map source trajectories into the target's allowed input/output and effect contract, preserving executable observations. Exclude behaviors incompatible with target objectives, such as treating a plausible proposal as a verified result. Train only the target adapter from accepted source examples mixed with target retention data. Keep source weights frozen and track mixture weights, curriculum scope, and target-specific negative examples. Avoid claiming transfer solely because source-task loss decreases.

Implement the transfer dataset and evaluation adapter near `src/agent_profile.c`, `src/provider.c`, and `src/self_improve.c`. If role artifacts or datasets are absent, provide two small trainable role curricula and minimal manifests so this works independently. A proposed `dsco role-train transfer` interface must declare trainable target tensors, frozen source, contract mapping, and a separate target holdout. Use a bounded local trainer or already authorized compute; produce and reload an actual parameter artifact.

Acceptance transfers partitioning behavior from a candidate proposer into a verifier on generated code-task corpora. Independent execution oracles measure proposal usefulness and verifier false acceptance separately. Include a persuasive but incorrect candidate, missing evidence, and a source trace that bypasses a required check. Show that the adapter does not learn to accept those shortcuts. Compare target-only training, transferred training, and the unmodified target under equal budgets, including cases where transfer hurts. Verify source tensor identity and target tensor deltas. Report target correctness, false positives, recursion cost, and retained competence, retaining the prior target when the transfer does not meet its declared acceptance threshold.

The REPL holds external context; generated code invokes children and assigns the final output variable.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 119 — Admit trained models with their native runtime generation

A trained role policy may depend on REPL or native-module behavior that changed during self-rewriting. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, implement admission for the pair of model generation and resident native generation. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/executor.py`'s `execute_one()` runs a binary against a goal, but does not validate a neural/native pair. Ground the role contract in [RLM external execution semantics](https://arxiv.org/html/2512.24601v3).

Define a candidate pair binding root adapter tensor digest, base/tokenizer, frozen leaf policy, REPL protocol, native module generation, and effect schema. Test the four old/new model-runtime combinations to distinguish model gains, runtime gains, and compatibility interactions. A model trained against changed observation formatting must not be silently served on the old runtime. Pair admission requires independent correctness, resource limits, and preserved capability enforcement; neither a good benchmark nor a model's confidence can waive a mismatch. In-flight sessions retain their admitted pair until a supported boundary.

Add a new joint-admission module with small hooks in `src/hotplug.c`, `src/provider.c`, and `src/self_improve.c`. Reuse existing loading and serving mechanisms rather than redesigning them. If either lifecycle is absent, provide a minimal isolated two-implementation callback plus pinned model-serving adapter that can prove the joint decision without claiming unsupported hot activation. A proposed `dsco role-model qualify-pair` interface should return accepted pair, rejected obligations, and rollback target.

For acceptance, train a real small root adapter on an explicitly versioned environment and produce a native candidate that changes an allowed parsing helper. Verify finite neural tensor changes and distinct native artifacts. The actual DSCO binary must run an independent oracle suite on held-out task families across all four combinations, including a deliberately incompatible output schema and a candidate that weakens a denied effect. Reject the unsafe pair even if faster. Demonstrate one supported admitted pair serving a role task with exact identity receipts and one rejected pair preserving the incumbent. Report compatibility evidence, training lineage, and measured interaction effects; metadata changes alone cannot satisfy either update proof.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.

---

# 120 — Demonstrate every role training serving and improving

Deliver a runnable proof of “each role trains an RLM” across a small complete DSCO improvement loop. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, use `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py`'s `FourModelHierarchicalSwarm` as orchestration prior art. It selects fixed model roles; the new demonstration must produce separate learned root policies under the [RLM paper's execution contract](https://arxiv.org/html/2512.24601v3).

Define at least two complementary roles, such as candidate proposer and independent verifier. Each owns a root adapter, role contract, external-context curriculum, training split, and distinct executable objective. Shared frozen base weights and leaf models are acceptable. Collect or generate actual root trajectories, train both adapters with bounded real optimizer steps, validate their tensors, and serve their immutable identities. Keep prompts fixed when demonstrating neural learning. Give each role persistent context variables, programmatic recursive calls, and variable-based finalization; ordinary chat delegation is insufficient.

Wire a minimal demonstrator through `src/agent_profile.c`, `src/provider.c`, and `src/self_improve.c`, reusing `src/hotplug.c` only for supported native candidate execution. If training, RLM, or candidate interfaces are absent, implement the smallest explicit adapters needed within this task rather than requiring another prompt. The proposer creates a concrete local code candidate; the verifier evaluates it using an independent test oracle it cannot redefine. A candidate reaches resident execution only through the existing validated lifecycle; unsupported activation must be reported precisely.

Acceptance runs the real binary end to end on a generated algorithmic repair family with sealed held-out variants. Show changed finite tensors for both role adapters, unchanged frozen tensors, model-serving receipts, and separate root/leaf accounting. Compare the original pair against the trained pair on actual correctness, false acceptance, and net execution benefit, allowing a no-improvement result that keeps the incumbent. Inject a forged trained-artifact manifest, swapped role adapters, a verifier timeout, and a candidate that passes visible tests but fails a hidden edge case. None may yield successful improvement. Provide one reproducible local command, declared device/time budget, final artifacts, and complete evidence from training data to served model to accepted or rejected code behavior.

The REPL holds external context; generated code invokes children and assigns the final output variable. Claims of native improvement additionally require changed instruction bytes, retained heap/session state, and no exec.

Preserve dirty work; use new modules and small C hooks. Route dynamic tool effects through `tools_execute_for_tier()` and preserve denials. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; isolate state and never install worker binaries.
