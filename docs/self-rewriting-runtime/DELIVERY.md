# Delivering the resident self-rewriting runtime with concurrent agents

The work is organized around one observable product result: the active native implementation changes while the running agent retains its address space, state and ongoing work, and an independent check establishes why the change is better. All 120 prompts are standalone tasks. Prompts 61–120 additionally require trained role RLMs and the infrastructure that produces and serves them. This document adds coordination when several are executed together; it does not launch agents or implement the scheduler.

For role training, apply [the RLM training contract](ROLE_RLM_TRAINING.md): coordinate C environment/bridge work, dataset/trajectory work and trainer/evaluation work around one pinned interface. Each role owns its checkpoint lineage and independent evaluation; worker concurrency does not imply unbounded training concurrency.

## Split engineering ownership by contract

Start with one coordinator/integrator and three engineering sessions:

| Session | Main ownership | First concrete responsibility |
| --- | --- | --- |
| Native runtime | Prompts 01–20: code cells, compilation, generation publication, state and frame lifetime | A bounded native scorer can change generation without `exec` or state loss |
| Evidence runtime | Prompts 21–40: claims, observations, retrieval, provenance and cognitive dataflow | A reported defect resolves to the exact active code generation and a reproducible observation |
| Reasoning runtime | Prompts 41–60: procedures, hypotheses, experiments, calibration and learning | The proposer distinguishes causes, produces a candidate, and cannot approve itself |
| Coordinator | Frozen interfaces, task ownership, review scheduling and the integration branch | Accept only the tested combination of code and evidence contracts |

A completed worker slot becomes an independent review slot. The proposer does not review its own patch. Candidate evaluation and integration take precedence over generating more unreviewed code. Increase concurrency only after measuring throughput through this complete loop.

## Freeze the shared interface before parallel edits

Publish an immutable contract artifact containing the smallest shared definitions:

- **Code cell identity:** cell ID, parent and candidate generation, IR/native hashes, ABI/effect/state versions.
- **Evidence reference:** source content hash, region, active code generation, observation ID, and explicit valid/unknown/failed state.
- **Reasoning stage:** typed inputs/outputs, prerequisite validity, cancellation and terminal semantics.
- **Experiment:** hypothesis, expected discriminating observations, isolated input snapshot, oracle version and resource envelope.
- **Acceptance receipt:** candidate and parent hashes, verifier identity, task family/split, outcomes, resource usage and current acceptance state.
- **Activation request:** expected active generation, admitted candidate receipt, migration record, frame-lifetime policy and rollback target.

Bind these through small headers and modules. Do not let three sessions independently invent incompatible meanings for `confidence`, `generation` or `success`. Confidence estimates cannot replace evidence status; process exit cannot replace verification; bytecode compilation cannot be reported as native activation.

Each prompt remains independently useful by defining the minimal contract it needs if none exists. During a coordinated run, the coordinator supplies the pinned common contract instead. If an interface must change, publish a new version, identify consumers, and revalidate them together. Do not mutate an accepted interface artifact in place.

## Preserve the dirty source baseline

Use a private seed repository built from reviewed current source bytes, including required untracked files. Do not assume `HEAD` contains the user's present runtime. Keep the original checkout, index and running executable unchanged. Verify full path/type/mode/content inventories before and after capture and compare the captured tree with that inventory. Exclude credentials, caches, live databases and unrelated outputs.

Create one private worktree or clone per attempt, with separate build directories, HOME, TMPDIR, sockets, IPC stores and test ports. Worker builds use `DSCO_NO_INSTALL=1 make -j2 dsco` with explicit targets. The current default Makefile target installs binaries, so plain `make` is inappropriate for concurrent workers. JIT experiments build a separate owned experimental executable with explicit platform configuration, never silently alter the installed application's signing settings.

Source files belonging to the user's Python projects are donor references. This delivery task targets the C runtime; do not change those projects to make an example appear to work.

## Ownership and communication

Reuse `src/blackboard.c` for atomic task claims, generation/token leases, immutable candidates, dependencies and acceptance. A proposed deterministic coordinator launches and monitors sessions; the existing board does not itself launch them.

