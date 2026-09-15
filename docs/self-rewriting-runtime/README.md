# A self-rewriting binary that reasons and improves

120 implementation prompts grounded in the supplied DSPy, cognitive-agent and metaprogramming source. Prompts 01–60 establish the resident self-rewriting runtime. The 60 new prompts, 61–120, extend it into a system where each operational role trains its own Recursive Language Model (RLM), with executable trajectories, independent evaluation, and versioned model weights or adapters.

The complete collection now contains **3,120 prompts**: these 120 foundations plus [3,000 specialist role-feature prompts, 121–3120](expansion/README.md). The expansion has its own searchable catalog, role readers and validator.

Each prompt is independently usable: copy its entire file into a session opened in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned private worktree. It contains its own source references, mechanism and falsifying acceptance case. For coordinated work, bind the shared interfaces described in the delivery design. These are specifications; this package does not implement or activate the native runtime.

- [The 60 new role-RLM prompts, 61–120](PROMPTS_061_120.md)
- [3,000 additional prompts, 121–3120: specialist roles × feature mechanisms](expansion/README.md)
- [All 120 prompts in one document](PROMPTS.md)
- [How each role trains an RLM](ROLE_RLM_TRAINING.md)
- [Native runtime architecture and first proof](ARCHITECTURE.md)
- [Multi-agent delivery and integration](DELIVERY.md)
- [What the supplied source actually implements](SOURCE_AUDIT.md)
- [Machine-readable catalog](catalog.json) and [source hashes/symbols](sources.json)

The first milestone is an inference-proposed correction to a native procedure, executed from new machine-code bytes with resident heap/session continuity, no `exec`, independent acceptance and a working rollback path. Changing prompts or preserving the PID alone does not prove that milestone.

Validated: **120 prompts, 296–395 words each; 42,072 words total.**

## The six parts

| IDs | Purpose |
| --- | --- |
| 01–20 | Executable self-model, code generation, ABI, state migration, live activation and evolution |
| 21–40 | Evidence, retrieval, memory, experiments and cognitive result flow |
| 41–60 | Reasoning procedures, causal tests, calibration and learned improvement strategies |

| New IDs | Purpose |
| --- | --- |
| 61–80 | Trained native-runtime specialist RLMs |
| 81–100 | Trained learning and evidence specialist RLMs |
| 101–120 | Per-role RLM environments, datasets, training, serving and joint admission |

## Prompt index

