# 120 prompts for a resident self-rewriting reasoning runtime

Each numbered section is one complete standalone implementation prompt.

# 01 — A resident executable self-model

Make DSCO inspect and replace a bounded part of its own executable behavior while remaining the same running process. Introduce resident program cells for a pure scoring or scheduling function. Each cell identifies its active machine-code entry, generation, source digest, compiled-artifact digest, input/output schema, effect contract, owned state, and measured evaluation results. A cell is an executable unit, not a prompt string or an inventory entry detached from running code.

Provide a minimal native descriptor and registration interface if none exists. Maintain immutable descriptors per generation and a separate active reference. Expose inspection that follows the actual invoked function back to its descriptor and source, reporting unavailable provenance explicitly. A candidate descriptor must resolve a callable entry and pass contract checks before becoming eligible for activation. Keep stable host services and capability enforcement outside the replaceable region; define precisely which resident behavior is mutable.

Demonstrate the self-model with two implementations of the same deterministic scorer. Let the resident process enumerate its current cell, evaluate an input, register a candidate, activate it, and show the changed output and generation without exec, supervisor relaunch, or resetting session state. Record heap-sentinel and session-handle continuity and audit that no exec occurred; PID and process-start identity alone cannot establish continuity because exec preserves them. Keep inspection free of arbitrary pointer dereferences; bounded source and symbol metadata should be sufficient for analysis.

Falsifying test: attach source from version A to version B's executable artifact and require provenance rejection. Then activate a correctly paired version B and observe a changed native result, unchanged process-start identity, and both historical descriptors. If only configuration text changes, or execution still calls A, the feature fails.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/ast.c`, `src/vm.c`, `src/plugin.c`, `src/self_improve.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register`, `MetaToolRegistry.get_source`, `MetaToolRegistry.get_ast`. Observed donor behavior: The registry associates callable names with source and cached AST descriptions; it does not establish a native executable self-model.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 02 — Versioned live function dispatch

Give a running DSCO process a function registry whose native implementations can change atomically while concurrent calls continue. A stable cell identifier resolves to an immutable generation record containing a typed entrypoint, ABI digest, code owner, and state reference. Invocation acquires a generation lease before dereferencing the entrypoint and releases it after returning; activation swaps the active record rather than overwriting instructions being executed.

Define a small usable C interface for register, acquire, invoke, release, inspect, and activate. Activation includes the expected current generation so two improvement workers cannot silently overwrite each other's result. Reject duplicate generations with different bytes and reject ABI-incompatible replacements before publication. Keep lookup lifetime separate from code-image lifetime, and specify synchronization ordering for the descriptor pointer and its dependent state. If a supporting registry is absent, implement this minimal contract locally rather than requiring another feature.

Integrate one pure native scorer through the registry and expose a bounded resident demonstration path. Existing callers without a mutable cell must preserve their behavior. Distinguish a plugin's existing load/unload operation from the new safe invocation protocol: simply replacing a tool array and calling dlclose is insufficient. Dynamic tool cells must use a governed adapter; the current VM callback shortcut must not become an escape hatch.

Falsifying test: pause one version-A invocation after lease acquisition, activate B, start a new invocation, and verify the old call returns A's result while the new call returns B's. Race two compare-and-swap activations and require exactly one winner. Run repeated swaps under concurrent readers; any torn descriptor, stale successful activation, premature unload, or process replacement fails acceptance.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/vm.c`, `src/plugin.c`, `include/vm.h`, `include/plugin.h`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `MetaToolRegistry.transform_tool`. Observed donor behavior: Registering transformed Python source replaces a named callable in a dictionary; the donor has no concurrent native generation protocol.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 03 — Typed mutable reasoning IR

Let DSCO rewrite executable reasoning graphs in memory and run the verified result immediately. Define a bounded typed intermediate representation with constants, records, branches, bounded iteration, native-cell calls, governed effects, and explicit evidence values. Support transformations of this representation as data, followed by verification and lowering into an executable VM program or registered native graph. Changing an instruction graph must alter runtime behavior; editing a prompt document alone does not satisfy this feature.

Specify value types, block parameters or stack states, entry and exit contracts, and a finite execution budget. The verifier checks every reachable instruction, opcode and operand range, control-flow target, type join, declared native signature, and effect annotation. Values carrying failed observations must remain distinct from successful evidence. A graph claiming purity cannot call a tool or smuggle an effect through an unchecked native pointer. Reject unsupported operations rather than allowing verifier/runtime disagreement.

Use existing prompt-program and workflow formats as import surfaces only where their semantics are sufficient; document the new executable representation separately. Provide a minimal interpreter or lowerer if the current VM lacks needed operations. Keep immutable verified generations, with candidate rewrites isolated from the active graph until verification succeeds. Include an inspection view showing the verified instructions actually executing and their source-generation relationships.

Falsifying test: run a baseline graph, rewrite its scoring branch, and observe changed outputs in the same PID. Mutate a branch target outside the program, merge incompatible types, call a mismatched native signature, and place a network effect behind a purportedly pure node. Every invalid candidate must fail before any instruction or external effect executes, while the previous graph remains usable.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/prompt_program.c`, `src/vm.c`, `src/lingo_workflow.c`, `include/vm.h`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.parse`, `ASTAnalyzer.transform`, `MetaToolRegistry.compose`. Observed donor behavior: The donor parses and transforms Python ASTs and composes callables, but does not verify typed native control flow or effect compatibility.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 04 — Resumable VM generation switching

Allow a suspended DSCO reasoning program to continue through an improved executable generation without restarting the process or repeating completed work. Introduce explicit VM safepoints with stable continuation labels, live-value schemas, generation identity, and a ledger of completed effect references. A rewrite may supply a checked continuation map from the old label and live values into the new program. Incompatible frames remain pinned to their original generation until they finish.

Define the switch operation as a transaction over one suspended frame: validate the target generation, continuation label, live-value conversion, and effect frontier before changing the frame. Never reinterpret an old numeric program counter in a new instruction array. A failed conversion leaves the original stack, registers, owned values, and resumption target intact. Preserve ownership of strings and records across code lifetimes; borrowed pointers into reclaimed code or arenas cannot survive migration.

Add a minimal resumable frame abstraction if necessary, with an instruction budget and cooperative yield integrated into the existing event loop. Exercise a real two-stage reasoning computation: collect a local governed observation, suspend, replace the downstream scoring program, then resume using the existing observation. Emit generation-aware transitions that let an operator distinguish old-frame completion from successful migration. Specify which frame shapes are supported initially instead of pretending arbitrary C stacks can migrate.

Falsifying test: increment a fixture counter through a governed effect before suspension, switch to a generation that transforms the collected value differently, and require one counter increment plus the new result. Reject a migration missing a required live value and resume the unchanged original frame successfully. Any replayed effect, mixed stack layout, process exec, or silently discarded continuation fails.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/vm.c`, `include/vm.h`, `src/lingo_workflow.c`, `src/event_loop.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_structured`, `ToolExecutor._execute_sequential`. Observed donor behavior: The donor schedules dependency-ready calls and passes intermediate results; it has no resumable frame migration across program rewrites.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 05 — Compile candidate C into a live native module

Have DSCO generate a bounded C function, compile it into a loadable native image, evaluate it, and redirect a resident program cell to that entrypoint without process restart. Start with a pure fixed-signature scorer over a bounded input record. Keep compilation, candidate loading, qualification, and activation as distinct states, each tied to exact source and object hashes. This is native executable replacement, not prompt optimization.

Use a configured compiler with shell-free argv and a private candidate directory. Compile against a small versioned host ABI that exposes no ambient tool callbacks. Inspect the resulting image for expected architecture, exports, forbidden imports, constructors, and ABI descriptor before loading; run dangerous or malformed qualification cases in a child process before admitting the image to the resident host. Loading a shared image can itself execute initialization code, so reject unsupported initializer behavior before resident loading rather than treating dlopen as harmless validation.

Reuse platform loader facilities but introduce the minimal generation ownership and invocation contract needed to avoid unloading active frames. Do not call this a JIT or modify the original executable file. A source compiler and dynamic loader are the mechanism. Include diagnostics for missing toolchains, unsupported target architecture, compilation failure, unresolved imports, and failed qualification. The active implementation remains callable throughout candidate preparation.

Falsifying test: compile two scorers producing distinct known results, qualify B against fixed and hidden generated cases, and activate B in the same resident PID and process-start identity. Supply syntax errors, an incompatible descriptor, and an unexpected imported symbol; all must fail without changing active behavior. Preserve compilation evidence and prove the invoked address belongs to B's loaded image.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `include/plugin.h`, `src/autoresearch.c`, `src/execution_layer.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `MetaToolRegistry.transform_tool`. Observed donor behavior: The donor turns generated Python source into callable functions; the proposed extension compiles C into actual native code.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 06 — Generate and execute bounded machine code in memory

Make DSCO emit and execute actual machine instructions for a narrowly defined pure scoring expression while it remains resident. Implement one explicitly supported architecture first, preferably the current host, with a small expression language such as checked integer addition, comparison, constants, and bounded record-field loads. Parse and verify the expression, generate bytes into a writable candidate region, seal it executable, and call it through a fixed native signature. Neither dlopen nor an external compiler counts as this mechanism.

Specify register use, stack alignment, return convention, permitted memory operands, overflow behavior, and instruction-cache synchronization. Enforce bounds on expression depth, emitted bytes, and input reads. The emitter must not accept arbitrary instruction bytes, indirect branches, syscalls, or unrestricted memory addresses. Retain an interpreter for differential qualification and a generation record linking expression, bytes, and actual entrypoint.

Handle platform execution permissions explicitly. On hardened macOS inspect whether the running binary has the required JIT entitlement and use the appropriate MAP_JIT and write-protection protocol, including supported write callbacks, when available; otherwise report unsupported without altering signing or security settings. On supported systems enforce writable-versus-executable transitions, using the platform's permitted mechanism. Do not patch signed host text, disable host protections, or advertise an interpreter fallback as native code generation.

Falsifying test: generate A and B for distinguishable arithmetic expressions, compare both against an independent reference over boundary and randomized inputs, and activate B in the same PID. Record emitted-byte hashes and show execution addresses inside the generated mappings. Invalid offsets, overflow-contract violations, or unavailable platform permissions must reject the candidate while A remains available. Check that code-writing windows never permit arbitrary concurrent execution of partially emitted bytes.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/vm.c`, `include/vm.h`, `src/cost_frontier.c`, `src/arena_alloc.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`, `MetaToolRegistry.register_code`. Observed donor behavior: The donor executes transformed Python source; it does not generate machine instructions or manage executable memory.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 07 — Native ABI, relocation, and import contracts

Give every replaceable native DSCO function an explicit ABI and import contract that the resident host can validate before execution. Define a descriptor containing schema version, architecture, calling convention, entrypoint signature, record sizes and alignments, field offsets, ownership rules, state schema, and required host imports. Canonicalize this descriptor into an ABI digest shared by caller and candidate; a matching function name alone cannot establish compatibility.

Support a bounded native image format or the inspected subset of the existing platform loader. Validate exported entrypoints, relocation targets, relocation widths and bounds, import names, and executable/data section permissions. Resolve host imports through a typed allowlist rather than exposing arbitrary process symbols. Pure functions receive only pure imports; tool effects use a stable governed service adapter. Reject unexpected constructors and unsupported relocation kinds before admitting a candidate into the resident address space.

Provide a minimal compatible caller/candidate pair and expose a precise compatibility report identifying the first violated contract. Distinguish loader success from ABI success. Define how string buffers, errors, allocators, and returned memory cross the boundary so a new generation cannot free memory with the wrong allocator. Record resolved import versions in the candidate identity; later host-service changes cannot silently invalidate qualification evidence.

Falsifying test: build candidates with the same exported name but changed field offsets, a different return convention descriptor, an unresolved import, and an out-of-bounds relocation in a copied image. Each must be rejected before candidate execution, with the original function still callable. A valid candidate must execute repeatedly through the checked entrypoint in the same PID. Include observable counters proving rejected initialization code never ran and tests showing imported effects still meet the ordinary capability gate.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `include/plugin.h`, `src/vm.c`, `include/vm.h`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`. Observed donor behavior: The donor discovers Python functions and invokes them with kwargs; it does not establish machine-level ABI or import compatibility.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 08 — Safe reclamation of live native generations

Let DSCO replace native functions repeatedly under concurrent execution without unloading code that an active frame, callback, or continuation still needs. Implement a generation lifetime protocol covering entry acquisition, in-flight calls, suspended VM frames, composed-program references, and retirement. Choose reference counting, epochs, or another justified bounded approach, but make the ownership and memory-ordering contract explicit.

Activation marks the old generation retired and routes new acquisitions to the new generation. Retirement does not immediately dlclose an image or unmap generated code. Reclamation occurs only after the system proves that every execution and retained code reference has left the generation. Separate descriptor reclamation, executable mapping reclamation, and owned-state cleanup. Cleanup callbacks must not run under registry locks or reenter a half-destroyed generation.

Integrate a small resident native function with a controlled blocking point and a second replacement implementation. Track active leases and retirement reasons through bounded inspection. Handle failed activation, cancelled invocations, nested calls, and a suspended continuation retaining an old function. Decide how shutdown drains or deliberately retains outstanding images; leaking bounded retired code with a clear diagnostic is preferable to unsafely unloading an active frame, but normal workloads must reclaim successfully.

