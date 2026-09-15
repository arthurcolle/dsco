# Delivering the 3,000-prompt expansion

This is a backlog of 100 specialist roles crossed with 30 concrete feature mechanisms. A prompt is one independently executable implementation request. It is not an instruction to start 3,000 training jobs, create 3,000 separate foundation models, or duplicate shared infrastructure 3,000 times.

## Choose work by the complete contract

Select a role by its task oracle and a feature by its desired behavior. Read the full prompt before dispatch. Record the selected ID, prompt hash, source baseline, expected output, training budget, dependency contract and verifier identity. The catalog includes a coordination key for each role. Related prompts deliberately share their domain contract or feature mechanism; that relationship is part of the delivery plan, not hidden duplication.

Each prompt provides its own data, recursive execution behavior, neural training requirement and acceptance cases. In an independent session, implement the smallest missing interface it needs. In a coordinated run, reuse a frozen shared environment, trainer adapter, episode schema, model identity contract and authority boundary. Distinct prompts can extend one role module rather than create incompatible copies.

## Bound concurrent ownership

Start with one coordinator and a small pool of workers. Dispatch different role modules concurrently when their write surfaces do not overlap. Serialize mutations within a role unless the coordinator has explicitly divided its interfaces. Feature mechanisms commonly touch shared exporter, scheduler, renderer or trainer adapters; one owner integrates those hooks while other workers submit contract-compatible modules and tests.

Use the existing blackboard's task claims and fenced generations for task ownership. Renew leases through independent verification, including after a worker exits. Bind immutable candidate bundles and verifier receipts to the exact task generation and tested tree. Reject late output from expired or reassigned attempts. Large artifacts live in content-addressed storage; the board carries bounded references.

Every worker uses an isolated worktree or captured dirty-tree baseline, private build and runtime state, and `DSCO_NO_INSTALL=1 make -j2 dsco`. Preserve the user's original index, source and installed executable. All generated tool effects still enter `tools_execute_for_tier()`. Source reservations coordinate editing; they do not introduce filename-based capability locks.

## Separate implementation from training and admission

The implementation receipt establishes that the feature works under its declared tests. The training receipt establishes the exact dataset, trainable parameters, optimizer progress and checkpoint tensors. The evaluation receipt establishes role behavior relative to a matched baseline. A runtime admission receipt binds the tested combination of model weights, child policies, environment version and native generation.

A passing build does not establish learning. A changed checkpoint does not establish improved task outcomes. A better role metric does not establish safe native activation. Permit a completed experiment to retain the incumbent when the proposed feature or training update does not improve its declared objective.

Training queues have explicit collection, teacher-inference, optimizer, evaluation and storage budgets. Frozen leaf identities remain fixed for root attribution. A role may own separate candidate adapters without loading them all simultaneously. The user-facing input loop remains independent of compilation, Tool Management discovery, rollouts and training completion.

## Shared-family and holdout handling

Preserve episode ancestry across all role-feature tasks. Two workers cannot independently assign sibling traces to train and sealed evaluation merely because they work on different prompts. The coordinator owns the family registry, development/calibration/final partitions and oracle versions. Training rewards may consume development validation; sealed final results never feed training or checkpoint selection.

Some semantic roles require independently adjudicated reference labels. Keep ambiguous cases explicitly uncertain. Agreement among role agents is not a substitute for those labels. For deterministic tasks, test candidate behavior against an independent interpreter, reference implementation, exhaustive bounded checker or generated ground truth.

## Integrate and verify the combination

Rebase candidate changes onto an owned integration tip and run relevant checks on that exact resulting tree. Review shared contracts, capability-denial paths, model/adapter identity, input responsiveness, budget accounting and role-specific adversarial cases. Serialize promotion with receipt invalidation; comparing only a Git ref or native parent generation cannot detect revoked evidence.

For native-code claims, observe changed instruction bytes, retained heap/session state, valid in-flight frame lifetimes, and no `exec`. Model retraining, prompt edits and same-PID restarts are separate claims. Keep rollback targets compatible with current state, and never discard post-activation writes by silently restoring an old snapshot.

See [the full delivery design](../DELIVERY.md), [role training architecture](../ROLE_RLM_TRAINING.md), and [native runtime architecture](../ARCHITECTURE.md) for the common contracts. The prompt catalog creates reviewable work; these coordination mechanisms remain implementation requirements wherever the runtime does not yet provide them.
