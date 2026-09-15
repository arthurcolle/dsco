# 3,000 more prompts: roles that train their own RLMs

Prompts **121–3120** extend the original 120 to **3,120 total**. Each new prompt is **328–376 words**; the expansion contains **1,047,680 words**.

The catalog contains **100 specialist roles × 30 concrete feature mechanisms**. It is an explicitly structured expansion: related prompts share their role contract or training mechanism. Each combination specifies a domain, executable behavior, actual root training, independent scoring, a domain counterexample and a feature-specific acceptance test. These are implementation specifications, not 3,000 already-built features or trained models.

- [All 3,000 prompts](PROMPTS_0121_3120.md)
- [Feature mechanisms](FEATURES.md)
- [Parallel delivery](DELIVERY.md)
- [Machine-readable catalog](catalog.json) and [full prompt JSONL](prompts.jsonl)
- [Validation results](qa.json) and [verified source references](sources.json)
- [Role RLM training architecture](../ROLE_RLM_TRAINING.md)

## Find and export prompts

```sh
python3 docs/self-rewriting-runtime/expansion/select.py --query "recovery" --limit 12
python3 docs/self-rewriting-runtime/expansion/select.py --role 001 --list
python3 docs/self-rewriting-runtime/expansion/select.py --ids 121,122,150 --output /tmp/selected-rlm-prompts.md
python3 docs/self-rewriting-runtime/expansion/build.py --check
```

## Role readers