Falsifying test: pause A inside its native body, activate B, call B successfully, and attempt reclamation. A's image must remain mapped until its paused call returns. Add a continuation reference and verify it extends that lifetime even after ordinary calls drain. Then release all references and observe exactly one cleanup and unmap. Stress repeated replacements under threads with address sanitization where supported; use-after-unload, deadlock, double cleanup, unbounded ordinary retirement growth, or process restart fails acceptance.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `src/vm.c`, `src/event_loop.c`, `include/plugin.h`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `MetaToolRegistry.compose`. Observed donor behavior: The donor stores function references in dictionaries and compositions; it does not handle native image reclamation during concurrent execution.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 09 — Transactional migration of resident heap state

Allow an improved native DSCO function to change its owned heap-state schema without restarting or corrupting ongoing work. Represent mutable-cell state through explicit schema identifiers and bounded handles, not raw pointers serialized across generations. Define a migration contract that consumes an immutable old-state snapshot and creates a separate candidate state arena with checked ownership and reference relationships.

The migration runs in a prepare phase with no external effects. Validate required fields, ranges, reference integrity, allocation budget, and the new function's state invariants before activation. Commit the code-generation and state-root pair atomically. On failure, discard the candidate arena and leave the active pair untouched. Old in-flight calls retain their original code and state snapshot according to a documented concurrency policy; a migration cannot reinterpret their memory beneath them.

Fence and drain writers before snapshot through commit; reject stale writes. Test a delayed writer. Rollback after new writes must migrate current state losslessly, never silently restore an obsolete snapshot.

Demonstrate a stateful scorer that changes from a flat running-average record to a versioned record containing count, sum, and bounded calibration bins. Specify numeric conversions, missing-field defaults, and when a reverse migration is possible. Rollback must retain an appropriate compatible state or explicitly reject reversal after irreversible schema changes; do not promise arbitrary state rollback. Provide a minimal local state descriptor if none exists.

Falsifying test: migrate populated state while a version-A call is paused, activate B, and verify A completes using its original layout while B reads the transformed values correctly. Inject allocation failure, malformed references, numeric overflow, and an invariant violation during migration. Every failed preparation must preserve the old state hash and observable results. Track allocations to prove discarded candidate arenas and retired old state are eventually reclaimed, with no code/state generation mismatch.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/arena_alloc.c`, `include/arena_alloc.h`, `src/plugin.c`, `src/vm.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `CodeSandbox.execute`, `MetaToolRegistry.register_code`. Observed donor behavior: The donor retains shared Python locals across executions but supplies no schema-versioned state migration or rollback transaction.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 10 — Atomic activation of cooperating executable cells

Let DSCO activate an improvement spanning several cooperating native functions as one indivisible resident change. A bundle names exact code generations, their ABI contracts, import edges, state roots, and required evidence. Calls acquire a bundle-generation root so an encoder, scorer, and reducer cannot accidentally come from incompatible releases during one computation.

Implement prepare, validate, activate, and retire phases over immutable bundle descriptors. Preparation resolves all internal references and checks dependency closure, shared-state compatibility, import versions, and qualification receipts. Activation compares the expected active bundle generation and publishes one root pointer with defined synchronization. Any missing cell, failed migration, or stale expectation leaves the old root active. Maintain a bounded history sufficient for rollback without making arbitrary individual cell updates visible mid-transaction.

Use a minimal three-function native pipeline whose input encoding changes alongside its scorer. The first version and second version should each be correct internally but produce an unmistakable error when mixed. Include optional state migration through a local interface if no reusable facility exists, rather than depending on another numbered feature. Explain how callers pin nested dispatches to the acquired bundle instead of rereading global active cells on each step.

Falsifying test: run concurrent calls continuously while repeatedly swapping complete bundles. Every observed result must belong to the valid old or valid new result set; the deliberate mixed-version sentinel must never appear. Fail preparation after the first candidate cell is ready and prove no partial publication occurred. Race two bundle activations and require one winner. Confirm old in-flight computations finish correctly, retired bundles release only after their references drain, and the process identity remains unchanged throughout.

Serialize acceptance-epoch validation and publication against revocation. Test receipt invalidation immediately before publication: an unchanged parent generation must not permit activation.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `src/vm.c`, `src/lingo_workflow.c`, `src/execution_kernel.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.compose`, `MetaToolRegistry.transform_tool`. Observed donor behavior: The donor composes named functions but updates registrations individually, without a transaction over interdependent executable versions.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 11 — Shadow native candidates with replayed observations

Evaluate a new native DSCO reasoning implementation against a running champion using the same real observations, without duplicating filesystem writes, network requests, or subprocess effects. Introduce a shadow invocation mode that supplies immutable input records and a bounded effect transcript to the candidate. The active champion performs authorized effects once; the shadow receives recorded responses through a replay adapter and cannot call ambient effectful services.

Match observations by stable operation identity, canonical arguments, and dependency position, not merely call order. A candidate requesting a missing or changed observation must stop with an explicit divergence, or request a separately authorized experiment outside the shadow comparison. Never silently fabricate a tool result. Record exact champion and candidate code generations, input hashes, transcript hashes, outputs, and independently checked outcomes.

Start with a native scoring pipeline that consumes a governed local observation. Enforce the replay-only service table at the candidate ABI and preserve ordinary capability checks on the champion path. Mutable state must be copied or version-pinned so shadow execution cannot alter champion state. Compare semantic outputs and resource use; a different answer is not automatically a regression, and model confidence is not an outcome oracle.

Falsifying test: make the champion increment a fixture counter and read its value, then run two candidate generations against that transcript. The counter must increment exactly once across all three executions. A candidate requesting a different counter write must report divergence before any effect. A pure improved candidate must produce a verifiably better result from the same observations. Include transcript tampering and missing observations; reject both while the champion continues serving live requests in the same PID.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/execution_kernel.c`, `src/execution_events.c`, `src/event_stream.c`, `src/lingo_workflow.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_structured`, `ToolExecutor._execute_single`. Observed donor behavior: The donor forwards dependency results and caches call outputs; it has no native shadow-execution contract preventing duplicate effects.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 12 — Counterexample-guided native executable synthesis

Make DSCO improve a bounded native function through an actual counterexample-guided synthesis loop. A task contract defines the fixed function ABI, permitted source region or expression grammar, reference oracle, resource limits, and training inputs. The agent proposes executable candidates; a compiler or verified emitter produces code; independent execution finds failing inputs; a reducer minimizes those counterexamples and feeds them into the next synthesis attempt.

Preserve every validated counterexample as a nonregression obligation. Keep oracle implementation and hidden test generation outside the candidate's writable region and unavailable to the proposal model where possible. Separate syntax success, runtime safety, semantic correctness, and performance qualification. A candidate may be rejected for one dimension even if another improves. Tie evaluation to exact code bytes, and never accept a reported score from the candidate itself.

Use an initially flawed bounded parser, scorer, or numerical transform with a meaningful edge case. Supply the smallest local candidate-loader and generation interface needed to activate a qualified native entrypoint in the same process; do not require a complete optimizer framework before delivering the loop. Limit iterations, compilation resources, and inference spend, and retain the champion whenever no candidate qualifies. Success must change executable behavior rather than only produce an improved source file on disk.

Falsifying test: seed a candidate that hardcodes public examples and require hidden generated inputs to reject it. Then require the loop to incorporate a minimized failure, produce a corrected executable, and pass both retained counterexamples and untouched holdout cases before activation. Reintroduce the original bug in a later proposal and verify rejection. Record actual native entrypoint generations, unchanged process identity, oracle results, and measured improvement over the original flawed function.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/autoresearch.c`, `src/ast.c`, `src/plugin.c`, `src/vm.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `MetaToolRegistry.transform_tool`. Observed donor behavior: The donor can define and transform executable Python tools; it does not require an independent oracle or preserve counterexamples across revisions.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 13 — Resident rollback with a bounded behavioral watchdog

Let a resident DSCO process automatically withdraw a newly activated native implementation when independently measured behavior regresses. Define a probation policy over accepted outputs, invariant failures, and bounded latency measurements, tied to a candidate generation and a retained champion. The watchdog samples evidence produced outside the candidate's self-reporting path and atomically redirects future calls to the compatible champion when a declared condition is met.

Scope the first version to admitted pure or cooperatively bounded callbacks whose state and lifetime contracts are explicit. Do not claim that a watchdog can safely recover arbitrary in-process memory corruption, SIGSEGV, or an uninterruptible native loop. Pre-qualify such faults in a child process; resident rollback addresses observed wrong results, contract violations, or cooperative deadline breaches. Document this boundary plainly.

Retain champion code and compatible state until probation ends, and ensure rollback never frees an in-flight candidate frame. Distinguish dispatch rollback from reversal of external effects: completed writes cannot be undone by changing a function pointer. Include hysteresis or a declared observation count to avoid oscillation, and require a fresh qualified generation before the withdrawn candidate can return. Expose the exact evidence and transition responsible for rollback.

Falsifying test: activate a native scorer that passes ordinary inputs but returns a wrong result for a hidden boundary case. Feed that case during probation and require the next invocation to use the retained champion in the same PID. Pause an earlier candidate call and verify its image survives until return. Inject a noisy slow sample below the policy threshold and ensure it does not trigger rollback. Any unsupported crash recovery claim, repeated effects, premature unload, or silent state mismatch fails.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `src/event_loop.c`, `src/supervisor.c`, `src/execution_kernel.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/scaling.py` symbols `ScaledSwarm._run_replica`, `ScaledSwarm._select_best`. Observed donor behavior: The donor contains failed replicas and chooses a survivor; it does not roll back resident executable generations.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 14 — Causal profiling mapped to executable versions

Give DSCO's self-improvement loop an accurate view of which resident executable generation caused each observed result and resource cost. Instrument mutable-cell entry and exit with generation identity, parent invocation, workload features, elapsed monotonic time, allocation deltas where available, and independently evaluated outcome references. Preserve a bounded trace of native calls and VM transitions that can be joined to source or IR locations for the exact code version.

Calls must capture their generation at acquisition and retain it through completion, even when the active registry changes meanwhile. Distinguish inclusive time, exclusive time, provider wait, governed effect time, and local computation so the optimizer does not optimize an irrelevant timer. Attribute shared work explicitly rather than counting it once per child. Record missing measurements as unknown.

Add a small profiler-facing API and an inspection report focused on actionable rewrite sites: expensive repeated pure subexpressions, branch distributions, and failures tied to input classes. This is more than a dashboard: feed one measured hotspot into a candidate-selection hook that proposes a bounded executable transformation, while keeping outcome evaluation independent. Avoid tracing sensitive input bodies unless explicitly required; hashes and declared features often suffice.

Falsifying test: pause an A invocation, switch to B, finish A, and prove its samples still identify A. Create a fixture with a slow external observation and a cheap native scorer; the profiler must attribute the wait to the effect, not recommend rewriting the scorer for the entire delay. Compare instrumented and uninstrumented throughput, reproduce per-generation totals from raw samples, and reject any improvement claim based only on lower logging overhead or changed self-reported scores.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/event_stream.c`, `src/execution_events.c`, `src/tool_telemetry.c`, `src/vm.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/base.py` symbols `AgentStats.record_call`, `BaseAgent.execute`. Observed donor behavior: The donor records per-operation calls, timing, and errors, but has no attribution to live native code generations or causal improvement experiments.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 15 — Effect-aware executable composition

Let DSCO synthesize new executable functions by composing existing native cells and governed operations, then optimize the composition without changing its meaning. Define a composition graph with typed ports, explicit value bindings, ownership rules, and per-node effects. Distinguish pure computation, observations, writes, execution, secrets, untrusted input, and runtime control according to the existing capability model.

A compiler validates port compatibility, dependency closure, bounded fan-out, and effect ordering before lowering the graph to an executable program. Pure nodes may be fused or scheduled concurrently; effectful nodes require explicit ordering or a demonstrated independence contract. An observed failure is a typed result that cannot silently become successful evidence for a downstream stage. Native imports must preserve these distinctions rather than hide a tool call behind a function pointer.

Implement a minimal composition surface that builds and invokes a resident function with its own generation and source graph. Use explicit field mappings instead of the donor's implicit kwargs forwarding. Include at least one real optimization, such as fusing adjacent pure arithmetic stages, and show that the compiled function produces the same checked result with less dispatch overhead. Governance remains at every actual effect boundary, including newly synthesized functions.

