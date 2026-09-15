# Thirty feature mechanisms for each specialist RLM

Each numbered mechanism adds a specific implementation and training capability to a role. Its independent task oracle and domain challenge come from the role profile.

## 01 — Executable curriculum compiler

Compile a task grammar with explicit difficulty axes, valid transformations, and known failure labels. Generate positive and negative episodes, retaining their originating family identifiers. Train the role's root policy on a balanced development curriculum and expose a reproducible curriculum-build command.

Acceptance: Reject a generator that changes the answer without updating its reference or produces duplicate families under different seeds. Measure valid-case coverage and trained-root success separately.

## 02 — Teacher rejection distillation

Collect multiple executable teacher trajectories per development task. Replay their programs against the fixed oracle, reject incorrect or incomplete episodes, and deduplicate shared derivations before extracting root action targets. Distill accepted trajectories into the role policy with balanced family sampling.

Acceptance: A fluent teacher episode containing the expected answer but an incorrect executable result must be excluded. Compare the trained root against its initial checkpoint on untouched families.

## 03 — Faithful root-turn supervision

Export each root decision as its exact preceding visible history and that decision's generated code. Preserve environment object identities and recursive child receipts. Train only the current root target, masking earlier turns, child replies, and tool observations; verify masks after the actual renderer tokenizes each sample.

Acceptance: Reject a sample whose prefix contains a later observation. A deliberately relabeled child response must receive no root target loss, and the exported sequence must replay exactly.

## 04 — Learned execution-error recovery

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Acceptance: Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

## 05 — Counterfactual branch preference training

Fork identical development states into isolated resettable environment snapshots or a fixed replay oracle with matched randomness. Reject comparisons when reset fidelity is uncertain. Execute alternative root programs under equal budgets, derive preferences from correctness then resource use, and train a declared preference objective on root continuations.

Acceptance: A shorter incorrect branch must lose to a longer correct branch. Swapping presentation order must preserve the preference, and branches with different starting states must be rejected as unmatched.

## 06 — Verifiable terminal-reward learning

Run bounded on-policy root rollouts with a fixed leaf policy and independently computed terminal task reward. Record policy versions and action ownership, reject overly stale trajectories, and train the root with an explicitly selected reinforcement-learning objective. Separate correctness constraints from efficiency rewards. Pin resolved reference labels before scoring; disputed or unavailable judgments remain unknown.

Acceptance: An empty action program, forged success flag, or omitted failing partition must never receive success reward. Confirm that a genuine verified completion produces the declared reward and a reloadable parameter update.

## 07 — Learned semantic batch control

Expose variable-size partitions and token estimates to the root. Train it to choose semantic batch boundaries, call children on those batches, and join outputs by stable item identity. Include interactions crossing naive byte boundaries and charge actual calls and processed tokens.

Acceptance: A boundary splitting a required dependency must trigger regrouping or explicit reconciliation. Compare fixed-size batching with learned batching under identical task and cost limits, retaining missing and duplicated items as failures.

## 08 — Adaptive recursion-depth policy

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Acceptance: A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

## 09 — Evidence-based stopping policy

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Acceptance: Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

## 10 — Learned context-access planning

Expose context-region descriptors and bounded reads while keeping the full input outside the root window. Train the root to generate access programs that retrieve discriminating regions and recursively examine relevant slices. Log coverage, read amplification, and every region supporting the final artifact.

Acceptance: Plant decisive information outside the usual prefix and supply a plausible distractor near the beginning. A policy that answers from previews alone must fail the oracle, and an unbounded full-context copy must be detected.

## 11 — Coverage-directed recursive traversal

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Acceptance: Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

## 12 — Verified long-output assembly

Train the root to assemble semantically required role outputs exceeding one generation, using an expected coverage manifest and externally checked fragments. Return the typed result handle and digest with streaming backpressure. Padding cannot satisfy output length; validate the whole artifact against domain obligations after assembly.

Acceptance: A correct prefix followed by silent truncation must fail completeness. Reordering asynchronous fragments or repeating one fragment must be caught before final publication, without copying the whole output into the root prompt.

## 13 — Learned persistent-buffer reuse

Train the root to recognize reusable intermediate results by their context, procedure, and dependency identities. Maintain external result buffers with explicit invalidation conditions; recursive children receive scoped references. Measure saved work alongside semantic correctness, including reuse across resumed episodes.

Acceptance: Change one dependency while leaving the buffer's human-readable label unchanged. The policy must recompute affected results and retain unaffected ones; stale reuse or unnecessary replay of completed effects fails.

## 14 — Contradiction-guided program revision

Generate episodes where two independently sourced intermediate results conflict. Train the root to localize the conflicting assumptions, recursively investigate discriminating evidence, and revise dependent program steps. Retain both original claims and the evidence that resolved or preserved the disagreement.

Acceptance: Several copied supporting reports must not overwhelm one valid counterexample. If the conflict remains unresolved, the output must preserve that state rather than select whichever child answered last.

## 15 — Cost-aware child-policy selection

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Acceptance: A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

## 16 — Learned asynchronous recursion

Expose bounded futures for recursive calls and train submit, await, batch, and cancel decisions on controlled completion distributions. Reservations span all descendants. Keep completed result identities stable and ensure training and inference scheduling never block the terminal input handler.

Acceptance: Delay a required child indefinitely while delivering other results out of order. The system must remain responsive, preserve partial work, and report unresolved completeness; dropping the slow child cannot manufacture a successful fast result.

## 17 — Learned typed role handoffs