Give claims a bounded TTL, renew during implementation and during queued/running verification, and stop admitting output after expiry. Every output binds the original task, generation, contract and baseline. A stale worker may finish in its private tree, but cannot publish accepted changes after reassignment. Store progress as board events and artifact references rather than source commits containing chat logs.

Reserve shared edits by symbol or contract: VM opcode tables, tool registration hooks, central prompt dispatch, plugin lifetime operations and Makefile entries. New modules and tests proceed independently. Reservations coordinate writers; they are not filename-based security locks. Generated tool effects still enter `tools_execute_for_tier()`. Source editing remains ordinary `fs_write`; activation exposed as a control-plane tool must retain the applicable control grant.

The board accepts at most 32 KiB of artifact text and 16 dependencies per task. Publish small manifests pointing to content-addressed patch bundles and test receipts. Long native stress tests run in separate supervised verifier jobs; the board's short checker verifies trusted receipt identity within its current 120-second ceiling. Workers cannot supply their own `verified:true` in place of those jobs.

Coordinator-owned verifier jobs issue authoritative receipts through an authenticated coordinator channel or a verifier signing identity unavailable to workers. Bind each receipt to exact source-tree and native-image hashes, parent/input generations, contract, oracle bytes/version, environment, task split, terminal result and acceptance epoch. The short checker verifies issuer and all bindings against the current board snapshot. Stale, substituted, partial or worker-authored receipts fail admission. Resident activation serializes receipt validity checking and publication with invalidation, using the same authority epoch; Git promotion alone does not establish live-code admission.

## Deliver vertical demonstrations, not sixty disconnected components

The suggested integration sequence is:

1. **Native generation change:** combine dispatch, bounded machine-code generation, lifetime management and a retained sentinel. A controlled candidate establishes actual instruction replacement and no `exec`.
2. **Measured rejection and rollback:** add isolated comparison, hidden counterexamples and rollback. Prove both acceptance and rejection paths; a staged failure must not retire the last usable generation prematurely.
3. **Reasoned self-repair:** connect exact observations, competing fault hypotheses, a discriminating experiment and a code-producing proposer. Record whether candidate generation used real inference or a fixture; only the former supports the autonomous-proposal claim.
4. **Accumulating improvement:** add reusable rewrite operators, learned applicability, calibration and historical counterexample retention. Demonstrate transfer to new cases without sacrificing previous invariants.
5. **Broader reasoning:** add source-grounded multi-hop research, quantitative reasoning, causal experiments and adaptive compute allocation as further optimizable code cells/programs.

These are integration milestones, not hidden dependencies for copying an individual prompt into a session. The [catalog](catalog.json) supplies source anchors, mechanism and falsifying challenge for each feature.

## Integrate the exact tested revision

Workers submit immutable commits/bundles, changed-path manifests, ABI/schema deltas and test receipts. The integrator applies them to a fresh candidate tip, resolves conflicts narrowly, and reruns checks against that resulting tree. A worker's passing test on a different tree is not evidence for a merged revision.

Use one immutable integration task per tip with exact input artifact IDs. Serialize promotion and invalidation decisions through the coordinator, recheck current acceptance, then compare-and-swap the expected Git ref. Reconcile the ref, board and intent journal after crashes. Git CAS alone does not prevent an accepted input from being independently invalidated; a detected authority mismatch quarantines the tip and blocks downstream release.

Keep **source integration**, **experimental live activation** and **installed release** separate. Native experiments may activate code only inside their explicitly owned test process. Accepted source does not authorize altering unrelated running sessions. A release owner handles installation only within the authority for that release.

## Required evidence for the whole system

Record the binary/source baseline, candidate native bytes, actual entrypoint generation, resident sentinel and session continuity, no-`exec` observation, old-frame completion, state migration outcome, independent correctness results and full optimization costs. Preserve failed, cancelled and inconclusive attempts. Check terminal input during compile/evaluation. Repeat capability denial tests through generated tool-call paths.

A test harness can prove generation switching deterministically. A controlled benchmark can measure improved outcomes on specified task families. Neither alone proves open-ended self-improvement, arbitrary native-code safety or improved model weights. The release report states exactly which claim each experiment supports.