Falsifying test: compose a local counter write, a read, and a pure transform. A candidate rewrite that duplicates the write or moves the read before it must be rejected or fail independent equivalence testing before activation. Two independent pure stages should execute concurrently and join correctly. Deny the required write capability and confirm the composed native entrypoint cannot bypass that denial. Changing only a JSON plan without executing the resulting resident composition does not satisfy acceptance.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/lingo_workflow.c`, `src/vm.c`, `src/execution_layer.c`, `src/capability.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.compose`, `MetaToolExecutor._execute_composed`. Observed donor behavior: The donor composes callables by forwarding values directly, without typed effect analysis or per-stage governance.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 16 — Profile-driven native specialization

Make DSCO turn a frequently executed general function into a faster specialized native implementation while remaining resident. Choose a bounded pure scorer or record transform with measured repeated constants, common schema, or a dominant branch. Gather profile evidence, identify an explicit specialization predicate, partially evaluate only the proven invariant expressions, compile or emit the candidate, and install a guarded dispatch entry.

Every specialized entry includes its guard, source-generation dependency, ABI, assumed constants, and independent equivalence evidence. Guard failure immediately chooses the generic implementation; it must not approximate or silently accept an unsupported input. If the generic source generation changes, retire or requalify dependent specializations. Define arithmetic overflow, floating-point behavior if supported, aliasing, and record bounds so an optimization cannot change semantics accidentally.

Use a small transformation set rather than an unrestricted compiler project. Constant folding, branch elimination under a schema guard, and fused field loads are sufficient if actual native instructions change and measured work decreases. Preserve the generic function as a deoptimization target and keep guard evaluation bounded. Prevent a candidate from reporting its own speed or correctness; collect both outside its body on matched workloads.

Falsifying test: train the profile on one record schema, qualify a specialization, and show lower dispatch or instruction cost on unseen matching inputs with identical outputs. Then supply a record with a different schema, an out-of-range field, and a changed constant; each must fall back to the generic path and return its exact reference result. Replace the generic generation and require invalidation of the old specialization. Report end-to-end timings as well as guard overhead; a faster isolated candidate that slows the full guarded call fails the performance objective.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/vm.c`, `src/ast.c`, `src/cost_frontier.c`, `src/autoresearch.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`, `MetaToolRegistry.compose`. Observed donor behavior: The donor rewrites Python ASTs and chains functions but does not specialize native executable paths from measured runtime invariants.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 17 — Verified ephemeral tools synthesized as native code

Let DSCO create a genuinely new executable tool during a session, qualify it against a declared contract, and use it immediately without rebuilding or restarting the host. Start with bounded pure data transformations such as a domain-specific parser or scoring function. The synthesis request names input/output schemas, examples, independent hidden checks, resource limits, and permitted effects; the resulting candidate is native code with a versioned tool descriptor.

Separate proposed tools from qualified tools in discovery. Only exact candidate bytes that passed the independent contract may become callable. Define a session-scoped namespace, collision behavior, replacement generations, and cleanup lifetime. A generated name cannot shadow a privileged built-in or silently widen its capability contract. Include source and executable provenance so the resident system can inspect the actual implementation it created.

Route calls through the ordinary tool execution gate even when the native implementation is dynamically registered. Do not attach candidate callbacks directly to the current VM's unchecked dispatch path. The tool's ABI receives a bounded typed input and controlled host services; pure tools receive no effect imports. If future effectful tools are supported, every leaf effect must reenter the existing gate with the correct tier and taint context.

Falsifying test: ask the implementation loop to synthesize a parser for a small record format, compile it, reject a visible-example memorizer on hidden malformed records, and register a surviving native candidate. A later invocation in the same PID must use that newly generated tool and return independently verified outputs. Attempt a colliding built-in name, an undeclared filesystem effect, and a capability-denied nested effect; all must fail without changing existing tools or exposing an unqualified candidate as ready.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `src/tools.c`, `src/execution_kernel.c`, `src/capability.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `define_tool`, `MetaToolRegistry.list_tools`. Observed donor behavior: The donor defines executable tools from source and lists them, but lacks independently verified native admission and DSCO capability enforcement.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 18 — Learn semantics-tested executable rewrite operators

Make DSCO learn reusable executable transformations from verified before-and-after program pairs, then apply them to new resident code. A learned operator contains a structural match, side conditions, replacement template, effect constraints, and provenance linking the examples that motivated it. Begin with a bounded IR or AST subset and a small family of transformations such as repeated pure-expression elimination or guarded arithmetic simplification.

Learning proposes an operator; it does not authorize that operator to rewrite arbitrary host code. Validate each proposal against independent equivalence tests, negative examples where side conditions fail, and held-out program shapes. Store counterexamples that narrow its applicability. Only qualified operators enter a library consulted by the candidate-generation loop; application still produces a fresh candidate that must pass its own task contract before activation.

Define how variable binding, integer width, overflow, aliasing, and side effects are represented so matching cannot confuse similar-looking but different programs. Use exact code-generation identities and retain the unmodified champion. A confidence score may guide exploration but cannot replace executable equivalence evidence. Provide the minimal resident-cell interface needed to demonstrate an operator changing native behavior or performance within the same process.

Falsifying test: train on an optimization that removes duplicate reads of a pure value, then present a superficially similar program whose second read observes a changed counter. The learned operator must reject the effectful match. Supply hidden overflow cases for an algebraic rewrite and require rejection if semantics differ. Finally apply a qualified operator to an unseen compatible program, compile or emit it, and show equivalent outputs with a measured performance gain. Operator selection based only on model praise or source similarity fails.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/autoresearch.c`, `src/ast.c`, `src/vm.c`, `src/self_improve.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`, `ASTAnalyzer._rename_function`, `MetaToolRegistry.transform_tool`. Observed donor behavior: The donor has hand-authored AST rename and decorator operations; it does not learn reusable transformations from verified improvement pairs.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 19 — An evolution curriculum that retains prior capabilities

Teach a resident DSCO implementation new capabilities through a staged executable curriculum while retaining what it already does correctly. Define task families with independent oracles, training examples, untouched holdout generators, explicit resource budgets, and nonregression obligations. The evolving object is a native function bundle or verified executable graph, not merely a growing prompt or an archive of suggestions.

A curriculum stage proposes new source or IR, compiles or emits a candidate, evaluates both new tasks and retained capabilities, and activates only a qualified generation. Track a vector of verified outcomes and costs instead of collapsing every objective into an opaque self-reported score. Declare which tradeoffs are allowed; a faster implementation that loses required correctness must fail. Preserve representative old cases plus adversarially discovered counterexamples, with holdouts protected from optimization leakage.

Start with a meaningful bounded domain, such as a parser learning additional syntax while preserving malformed-input rejection, or a scorer learning another constraint family. Use measured failures to choose the next learning stage and stop when no candidate qualifies within budget. Include generation lineage and exact code artifacts so a capability gain can be traced to executable changes. If no curriculum support exists, implement one small domain end to end before generalizing.

Falsifying test: create a candidate that solves the new family but intentionally breaks an earlier boundary case and require rejection before activation. Then admit a candidate that passes old and new hidden cases, demonstrate both through the same resident process, and verify its outputs originate from the new native generation. Shuffle training order to expose forgetting, and include an overfitted candidate exploiting visible examples. More training tokens or a higher model confidence score cannot establish success.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/autoresearch.c`, `src/self_improve.c`, `src/cost_frontier.c`, `src/blackboard.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` symbols `compile_with_mipro`, `compile_with_gepa`, `swarm_metric`. Observed donor behavior: The donor invokes optimizers with a default substring-or-nonempty-answer metric; it does not protect previously acquired native capabilities during evolution.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 20 — A closed-loop same-process native self-repair demonstration

Deliver a single runnable demonstration in which DSCO observes a defect in its own bounded resident native function, reasons about the failure, writes a candidate implementation, evaluates unseen cases, and activates the repaired machine-code entrypoint without restarting. The mutable region should be a meaningful pure parser or scorer with an independent reference oracle. The stable host, capability checks, and evaluator remain outside the region the improvement agent may rewrite.

The demonstration owns its complete minimal loop: capture a failing input and exact active generation; request a bounded repair using the configured model; compile or emit candidate native code; inspect its ABI and imports; evaluate training, counterexample, and hidden cases; atomically activate the qualified implementation; continue serving requests through the resident entrypoint. If supporting interfaces are absent, implement small local versions instead of requiring other numbered features.

Record source and executable hashes, entrypoint ownership, actual oracle results, costs, and process-start identity throughout. A test-only deterministic proposal source may verify mechanics, but the delivered live path must exercise genuine model-proposed code when explicitly invoked with a configured inference budget. Do not claim autonomous improvement from a fixture alone. Retain the champion and compatible state so a later qualified-looking regression can be withdrawn safely.

Falsifying test: the original version must fail a real hidden boundary case; the repair must change native executable behavior and pass untouched holdouts in the same PID with no exec or supervisor relaunch. Then present a later candidate that passes visible examples but fails a probation invariant: require rejection before activation or same-process rollback of future dispatch to the retained champion. Verify a fixture external-effect counter is never duplicated during evaluation. A source diff, prompt update, child-only result, or rebuilt installed binary is insufficient.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/autoresearch.c`, `src/self_improve.c`, `src/plugin.c`, `src/vm.c`, `src/execution_kernel.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/agent_system.py` symbols `SelfInterrogation.interrogate`. Observed donor behavior: The donor asks an LM structured questions about progress and improvement; it does not rewrite and activate resident machine code.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.

---

# 21 — Bind self-modification claims to executable evidence

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `NotesExtractor`, `UniversalMultiHopRAG.forward`. Extracted notes are free-form strings; sources are recorded at retrieval level, and each context is truncated to its first 500 characters.

A resident process must distinguish “this branch preserves cancellation” from evidence that the currently loaded procedure actually preserves cancellation. Build a proposed evidence-claim module with claim ID, proposition, claim kind, procedure ID, procedure revision, source digest, byte span, observation IDs, and status. Source claims require exact retrievable bytes; behavioral claims require an execution receipt identifying the exercised procedure revision, inputs, outputs, outcome, and verifier. Neither a model's explanation nor a successful compilation is sufficient behavioral support.

Use context-fabric objects for immutable content and a small indexed relation store for claim-to-evidence edges. Validate spans against the stored digest, preserve original quotations, and distinguish observed, inferred, contradicted, and untested claims. The resident optimizer should receive an explicit supported/unsupported result for its proposed replacement. Provide a minimal callable interface and local JSON fixture entrypoint so this feature works without another prompt's implementation. Existing Chronicle projection is telemetry, not independent proof; add verification metadata instead of relabeling exported events.

Challenge the built binary with two implementations sharing a function name: only the old one was tested. A forged new-revision receipt and an invented quotation must fail resolution. Then execute the new implementation through an owned fixture, attach its actual receipt, and show support changes only for matching claims. Preserve counterevidence and historical revisions. Restart storage and repeat resolution; identical evidence must yield identical claim status without inference.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/trace_kg.c`, `src/execution_kernel.c`. Proposed module: `src/code_evidence.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 22 — Investigate the missing obligations of a self-rewrite

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `MultiHopQueryGenerator`, `UniversalMultiHopRAG.forward`. The next query uses accumulated notes and a hop number; there is no explicit representation of unanswered obligations or coverage.

Teach the resident reasoner to turn a proposed procedure rewrite into falsifiable obligations before searching or editing. For an input-loop replacement, obligations might include input ownership, cancellation latency, exactly-once submission, bounded memory, and compatibility with queued work. Represent each obligation with predicate, relevant procedure revision, required evidence class, dependencies, status, and an explicit unknown reason. A source comment may satisfy a documentation question but cannot satisfy a runtime latency predicate.

Add a bounded investigation planner that selects the next unresolved obligation using importance, likely discriminating evidence, and cost. It should emit a concrete source query, trace query, or experiment request and then consume an actual observation to update the frontier. Integrate with existing plan dependency validation; do not mistake parent-child hierarchy for evidence dependencies. Satisfied, contradicted, unavailable, and exhausted states must remain separate. Export a structured readiness result that the replacement controller can consume without trusting prose.

Supply an independent local interface accepting a proposed rewrite and a small obligation set, so no numbered feature is required. Test a fixture where a candidate improves average response time but loses partial UTF-8 input during cancellation. The system must investigate the cancellation obligation, record the counterexample, and reject completeness despite the favorable average. A second fixture with a genuinely irrelevant obligation must require an explicit scoped exclusion, never silently mark it satisfied. Demonstrate bounded termination when an observation source is unavailable and persistent unfinished obligations across restart.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/plan.c`, `src/plan_dag.c`, `src/context_fabric.c`, `src/agent.c`. Proposed module: `src/investigation_obligations.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 23 — Resolve conflicting explanations through directed retrieval

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `MultiHopQueryGenerator`, `NotesExtractor`, `UniversalMultiHopRAG.forward`. All notes accumulate together; contradictory statements do not trigger a targeted distinguishing query or separate hypotheses.

Give the self-rewriting process a research operator that investigates contradictions instead of averaging them into a confident rewrite rationale. Accept two structured claims about one procedure, their evidence handles, version and temporal scopes, and the decision affected. Classify the unresolved distinction into source revision, dynamic path, configuration, entity identity, units, or a genuinely conflicting assertion. Generate a query for evidence that separates the competing explanations, then store the new observation and an explicit resolution or unresolved result.

Preserve the original claims and their support; resolution adds a relationship rather than deleting inconvenient evidence. The query planner must be bounded by retrieval and tool budgets, and it must never treat a higher lexical score as adjudication. Use a minimal local conflict record plus retrieval callback so implementation does not depend on a larger hypothesis engine. The returned object should name the discriminating observation and the candidate rewrites it rules out.