| Role | Prompt range | Independent task contract |
| --- | --- | --- |
| [001 · Bounded binary-record decoder](books/001-binary-record-decoder.md) | 121–150 | Learn to synthesize bounded native decoders for length-prefixed journal records with explicit byte-order and checksum semantics. |
| [002 · Incremental Unicode decoder](books/002-incremental-unicode-decoder.md) | 151–180 | Learn native Unicode decoding procedures that preserve exact scalar values and error positions across arbitrary input chunking. |
| [003 · Incremental stream-frame recognizer](books/003-stream-frame-recognizer.md) | 181–210 | Learn executable stream recognizers that distinguish complete messages, partial frames, terminal markers, and malformed framing without guessing. |
| [004 · Fixed-width arithmetic transformer](books/004-fixed-width-arithmetic-transformer.md) | 211–240 | Learn native arithmetic rewrites that reduce operations while preserving declared signedness, width, overflow, and division behavior. |
| [005 · Bounded regex automaton compiler](books/005-regex-automaton-compiler.md) | 241–270 | Learn native matcher generation for a declared regular-expression subset using bounded automata instead of uncontrolled backtracking. |
| [006 · Canonical record serializer](books/006-canonical-record-serializer.md) | 271–300 | Learn native serializers whose canonical bytes preserve typed record semantics across optional fields and schema ordering variations. |
| [007 · Revision-scoped interval index designer](books/007-interval-index-designer.md) | 301–330 | Learn interval index procedures for source spans that answer overlap and containment queries accurately after revision changes. |
| [008 · Invocation arena allocator synthesizer](books/008-invocation-arena-allocator.md) | 331–360 | Learn arena allocation procedures that exploit invocation lifetimes while preserving alignment, nested scopes, and concurrent ownership. |
| [009 · Bounded ring-buffer algorithm designer](books/009-bounded-ring-buffer-designer.md) | 361–390 | Learn bounded native ring-buffer procedures with explicit overwrite, backpressure, ordering, and consumer visibility contracts. |
| [010 · Filesystem tree-diff algorithm designer](books/010-filesystem-tree-diff-designer.md) | 391–420 | Learn executable filesystem diff procedures that distinguish content edits, renames, mode changes, symlinks, and missing paths. |
| [011 · Logical snapshot codec designer](books/011-logical-snapshot-codec.md) | 421–450 | Learn compact native codecs for immutable logical snapshots while preserving shared references, cycles, and schema identities. |
| [012 · Typed protocol request-response codec](books/012-typed-protocol-codec.md) | 451–480 | Learn native protocol codecs that preserve request identity, typed errors, optional fields, and forward-compatible message semantics. |
| [013 · Semantic build-unit partitioner](books/013-semantic-build-unit-partitioner.md) | 481–510 | Learn compilation-unit partitions and reuse decisions that reduce native rebuild work without missing changed semantic dependencies. |
| [014 · Scope-aware native symbol resolver](books/014-scope-aware-symbol-resolver.md) | 511–540 | Learn native symbol resolution procedures that preserve scope, visibility, overload contracts, and generation-specific definitions. |
| [015 · Native relocation validator](books/015-native-relocation-validator.md) | 541–570 | Learn relocation decoding and validation procedures for an explicit native object-format subset before executable admission. |
| [016 · Transactional state-write planner](books/016-transactional-state-write-planner.md) | 571–600 | Learn native write protocols that atomically publish owned state changes while preserving concurrent updates and failure recovery. |
| [017 · Generation reclamation policy designer](books/017-generation-reclamation-designer.md) | 601–630 | Learn reclamation policies that release obsolete native images promptly while preserving frames, callbacks, and suspended continuation references. |
| [018 · SIMD lane-and-tail kernel designer](books/018-simd-lane-tail-designer.md) | 631–660 | Learn native vector kernels that choose lane operations and tail strategies while preserving an explicit scalar contract. |
| [019 · Branch-distribution native specialization designer](books/019-branch-distribution-specializer.md) | 661–690 | Learn native branch specialization choices from measured input distributions without eliminating rare but required semantic paths. |
| [020 · Native hashmap collision policy designer](books/020-hashmap-algorithm-designer.md) | 691–720 | Learn native hashmap lookup, insertion, deletion, and resizing procedures that preserve key identity under adversarial collisions. |
| [021 · Weighted queue-service policy designer](books/021-weighted-queue-service-designer.md) | 721–750 | Learn executable queue-service policies that balance declared weights, deadlines, and starvation bounds across competing task classes. |
| [022 · Semantic cache admission and eviction designer](books/022-semantic-cache-policy-designer.md) | 751–780 | Learn native cache policies that preserve semantic key validity while choosing useful admission and eviction under bounded storage. |
| [023 · Cooperative cancellation protocol designer](books/023-cooperative-cancellation-designer.md) | 781–810 | Learn executable cancellation protocols that terminate owned work at defined boundaries without cancelling replacements or losing completion truth. |
| [024 · Dependency-aware execution scheduler designer](books/024-dependency-execution-scheduler.md) | 811–840 | Learn native execution schedules that exploit dependency parallelism while respecting resource limits and failed-prerequisite semantics. |
| [025 · Allocation-trace causal analyst](books/025-allocation-trace-causal-analyst.md) | 841–870 | Learn executable analyses that reconstruct allocation lifetimes and distinguish leaks, bounded retention, fragmentation, and measurement gaps. |
| [026 · Executable dependency-graph reconstructor](books/026-executable-dependency-reconstructor.md) | 871–900 | Learn procedures that reconstruct executable dependency graphs from typed IR, imports, state access, and observed invocation relationships. |
| [027 · Stable numerical reduction designer](books/027-stable-numerical-reduction-designer.md) | 901–930 | Learn native summation and reduction kernels that meet explicit error limits across cancellation, extreme ranges, and exceptional values. |
| [028 · Semantic change-impact analyst](books/028-semantic-change-impact-analyst.md) | 931–960 | Learn change-impact procedures that identify every affected executable contract while avoiding unnecessary requalification of independent cells. |
| [029 · Logical checkpoint-schema migrator](books/029-logical-checkpoint-schema-migrator.md) | 961–990 | Learn executable migrations that restore logical checkpoints into compatible new schemas without raw-pointer reinterpretation or repeated effects. |
| [030 · Sparse matrix native kernel designer](books/030-sparse-matrix-kernel-designer.md) | 991–1020 | Learn native sparse matrix kernels and representation choices that preserve indexing, duplicate-entry, and numerical semantics. |
| [031 · Generation-pinned dispatch designer](books/031-generation-pinned-dispatch-designer.md) | 1021–1050 | Learn dispatch procedures that route nested native calls through one compatible generation while allowing later calls to use replacements. |
| [032 · Typed native import linker](books/032-typed-native-import-linker.md) | 1051–1080 | Learn native linking plans that resolve only permitted typed host services and reject incompatible or undeclared imports. |
| [033 · Deterministic causal replay designer](books/033-deterministic-causal-replay-designer.md) | 1081–1110 | Learn replay programs that reproduce recorded logical outcomes using pinned observations and explicit causal order within an admitted domain. |
| [034 · Executable resource-bound certifier](books/034-executable-resource-bound-certifier.md) | 1111–1140 | Learn resource certificates for bounded executable cells that admit useful loops without trusting candidate runtime estimates. |
| [035 · Source provenance joiner](books/035-source-provenance-joins.md) | 1141–1170 | Train an evidence RLM to join reported claims with originating executions rather than copied descriptions. |
| [036 · Quotation and modality interpreter](books/036-quotation-modality.md) | 1171–1200 | Train a root RLM to preserve who asserted what, with negation, uncertainty, quotation, and endorsement intact. |
| [037 · Temporal claim reasoner](books/037-temporal-claims.md) | 1201–1230 | Train a temporal RLM to distinguish historical observations, current applicability, and forecasts about changing procedures. |
| [038 · Citation resolver](books/038-citation-resolution.md) | 1231–1260 | Train a citation RLM to resolve each technical assertion to an actual retrievable supporting passage. |
| [039 · Tabular evidence reconciler](books/039-tabular-reconciliation.md) | 1261–1290 | Train a table RLM to reconcile conflicting extracted cells before numerical evidence influences executable decisions. |
| [040 · Equation structure interpreter](books/040-equation-parsing.md) | 1291–1320 | Train an equation RLM to turn page-level mathematical expressions into typed, checkable syntax trees. |
| [041 · Unit and scale normalizer](books/041-unit-normalization.md) | 1321–1350 | Train a unit RLM to normalize quantitative observations without losing dimensions, scales, or measurement conventions. |
| [042 · Document revision differ](books/042-document-revision-diffs.md) | 1351–1380 | Train a revision RLM to identify substantive evidence changes across documents despite layout and wording churn. |
| [043 · Contradiction localizer](books/043-contradiction-localization.md) | 1381–1410 | Train a contradiction RLM to locate the smallest incompatible claim pair and the scope making it contradictory. |
| [044 · Semantic occurrence deduplicator](books/044-semantic-deduplication.md) | 1411–1440 | Train an evidence RLM to deduplicate repeated observations without merging genuinely distinct or contradictory occurrences. |
| [045 · Entity identity resolver](books/045-entity-resolution.md) | 1441–1470 | Train an entity RLM to resolve identities across traces and documents without collapsing similar names. |
| [046 · Evidence graph path reasoner](books/046-graph-path-reasoning.md) | 1471–1500 | Train a graph RLM to construct valid explanatory paths while respecting edge type, direction, and scope. |
| [047 · Causal diagnostic investigator](books/047-causal-diagnostics.md) | 1501–1530 | Train a diagnostic RLM to discriminate competing runtime fault mechanisms using controlled interventions and preserved counterevidence. |
| [048 · Probabilistic evidence modeler](books/048-probability-models.md) | 1531–1560 | Train a probabilistic RLM to build explicit evidence models and execute their arithmetic rather than invent confidence. |
| [049 · Empirical reference-cohort builder](books/049-reference-cohorts.md) | 1561–1590 | Train a cohort RLM to select empirically comparable improvement attempts without excluding inconvenient failures or unsupported contexts. |
| [050 · Measurement comparability auditor](books/050-measurement-comparability.md) | 1591–1620 | Train a measurement RLM to establish whether two observed performance results support a fair candidate comparison. |
| [051 · Event sequence miner](books/051-event-sequence-mining.md) | 1621–1650 | Train a sequence RLM to discover recurring runtime behavior patterns with explicit ordering and episode boundaries. |
| [052 · Executable specification extractor](books/052-specification-extraction.md) | 1651–1680 | Train a specification RLM to extract executable obligations from implementations, schemas, examples, and explicit technical requirements. |
| [053 · Dependency obligation auditor](books/053-dependency-obligation-audit.md) | 1681–1710 | Train an obligation RLM to verify that prerequisite completion actually supplies the evidence needed by dependent work. |
| [054 · Error cluster discoverer](books/054-error-cluster-discovery.md) | 1711–1740 | Train an error-analysis RLM to discover actionable failure clusters without letting duplicate wording overwhelm underlying mechanisms. |
| [055 · Task grammar inducer](books/055-task-grammar-induction.md) | 1741–1770 | Train a grammar RLM to infer typed task-composition rules from verified examples rather than memorize surface templates. |
| [056 · Protocol state inferencer](books/056-protocol-state-inference.md) | 1771–1800 | Train a protocol RLM to infer hidden interaction states through bounded probes and distinguishing response sequences. |
| [057 · Behavior-grounded algorithm retriever](books/057-algorithm-retrieval.md) | 1801–1830 | Train an algorithm RLM to retrieve reusable procedures using behavioral requirements and checked assumptions rather than vocabulary alone. |
| [058 · Cross-language symbol mapper](books/058-cross-language-symbol-matching.md) | 1831–1860 | Train a symbol RLM to map corresponding program entities across languages while preserving ABI and behavioral distinctions. |
| [059 · Evidence set minimizer](books/059-evidence-minimization.md) | 1861–1890 | Train an evidence RLM to find small sufficient support sets without deleting decisive counterexamples or required conditions. |
| [060 · Change-point discoverer](books/060-changepoint-discovery.md) | 1891–1920 | Train a drift RLM to segment changing runtime behavior without mistaking single incidents for new operating regimes. |
| [061 · Discriminating experiment designer](books/061-experiment-design.md) | 1921–1950 | Train an experiment RLM to choose permitted probes that distinguish current hypotheses under realistic costs and controls. |
| [062 · Critical-path evidence analyst](books/062-critical-path-analysis.md) | 1951–1980 | Train a critical-path RLM to identify which dependency delays actually constrain end-to-end completion rather than summing durations. |
| [063 · Quantitative claim checker](books/063-quantitative-claim-checking.md) | 1981–2010 | Train a quantitative RLM to turn performance assertions into executable calculations with explicit populations and failure accounting. |
| [064 · Complete long-output enumerator](books/064-long-output-enumeration.md) | 2011–2040 | Train an enumeration RLM to extract every qualifying item from large corpora without truncation, duplicates, or invented completeness. |
| [065 · Multimodal evidence aligner](books/065-multimodal-alignment.md) | 2041–2070 | Train an alignment RLM to connect textual claims with the correct visual regions and preserve cross-modal disagreements. |
| [066 · Missingness and coverage analyst](books/066-missingness-analysis.md) | 2071–2100 | Train a coverage RLM to reason about what unavailable observations prevent the system from concluding. |
| [067 · Cross-repository contract discoverer](books/067-cross-repo-contract-discovery.md) | 2101–2130 | Train a contract-discovery RLM to locate and check semantic boundaries between services maintained in separate repositories. |
| [068 · Integration conflict resolver](books/068-integration-conflict-resolver.md) | 2131–2160 | Propose conflict resolutions that preserve independently established behavior from concurrent agents rather than accepting whichever patch applies. |
| [069 · Behavioral test selector](books/069-behavioral-test-selector.md) | 2161–2190 | Select the smallest informative test set that can reject a proposed runtime change without hiding untested obligations. |
| [070 · Flaky test diagnostician](books/070-flaky-test-diagnostician.md) | 2191–2220 | Distinguish nondeterministic product defects from unreliable test infrastructure before proposing code changes or quarantining tests. |
| [071 · Mutation test designer](books/071-mutation-test-designer.md) | 2221–2250 | Construct targeted mutations that reveal whether tests protect semantic obligations rather than superficial output formatting. |
| [072 · Tool argument repairer](books/072-tool-argument-repairer.md) | 2251–2280 | Repair malformed tool arguments while preserving the user's requested operation and the existing capability boundary. |
| [073 · API version migrator](books/073-api-version-migrator.md) | 2281–2310 | Migrate integration calls across explicit API contracts while identifying semantic changes that require caller decisions. |
| [074 · Capability request planner](books/074-capability-request-planner.md) | 2311–2340 | Find a least-authority execution plan that satisfies the objective or explains the precise missing permission. |
| [075 · Session recovery strategist](books/075-session-recovery-strategist.md) | 2341–2370 | Reconstruct the next valid action after interruption without replaying effects whose outcomes are already committed or uncertain. |
| [076 · Runaway recursion triager](books/076-runaway-recursion-triager.md) | 2371–2400 | Locate recursive branches that consume resources without advancing the task and stop only work proven redundant or invalid. |
| [077 · Provider failure classifier](books/077-provider-failure-classifier.md) | 2401–2430 | Classify inference failures precisely enough to choose retry, credential repair, alternate routing, or abstention without duplicate effects. |
| [078 · Rate allocation strategist](books/078-rate-allocation-strategist.md) | 2431–2460 | Allocate limited provider request capacity among role workloads without starving critical tasks or violating endpoint quotas. |
| [079 · Spend reservation reconciler](books/079-spend-reservation-reconciler.md) | 2461–2490 | Reconcile estimated, reserved, settled, and uncertain spend so concurrent role calls cannot reuse the same remaining budget. |
| [080 · Workload placement planner](books/080-workload-placement-planner.md) | 2491–2520 | Place role inference and training workloads where hardware, authority, latency, and data locality jointly satisfy their contracts. |
| [081 · Lease fencing examiner](books/081-lease-fencing-examiner.md) | 2521–2550 | Detect stale worker authority before results or code candidates can alter shared runtime state after reassignment. |
| [082 · Artifact provenance investigator](books/082-artifact-provenance-investigator.md) | 2551–2580 | Explain which exact source, dataset, model, and execution produced an artifact without treating labels as proof. |
| [083 · Training dataset curator](books/083-training-dataset-curator.md) | 2581–2610 | Choose role-training episodes that provide valid executable supervision without leaking holdout answers or amplifying duplicated failures. |
| [084 · Renderer loss-mask inspector](books/084-renderer-loss-mask-inspector.md) | 2611–2640 | Check whether role-root SFT trains exactly the intended generated tokens under the selected model renderer. |
| [085 · Checkpoint integrity examiner](books/085-checkpoint-integrity-examiner.md) | 2641–2670 | Qualify trained role artifacts by actual tensor integrity and compatibility before any runtime serves them. |
| [086 · Adapter route verifier](books/086-adapter-route-verifier.md) | 2671–2700 | Ensure each role and recursive tree receives its intended immutable adapter rather than a similarly named model. |
| [087 · Model drift diagnostician](books/087-model-drift-diagnostician.md) | 2701–2730 | Distinguish learned-policy regression from workload drift, serving substitution, and changes in the evaluation instrument. |
| [088 · Reward exploit investigator](books/088-reward-exploit-investigator.md) | 2731–2760 | Find behaviors that increase a role's training reward while violating the actual executable objective. |
| [089 · Training failure recovery planner](books/089-training-failure-recovery-planner.md) | 2761–2790 | Recover failed role training without mixing checkpoints, repeating committed updates, or claiming configuration validation as training progress. |
| [090 · Rollout staleness manager](books/090-rollout-staleness-manager.md) | 2791–2820 | Admit asynchronous training rollouts only when their behavior-policy versions and statistical assumptions remain valid. |
| [091 · Evaluator calibration analyst](books/091-evaluator-calibration-analyst.md) | 2821–2850 | Measure whether a role evaluator's confidence and labels predict independently verified outcomes instead of reinforcing self-assessment. |
| [092 · Multiagent handoff planner](books/092-multiagent-handoff-planner.md) | 2851–2880 | Transfer unfinished role work with enough executable state for another agent to continue without repeating settled actions. |
| [093 · Shared contract negotiator](books/093-shared-contract-negotiator.md) | 2881–2910 | Resolve incompatible interface assumptions between role implementations before their independently correct components are integrated. |
| [094 · Asynchronous result reconciler](books/094-async-result-reconciler.md) | 2911–2940 | Assemble asynchronous role outputs into a coherent decision without losing completed work or accepting stale results. |
| [095 · User correction prioritizer](books/095-user-correction-prioritizer.md) | 2941–2970 | Convert user corrections into precise objective changes while retaining authorized work that the correction does not supersede. |
| [096 · Input latency investigator](books/096-input-latency-investigator.md) | 2971–3000 | Find why terminal input stalls during tool discovery, training, or recursive execution without confusing rendering delay with admission delay. |
| [097 · Release admission examiner](books/097-release-admission-examiner.md) | 3001–3030 | Decide whether a tested code/model pair satisfies release obligations without confusing recorded assertions with verified evidence. |
| [098 · Resource forecast analyst](books/098-resource-forecast-analyst.md) | 3031–3060 | Forecast role training and recursive execution resources using measured cohorts, uncertainty, and correct dimensional relationships. |
| [099 · Synthetic fixture author](books/099-synthetic-fixture-author.md) | 3061–3090 | Generate adversarial executable fixtures with known answers that teach role procedures rather than memorized superficial cues. |
| [100 · Interoperability conformance examiner](books/100-interoperability-conformance-examiner.md) | 3091–3120 | Check that role runtimes honor negotiated protocol contracts across clients without mistaking permissive parsing for compatibility. |

## What validation establishes

The builder checks numbering, counts, lengths, unique bodies without titles, unique structured role-feature contracts, source files and Python symbols, links, byte-for-byte generated artifacts, and preservation of the original 120 prompts. Those structural checks do not establish semantic novelty, training efficacy or runtime correctness. Domain profiles and feature mechanisms were reviewed separately; selected complete prompts were sampled for coherence.

Run `build.py --write` only after intentional profile or mechanism edits. `--check --zip` packages the expansion with the foundation prompts, supporting documents and validator in `docs/self-rewriting-runtime.zip`. Checking C and Python source references requires the original checkouts; those source trees are not bundled. No command in this package launches training.