Train the root to delegate through explicit input, output, evidence, and uncertainty schemas while its partner policies stay frozen. Child prompts contain sufficient scoped context and exact verified dependency values. Failed or incompatible handoffs return typed diagnostics that the root can repair.

Acceptance: A partner omitting required evidence or returning a result from another task must be rejected. Matching prose labels cannot replace schema and identity checks, and a repaired handoff must preserve its original authority.

## 18 — Active example acquisition

Train the root to select the next development example or oracle query using uncertainty, task importance, and expected decision benefit. Record selection probabilities and the population from which examples were chosen. Keep a fixed independent evaluation distribution for measuring actual improvement.

Acceptance: A policy that repeatedly samples easy successes must lose coverage credit. A highly uncertain but irrelevant example must not outrank a decisive missing case merely because it produces a larger confidence change.

## 19 — Role-specific counterexample adversary

Train an auxiliary root policy to generate valid challenging inputs for the role's current candidate behavior. A fixed independent referee checks input validity and confirms violations. Retain diverse minimized witnesses, and evaluate the main role against non-adaptive sealed cases as well.

Acceptance: A malformed input, altered reference, or induced harness failure cannot earn adversarial success. A genuine in-contract error must produce a reproducible witness tied to the exact candidate and environment revisions.

## 20 — Invariant-conditioned program generation

Attach explicit executable invariants to development tasks and train the root to carry them through decomposition, child-result consumption, and final assembly. Check invariants outside the policy, distinguishing proven bounded conditions from sampled checks and unresolved assumptions.

Acceptance: A candidate that satisfies visible examples while violating a required invariant on a boundary case must fail. Removing the check or replacing it with the root's assurance must not change the verdict.

## 21 — Learned minimal-witness extraction

Train the root to shrink a failure of this role's program, preserving its original domain obligations and fixed failure identity. It removes candidate context or program regions, recursively checks reductions, and stores a verified failure witness separately from the normal role result. Measure size and replay reliability separately.

Acceptance: Deleting the evidence until a different error appears must be rejected as failure drift. The final witness must reproduce the original oracle mismatch, and timeout cannot establish that a removed region was unnecessary.

## 22 — Calibrated role abstention

Train an explicit abstention decision using development outcomes and calibrate its thresholds on a separate calibration split. Preserve task difficulty, evidence availability, and error categories. Evaluate selective accuracy together with retained coverage so refusing every task cannot appear useful.

Acceptance: A timeout or unavailable oracle must remain unknown. Report the retained-task denominator and coverage; a high-confidence wrong answer and an always-abstain policy must both fail their predeclared acceptance conditions.

## 23 — Drift-adaptive role policy

Train the root to distinguish transient anomalies, changed input distributions, and changed task semantics from chronological observations. It selects bounded diagnostic probes and proposes scoped relearning. Keep an immutable pre-change checkpoint and evaluate detection delay, false alarms, and post-change correctness.

Acceptance: One extreme outlier must not trigger wholesale relearning. A sustained change affecting the role's contract must be detected without access to future observations, and unaffected task families must retain their prior behavior.

## 24 — Dependency-scoped retraining

Bind training episodes and learned procedures to content and environment dependencies. When one dependency changes, select the affected development slices and train a bounded role update with replay from unaffected families. Version the dependency graph and resulting checkpoint separately.

Acceptance: Editing content at the same path must reopen dependent learning claims. Retraining only on the changed slice must not hide regressions elsewhere, and unrelated episodes must not be relabeled stale merely to improve reported coverage.

## 25 — Checkpoint recovery policy training

Create interrupted role episodes with committed buffers, pending child calls, and explicit effect receipts. Train the root to reconstruct logical state, reconcile pending work, and continue from a compatible model/environment tuple. Treat unavailable state as a recoverable diagnostic rather than invented memory.

Acceptance: Resume after an effect completed but before its acknowledgement was recorded. Reconcile the receipt instead of repeating the effect; incompatible state schemas or changed model identities must prevent a misleading continuation claim.

## 26 — Hierarchical context navigation

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Acceptance: A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

## 27 — Constrained Pareto policy distillation

Collect independently verified root trajectories with quality, latency, memory, and cost measurements. Construct a constrained frontier and distill policies conditioned on declared operating budgets. Keep correctness and capability rules as hard requirements rather than exchangeable score terms.

Acceptance: A fast invalid trajectory must never enter the feasible frontier. Tightening a resource budget should produce a valid lower-cost policy or explicit infeasibility, not an unreported reduction in required task coverage.

## 28 — Measured cross-family transfer

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Acceptance: A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

## 29 — Capability-constrained program learning

Train the root to generate useful programs within a declared host-service contract, using structured action schemas and explicit capability denials as observations. Constraints shape valid proposals, but the host still authorizes every effect independently. Evaluate task completion under different legitimate grant sets.

Acceptance: A recursive child requesting broader authority cannot obtain it from generated code or a rewritten schema. The root must select an allowed alternative or report inability, and a denied action cannot be recorded as completed.

## 30 — Joint model and runtime admission

Train the role to identify incompatible model, adapter, child-policy, environment, and native-procedure versions. Store a typed admission plan, pinned tuple, receipts, and decision in feature_artifact; role_result retains the domain artifact. A separate authority validates compatibility and serializes admission against revocation.

Acceptance: An unchanged parent generation with a revoked receipt must still fail admission. Native-improvement claims require changed instruction bytes, retained heap/session state, and no exec; a trained model's confidence alone cannot authorize activation.