Build an adversarial corpus where an obsolete design note says callbacks execute without a mutex, the current source holds a mutex only under one configuration, and a trace captures that configuration. The first broad search should expose disagreement; the next query must target the condition and current call path. Assert that the resulting repair rationale names the conditional lock rather than concluding one source is universally false. Add a conflict that cannot be resolved from available evidence: both hypotheses survive, expensive rewrite selection pauses, and the result states the missing discriminant instead of fabricating consensus.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/semantic.c`, `src/trace_kg_store.c`. Proposed module: `src/contradiction_research.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 24 — Prevent copied evidence from becoming false consensus

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `UniversalContentRetriever.retrieve`, `UniversalContentRetriever.rerank`. Results from different retrievers are merged by raw score; no originating-source or copied-content independence is modeled.

Implement evidence fusion for a process that chooses its own replacement algorithms from source, traces, benchmark reports, and external documents. Every support item needs an immutable content key, originating observation or publication ID when known, derivation parents, retrieval source, and measured relevance. Compute bounded source families from explicit citation ancestry and exact or near-duplicate content. Treat unknown independence as unknown; a different filename, URL, or agent author is not automatically an independent observation.

Expose separate aggregates for retrieval relevance, independent supporting families, independent counterexamples, and uncertain duplicates. Do not invent a universal numerical probability from these counts. A single valid behavioral counterexample to a universal claim must remain decisive even when many summaries support it. Store derivation edges so conclusions can explain which support is duplicated and why. Use existing lexical and content-addressed primitives, with deterministic similarity thresholds and an inspectable fallback when comparison limits are reached.

Validate using a resident optimization fixture: twenty generated reports all quote one favorable benchmark, while a separate execution demonstrates a rare deadlock. The fused decision record must identify one favorable originating experiment and one independent counterexample; duplicating the reports cannot improve readiness for replacement. Also test two genuinely independent runs with identical textual summaries: explicit distinct execution receipts preserve their independence. Require stable results under shuffled ingestion order, bounded memory for duplicate floods, and a live binary query showing the retained dissent and provenance. Provide a minimal standalone evidence-ingest and evaluate surface, not a new fleet coordinator.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/semantic.c`, `src/vfs.c`. Proposed module: `src/evidence_fusion.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 25 — Stop self-investigation when retrieval adds no knowledge

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `UniversalMultiHopRAG.forward`, `UniversalContentRetriever.retrieve`. The loop always attempts num_hops, continues after errors, and reports configured hop_count rather than successful information-gathering steps.

Create a proposed research controller for the resident optimizer that can recognize when another retrieval hop will merely repeat itself. Maintain a bounded state containing unresolved question IDs, normalized query fingerprints, retrieved content identities, originating evidence families, accepted novel claims, failed sources, and observed cost. After each hop, compute explicit deltas: new source content, new independent observations, newly resolved questions, and newly exposed contradictions. Keep these separate from the model's subjective statement that a search was useful.

Select continue, broaden source class, run a discriminating experiment, or stop with an exhaustion reason. Permit a bounded number of exploratory hops that make no immediate progress, but cap both attempts and actual elapsed work. Empty retrieval, transport failure, repeated content, and sufficient evidence have different terminal states. Return the actual number of successful retrievals and resolved questions to the rewrite controller. Integrate through a minimal callback contract that can be driven by deterministic local fixtures.

Construct a loop whose query generator produces unlimited paraphrases while all sources return mirrors of one benchmark paragraph. The binary must terminate at the configured stagnation bound, preserve the unresolved correctness question, and avoid treating the repeated paragraph as stronger evidence. A second fixture introduces a genuinely new counterexample on the last allowed exploratory hop; it must be retained and reflected in the result. Exercise cancellation while a source is stalled and prove the resident input path remains usable. Record the stop decision with the exact novelty deltas needed to reproduce it.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/semantic.c`, `src/agent.c`. Proposed module: `src/retrieval_novelty.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 26 — Retrieve typed evidence from the resident runtime and source corpus

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `RetrievalContext`, `UniversalContentRetriever.retrieve`. A generic source string and raw score combine heterogeneous results without typed joins, calibrated ranking, or execution identity.

Build the resident process a retrieval operator over its source snapshots, procedure manifests, test receipts, failure traces, and learned explanations. Define typed records with content digest, record kind, procedure identity, revision, session or execution identity, timestamp, and source span. Queries combine exact predicates with textual relevance: for example, the current procedure's failed cancellation tests plus source regions discussing ownership. Exact identity filters must run before approximate ranking.

Use actual SQL tables and indexed joins for structured predicates, existing BM25 or TF-IDF for lexical retrieval, and the context broker's real embedding index only when embeddings are available. Expose which retrieval path ran. Do not create random vectors, hash strings into pseudo-embeddings, or advertise semantic search when a lexical fallback executed. Bound candidate counts and join fanout; preserve provenance through deduplication and return stable record handles rather than synthesized search descriptions.

The independent implementation should ingest a small local corpus and answer typed queries through the binary without external services. Seed two procedures with the same name in different revisions, copied documentation, an unrelated highly similar error message, and one exact execution receipt. The current-revision query must retrieve the exact failure and its source mapping while excluding the stale revision unless explicitly requested. Disable the embedding backend and verify honest deterministic fallback. Include a query joining a failed experiment to its input artifact and compiled procedure identity; demonstrate that returned references resolve to original bytes and cannot be mistaken for a proof that a proposed replacement works.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/vfs.c`, `src/semantic.c`, `src/context_fabric.c`, `src/trace_kg_store.c`. Proposed module: `src/runtime_retrieval.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 27 — Extract evidence that survives different chunk boundaries

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` — `ChunkedDocumentAnalyzer.create_chunks`, `ChunkedDocumentAnalyzer._aggregate_results`, `KnowledgeExtractionSignature`. Aggregation deduplicates normalized strings, records chunk starts rather than exact spans, and counts entity and bias occurrences repeatedly across overlaps.

A self-improving binary must not learn different facts merely because its context budget changed. Implement chunk-based evidence extraction with immutable document identity, encoding, absolute byte coordinates, optional line coordinates, and a verified quotation for every extracted proposition. Chunks carry mapping information into their parent object; extracted spans are resolved against the parent before acceptance. Reject invented spans and record uncertain mappings rather than assigning a chunk's start to every claim.

Reconcile overlapping observations by source interval and proposition identity. Identical observations of the same occurrence merge, while distinct occurrences retain distinct provenance. Preserve negation, temporal qualifiers, and numbers when proposing paraphrase equivalence; semantic similarity alone cannot merge contradictory propositions. Ensure chunk construction always progresses, validates overlap against effective chunk length, and handles sentence adjustment without looping. Produce deterministic partial results when one extraction fails.

Use a minimal extractor callback with structured output and an owned local test driver, so no external model or numbered prompt is required. Analyze the same source-and-design corpus with three chunk sizes and overlap settings. A key claim crossing a boundary must produce one canonical occurrence each time; a later actual repetition should remain a second occurrence. Include “safe under one writer” and “unsafe under two writers” to defeat careless semantic deduplication. Validate UTF-8 byte offsets and out-of-range model spans. Show the resident learning input remains identical after rechunking and that invalid extractor output cannot contaminate stored procedure assumptions.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/semantic.c`, `src/vfs.c`. Proposed module: `src/evidence_chunks.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 28 — Distinguish real contradictions from revision and time changes

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` — `ChunkKnowledge`, `ChunkedDocumentAnalyzer._aggregate_results`. Claims retain text and chunk location but no temporal validity, revision scope, typed predicate, or cross-document contradiction relation.

Create a contradiction graph for the resident binary's beliefs about itself. Represent subject identity, predicate, value, units, qualifiers, configuration, procedure revision, observation interval, assertion time, and evidence handles. Separate “was true for revision A” from “is currently true,” and separate a measured instance from a universal property. Candidate conflict detection may use semantic matching, but final edges must retain an explicit reason and comparable scopes.

Support incompatible-value, counterexample-to-universal, changed-over-time, superseded-revision, and unresolved-identity relations. A new observation must not destructively rewrite an old claim. Provide queries for current conflicts affecting a procedure replacement and historical changes explaining a regression. Store scope uncertainty explicitly; absent configuration is not proof of all configurations. Integrate with content-addressed evidence and existing trace storage using bounded incremental updates rather than rebuilding every claim pair.

Test a sequence where revision A is single-threaded, revision B adds concurrency, and a document still describes A. The graph must report a stale applicable assumption, not claim that historical A evidence was false. Then add two genuinely incompatible observations for B under the same configuration, including a counterexample to “never loses input”; that conflict must block a supported replacement rationale. Add different workloads and nonoverlapping time intervals to test false-positive suppression. Exercise restart and out-of-order evidence arrival through a real binary fixture, proving relation identities and current-conflict results remain stable.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/trace_kg_store.c`, `src/vfs.c`. Proposed module: `src/revision_contradictions.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 29 — Preserve modality and attribution in learned memory

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` — `KnowledgeExtractionSignature`, `ChunkKnowledge`, `ChunkedDocumentAnalyzer._aggregate_results`. The extractor distinguishes claims, beliefs and utterances, but document aggregation discards utterances and does not preserve modality or attribution relationships.

Implement learned-memory records that distinguish observation, hypothesis, conditional, forecast, attributed opinion, quotation, and verified inference. Each record carries an exact source reference, speaker or asserting agent when known, polarity, modal operator, temporal scope, and endorsement relation. Preserve nested quotation and rebuttal: a document can report a claim without endorsing it, and the resident optimizer can consider a hypothesis without believing it.

Define explicit promotion rules for using a record as a replacement precondition. Repeated retrieval or high access count must not convert quoted speculation into an established fact; promotion requires matching evidence and a recorded transition. Connect to existing memory tiers through a narrow eligibility hook, retaining compatibility with old untyped records by marking their status unknown. Provide retrieval that includes relevant hypotheses but labels them so executable reasoning procedures cannot silently consume them as facts.

Use a standalone ingestion fixture containing “If the queue were lock-free, latency might improve,” a quoted recommendation to remove a lock, and a later paragraph demonstrating why that recommendation is unsafe. The stored graph must retain the conditional and the rebuttal, and the candidate rewrite's precondition query must not report that lock removal is validated. Add a measured result explicitly supporting one restricted configuration; only that scope becomes eligible. Test nested quotations, negation, and a malformed attribution span. Show persistence and repeated recalls cannot change epistemic type without a new evidence-backed transition, even when legacy memory promotion would increase retention priority.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/session_memory.c`, `src/context_fabric.c`, `src/memory_tier.c`. Proposed module: `src/epistemic_memory.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 30 — Invalidate learned conclusions when their content dependencies change

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/analysis_cache.py` — `AnalysisCache._get_cache_key`, `AnalysisCache.get`, `AnalysisCache.set`. Analysis keys use PDF path, chunk size, overlap and model, omitting source content, extractor version, prompt signature and derived-claim dependencies.

Give the resident learner a content-addressed derivation store rather than a filename-based analysis cache. A derived result must identify all source objects, procedure revisions, extractor or evaluator version, normalized configuration, and parent conclusions. Its identity is computed from those immutable dependencies. Store dependency edges separately from display paths and maintain a reverse index for affected assumptions and learned procedures.

When new source bytes or a changed verifier arrive, create a new revision and mark dependent conclusions stale until recomputed. Do not mutate historical records or invalidate unrelated branches. Distinguish a changed display path from changed content, and a missing source from proven false content. For executable procedure adoption, stale required evidence produces an explicit unavailable-support result; retention of the old bytes does not mean the old conclusion applies to the current runtime. Bound dependency fanout and detect cycles at insertion.

Implement a minimal local ingestion, derive, update, and affected-query interface over existing storage, without assuming another prompt's claim graph. Seed a benchmark report, a derived performance comparison, and a proposed resident replacement that depends on it. Replace the report at exactly the same path while preserving file size and timestamp: the comparison and replacement support must become stale, while an independent cancellation proof remains valid. Rename an unchanged file and show its content-derived result remains reusable. Test a changed evaluator signature, missing dependency, cyclic insertion, and restart during invalidation. Expose the precise invalidation path in structured output, then demonstrate recomputation restores only matching descendants.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/vfs.c`, `src/plan_cache.c`. Proposed module: `src/reasoning_dependencies.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 31 — Verify decisive algorithm evidence against PDF pages and figures

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` — `PDFVisionAnalyzer._convert_pdf_to_images`, `PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`. The implementation transcribes rasterized pages, applies max_pages only after rendering the whole PDF, and treats response length as its main validity check.

Build a proposed evidence operator for the resident optimizer when an algorithm's justification comes from a paper containing equations, figures, or tables. Start from a concrete disputed claim and PDF content digest. Preserve page identity through textual extraction, retrieve relevant passages, and render only the bounded pages needed to verify that claim. Store the page image digest, region when available, text extraction, visual interpretation, and an explicit agreement or disagreement record.

The operator must distinguish transcription from validation. Require source-grounded values, axis labels, units, legends, and stated experimental conditions before converting a figure into an algorithm-selection premise. Do not claim a model-generated description proves the paper's broader conclusion. Integrate any external renderer or vision tool through the existing execution gate, with a local renderer fixture sufficient for independent development. If vision is unavailable, return an unresolved evidence requirement rather than inventing a visual reading.

Create a small deterministic PDF fixture where extracted text loses a minus sign and a chart uses logarithmic scaling. A candidate procedure appears faster under the corrupted interpretation. The binary must select the decisive page, preserve the page evidence, detect the mismatch, and prevent that interpretation from supporting adoption. Include an irrelevant large appendix and assert only the selected page range is rendered. Test a scanned page, rotated label, and incomplete transcription. Report exactly which interpretation changed and keep both text and visual observations as historical evidence. Acceptance is the corrected algorithm premise and bounded page processing, not merely producing an image or a long transcription.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/tools.c`, `src/execution_kernel.c`. Proposed module: `src/algorithm_evidence.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 32 — Make self-improvement performance claims executable

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` — `ChunkedDocumentAnalyzer._aggregate_results`, `ChunkedDocumentAnalyzer._extract_key_findings`. Quality is an average of positive model-generated scores; failed chunks are omitted, and no executable dimensional or denominator checks substantiate aggregate claims.

