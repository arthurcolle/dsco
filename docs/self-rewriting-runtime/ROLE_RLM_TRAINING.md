# Each role trains its own Recursive Language Model

The new requirement changes the learning unit: every operational role owns a trainable RLM policy, its executable experience, and its evaluation contract. The native-runtime role learns to investigate and generate code changes; the evidence role learns how to examine large sources; the reasoning role learns how to decompose experiments. Integration, verification and training operations can also have trained role policies. A role produces a versioned checkpoint or adapter, and future invocations explicitly select that artifact.

This document and prompts 61–120 specify the system. No model training or native activation was performed while producing this package.

## The paper's contribution and our extension

The supplied [Recursive Language Models, version 3](https://arxiv.org/html/2512.24601v3) defines an external prompt environment with programmatic recursive model calls. Appendix A studies training the root's environment interaction and decomposition behavior, including distillation and a verifiable-reward experiment. Its results motivate the design; they do not establish that DSCO's roles will improve or that a particular dataset size will suffice.

Our extension is **role-owned training around a shared resident runtime**. A common base model can support separate role adapters. This does not require training a foundation model from scratch for every role, nor running every specialization simultaneously. Start with one native specialist and one evidence specialist, then add roles when their task distribution and independent reward are defined. An adapter is an implementation option to qualify against the selected trainer and serving stack, not an assumed property of every model/provider.

## The role contract

| Field | Required meaning |
| --- | --- |
| Role identity | Stable role ID, task distribution, allowed host services and output schema |
| Root policy | Exact base weights and optional adapter digest, tokenizer, renderer and generation settings |
| Environment | REPL implementation/version, context object schema, execution limits and result protocol |
| Child policy | Frozen model/adapter identities, recursive depth semantics and cross-role delegation contracts |
| Learning data | Episode roots, parent/child tree, observable code actions, execution results and terminal oracle receipts |
| Training objective | Supervised root action targets or a specified verifiable terminal reward; declared trainable parameters |
| Evaluation | Disjoint task families, baseline, resource envelope, correctness conditions and promotion rule |
| Lineage | Dataset, environment, native generation, oracle, trainer configuration and resulting weight hashes |

Operational roles may expose several specialists from prompts 61–100. Keep a specialist's task identity in its data even when training a shared role adapter; otherwise a pooled mean can hide regressions in a rare responsibility. The infrastructure tasks in 101–120 support these role learners rather than requiring twenty independent copies of the training stack.

## What qualifies as an RLM here

Load the complete task context into an external environment object, such as `context`. The root receives its type, length, content identity and bounded previews. It generates executable programs that inspect slices, transform records, call a child model or child RLM inside loops, and retain outputs in environment variables. Return large final outputs through a typed environment handle. Root messages and previews remain bounded; an answer larger than a model generation limit is assembled in the environment.

The distinction is observable. A test should provide context larger than the root window, require evidence from distant regions, and inspect model requests to confirm the full context was never pasted into the root. A separate test requires an output larger than one root generation and verifies the final object's exact content. Programmatic child calls must originate from executed code; manually enumerating a few delegation messages does not exercise that capability.

Depth zero permits environment work without recursive calls. Depth one permits leaf model calls. Greater depth permits children with their own persistent environment loop. Each environment has separate mutable variables and explicit passed context; descendants do not silently inherit the parent's full state. Content handles need scope and lifetime checks. A short printed preview never becomes the authoritative copy of a result.

The runtime still has finite resources. Bound total calls, tokens, execution time, memory, outstanding futures and root iterations. Metadata truncation alone does not bound accumulated root history; stop or explicitly checkpoint the reasoning state before its window is exceeded. Charge reservations across the entire recursion tree so simultaneous branches cannot each spend the same remaining budget. Compilation, training and Tool Management discovery remain outside the input-handling critical path.

## Concrete role objectives

| Role | External context | What the root learns | Independent outcome |
| --- | --- | --- | --- |
| Native runtime | Source snapshots, failing traces, code contracts and profiles | Choose slices and probes, construct executable repairs, select compiler transformations | Untouched functional cases, schedule replay and admitted resource contracts |
| Evidence runtime | Documents, observations, revisions and source ancestry | Partition semantic work, recurse on relevant regions and combine exact results | Span-resolved facts, complete coverage where required, preserved contradictions |
| Reasoning runtime | Hypotheses, prior experiments and decision constraints | Build a dependency graph, select discriminating experiments and consume verified results | Correct decisions under controlled interventions and known counterexamples |
| Integrator | Candidate trees, dependency contracts and verifier receipts | Discover interacting changes and select necessary combined checks | Exact integrated revision passes independent checks; stale receipts fail |
| Verifier | Candidate behavior and hidden challenge interfaces | Find informative counterexamples and minimize failing cases | A separate executable oracle confirms a valid failure; false accusations lose credit |
| Training operator | Dataset inventories, trainer telemetry and checkpoint records | Diagnose failed training jobs and choose valid recovery/configuration actions | Completed, reproducible runs within budget with exact lineage and intact splits |

A trained verifier is an investigator. The deterministic scorer or separately controlled reference remains the authority for reward. If verifier training and candidate training can alter each other's labels, both may learn to agree on a false success. Keep that authority outside the current training objective.

## From work to trainable experience

Capture one episode tree with root role/model identities, native/environment generations, context object hashes, generated code, bounded observations, child-call arguments and result handles, tool receipts, failures, cancellation, latency and resource use. Record observable model outputs and executable actions. Do not invent unavailable reasoning traces.

Split by the original problem family, repository lineage and shared source ancestry before creating turn samples. Siblings, retries, paraphrases, minimized counterexamples and teacher/student variants stay in the same partition. If an evaluation failure becomes new training material, retire that holdout item and create a fresh independent evaluation family.

For an initial supervised run, collect teacher episodes and independently qualify their terminal results. Replay retained code to ensure the serialized environment operations actually work. Each root-turn sample contains the exact history visible before that action as its input and only that turn's root output as its target. Prior root outputs, child replies, user context and tool observations condition the example but receive no target loss in this representation. Preserve failed trajectories separately for diagnosis, rejection examples or explicitly designed preference/reward learning.

Correct protocol errors only when the intended operation is unambiguous and a replay verifies the change. Store both versions and the transformation. A missing final variable cannot be repaired by inventing the desired answer. Syntax success does not qualify the episode's semantics.

## Training and Prime Intellect integration

Use separate Python training workers around the C host's versioned interfaces. DSCO owns task state, tool authority and native activation; the trainer owns gradient updates and checkpoint artifacts. There is no need to embed a GPU training stack into the C executable.

The authors' current [training directory](https://github.com/alexzhang13/rlm/tree/main/training) provides a depth-one local subprocess REPL training harness intended for Prime RL integration. Treat it as a reference adapter to inspect. A subprocess shares host authority unless further isolated, and compatibility with a separately updated dependency is something to test. Pin a mutually compatible RLM, verifiers, prime-rl and renderer set before implementing the bridge.

[Prime RL's training documentation](https://github.com/PrimeIntellect-ai/prime-rl/blob/main/docs/training.md) distinguishes dataset SFT from its orchestrated training paths. Its prompt/completion layout masks the prompt; its messages layout trains assistant turns, and takes precedence if both layouts are supplied. For our per-root-turn export, use completion-only targets and verify actual token masks. Resolve the current configuration schema rather than transplanting the paper's batch-size values into fields with different units.

Begin with supervised root training while leaf policies, task environments and oracles remain frozen. Require an actual trainable-weight change, optimizer progress and a reloadable resulting artifact to claim that training occurred. A different file hash alone is inadequate: compare tensor values and run the loaded checkpoint. This proves training mechanics; task improvement needs held-out comparison.

Then consider RL with verifiable rewards when episode generation is reliable. Declare a terminal correctness indicator, validity constraints, and measured costs. Invalid or incorrect candidates cannot obtain a positive success reward through speed or fewer calls. Retain timeout and infrastructure-failure categories rather than scoring them as success. Any auxiliary shaping term needs an exploitation test, including the policy that does nothing and the policy that floods cheap subcalls.

Freeze the leaf model during root attribution experiments. Next run crossed root/leaf revisions to measure interactions before training both. Record the generating policy revision on every rollout and bound staleness. A changed child endpoint mid-episode invalidates the claimed fixed-policy comparison.

## Evaluate learning and the whole system separately

Use paired evaluations on the same task families and declared seeds with matched permissions and budgets. Compare the original root inside the same RLM environment against the trained root. Also evaluate depth-zero and fixed-decomposition controls where they answer the experiment's question. Include short and long contexts, dense aggregation, distant dependencies, large outputs, malformed intermediate code and budget exhaustion.

Measure task correctness first, then cost per verified success, latency distribution, peak environment memory, syntax-recovery behavior, subcall count and decomposition coverage. Report confidence intervals clustered by source family, missing/censored outcomes and the practical effect threshold. Predeclare how repeated checkpoint selection and comparisons across many roles are handled. Use development for training/search, calibration for grader thresholds, and a sealed final evaluation for the release claim.

The companion `evaluation/` directory contains a deterministic partition blueprint for proposed task families. It contains no measured results and is not a populated benchmark. Family assignments become binding before generated task instances or model completions are created; every descendant inherits the family's partition. Fill the required evaluation-contract fields from real artifacts before a run.

Promote a checkpoint only on evidence meeting its role contract. A legitimate experiment can finish with no improvement and retain the incumbent. Shared base weights or identical task labels never justify averaging incompatible role adapters. Cross-role transfer first produces a candidate through an explicitly evaluated procedure.

## Connect trained RLMs to resident code evolution

Version model weights, REPL policy, environment contract and native code separately. A trained root may propose a new native cell, but its training score does not authorize native execution. Native candidates still pass independent ABI, lifetime, state and behavior qualification described in [the architecture](ARCHITECTURE.md).

Pin the tuple `(role, root weights, leaf policies, REPL contract, native generation)` for each episode. New episodes can use a newly admitted tuple while existing ones drain on their original tuple. Keep acceptance epochs fenced against revocation. If an updated root calls a host function unavailable in its pinned native generation, reject the contract before starting work.

The first joint demonstration uses two roles: a native repair RLM and an evidence RLM. Both generate externally scored trajectories and produce separately trained artifacts. Evaluate each against its own unchanged baseline, serve the admitted pair, and run a new repair task. The final proof joins the trained model identities to the generated code, independent checks, new executed instructions and continuing resident session. Report any unchanged or rejected role explicitly.

## Parallel delivery

One agent owns the C environment and bridge contract, a second owns trajectory export and role datasets, and a third owns trainer/evaluation integration. The integrator freezes interfaces before these streams diverge and verifies their combination. Implement specialist prompts on top of the accepted contract, or supply a small compatible local contract when running a prompt independently.

Keep active training jobs outside interactive sessions and account for collection, teacher calls, evaluation and training together. A role owns a bounded training queue and artifacts, not unlimited background GPU allocation. Installation and live native activation remain distinct from producing source patches or model checkpoints. The existing [delivery design](DELIVERY.md) governs worktree isolation, leases, independent receipts and integration.