| ID | Feature | Words | Falsifying challenge |
| --- | --- | ---: | --- |
| 01 | [A resident executable self-model](prompts/01-resident-executable-self-model.md) | 352 | Refuse a mismatched source/code pair and demonstrate two executable generations within one PID. |
| 02 | [Versioned live function dispatch](prompts/02-versioned-live-function-dispatch.md) | 369 | Concurrent calls must finish entirely on the generation they acquired, while stale activations fail. |
| 03 | [Typed mutable reasoning IR](prompts/03-typed-mutable-reasoning-ir.md) | 348 | Reject bad joins, invalid native signatures, and effects on a pure branch before execution. |
| 04 | [Resumable VM generation switching](prompts/04-resumable-vm-generation-switching.md) | 347 | A suspended computation must resume on a new graph without repeating its completed effect. |
| 05 | [Compile candidate C into a live native module](prompts/05-compile-live-native-candidates.md) | 378 | An actual compiled function must change resident execution without restart; malformed candidates never activate. |
| 06 | [Generate and execute bounded machine code in memory](prompts/06-in-memory-machine-code-scorer.md) | 378 | Execute emitted instructions, then replace their generation in the same PID without a compiler or dlopen. |
| 07 | [Native ABI, relocation, and import contracts](prompts/07-native-abi-relocation-contracts.md) | 354 | Reject incompatible layouts and relocations before any candidate initializer or function executes. |
| 08 | [Safe reclamation of live native generations](prompts/08-safe-native-generation-reclamation.md) | 344 | An old call survives activation and returns correctly; old code unloads only after its last reference drains. |
| 09 | [Transactional migration of resident heap state](prompts/09-transactional-resident-state-migration.md) | 369 | Migration failure leaves old state unchanged; successful migration never pairs new code with old layout. |
| 10 | [Atomic activation of cooperating executable cells](prompts/10-atomic-multicell-activation.md) | 392 | Concurrent callers observe either the complete old bundle or the complete new bundle, never mixed results. |
| 11 | [Shadow native candidates with replayed observations](prompts/11-shadow-native-candidate-replay.md) | 346 | A shadow attempt to alter or repeat a recorded effect must stop; valid replay produces comparable native outputs. |
| 12 | [Counterexample-guided native executable synthesis](prompts/12-counterexample-guided-native-synthesis.md) | 353 | A candidate that memorizes visible examples must fail hidden cases; repaired code must execute in the resident process. |
| 13 | [Resident rollback with a bounded behavioral watchdog](prompts/13-resident-native-rollback-watchdog.md) | 378 | A behavioral regression triggers same-PID rollback while in-flight calls retain valid generation ownership. |
| 14 | [Causal profiling mapped to executable versions](prompts/14-executable-version-causal-profiling.md) | 344 | Calls spanning a hot swap must remain attributed to their acquired generation, and pure delay must not look like quality gain. |
| 15 | [Effect-aware executable composition](prompts/15-effect-aware-executable-composition.md) | 342 | Reject illegal reordering or duplicated effects while parallelizing truly independent pure stages. |
| 16 | [Profile-driven native specialization](prompts/16-profile-driven-native-specialization.md) | 350 | An input violating specialization guards must execute the generic path and preserve exact semantics. |
| 17 | [Verified ephemeral tools synthesized as native code](prompts/17-verified-ephemeral-native-tools.md) | 354 | A new native tool must solve unseen cases, and denied effects must remain denied through its dynamic dispatch. |
| 18 | [Learn semantics-tested executable rewrite operators](prompts/18-learned-executable-rewrite-operators.md) | 348 | A rewrite that improves training examples but changes hidden semantics must not enter the learned operator library. |
| 19 | [An evolution curriculum that retains prior capabilities](prompts/19-native-evolution-curriculum.md) | 358 | A candidate gaining a new task while regressing a required old capability must be rejected. |
| 20 | [A closed-loop same-process native self-repair demonstration](prompts/20-same-process-native-self-repair.md) | 393 | The full loop must change executed native bytes without exec and reject or roll back a deceptive later candidate. |
| 21 | [Bind self-modification claims to executable evidence](prompts/21-executable-evidence-claims.md) | 301 | A passing test from the old procedure must not validate its replacement, and a fabricated quotation must not resolve. |
| 22 | [Investigate the missing obligations of a self-rewrite](prompts/22-self-investigation-obligations.md) | 314 | An optimization supported only by average latency must remain incomplete while cancellation and ownership obligations are unresolved. |
| 23 | [Resolve conflicting explanations through directed retrieval](prompts/23-contradiction-directed-investigation.md) | 318 | A stale design document and current execution trace disagree about a lock; the reasoner must inspect the actual revision and call path. |
| 24 | [Prevent copied evidence from becoming false consensus](prompts/24-independent-support-fusion.md) | 318 | Twenty copied performance summaries must not outweigh one independent failing execution. |
| 25 | [Stop self-investigation when retrieval adds no knowledge](prompts/25-retrieval-novelty-budget.md) | 320 | Repeated mirrors and paraphrased queries must stop without falsely increasing confidence or exhausting the entire turn. |
| 26 | [Retrieve typed evidence from the resident runtime and source corpus](prompts/26-typed-runtime-retrieval.md) | 325 | A similarly named function in another revision must not outrank the exact failing invocation merely because its prose matches. |
| 27 | [Extract evidence that survives different chunk boundaries](prompts/27-overlap-invariant-extraction.md) | 312 | The same assertion straddling two windows must count once, while two real occurrences remain distinguishable. |
| 28 | [Distinguish real contradictions from revision and time changes](prompts/28-revision-aware-contradictions.md) | 296 | A function safe in revision A and unsafe in revision B is a change, while simultaneous incompatible claims about B are a conflict. |
| 29 | [Preserve modality and attribution in learned memory](prompts/29-attributed-epistemic-memory.md) | 316 | A paper quoting an unsafe recommendation to refute it must not teach the runtime to adopt that recommendation. |
| 30 | [Invalidate learned conclusions when their content dependencies change](prompts/30-reasoning-dependency-invalidation.md) | 324 | Replacing source bytes at the same path must invalidate dependent optimization conclusions without discarding independent evidence. |
| 31 | [Verify decisive algorithm evidence against PDF pages and figures](prompts/31-algorithm-figure-verification.md) | 333 | A missing minus sign or log-scale axis must reverse an apparently favorable algorithm comparison before adoption. |
| 32 | [Make self-improvement performance claims executable](prompts/32-quantitative-rewrite-claims.md) | 317 | A faster mean on fewer successful requests must not be accepted as a latency improvement for the full workload. |
| 33 | [Localize its own failures through competing source hypotheses](prompts/33-hypothesis-fault-graph.md) | 321 | The last logged function must not be blamed automatically when an earlier ownership transfer caused the failure. |
| 34 | [Record predicted observations before running self-experiments](prompts/34-prediction-observation-ledger.md) | 327 | After a failed experiment the agent cannot rewrite its original prediction and report confirmation. |
| 35 | [Branch and merge competing research states without erasing dissent](prompts/35-research-state-branches.md) | 324 | Two branches choosing different replacement algorithms must merge their counterexamples rather than selecting whichever report arrived last. |
| 36 | [Execute cognitive stages with typed results and bounded repair](prompts/36-typed-cognitive-dataflow.md) | 334 | A deliberately wrong execution result must cause a bounded repair or rejection, never unconditional success. |
| 37 | [Preempt long self-investigation without losing cognitive state](prompts/37-preemptible-reasoning-state.md) | 329 | A stalled retrieval during self-rewrite analysis must not lose an urgent user correction or rerun an already completed experiment on resume. |
| 38 | [Require evidence-sensitive completion or explicit abstention](prompts/38-evidence-sensitive-completion.md) | 320 | A model claiming 99 percent confidence after all retrievals fail must not mark a replacement ready. |
| 39 | [Replay reasoning under controlled framing interventions](prompts/39-reasoning-intervention-replay.md) | 324 | Changing a source prestige label or evidence order must not silently change adoption when facts and validity are fixed. |
| 40 | [Reopen learned procedures when their evidence expires](prompts/40-evidence-expiry-revalidation.md) | 332 | A cached improvement validated under an old runtime configuration must stop claiming current support after that configuration changes. |
| 41 | [Compile reasoning methods into executable obligations](prompts/41-reasoning-method-compiler.md) | 336 | Missing competing likelihood prevents Bayesian execution; a quoted keyword does not select a method. |
| 42 | [Compete causal fault hypotheses before rewriting](prompts/42-abductive-fault-competition.md) | 342 | A probe equally predicted by both hypotheses earns no discriminating credit. |
| 43 | [Check deductive fragments for candidate preconditions](prompts/43-deductive-proof-fragments.md) | 346 | Affirming the consequent, integer overflow, and unsupported generalization fail independently of persuasive text. |
| 44 | [Perform fully specified Bayesian candidate revision](prompts/44-deterministic-bayesian-revision.md) | 333 | One-percent prior with .99 sensitivity and .05 false-positive rate yields about16.67%, not99%. |
| 45 | [Identify causal benefit of runtime interventions](prompts/45-runtime-causal-interventions.md) | 335 | A warm cache improves both old and new code; sequential benchmark timing must not credit the patch. |
| 46 | [Forecast candidate performance from comparable episodes](prompts/46-reference-class-code-forecast.md) | 340 | Removing failed candidates or mixing hardware generations changes apparent benefit and must be exposed. |
| 47 | [Choose code changes on robust constrained frontiers](prompts/47-robust-code-decision-frontier.md) | 344 | A faster candidate with one correctness violation cannot buy admission with any speed score. |
| 48 | [Select the next experiment by decision value](prompts/48-value-of-information-experiments.md) | 348 | A high-entropy unknown that cannot change the selected implementation has zero decision value. |
| 49 | [Turn rewrite premortems into executable conditions](prompts/49-candidate-premortem-triggers.md) | 340 | Missing telemetry is unknown and counter reset cannot satisfy a safety trigger. |
| 50 | [Transfer algorithms through structural analogy checks](prompts/50-structural-algorithm-transfer.md) | 350 | A cache strategy valid for idempotent reads must not transfer to exactly-once mutations. |
| 51 | [Ground decision-bias audits in source spans](prompts/51-grounded-self-modification-audit.md) | 351 | Mentioning past investment as historical context alone must not count as sunk-cost reasoning. |
| 52 | [Test code decisions for framing invariance](prompts/52-decision-framing-invariance.md) | 338 | Equivalent ninety-percent success versus ten-percent failure records should not reverse identical code choices. |
| 53 | [Propagate critique into code decisions](prompts/53-critique-correction-propagation.md) | 347 | A disproven bounds premise cannot survive into the final choice through an unchanged original synthesis. |
| 54 | [Investigate rewrite candidates with independent dissent](prompts/54-independent-adversarial-investigation.md) | 342 | Three agreeing investigators citing one erroneous measurement cannot outvote a concrete failing input. |
| 55 | [Calibrate rewrite confidence against actual outcomes](prompts/55-outcome-calibration-abstention.md) | 337 | Verification timeout must remain unknown and never improve a candidate success statistic. |
| 56 | [Bound rewrite resources with dimensional estimates](prompts/56-units-aware-resource-estimation.md) | 338 | Requests per second times bytes per request is bandwidth, not total memory without a retention duration. |
| 57 | [Escalate reasoning only when observed risk warrants it](prompts/57-selective-dual-process-escalation.md) | 353 | Confident cheap-model output cannot suppress escalation for a patch changing ownership or bounds. |
| 58 | [Allocate reasoning portfolios by complementary error](prompts/58-complementary-reasoning-portfolios.md) | 347 | Two highly accurate agents making the same errors can add less value than a weaker complementary verifier. |
| 59 | [Distill runtime lessons into typed executable procedures](prompts/59-typed-procedure-distillation.md) | 348 | A lesson that hardcodes one example path or repeats a failed action cannot pass transfer acceptance. |
| 60 | [Optimize prompts programs and topology under one oracle](prompts/60-joint-runtime-policy-optimization.md) | 357 | Expected-answer text embedded in a wrong output and altered grader code must never earn promotion. |
| 61 | [Train an RLM to reproduce decisive native schedules](prompts/61-schedule-replay-specialist-rlm.md) | 356 | Reproduce a failing interleaving after wall-clock delays change and reject a fabricated replay schedule. |
| 62 | [Train an RLM to prove bounded executable equivalence](prompts/62-symbolic-equivalence-specialist-rlm.md) | 357 | An overflow-sensitive rewrite must yield a concrete differing input instead of an unsupported equivalence claim. |
| 63 | [Train an RLM for reversible debugging of pure native cells](prompts/63-reversible-debugging-specialist-rlm.md) | 363 | Rewind across a branch and reproduce values exactly; attempting to rewind an external effect must be refused. |
| 64 | [Train an RLM to minimize native counterexamples structurally](prompts/64-counterexample-minimization-specialist-rlm.md) | 360 | A smaller crashing input with a different defect must not replace the original counterexample. |
| 65 | [Train an RLM for executable change-impact slicing](prompts/65-change-impact-slicing-specialist-rlm.md) | 354 | A changed branch predicate must invalidate a downstream result even when its direct call graph is unchanged. |
| 66 | [Train an RLM to construct checkable loop resource certificates](prompts/66-loop-certificate-specialist-rlm.md) | 360 | Reject a decreasing-looking loop whose counter wraps and exceeds its claimed bound. |
| 67 | [Train an RLM to synthesize pointer-free native host bindings](prompts/67-typed-host-handle-specialist-rlm.md) | 356 | A stale or cross-kind handle must never resolve after slot reuse or code replacement. |
| 68 | [Train an RLM to generate SIMD native batch kernels](prompts/68-simd-codegen-specialist-rlm.md) | 357 | Tail lengths, misalignment, and unsupported ISA must preserve scalar semantics without out-of-bounds reads. |
| 69 | [Train an RLM to autotune resident data layouts](prompts/69-data-layout-autotuning-specialist-rlm.md) | 354 | A faster layout must not lose concurrent writes or break external schema access. |
| 70 | [Train an RLM to remove allocations through escape analysis](prompts/70-escape-analysis-specialist-rlm.md) | 360 | A callback-retained reference must prevent temporary allocation placement even if ordinary tests return early. |
| 71 | [Train an RLM to adapt native algorithms to allocation pressure](prompts/71-allocation-pressure-specialist-rlm.md) | 355 | Memory pressure must change the chosen algorithm without oscillation, lost results, or falsely claiming an allocation estimate is measured. |
| 72 | [Train an RLM to reconstruct deoptimization state](prompts/72-native-deoptimization-specialist-rlm.md) | 353 | Every supported safepoint must reconstruct logical values and resume without duplicating completed effects. |
| 73 | [Train an RLM for semantic incremental native compilation](prompts/73-incremental-compilation-specialist-rlm.md) | 349 | A semantic change hidden behind an unchanged symbol name must rebuild its dependent specialized code. |
| 74 | [Train an RLM for generation-aware native source debugging](prompts/74-native-source-debugging-specialist-rlm.md) | 355 | A breakpoint must inspect the source and locals of its pinned generation despite a concurrent replacement. |
| 75 | [Train an RLM for cross-compiler native qualification](prompts/75-cross-compiler-specialist-rlm.md) | 345 | Compiler agreement cannot accept a candidate that independently violates the oracle, and disagreement must remain unresolved. |
| 76 | [Train an RLM to search native optimization phase orders](prompts/76-optimization-phase-order-specialist-rlm.md) | 359 | A phase order that wins microbenchmarks but regresses the complete executable must not be promoted. |
| 77 | [Train an RLM for checked speculative native parallelization](prompts/77-speculative-parallelization-specialist-rlm.md) | 350 | A hidden dependency conflict must discard speculative state and reproduce the serial result without duplicated effects. |
| 78 | [Train an RLM to optimize native precision under error contracts](prompts/78-precision-autotuning-specialist-rlm.md) | 355 | A fast reduction that fails catastrophic-cancellation or NaN behavior must be rejected despite ordinary-data gains. |
| 79 | [Train an RLM for budgeted native compilation reuse](prompts/79-native-code-cache-specialist-rlm.md) | 358 | An evicted artifact must never unmap active code; changed ABI or compiler semantics must prevent stale reuse. |
| 80 | [Train an RLM for lossless logical code-cell checkpoints](prompts/80-logical-checkpoint-specialist-rlm.md) | 353 | Restore into different addresses without raw pointers, repeated effects, or changed suffix outputs. |
| 81 | [Train a trace-to-state-machine induction RLM](prompts/81-rlm-state-machine-induction.md) | 355 | Identical output prefixes followed by different legal continuations must not be merged into one state. |
| 82 | [Train an active protocol-learning RLM](prompts/82-rlm-active-protocol-learning.md) | 351 | A retry that consumes a token changes protocol state even though its visible response matches a harmless retry. |
| 83 | [Train an executable API-contract mining RLM](prompts/83-rlm-api-contract-mining.md) | 355 | A nonempty output is not success when the API contract requires a matching request identity and sorted records. |
| 84 | [Train minimal causal-feature discovery for failures](prompts/84-rlm-causal-feature-discovery.md) | 362 | A timestamp correlated with crashes must lose to the actual ownership-state feature when interventions separate them. |
| 85 | [Train quality-diversity search over rewrite behaviors](prompts/85-rlm-quality-diversity-search.md) | 367 | An extremely fast candidate on uniform inputs must not erase a robust candidate needed for skewed workloads. |
| 86 | [Train constrained Bayesian compiler-search programs](prompts/86-rlm-bayesian-compiler-search.md) | 361 | An aggressive flag combination with excellent speed but incorrect overflow behavior must remain infeasible. |
| 87 | [Train propensity-aware contextual exploration](prompts/87-rlm-contextual-bandit-learning.md) | 361 | A policy favored only because easy tasks received its actions must fail a properly weighted comparison. |
| 88 | [Train pairwise ranking of rewrite candidates](prompts/88-rlm-pairwise-rewrite-ranking.md) | 359 | A verbose candidate rationale cannot beat a concise candidate with stronger matched-workload evidence. |
| 89 | [Train compositional task generation from capability grammars](prompts/89-rlm-capability-task-generation.md) | 372 | A generated challenge must not require an unavailable capability or hide an impossible postcondition behind fluent wording. |
| 90 | [Train adversarial coevolution with a protected referee](prompts/90-rlm-adversarial-coevolution.md) | 369 | A test that merely changes the specification or crashes the harness must not earn credit for defeating a candidate. |
| 91 | [Train executable invariant discovery with held-out checking](prompts/91-rlm-invariant-discovery.md) | 365 | An invariant learned from positive counters must fail when a hidden wraparound transition violates it. |
| 92 | [Train error-cluster curriculum scheduling](prompts/92-rlm-error-curriculum.md) | 366 | A huge duplicate cluster of easy failures must not consume all training while a small catastrophic class is forgotten. |
| 93 | [Train abstract-interpretation-guided candidate bounding](prompts/93-rlm-abstract-interpretation.md) | 371 | A rare loop-growth path must not be certified bounded because sampled traces terminate. |
| 94 | [Train semantic indexing by executable behavior](prompts/94-rlm-behavioral-indexing.md) | 372 | Two similarly named functions differing only on empty input must remain distinguishable in retrieval. |
| 95 | [Train uncertainty-aware surrogate performance reasoning](prompts/95-rlm-performance-surrogates.md) | 366 | A surrogate trained on small arrays must flag unsupported extrapolation to cache-thrashing sizes. |
| 96 | [Train amortized experiment-selection policies](prompts/96-rlm-amortized-experiment-selection.md) | 382 | A memorized cheap probe must be rejected when its result cannot distinguish the current hypotheses. |
| 97 | [Train hierarchical skill-option induction](prompts/97-rlm-option-induction.md) | 380 | A retry-until-success fragment must not become an unbounded skill that runs after its preconditions stop holding. |
| 98 | [Train online concept-drift segmentation](prompts/98-rlm-concept-drift-segmentation.md) | 373 | A single outlier must not trigger a new regime, while a persistent shifted workload must not be averaged away. |
| 99 | [Train compression-driven reusable procedure libraries](prompts/99-rlm-library-compression.md) | 377 | A short library that memorizes training constants or changes effect order must lose despite excellent apparent compression. |
| 100 | [Train transfer attribution and negative-transfer prevention](prompts/100-rlm-transfer-attribution.md) | 384 | A transferred caching skill that helps pure transforms but corrupts stateful counters must be rejected for the latter target. |
| 101 | [Give every role an independently trained RLM identity](prompts/101-role-rlm-registry.md) | 349 | A renamed base checkpoint or adapter for another role must not be accepted as a newly trained role model. |
| 102 | [Implement the external-context RLM execution contract](prompts/102-external-context-rlm-runtime.md) | 336 | Huge stdout or a large final variable must not be copied into root history or truncated into the returned answer. |
| 103 | [Train programmatic recursion within role boundaries](prompts/103-role-recursive-call-tree.md) | 342 | Programmatically generated child calls cannot request a broader role or escape the inherited capability set. |
| 104 | [Export faithful root-turn training trajectories](prompts/104-root-turn-trajectory-export.md) | 355 | Flattening child assistant turns into root targets or including future observations must fail validation. |
| 105 | [Distill only executable teacher trajectories](prompts/105-teacher-rejection-distillation.md) | 356 | A correct answer string with nonexecuting code or unverifiable tool results must not enter the accepted training set. |
| 106 | [Align root SFT targets with actual token boundaries](prompts/106-root-sft-loss-masks.md) | 364 | Literal assistant markers inside tool output cannot acquire supervision or shift target masks. |
| 107 | [Expose each role as a verifiable RLM environment](prompts/107-role-verifiers-environment.md) | 362 | Generated code cannot edit grading files, and a zero-reward or failed episode cannot disappear from statistics. |
| 108 | [Bridge role root datasets to actual Prime SFT jobs](prompts/108-role-prime-sft-bridge.md) | 359 | Configuration-only success, unchanged tensors, or a trainer silently using messages instead of root targets must fail acceptance. |
| 109 | [Train role roots with terminal verifiable rewards](prompts/109-role-root-rlvr.md) | 362 | Reward hacking, empty rollouts and timeouts cannot be scored as correct or yield fabricated optimizer progress. |
| 110 | [Separate root and leaf learning credit experimentally](prompts/110-root-leaf-credit-experiments.md) | 351 | A stronger leaf silently substituted after root training must not be credited to the root adapter. |
| 111 | [Learn recursion depth and batching as root behavior](prompts/111-learned-recursion-batching.md) | 364 | Blind maximum recursion or batching across dependent items loses correctness despite fewer model invocations. |
| 112 | [Measure role RLM length generalization](prompts/112-role-length-generalization.md) | 372 | Needles fixed at the same offsets or train/eval duplicates must not masquerade as length generalization. |
| 113 | [Serve isolated role adapters by immutable version](prompts/113-role-adapter-serving-isolation.md) | 363 | Concurrent adapter reload cannot switch an in-flight tree or serve role A with role B weights. |
| 114 | [Train interrole protocols against frozen partners](prompts/114-frozen-partner-protocol-training.md) | 349 | Partners colluding through leaked labels or changing together cannot establish one role learned a transferable protocol. |
| 115 | [Split role training data by repository ancestry](prompts/115-role-training-leakage-splits.md) | 357 | Renamed copies, nearby commits and paraphrased tasks cannot cross into holdout without being detected. |
| 116 | [Train recovery from REPL syntax and finalization errors](prompts/116-rlm-recovery-training-data.md) | 348 | Repaired syntax that silently drops required work or returns an undefined final variable remains incorrect. |
| 117 | [Train asynchronous recursion policies without blocking input](prompts/117-async-recursion-scheduler-training.md) | 339 | A policy cannot gain reward by dropping slow tasks, hiding errors or blocking terminal input during training. |
| 118 | [Distill across roles without hiding negative transfer](prompts/118-cross-role-distillation.md) | 360 | Transferring an aggressive proposer policy into a verifier can raise apparent productivity while increasing false acceptance. |
| 119 | [Admit trained models with their native runtime generation](prompts/119-model-native-joint-admission.md) | 358 | A trained model and new runtime that each pass alone can still form an incompatible pair and must be tested together. |
| 120 | [Demonstrate every role training serving and improving](prompts/120-role-train-serve-improve-demo.md) | 395 | One shared unchanged model with different role prompts, or improvement measured only on training cases, fails the demonstration. |

## Recheck the package

```sh
python3 docs/self-rewriting-runtime/validate.py
```

Use `--refresh` only after intentionally changing prompt/source references; it rebuilds the catalog, combined document and source hashes. `--zip` packages the validated documents. Neither option starts models or executes candidate code. Source hashes detect later drift; structural validation does not prove cognitive or native-code correctness.