Implement a quantitative claim evaluator for the binary's own rewrite proposals. Represent each claim with metric definition, units, population, workload identity, procedure revision, numerator, denominator, aggregation method, missing-data policy, and source execution IDs. Keep measured values separate from derived values and model estimates. Compile supported estimators into deterministic local calculations rather than accepting arithmetic embedded in prose.

Support a bounded initial set: count, rate, arithmetic mean, selected quantiles with a named algorithm, and relative change. Validate dimensional compatibility, denominator identity, zero denominators, finite numbers, sample exclusions, and paired baseline/candidate workloads. Return supported, contradicted, or insufficient-data together with the exact calculation and included observations. A candidate's lower latency among completed requests cannot erase failed or cancelled requests from the comparison. Do not treat telemetry collection itself as an independently controlled experiment.

Use actual local benchmark fixtures for two versions of a small procedure. The candidate finishes half the requests quickly and times out on the rest; a naive mean says it improved. The evaluator must reject the unrestricted improvement claim or narrow it explicitly to completed requests while exposing the reliability loss. Add milliseconds versus microseconds, changed concurrency, and a percentile computed over averaged batches to challenge invalid comparisons. Verify repeatable calculations from stored evidence after restart. Expose a minimal evaluate interface usable by any resident procedure controller, and demonstrate that a genuinely improved candidate under matched workload produces a valid supported claim without consulting an LLM for the arithmetic.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/tool_telemetry.c`, `src/chronicle.c`, `src/context_fabric.c`. Proposed module: `src/quantitative_claims.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 33 — Localize its own failures through competing source hypotheses

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_integration.py` — `CognitiveHandlers._perceive_user_input`, `CognitiveHandlers._plan_response`, `CognitiveHandlers._reflect_on_response`. Perception uses keywords, planning uses input length, and reflection records response_received=True without relating failure observations to source hypotheses.

Create a self-diagnosis mechanism that turns a resident process failure into competing, testable source hypotheses. Start with an execution identity, procedure revision, observed invariant violation, relevant inputs, and trace evidence. Represent hypotheses as source predicates with predicted consequences, supporting observations, contradictory observations, and a proposed discriminating experiment. Map functions and call edges by stable revision-aware identities; proximity in a log is not causality.

Build a bounded fault-localization graph joining execution events to source regions and state transitions. Rank candidate causes using explicit evidence factors, expose uncertainty, and retain alternatives until an experiment rules them out. The initial implementation can support one failure class such as lost queued input or a double-owned buffer, with a local event adapter and immutable source snapshots. Emit a repair target only when its rationale identifies both the violated invariant and evidence connecting that target to the failure.

Test a fixture where the crash appears inside a renderer but the actual defect is an earlier buffer ownership transfer. A second plausible hypothesis blames the renderer's UTF-8 parsing. Give the system traces that initially support both, then execute a bounded experiment varying ownership while holding input bytes fixed. It must update the graph, retain the historical mistaken hypothesis, and localize the supported cause rather than choosing the last stack frame. Verify that observations from a different procedure revision cannot silently settle the current failure. Show the resulting repair rationale through the built binary with resolvable source and experiment evidence.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/trace_kg.c`, `src/trace_kg_store.c`, `src/context_fabric.c`, `src/execution_kernel.c`. Proposed module: `src/fault_hypotheses.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 34 — Record predicted observations before running self-experiments

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/integration_hooks.py` — `HookEvent`, `HookResult`, `HookManager.emit`. Hooks carry request IDs and ordered stop decisions, but do not encode hypotheses, preregistered predictions or comparison of expected versus actual observations.

Implement an experiment ledger for the resident learner with separate immutable prediction, execution, observation, and adjudication records. Before a diagnostic or benchmark runs, record the competing hypotheses, expected observable outcomes under each, controlled variables, procedure revisions, measurement rule, and decision consequence. Bind that prediction to an execution request ID. Completion attaches raw observations and the actual termination condition; adjudication applies the preregistered comparison without editing the prediction.

Handle timeout, cancellation, malformed output, and partial measurement as distinct outcomes. A failed tool call is not evidence against a hypothesis unless the prediction explicitly describes that failure mode. Allow a revised experiment design only as a new record linked to the old one. Expose a compact callback usable around existing governed tool execution, and fix event-shape assumptions locally rather than importing Python hook wiring. Keep exploratory observations possible, but label them exploratory and ineligible as preregistered confirmation.

Construct a fixture with two hypotheses for a queue stall: lock contention and network initialization. Preregister an experiment that delays the network while varying contention, then run actual local processes and record measurements. Deliberately provide an agent-generated conclusion that changes the prediction afterward; the ledger must reject the mismatch and retain the original disconfirmation. Add replayed completion events, an execution from the wrong procedure revision, and cancellation before measurement. The real binary must associate observations exactly once, expose unresolved outcomes honestly, and recover the same prediction-versus-actual comparison after restart. This feature supplies learning evidence; it must not itself authorize arbitrary new experiments.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/execution_kernel.c`, `src/event_stream.c`, `src/context_fabric.c`. Proposed module: `src/experiment_ledger.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 35 — Branch and merge competing research states without erasing dissent

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `UniversalMultiHopRAG.forward`, `NotesExtractor`, `AnswerSynthesizer`. A single notes list accumulates all reasoning and final synthesis; alternative research states and contradictory supports are not separately preserved.

Give one resident process a way to pursue competing explanations without contaminating each branch's assumptions. A research branch references an immutable parent state, hypothesis set, evidence scope, unresolved obligations, and candidate procedure identity. New observations append to that branch. Forking should share immutable evidence objects while keeping asserted assumptions and derived conclusions separate; it does not copy live executable state or replay historical tool calls.

Define a merge operation that unions independent evidence, deduplicates identical observations, detects incompatible assumptions, and preserves dissenting conclusions with their support. A merge is not a majority vote or last-writer-wins overwrite. It must distinguish mutually exclusive hypotheses from complementary findings and require an explicit adjudication record before a disputed premise can support a resident replacement. Bound branch count and retained artifacts, and allow discarding a branch's active status without deleting its counterexamples.

Implement a standalone branch store and JSON fixture surface atop context scopes and existing persistence; no separate agent scheduler is needed. Test two branches evaluating a lock-free queue and a simpler locked queue. One finds a throughput advantage, the other a reclamation counterexample. Merge in both arrival orders and assert the same combined evidence, unresolved safety issue, and preserved dissent. A third branch containing duplicated benchmark evidence must not create extra support. Exercise interrupted merge and restart, verifying atomic branch-head changes and unchanged parent histories. Demonstrate that the rewrite controller receives an explicit disputed state instead of a synthesized recommendation that hides the losing branch's observation.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/vfs.c`, `src/plan_dag.c`. Proposed module: `src/research_branches.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 36 — Execute cognitive stages with typed results and bounded repair

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_integration.py` — `CognitiveHandlers.handle_user_input`, `CognitiveHandlers._plan_response`, `CognitiveHandlers._reflect_on_response`. Actions are ordered, but handlers consume the original message; reflection unconditionally marks a response received, and the verifier exercises a mock rather than real-client reasoning.

Turn the resident binary's cognitive cycle into executable dataflow rather than a sequence of named callbacks. Define typed outputs for observation framing, obligation plan, execution request, measured result, and reflection verdict. Each stage names its input object hashes and procedure revision, validates the expected schema, and writes an immutable result. Dependencies therefore carry actual values and failure states, not merely “previous callback completed.”

Reflection must compare the execution result against the plan's predicates, identify missing or contradictory evidence, and choose accept, repair, investigate, or terminate unresolved. A repair creates a new bounded attempt with explicit changed inputs or procedure revision; it cannot relabel the failed output. Distinguish model transport failure, invalid structured output, failed experiment, and a correctly observed negative result. Use the existing validated plan frontier and governed execution path with narrow hooks, avoiding a duplicate scheduler.

Provide a minimal in-process stage fixture and one real binary entrypoint to execute the cycle independently. Start with a procedure asked to preserve sorted order that returns a subtly misordered list. The execution stage succeeds mechanically, but reflection must produce the exact counterexample and trigger one allowed repair; the corrected procedure then passes. Exhaust the repair budget with a second fixture and require a terminal unresolved verdict. Cancel between stages and resume from committed outputs without replaying completed effects. Verify the final response is generated from the accepted result and explicit limitations, not from the original input or a hardcoded success flag. Retain the failed attempt as learning evidence.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/plan.c`, `src/plan_dag.c`, `src/execution_kernel.c`, `src/agent.c`. Proposed module: `src/cognitive_dataflow.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 37 — Preempt long self-investigation without losing cognitive state

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` — `CognitiveArchitecture._action_executor`, `CognitiveArchitecture.stop`, `ActionStack.pop_ready`, `ActionStack.fail`. A single executor awaits each action; stop cancels it, results are not checkpointed, and failed prerequisites can leave descendants pending indefinitely.

Add cooperative preemption to long resident reasoning procedures while preserving their evidence and control state. Represent a continuation with procedure revision, stage ID, immutable input and result handles, unresolved frontier, owned execution IDs, cancellation reason, and resumable versus terminal status. Introduce safe checkpoints between bounded retrieval, extraction, experiment, and reflection steps. Never checkpoint raw borrowed pointers or assume a suspended procedure revision remains executable after replacement.

Keep user input ownership in the existing composer and event loop. A correction can pause the investigation, update the objective revision, and resume only compatible work. Completed tool effects are referenced by receipts and must not be replayed merely because a continuation restarted. In-flight operations follow their real cancellation contract; uncertain outcomes remain uncertain until reconciled. Separate cancellation of reasoning admission from termination of owned external effects. Implement a minimal continuation interface and local staged fixture, independent of a full new reasoning framework.

Drive the built binary through an owned PTY while a retrieval fixture stalls. Type a partial UTF-8 draft, submit an urgent correction, and verify responsiveness under a stated latency bound plus exact draft preservation. Resume the investigation and show that an earlier completed experiment executes only once, while conclusions depending on the superseded objective are invalidated. Replace the procedure revision before resume and require explicit incompatibility handling rather than jumping through stale state. Test cancellation at every checkpoint, a failed prerequisite, and restart with a committed continuation. Preserve all useful observations even when the investigation itself is abandoned.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/agent.c`, `src/event_loop.c`, `src/context_fabric.c`, `src/execution_kernel.c`. Proposed module: `src/reasoning_preemption.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 38 — Require evidence-sensitive completion or explicit abstention

Donor: `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` — `AnswerSynthesizer`, `UniversalMultiHopRAG.forward`. Confidence is parsed from model-generated text, defaults to 50 on parsing failure, and synthesis proceeds even when no retrieval succeeded.

Implement a completion evaluator for the resident process's self-improvement reasoning. Its inputs are explicit obligations, verified evidence references, unresolved contradictions, coverage status, observed execution outcomes, and the procedure revision being considered. Its output is supported-complete, contradicted, incomplete, or abstained with machine-readable reasons. A model's confidence may be retained as an uncalibrated annotation but must not override missing evidence, failed predicates, or a concrete counterexample.

Define a small deterministic initial policy: every required obligation needs matching evidence, no unresolved blocking contradiction may remain, and required tests must refer to the exact candidate revision. Unknown is not false, timeout is not success, and a source unavailable during research does not prove a negative claim. Permit a bounded next-action recommendation identifying the missing observation; do not autonomously widen authority or keep investigating forever. Export a predicate the resident replacement controller can call, with an independent fixture interface for testing before any live rewrite machinery exists.

Build adversarial cases where every retrieval fails but synthesis returns 99 percent confidence, where only an obsolete revision passed tests, and where average speed improves despite a cancellation counterexample. Each must produce the appropriate incomplete or contradicted status and retain its evidence. A genuinely complete local candidate should become eligible only after its final required observation is attached. Shuffle evidence order, restart storage, and inject malformed confidence strings to prove deterministic status. Show through the built binary that a confident narrative cannot turn an unverified learned procedure into a completed improvement.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/plan.c`, `src/context_fabric.c`, `src/execution_kernel.c`. Proposed module: `src/evidence_completion.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 39 — Replay reasoning under controlled framing interventions

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` — `ChunkedDocumentAnalyzer.analyze_chunk`, `ChunkedDocumentAnalyzer._aggregate_results`. Bias detection emits labels from the first 2,000 characters; there is no controlled intervention establishing whether framing actually changes a decision.

Build a paired replay operator for a resident reasoning procedure that recommends a code replacement. Capture the procedure revision, exact structured inputs, evidence object set, inference configuration, tool-result fixtures, and decision schema. Run a baseline and one explicitly controlled intervention: shuffle evidence order, replace prestige labels with neutral source IDs, remove duplicated support, or reverse irrelevant framing. All factual content and authorized tool observations remain fixed unless the intervention explicitly changes them.

Compare structured decisions, cited support, rejected alternatives, and required obligations. Distinguish stochastic variation from intervention sensitivity using a bounded repeated-trial policy or deterministic fixture, and report the actual repetitions and uncertainty. Do not label every changed answer a cognitive bias; require an invariant that the chosen intervention was supposed to preserve. Replayed tool results are marked historical fixtures and must never trigger their original external effects. A changed fact is a separate experimental condition, not an irrelevant-framing test.

Use an owned local procedure fixture that chooses a faster but unsafe rewrite only when its source is labeled prestigious. Neutralizing that label must expose decision sensitivity while preserving the same counterexample evidence. Add an order-sensitive fixture and a control where new factual evidence legitimately changes the decision. The evaluator must flag the first two and accept the control distinction. Execute the replay through the built binary, verify exact input hashes and one-variable differences, and preserve both result branches. Provide a minimal replay interface without requiring another numbered feature or promising to expose hidden model reasoning.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/context_fabric.c`, `src/execution_kernel.c`, `src/event_stream.c`. Proposed module: `src/reasoning_replay.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 40 — Reopen learned procedures when their evidence expires

Donor: `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_cache.py` — `PDFTextCache._get_file_hash`, `PDFTextCache.get`. PDF text reuse is content-addressed, but downstream analysis and learned conclusions have no validity lease tied to runtime changes or expiring observations.

Implement an evidence-validity monitor for learned reasoning procedures inside the resident binary. Each supporting observation declares its applicability: immutable source identity, procedure revision, runtime configuration digest, workload class, optional wall-clock validity, and conditions requiring remeasurement. Distinguish storage retention from epistemic validity. An old result may remain valuable historical evidence even when it no longer justifies using a learned procedure now.

On a relevant revision, configuration, source, or validity-window change, mark affected support stale and enqueue a bounded revalidation obligation. Do not discard all memory or automatically replay expensive experiments. The controller should receive an explicit support-loss event naming affected procedures and reasons; it can continue under an established fallback or pause adoption according to existing runtime policy. Preserve already running invocation semantics, and do not switch code merely because a timer fired. Use monotonic deadlines for process-local timers and persisted wall-clock metadata with conservative handling of clock jumps.

Provide a standalone registration, event, and affected-procedure query surface integrated into the existing event loop and content store. Test two learned procedures: one depends on a benchmark under four workers, the other on immutable parser examples. Change worker count and require only the benchmark-dependent support to reopen. Then advance a controlled clock, replace a source at the same path, and restart the process; pending revalidation remains explicit and deduplicated. Demonstrate no automatic external request, no broad memory eviction, and no inference-confidence shortcut. Attach a fresh matching observation through the real binary and show current support is restored only for the intended procedure revision.

Implement in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, integrating `src/session_memory.c`, `src/memory_tier.c`, `src/context_fabric.c`, `src/event_loop.c`. Proposed module: `src/evidence_validity.c`; interface names are proposals. Preserve dirty work and capability gates; tool paths use `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install. Report live evidence and residual limits.

---

# 41 — Compile reasoning methods into executable obligations

A resident program deciding how to rewrite itself needs an executable reasoning procedure, not a collection of impressive labels. Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect donor `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/core.py`: `CognitiveAgentProgram` sends selected lenses to one ChainOfThought, while `select_lenses()` uses substrings. The new capability compiles method selections into operations whose applicability and completion are independently checkable.

Define a bounded reasoning-plan schema with typed observations, candidate-code handles, hypotheses, assumptions, operations, and terminal obligations. Initial operations should include comparing measured alternatives, checking a precondition, and updating a probability from complete inputs. Each operation declares required fields, deterministic versus model-assisted work, output type, cost bound, and failure state. Compile dependencies, reject cycles and incompatible types, and preserve the distinction between unavailable input and an executed test that failed. Method names inside quoted source text must not activate operations. A model may propose a plan; only the compiler establishes that it is executable.

Extend `src/prompt_program.c` through a new reasoning-plan module, keeping its existing local compilation contract intact. Connect a narrow plan-evaluation hook to `src/self_improve.c`; if no candidate interface exists, introduce a minimal handle plus typed observations and a returned decision record. A proposed `dsco reason compile`/`run` surface must support inspecting the plan before its governed effects. Producing prose that says a calculation occurred is not a completed operation.

Use the actual worktree binary with a candidate replacing a linear search. Require separate correctness, workload applicability, and measured-benefit obligations before it can recommend adoption. Feed an incomplete Bayesian node and prove no posterior is generated; provide a cycle and a unit/type mismatch and verify bounded rejection. Compare “William wrote this function” with a genuine forecast request to exclude substring dispatch. Show one valid plan completing, one blocked plan explaining its missing input, and the exact code-decision hook result.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 42 — Compete causal fault hypotheses before rewriting

Before DSCO changes a failing function in memory, make it compete concrete fault explanations against observations. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, use `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` as the donor: `AbductiveReasoningSignature` proposes explanations, while `ScientificReasoningWorkflow.forward()` leaves prediction derivation as a comment. Implement that missing operational link for runtime repair.

Represent each hypothesis with the affected code region, claimed mechanism, necessary assumptions, expected observations, and an explicit falsifier. A generated patch must identify which hypothesis it addresses. Generate bounded probes from contrasts between hypotheses: two explanations predicting the same result do not justify that probe. After a governed execution returns, classify each prediction as supported, contradicted, or unresolved and recompute the live candidate set. Distinguish a failed probe harness from evidence against the fault hypothesis. Preserve a null hypothesis that the symptom is environmental or unreproduced; do not force a code mutation.

Implement the hypothesis evaluator in a new C module with hooks near `src/self_improve.c` and existing tool dispatch in `src/tools.c`; inspect `src/agent.c` only for the minimal observation handoff. Expose a proposed `reason_fault` operation accepting symptom, candidate hypotheses, probe descriptions, and actual results. If the runtime lacks typed candidate handles, introduce a small local interface that returns the chosen fault target and unresolved alternatives without inventing a native loader.

Acceptance should reproduce intermittent wrong output in a local C fixture that can be caused either by stale cached state or an off-by-one boundary. A cache reset discriminates only some cases; a boundary matrix discriminates the rest. Prove that an attractive patch for the wrong hypothesis is rejected despite fixing one example. Inject a probe timeout and verify it eliminates neither hypothesis. Include a nondiscriminating probe and show its low priority. The real binary must select a justified candidate or explicitly request the next distinguishing observation, with observed outputs attached to each elimination.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 43 — Check deductive fragments for candidate preconditions

Make candidate rewrites carry small checkable arguments about the assumptions they require. The donor `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` defines `DeductiveReasoningSignature`, but its logical validity is model output. In `/Users/arthurcolle/Dsco/dsco-cli` or the assigned worktree, introduce a deliberately limited proof-fragment checker for decisions about resident code.

Start with typed integer bounds, array extents, nullability, equality, conjunction, implication, and explicit inference rules. Distinguish signed machine arithmetic from mathematical integers. A proof fragment names premises by supplied observation or contract handle, applies a supported rule, and derives a candidate precondition or postcondition. Unsupported constructs return unknown, not valid. Check premises against the exact candidate version and execution contract. Changing code or a premise invalidates dependent proof results. Do not claim full C verification: the output must enumerate the fragment proven, assumptions remaining, and properties left to runtime tests.

Place the checker in a new C module. Reuse numeric parsing where appropriate from `src/eval.c`, but do not treat its expression evaluator as a proof system. Add a narrow decision hook in `src/self_improve.c` and a proposed governed `reason_proof` operation through `src/tools.c`. If no candidate lifecycle API is present, the checker accepts immutable source/contract digests and returns an obligation report that a future lifecycle can consume. There must also be a concrete binary-accessible way to submit and check a fragment now.

Demonstrate a candidate replacing a guarded array lookup with an unchecked fast path. The fragment can justify the change only when the caller contract proves the index bounds on every supported path. Test a valid modus-ponens derivation, affirming the consequent, a missing premise, a stale source digest, and `INT_MAX + 1` under signed semantics. A fluent model assertion of “proven safe” must have no effect on the checker. Run the actual binary against both valid and invalid fragment fixtures and show that unproven obligations prevent its recommendation to remove the guard.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 44 — Perform fully specified Bayesian candidate revision

A failed test, benchmark gain, or surprising trace should change DSCO's belief in a candidate through explicit probability inputs. Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. The donor `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` has `ProbabilisticReasoningSignature`, which omits the alternative likelihood, and `ScientificReasoningWorkflow.forward()`, which supplies placeholder probabilities. Replace those gaps with deterministic arithmetic for runtime self-improvement decisions.

Define a hypothesis as a concrete candidate claim, such as “this patch removes the observed race without changing valid outputs.” Require a prior with provenance and either both conditional likelihoods or a normalized hypothesis likelihood table. Reject NaN, infinity, invalid probability ranges, zero-normalization cases, and ambiguous conditioning. Update in log space where needed; attach the exact inputs and arithmetic result. An LM can estimate a range and explain assumptions, but its asserted posterior cannot override the calculation. Produce sensitivity intervals when prior or likelihood inputs are ranges, and distinguish epistemic assumptions from measured frequencies.

Repeated evidence needs an independence declaration. The same probe result replayed or summarized by multiple agents cannot multiply evidence. Correlated probes should form one modeled dependence group or remain explicitly unresolved. Add a new C probability module, using `src/eval.c` only where its semantics are appropriate, with a small `src/self_improve.c` decision hook and proposed `reason_update` tool in `src/tools.c`. Supply a minimal candidate-claim input schema if none exists.

Through the real binary, verify a one-percent prior, 0.99 sensitivity, and 0.05 false-positive rate produce approximately 0.1667 posterior after a positive result. Missing false-positive information must return insufficient inputs. Replay the same evidence twice and show no additional update. Test contradictory evidence, extreme likelihoods, and uncertain priors. Finally compare two code candidates with identical rhetorical confidence but different likelihood evidence; the returned decision must reflect the computed posterior and unresolved assumptions, not prose length or agent votes.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 45 — Identify causal benefit of runtime interventions

When a new resident function appears faster, DSCO should determine whether the code caused the improvement. Inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py`: `REASONING_METHODOLOGIES` describes causal graphs, and `MethodologySignature` asks for causal links, but neither executes interventions. In `/Users/arthurcolle/Dsco/dsco-cli` or its worktree, implement a causal comparison layer for proposed runtime changes.

Represent treatment, outcome, workload features, cache state, contention, warmup, and measurement error as typed variables. A candidate declares which variable it changes and which assumptions support attributing an observed outcome to that change. Support an initial bounded class of randomized paired or interleaved comparisons; do not label arbitrary before/after measurements causal. Preserve intervention identity and reject comparisons where code, input, and configuration changed together without an identified design. Unknown confounding remains an explicit conclusion. Graph cycles should require a declared time-indexed model rather than accidental traversal.

Add a new causal-analysis module around measurements already available to `src/self_improve.c` and `src/baseline.c`. Inspect `src/hotplug.c` for a narrow candidate identity hook, without rebuilding its lifecycle. If no resident comparison interface exists, provide a minimal callback interface for invoking two equivalent implementations on controlled inputs and returning an effect estimate plus assumptions. A proposed `reason_intervention` operation should accept the design, observed batches, and candidate handles.

Use the actual worktree binary with old and candidate implementations of a local lookup routine. First run an intentionally confounded sequence where warming the cache helps both; the decision must refuse causal attribution. Then randomize paired order with identical workloads and show the genuine treatment effect separately from order. Include an irrelevant intervention and a workload interaction where the candidate helps large inputs but hurts small ones. Its recommendation must specify the supported operating region and uncertainty instead of announcing a universal speedup. Record the design, actual measurements, exclusions, and causal claim accepted or withheld.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 46 — Forecast candidate performance from comparable episodes

Give DSCO an outside view before it rewrites a hot function: what happened to genuinely comparable changes? `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_reasoning_modules.py` implements `ReferenceClassForecasting.forward()` as two LM calls, with `ReferenceClassIdentification` generating its statistics. Build empirical cohort forecasting in `/Users/arthurcolle/Dsco/dsco-cli` or the assigned worktree; generated base rates must never substitute for observations.

Define cohort eligibility using algorithm family, changed contract, input distribution, hardware/runtime identity, baseline bottleneck, and available validation. Include rejected and failed candidates in the denominator. Separate forecasts for correctness survival, speed gain, memory change, and validation cost. Compute a distribution or conservative interval from observed episodes and expose sample size, missing outcomes, weighting, and sensitivity to eligibility rules. Inside-view adjustments require explicit measurable differences and must not silently invent successful comparators. Sparse or incompatible cohorts produce an unsupported forecast with a useful next-data requirement.

Inspect `src/learned_cost.c` and `src/cost_model.c` for existing history/prediction behavior, then implement the new cohort estimator in a separate module. Its job is inference over supplied records, not another evidence store. Add a small hook in `src/self_improve.c` returning predicted tradeoffs before a candidate experiment. If there is no episode adapter, define a minimal bounded JSON input containing observed outcomes and eligibility metadata. Provide a proposed `reason_reference_class` operation so the actual binary can reproduce each forecast.

Create a fixture with ten optimization attempts: three accepted speedups, four neutral changes, and three correctness failures. Compare the honest forecast with a survivor-only subset and require the exclusion report to expose the difference. Mix hardware generations and verify that unsupported transport does not create false precision. Hold back later episodes to score forecast error and interval coverage. Test an empty cohort and one misleading near-match whose task text resembles the candidate but whose algorithmic bottleneck differs. Close with the exact cohort and arithmetic supporting the runtime's proceed, gather-more-data, or abstain recommendation.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 47 — Choose code changes on robust constrained frontiers

Code promotion should optimize an explicit objective while respecting properties that cannot be traded away. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, implement a robust decision frontier for resident rewrite candidates. The donor `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py` describes `decision_theory` inside `REASONING_METHODOLOGIES`; `MethodologySignature` emits an expected-value note, not a constrained optimizer.

Represent each candidate, including retaining the incumbent, by observed and uncertain correctness, latency, throughput, memory, disruption, and validation-cost attributes. Hard contract obligations are feasibility gates before utility calculations. A missing correctness result is unknown feasibility, not a zero penalty. Among feasible candidates, compute Pareto dominance and a small robust choice rule such as minimax regret across declared workload scenarios. Keep operator preferences explicit, versioned inputs; never infer that speed authorizes sacrificing a required invariant. Separate sensitivity to empirical uncertainty from sensitivity to preference weights.

Reuse measured-cost concepts from `src/plan_optimizer.c` and `src/cost_frontier.c`, but implement candidate-code decisions in a new module with a small `src/self_improve.c` hook. The module should consume bounded candidate records and return feasible set, frontier, chosen action, rejected constraints, and assumptions under which the choice changes. A proposed `reason_promote` interface can expose this now; if the native lifecycle is absent, supply the decision callback and demonstrable incumbent/candidate handles rather than claiming activation occurred.

Run the actual binary on three local implementations: fastest but incorrect on an empty input, moderate speed with lower memory, and the incumbent. The incorrect candidate must remain ineligible under every weight setting. Construct two workload mixes in which neither valid implementation dominates and show the tradeoff rather than hiding it in one score. Add an unknown validation outcome, a hard memory ceiling, and a scenario with high downside despite favorable mean latency. The selected action must follow the documented rule, and a no-change recommendation must be possible. Report exact inputs and reproducible calculations for every exclusion and tie.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 48 — Select the next experiment by decision value

Make the self-improving runtime spend its next unit of compute on the question most likely to change its code decision. Work in `/Users/arthurcolle/Dsco/dsco-cli` or its isolated worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/core.py` supplies `CognitiveAnalysisSignature.cruxes` and `next_steps` as text. Implement a value-of-information decision algorithm that turns such cruxes into ranked, executable experiments.

An experiment proposal declares candidate decisions, possible outcomes, outcome probabilities or bounded ranges, which uncertain parameter it measures, resource cost, and allowed tool effects. Compute current best expected utility, expected utility after each possible observation, and net expected value of sample information. Do not equate uncertainty reduction with useful work: a variable may be uncertain yet irrelevant to choosing code. For interval-valued inputs, return lower/upper value estimates and distinguish robustly worthwhile experiments from assumption-dependent ones. Include the option to stop investigating and retain the incumbent.

Implement the calculation in a new C module, consulting cost estimates from `src/plan_optimizer.c` and integrating the selected proposal through a narrow `src/self_improve.c` hook. A proposed `reason_next_experiment` operation in `src/tools.c` accepts a complete decision table and returns ranked experiments plus arithmetic. If automatic experiment submission is unavailable, implement a minimal governed callback taking the selected probe and returning a typed observation; ranking alone must still be inspectable independently.

Use the real binary on two candidate algorithms: their selection depends on the fraction of large inputs, but not on an unrelated documentation fact. Give the unrelated question greater entropy and verify it receives no decision value. A bounded workload probe should be selected when its expected improvement exceeds cost; raising that cost should select no experiment. Include a potentially informative destructive probe and ensure existing permissions block execution without changing the mathematical ranking. After a permitted local probe returns, recompute the selected implementation and demonstrate the predicted decision branch. Record estimated versus realized information value without treating one favorable outcome as calibration proof.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 49 — Turn rewrite premortems into executable conditions

A premortem becomes useful to a self-rewriting binary when its anticipated failures can actually be detected. Starting in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, operationalize `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_reasoning_modules.py`'s `PreMortemAnalysis`. That donor generates failure scenarios and mitigations; the new mechanism compiles candidate-specific scenarios into measurable conditions governing the code decision.

Each scenario names the candidate assumption it threatens, observable signals, units, evaluation window, minimum sample support, threshold, missing-data behavior, and response recommendation. Support bounded predicates over current measurements and previous windows, with hysteresis for noisy signals. A trigger may recommend reject, gather evidence, or stop the candidate experiment; it cannot silently grant new execution authority. Keep a human-readable explanation generated from the predicate, not an unrelated prose description. Unmeasurable scenarios remain open obligations instead of being labeled monitored.

Build a small predicate compiler and evaluator in a new C module, using observations near `src/baseline.c` and candidate decisions near `src/self_improve.c`. Reuse any existing lifecycle for stopping a candidate; otherwise expose a minimal callback that records a stop recommendation and prevents the next experimental invocation. A proposed `reason_premortem` tool through `src/tools.c` should accept scenarios, validate them, and evaluate controlled measurement batches. Bind predicates to the applicable candidate contract so an unrelated update cannot inherit stale conditions.

Exercise the actual binary with a rewritten allocator fixture that improves average time but grows retained memory only after repeated workloads. The compiled failure predicate must detect the trend at the defined support threshold. Show that a single noisy spike does not flap decisions, absent telemetry produces unknown, and a process-counter reset is handled explicitly. Include “users might dislike the change” without an observable definition and reject any claim that it is monitored. Demonstrate a safe fixture continuing and the leaking candidate being stopped or refused its next invocation, with the exact condition and measurements responsible.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 50 — Transfer algorithms through structural analogy checks

DSCO should reuse an algorithmic improvement because its structure fits the target, not because two descriptions share vocabulary. In `/Users/arthurcolle/Dsco/dsco-cli` or the assigned worktree, convert ideas from `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py`'s `AnalogicalReasoningSignature` into a checked transfer mechanism. The donor asks an LM for mappings and disanalogies; the new feature verifies the conditions under which a candidate transformation can apply.

Represent a reusable algorithm pattern as relations among inputs, state, ordering, ownership, effects, and required invariants. A target mapping binds those roles to its actual contract. Validate essential relations, identify unmapped obligations, and generate counterexamples at the boundary of each mapping. A successful analogy proposes a candidate implementation strategy and test obligations; it does not establish correctness by itself. Preserve material differences such as idempotence, numeric range, concurrency, aliasing, and resource lifetime. Support explicit refusal when no sound mapping is available.

Place the mapping and applicability checker in a new C module. Use `src/skill_index.c` only to locate candidate procedures, then pass the checked transformation recommendation through a small `src/self_improve.c` hook. A proposed `reason_transfer` operation in `src/tools.c` should accept source pattern, target contract, and proposed bindings. If no transformation registry exists, provide two bounded built-in pattern fixtures and a public typed schema; do not require a separate learning subsystem to exercise the algorithm.

Demonstrate transfer of memoization from a pure deterministic function to another pure function, with measurements showing whether its workload justifies the extra memory. Reject the same transfer onto a side-effecting counter increment despite superficially identical input/output signatures. Test an ownership mismatch for buffer reuse and an ordering mismatch for parallel reduction. The real binary must show which relation failed and the smallest target-specific counterexample, not merely a low similarity score. Include a target where one optional disanalogy is harmless and correctly distinguish it from a violated required condition. Report the recommended candidate, outstanding obligations, and actual transfer-test results.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 51 — Ground decision-bias audits in source spans

Audit the reasoning used to rewrite DSCO without inventing defects to fill a bias checklist. Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. The donor `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py` contains `BiasScanSignature`, which requests exact triggering spans, while `_dict_list()` still accepts incomplete records. Implement a decision-audit validator that requires a supported causal connection between quoted reasoning and the code choice.

An audit allegation identifies an exact span in a supplied decision record, the asserted reasoning error, the premise or weighting it affects, an alternative interpretation, and a concrete test that could clear the allegation. Validate UTF-8 offsets and exact quote equality. Separate rhetorical wording from load-bearing reasoning: mentioning prior effort is not a sunk-cost error unless that effort is used to justify future resource allocation. The output can explicitly find no evidenced bias. Severity should follow the decision consequence or broken obligation, not the presence of a familiar label.

Implement this as a new C decision-audit module using bounded JSON parsing from `src/json_util.c`, with a small hook in `src/self_improve.c` and proposed `reason_audit` operation through `src/tools.c`. Consume evidence handles supplied by existing storage; do not create another evidence database. If there is no structured decision object, define a minimal document containing candidate options, quoted rationale, and material premises. Audit output must distinguish validated allegation, unsupported allegation, and unresolved relevance.

Drive the actual binary with paired candidate-promotion memos. Both mention three days spent implementing a patch; only one treats that spent effort as a reason to deploy despite a regression. Verify the other is not accused merely for mentioning history. Add a fabricated quotation, Unicode offset mismatch, a correctly cited but irrelevant sentence, and a persuasive claim that lowering safety checks improves measured success. Require explicit identification of the altered success criterion. Finish by showing how a validated allegation changes the decision's obligations while unsupported allegations cannot silently lower confidence or veto useful code.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 52 — Test code decisions for framing invariance

Expose whether the resident rewrite planner changes its recommendation when the facts stay the same. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, turn `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_reasoning_modules.py`'s `ConsiderOpposite` and `DualProcessReasoning` into controlled metamorphic decision tests. Those donor components reconsider a claim in prose; this feature measures invariance under explicitly meaning-preserving transformations.

Define transformations for option order, candidate aliases, equivalent gain/loss rates, irrelevant author prestige, and an unrelated numerical anchor. The transformation must declare which semantic fields remain identical and provide a reversible mapping of candidate IDs. Do not assume that arbitrary paraphrases preserve meaning. Compare normalized chosen action, feasibility judgments, factual claims, and uncertainty across original and transformed records. A recommendation difference is a diagnostic requiring explanation or another test; it is not automatically proof of a particular psychological bias.

Use `src/prompt_program.c` for deterministic template construction, with a new invariance-evaluation module and a small decision hook in `src/self_improve.c`. Expose a proposed `reason_invariance` operation in `src/tools.c`. If no planner API is available, define a bounded callback accepting the decision record and returning candidate selection plus stated premises, so both deterministic fixtures and actual model-assisted planning can be tested. Keep the comparison oracle separate from the planner being evaluated.

Run the actual binary against a pair of candidate kernels with equal contracts and a fixed outcome table. Change “90% pass” to “10% fail,” reverse candidate order, and replace famous-author wording with anonymous wording. The fixture oracle must identify the same admissible option. Inject a deliberately anchor-sensitive planner response and verify the diagnostic catches its reversal while mapping IDs correctly. Include a legitimate transformation that changes a hard memory ceiling; it must be labeled semantic change and excluded from invariance scoring. Report inconsistent decisions with exact differing premises and measured frequency over a bounded set, without manufacturing a numerical bias-reduction claim.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 53 — Propagate critique into code decisions

A critic proving a candidate unsafe must change the final code decision, not disappear into a report appendix. The exact donor defect is in `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py`: `HybridReasoningPipeline.forward()` generates debiased analyses but returns the original `multi.synthesis`. Build a correction-propagation mechanism in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree that makes such stale synthesis detectable.

Represent material critiques as challenges to named premises, measurements, transformation steps, or candidate obligations. Each challenge receives one disposition: accepted with a concrete revision, rebutted with independently checkable evidence, or unresolved. Accepted corrections invalidate downstream conclusions derived from the old premise. A new candidate version requires its own affected checks; unrelated results may remain valid if their dependencies are unchanged. Merely appending “critique considered” cannot discharge an obligation. Preserve the original and revised decisions for comparison without requiring a new evidence-storage system.

Implement a new reconciliation module with a narrow hook in `src/self_improve.c`. Use `src/prompt_program.c` only where dependency resolution is reusable. A proposed `reason_reconcile` tool in `src/tools.c` should consume decision, critique, dispositions, and candidate revisions, returning which conclusions remain justified. If there is no typed candidate graph yet, provide a minimal bounded schema for premises, dependency edges, source digests, and a returned decision status; the capability must work independently.

Exercise the real binary with an initial optimization that removes a bounds check, followed by a critic supplying a failing empty-input case. A final answer repeating the original promotion recommendation without revising or rebutting that premise must fail reconciliation. Show a corrected candidate restoring a necessary guard and retaining unrelated validated improvements. Add an irrelevant stylistic critique that does not invalidate correctness, a stale critique for another candidate version, and circular rebuttals referencing one another. Record the final candidate recommendation and a complete disposition for every material challenge. The test passes only when actual dependency consequences propagate, not when a model promises to heed criticism.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 54 — Investigate rewrite candidates with independent dissent

Build a self-modification review that can retain useful dissent even when most agents agree. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_reasoning_modules.py`'s `ArgumentativeReasoning.forward()`: a counterargument call feeds one synthesis call. Extend the idea into independent investigations of a resident-code candidate with explicit adjudication rules.

Assign investigators distinct obligations, such as semantic equivalence, numeric boundaries, workload suitability, or adversarial scheduling. Give each the same frozen candidate and contract, but withhold other investigators' conclusions until their initial findings are fixed. A finding contains a reproducible witness or a precisely scoped unresolved concern. Normalize duplicate evidence so repeated reports of one observation do not count as independent support. Adjudication should prioritize verified counterexamples over consensus and preserve disagreement when tests cannot settle it. A stronger model's unsupported opinion receives no special authority.

Use existing roles and execution concepts in `src/topology.c` and `src/swarm.c`; implement the investigation protocol and adjudicator in a new C module with a small `src/self_improve.c` hook. If a programmatic investigator interface is absent, define a bounded callback for task, frozen inputs, and structured findings, with local subprocess or fixture implementations that allow immediate testing. A proposed `reason_investigate` surface should report obligations, findings, resolved challenges, and dissent that blocks recommendation.

Test through the actual binary using an optimized parser that passes ordinary examples but fails on a Unicode boundary. Three investigators should return the same attractive benchmark while a fourth provides the exact failing byte sequence. The candidate must not be recommended on a three-to-one vote. Add an invalid witness and prove the adjudicator checks it rather than trusting adversarial language. Verify blinding by recording investigator inputs, and inject one timeout without converting missing review into approval. Demonstrate that independent positive findings can support a corrected version while an unresolved material dissent remains visible with the next concrete test required.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 55 — Calibrate rewrite confidence against actual outcomes

Replace persuasive self-confidence with outcome-grounded estimates of whether a runtime rewrite will work. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/eval_bias_reduction.py`'s `evaluate_test_case()` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py`'s `ReasoningToolkit.reduce_biases()`. A failed verification detector becomes zero remaining biases, which can look like improvement. The new feature must make that confusion impossible in candidate decision statistics.

Define prediction targets before experiments: for example, passing a frozen correctness suite and meeting a latency ceiling on a specified workload. A confidence value belongs to that precise event and horizon. Record observed pass, observed fail, timeout, invalid measurement, and unresolved outcome separately. Compute Brier score and reliability bins only on eligible resolved outcomes, while reporting excluded counts and the unresolved fraction. Use a separate fitting split for any calibration mapping and a later evaluation split for measuring it. Do not treat model self-ratings or additional prose as empirical samples.

Implement the scorer and abstention rule in a new C module, reusing history adapters where sensible from `src/learned_cost.c` and connecting the result to `src/self_improve.c`. A proposed `reason_calibrate` tool through `src/tools.c` accepts bounded prediction/outcome records and returns scores, sample support, and whether the current candidate has enough evidence for a decision. Supply a minimal local record format if no suitable history API exists; avoid duplicating evidence storage.

Exercise the actual binary with a deliberately overconfident series of candidate predictions, a better-calibrated series, and a mixture containing verifier timeouts. Check the scores against hand-calculated values and show that adding unknown outcomes cannot improve measured accuracy or count as success. Test an unobserved operating regime and require abstention or explicit uncalibrated status. Include post-hoc relabeling of the predicted event and reject it. Close with a real candidate fixture whose recommendation changes because its empirical success estimate, uncertainty, and support changed—not because a reviewer used stronger language.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 56 — Bound rewrite resources with dimensional estimates

Before DSCO synthesizes a larger code path, make its resource reasoning obey units and conservative bounds. Work in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py` lists `fermi_estimation` in `REASONING_METHODOLOGIES`, but `MethodologySignature` produces prose. Implement executable decompositions for the compute, memory, and validation cost of resident rewrite candidates.

Define a small expression language over dimensioned quantities: bytes, seconds, calls, tokens, and currency, with intervals and provenance for each factor. Support products, sums, ratios, min/max, and declared conversion factors. Reject incompatible addition, ambiguous rates, nonfinite values, denominator intervals crossing zero, and unsafe overflow. Distinguish peak from average resource use and independent from shared buffers. Correlated or mutually exclusive factors require explicit composition assumptions; naive interval arithmetic may be conservative but must say so. Return the dominant uncertainty and which measurement would narrow the bound most.

Build a new dimensional-estimation module, taking reusable numeric machinery from `src/eval.c` without silently inheriting dimensionless semantics. Integrate estimate comparisons with `src/cost_model.c` and a small `src/self_improve.c` pre-experiment hook. A proposed `reason_estimate` operation should accept the decomposition and candidate resource ceilings. If no candidate metadata interface exists, define a bounded input/output schema that provides resource bounds and the resulting experiment admissibility recommendation.

Through the real binary, compare an incumbent streaming routine with a candidate buffering complete requests. Use requests per second, bytes per request, and retention duration to calculate memory; omitting duration must not produce a memory total. Test decimal versus binary byte units, milliseconds versus seconds, shared buffers across workers, and a worst-case input bound. A candidate with a favorable midpoint but a peak bound exceeding the allowed memory must be flagged before experimentation. Supply measured outcomes from local fixture runs and show whether they fall inside the estimate, preserving any exceeded bound as evidence of a failed assumption rather than rewriting history.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 57 — Escalate reasoning only when observed risk warrants it

A self-rewriting runtime should deliberate deeply when it could be wrong in consequential ways, while handling proven easy cases cheaply. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, adapt `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_reasoning_modules.py`'s `DualProcessReasoning.forward()` and `FastFrugalDecision.forward()`. The donor always performs sequential intuitive/deliberate model calls, and poor recognition can still win when cues are absent. Implement selective escalation based on observed decision risk.

Extract explicit features from candidate contracts and results: changed effects, ownership or arithmetic semantics, concurrency exposure, test disagreement, unknown preconditions, workload novelty, and measured benefit uncertainty. A deterministic policy chooses direct check, focused specialist reasoning, or a deeper adversarial plan. Model confidence may be recorded but must not cancel a hard escalation trigger. Bound escalation rounds and define stop, reject, and gather-evidence outcomes when obligations remain unresolved. Cheapness is a resource property, not a correctness proof; retaining the incumbent is a valid result.

Add the policy in a new C module and connect it through small hooks in `src/router.c`, `src/self_improve.c`, and only the necessary request lifecycle in `src/agent.c`. A proposed `reason_escalate` operation should accept the risk features and return the exact triggered rule and next reasoning budget. If there is no candidate feature adapter, provide a minimal typed record and at least one extractor for a real contract boundary; do not reduce the feature to arbitrary keyword scoring.

Use the actual binary with two candidate cases: a deterministic table substitution covered by exhaustive checks, and an allocator rewrite changing ownership on an uncommon branch. The first should avoid unnecessary model calls; the second must escalate despite a cheap model's 0.99 confidence. Inject contradictory test results, missing cues, and budget exhaustion. Show that unresolved risk never converts to approval when time runs out. Record stage counts, actual provider-fixture calls, decision latency, and correctness on a bounded labeled set. Compare with always-deep and always-cheap policies without claiming improvement from lower compute alone.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 58 — Allocate reasoning portfolios by complementary error

The useful reasoning portfolio for a code rewrite is the set that catches different mistakes within the available budget. In `/Users/arthurcolle/Dsco/dsco-cli` or an assigned worktree, extend the idea behind `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/debiased_async.py`'s `AsyncComprehensiveDebiasedReasoning.aforward()`. Its two concurrency waves are real, but method activation is fixed and partly keyword-based. Implement an adaptive portfolio using measured error complementarity.

Represent each method/model pair by its applicable obligations, observed error vector on labeled tasks, latency/cost distribution, and operating-region support. Estimate marginal coverage conditional on already selected methods rather than adding standalone accuracy scores. Group shared evidence and correlated failure modes. Choose a bounded portfolio under hard budget and latency constraints, with an exploration allowance that cannot waive candidate correctness requirements. New or shifted workloads require uncertainty and conservative fallback; model brand or number of agreeing outputs is not evidence of independence.

Use existing execution shapes in `src/topology.c`, estimates in `src/plan_optimizer.c`, and provider identity in `src/router.c`; implement the selector in a separate module with a narrow `src/self_improve.c` hook. A proposed `reason_portfolio` surface should accept labeled outcome matrices and the current obligations, returning selected method/model pairs, predicted marginal value, and unaddressed risks. If no callable portfolio adapter exists, add a small interface that invokes chosen local method fixtures and aggregates verified findings without inventing a new swarm engine.

Run the actual binary with two strong reviewers that share the same overflow blind spot and a lower-average-accuracy arithmetic checker that catches it. Under a two-worker budget, the complementary pair should be preferred when its measured conditional coverage is better. Test sparse samples, duplicated outcome rows, misleading cost estimates, and one failed method whose unfinished obligations remain open. Evaluate on untouched tasks rather than the selection matrix. Report coverage, false alarms, cost, latency, and whether the selected portfolio improved the decision about a concrete code candidate; more agents alone is not a success criterion.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 59 — Distill runtime lessons into typed executable procedures

Let the resident binary turn a successful self-repair into a reusable procedure whose behavior can be tested on another problem. Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py` distinguishes procedural `REASONING_METHODOLOGIES` from lenses, but both remain instructions and generated text. Implement distillation from observed repair episodes into typed, executable recipes.

A distilled procedure declares parameters, applicability preconditions, required observations, bounded operation steps, branch conditions, expected postconditions, and stop/rollback recommendations. Separate episode-specific constants from reusable parameters; each generalization must cite contrasting successful and failed cases supplied to the distiller. The generated recipe is a candidate, never automatically trusted because its source episode passed. Validate its types and effects before execution, then test it on a changed instance and a deliberately inapplicable instance. A procedure that cannot explain when not to run remains unpromoted.

Inspect `src/bg_learn.c`'s existing co-occurrence skill generation, `src/skill_candidate.c`'s local scaffolding, and `src/prompt_program.c`'s deterministic compilation. Preserve those contracts and add a new typed-procedure module with a narrow `src/self_improve.c` decision hook. A proposed `reason_distill` operation accepts a bounded episode and emits a validated candidate recipe; a companion execution path invokes its operations through the ordinary gate. If no episode adapter exists, define the smallest structured input containing action, observation, contract, outcome, and applicability fields.

Demonstrate a lesson that repairs a bounds bug by deriving the valid range, generating boundary probes, proposing the guarded change, and checking the result. Transfer it to a function with renamed variables and a different bound, then test a case where wraparound is intentional and the precondition must reject the recipe. Detect hardcoded file paths, copied output answers, unbounded loops, and a failed original action presented as a successful step. Use the real binary to execute accepted recipe steps against local fixtures, prove their actual effects and postconditions, and show that learning changes future procedure selection without granting extra capabilities.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

# 60 — Optimize prompts programs and topology under one oracle

Optimize the machinery that decides DSCO's next rewrite as well as the candidate code it proposes. In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py`: `compile_with_mipro()` and `compile_with_gepa()` wrap real DSPy optimizers, but `swarm_metric()` rewards nonempty answers or an expected-answer substring. Build a bounded joint optimizer whose fitness comes from independent executable outcomes.

A candidate genome contains supplemental prompt parameters, a typed reasoning-procedure choice, and a bounded topology specification. Keep provider identity, permissions, oracle, task splits, and resource ceilings outside the editable genome. Search a declared finite neighborhood using reproducible seeds, retaining the incumbent and Pareto-improving candidates. Measure whether the produced code passes frozen correctness tests, satisfies required contracts, and improves workload performance after reasoning and validation costs. A model may propose mutations or explain failures; it cannot alter labels or score its own success. Include an ablation so a gain can be attributed to prompt, program, topology, or their interaction.

Implement the experiment controller in a new C module using `src/prompt_program.c`, `src/topology.c`, and `src/plan_optimizer.c`, with a small `src/self_improve.c` hook that consumes the accepted decision policy. If these lack an execution interface, define a minimal candidate-genome callback and local oracle runner. A proposed `reason_optimize` command must separate train, selection, and sealed holdout partitions. Compare the supplemental-state boundary with [Prime Agent's pinned refinement implementation](https://github.com/PrimeIntellect-ai/prime-agent/blob/878410b3981f20c6d685faa210ad0e43426cf483/packages/coding-agent/src/core/refinement/refinement.ts); keep environment scoring independent and require neither hosted training nor weight updates.

Through the real binary, optimize a small local repair task family with a deliberately exploitable answer-substring baseline. A response echoing the expected text while producing wrong code must score failure. Reject mutations touching the grader or holdout labels. Show a bounded search, an untouched holdout evaluation, and a no-improvement outcome that retains the incumbent. Verify candidate re-evaluation from its genome, exact budget accounting, and unchanged runtime authority. Report correctness and net useful performance with failed, timed-out, and unknown evaluations separated; never call one favorable training example self-improvement.

Preserve dirty work. Use small C hooks and new modules. Route every dynamic tool effect through `tools_execute_for_tier()`; keep capability denials intact. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`, use isolated state, and never install worker binaries.

---

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
