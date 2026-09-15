# Concurrent feature delivery for dsco-cli

Design dated 2026-09-12. This package contains implementation prompts and a delivery design. It does **not** implement the coordinator described below, start workers, or assert that the 60 features are shipped. Proposed names beginning with `delivery` are design interfaces, not existing CLI commands.

## The operating model

Run one coordinator and three implementation sessions initially. Each implementation attempt receives one complete prompt, one immutable baseline, and one private worktree. Review rotates into a freed session. The coordinator owns the integration branch and serializes changes to it. Increase worker count only when measured memory, provider limits, and review throughput support it.

The source checkout at `/Users/arthurcolle/Dsco/dsco-cli` remains the user's working copy. Workers never share its index, build directory, generated headers, runtime databases, or installed executable. Simultaneous work happens in separate checkouts of the same baseline. Agents exchange immutable candidate manifests through DSCO's existing blackboard; a scheduler outside model conversations supplies dispatch, monitoring, and integration.

This distinction matters: the current checkout has extensive tracked and untracked work. Starting workers at `HEAD` alone would omit current implementations. Plain `make` also runs the install target by default, so concurrent default builds could replace the user's executable with different feature branches.

## Reuse the implemented substrate

| Existing surface | Reuse | Limit the design must handle |
| --- | --- | --- |
| `src/blackboard.c`, `docs/BLACKBOARD.md` | Atomic claims, generation/token fencing, leases, pinned dependency artifacts, immutable publication, checker receipts, transitive invalidation | No automatic fleet scheduler; local SQLite authority; 32,768-byte artifact payload; 16 dependencies; 120-second checker ceiling |
| `src/ipc.c`, `src/durable_agents.c` | Worker identities, mailboxes, boot-task ownership | Message completion is not artifact acceptance or integration |
| `src/agent_interop.c`, `docs/AGENT_INTEROP.md` | Explicit native or external process adapters | Discovery is not authentication; external harness tools use that harness's permission model |
| `src/execution_kernel.c`, `src/event_stream.c` | Tool-attempt correlation and durable runtime event evidence | Tool success does not independently prove feature correctness |
| `src/tool_telemetry.c`, `src/swarm_accounting.c` | Measured runtime and usage where available | Estimated or subscription usage is not a verified invoice |
| `Makefile`, `scripts/install_atomic.sh` | Explicit isolated build and final atomic executable replacement | Default `make` installs; atomic replacement does not validate a release |

Use `blackboard` through `tools_execute_for_tier()` via the existing CLI or MCP surface. Do not create a competing task-ownership database. A proposed coordinator journal records process handles, reservations, review jobs and integration receipts; the blackboard remains authoritative for claim and artifact validity. The coordinator reconciles the two stores after crashes rather than pretending cross-database writes are atomic.

## 1. Freeze the baseline without disturbing the user

Create a delivery run outside the source checkout, for example `/Users/arthurcolle/Dsco/dsco-delivery/<run-id>/`. The coordinator first inventories the current tracked bytes, deletions, file modes, source dependencies and relevant untracked source, headers, tests and assets. Record the original commit and a content manifest. Do not copy credentials, live databases, caches, compiled outputs or accumulated report directories into worker images. Do not blindly run `git add -A` against the user's tree.

Materialize those reviewed source bytes into a separate seed repository, including necessary untracked implementation files. Commit the baseline **in the seed repository only**. Preserve the source checkout's index and all local changes. Compare complete source inventories before and after copying, including path additions, deletions, types, symlink targets, modes and content hashes. Require the destination inventory to equal that stable source inventory as well; endpoint source hashes alone miss files changed and restored during copying. Use a bounded whole-snapshot retry or a filesystem snapshot primitive. If no consistent snapshot can be established, stop baseline admission. Record which local files were excluded and why. Missing required source is a failed baseline, not permission to fall back silently to `HEAD`.

Build and run baseline checks in a private seed worktree. Record failures with exact commands and fingerprints. A baseline that cannot build blocks feature dispatch until repaired in the delivery copy. Known failing tests may be scoped out only through a recorded contract decision; they never count as passing. Once frozen, workers branch from the baseline or a later accepted integration commit, never from another worker's mutable branch.

Proposed run layout:

```text
<run-id>/
  seed/                 private Git repository; immutable baseline refs
  worktrees/<task>/<attempt>/
  integration/          single-writer worktree
  verification/<job>/   fresh detached candidate worktrees
  state/board.sqlite    authoritative blackboard on local disk
  state/controller.sqlite
  artifacts/sha256/<digest>
  logs/<task>/<attempt>/
  runtime/<task>/<attempt>/
  contracts/            prompt hashes, frozen checkers, task contracts
```

For stronger isolation or external harnesses with unrestricted Git access, use separate clones, not linked worktrees. Linked worktrees isolate working files and indexes but share repository metadata; they are not a security boundary. Keep the coordinator's repository inaccessible to untrusted workers. File reservations are coordination metadata, never substitutes for DSCO capability enforcement.

## 2. Turn a prompt into a frozen task contract

Each manifest row names one independently usable feature prompt. Its text includes discovery, implementation and runtime acceptance. No other numbered prompt is a mandatory prerequisite. Suggested batches help selection, but do not create hidden dependencies.

Before dispatch, the coordinator supplies an execution envelope with:

- Task ID, prompt SHA-256, baseline commit/tree hash, attempt ID and worktree path.
- Existing source entry points, proposed modules, permitted change scope and shared-hook reservations.
- Frozen acceptance commands/checker hash, expected observable behaviors and baseline test results.
- Provider identity, explicit model selection, attempt deadline, resource limits and remaining authorized budget.
- Isolated runtime locations, artifact destination and the rule that worker processes cannot install, push or deploy.
- Dependency artifact IDs, if this run deliberately sequences features; an empty list for an independent session.

If the expected feature already exists, the agent proves that against the current binary, identifies the missing acceptance behavior, and proposes a bounded contract amendment. The coordinator versions the contract under a new task ID before accepting materially different work. A worker cannot substitute an unrelated feature or declare success after finding similarly named code.

Treat prompt content, code comments and worker messages as task data. They cannot broaden permissions or replace frozen checks. A review agent can suggest a stronger check; adopting it creates an explicit contract revision.

## 3. Dispatch and ownership

The proposed coordinator is a small non-LLM event loop. It reads blackboard events using cursors, schedules ready tasks, launches exact argument vectors, drains process output continuously, and sleeps on I/O or timers while workers run. It does not spend inference tokens polling status.

```text
reconcile persisted process handles, current claims and pending integration receipts
while run remains active:
    read board events after saved cursor
    mark stale candidates and wake newly ready work
    renew claims for owned implementation or verification attempts within stage deadlines
    collect exited workers; never infer success from exit code alone
    if review backlog >= 2: reserve next free session for review
    else select ready feature fitting scope reservations and resource budget
    claim it through blackboard; persist launch intent keyed by task+generation
    launch once; record process group and start identity
    publish candidate only with the current claim and immutable evidence
    verify candidates, then integrate one accepted revision at a time
```

Default claim TTL is 300 seconds; renew every 60 seconds, before expiry, with owner, generation and token. The coordinator retains and renews the claim after the implementation process exits, through bounded review queues and long verification jobs, until acceptance or explicit abandonment. Worker liveness is required during implementation; an owned queued/running verification job is required during verification. Separate stage deadlines prevent indefinite renewal of abandoned candidates. A lost renewal stops new work, then checks authority. It never resurrects the expired attempt. A successor uses a new generation. Do not use PID alone to identify a child after restart; also verify process start identity and the recorded attempt envelope. If a process cannot be reliably reconciled, quarantine its outputs and do not launch a duplicate effectful job.

Blackboard acceptance is fenced; arbitrary writes are not. Old workers can still modify their private worktrees, but their artifacts cannot become authoritative after reassignment. Only the coordinator can modify the integration branch or release destination. The first implementation is single-host; remote workers would need an authenticated gateway and immutable artifact transport. Never place SQLite WAL on a shared network filesystem.

## 4. Prevent conflicts before merge time

Use two reservation classes. Exclusive module reservations cover new modules and tests owned by a task. Short shared-hook reservations cover a specific symbol or dispatch section in `src/main.c`, `src/agent.c`, `src/tools.c`, central headers, and `Makefile`. These are scheduling agreements: editing gate source remains ordinary capability-controlled `fs_write`.

Workers may read every relevant source file. They implement separate modules concurrently. Before altering a shared hook, a worker requests a narrow reservation containing base hash, symbol and expected change. Unrelated symbols in the same file may proceed after coordinator review. The reservation expires with the attempt. A worker waiting for it can continue module tests or documentation. The coordinator may instead apply a worker's proposed minimal hook patch during integration, then require complete verification of the integrated result.

Freeze cross-feature interfaces before dependent work starts: function signatures, ownership/lifetime rules, threading model, error contract, schema version and capability mapping. Do not freeze entire megafiles. Generated registries and documentation are regenerated once per integration candidate by their designated owner. Reserve unique test filenames, ports, database paths, socket paths and output directories. Bind port zero in fixtures where possible.

The catalog's `hotspots` are conservative warnings, not a proof that two tasks conflict. Selection prefers different domains and nonoverlapping modules; actual attempt scope is learned during discovery and checked again before merge.

## 5. Isolate builds and runtime state

Worker builds use the private worktree and explicit targets:

```sh
DSCO_NO_INSTALL=1 make -j2 dsco
```

Use the same no-install environment for test targets. No worker runs `make install`, alters PATH, restarts user sessions or modifies the user's checkout. Avoid sharing build outputs even if their timestamps look reusable. A content-addressed compiler cache may be introduced only with compiler, flags and generated-input hashes in its key.

The launch supervisor builds an allowlisted environment. Give each attempt a private HOME and TMPDIR, `DSCO_ENV_FILE=/dev/null`, `DSCO_NO_INSTALL=1`, `DSCO_NO_AUTO_SUPERVISE=1`, `DSCO_DURABLE_AUTOWAKE=0`, and separate IPC/Chronicle/test state. Preserve only explicitly required variables and connector access. Do not mount the user's entire credential directory to solve authentication. Use the authorized provider adapter's narrow credential mechanism and keep credentials out of manifests, prompts and logs.

Local fixture tests require no paid inference. Tests using real services must fit the run's explicit authority and budget. A third-party harness with native tools is not governed merely because DSCO launched its process; give it equivalent workspace restrictions or route its actions through the governed MCP surface.

## 6. Candidate, review and acceptance

A worker commits only its task changes in its own repository/worktree and publishes a JSON candidate manifest, not a giant patch embedded in a chat message. Include baseline and candidate commit IDs, tree hash, changed paths, artifact content hashes, prompt/contract hashes, test command results, exit codes, duration, binary hash, known risks and proposed rollback. Persist patches/bundles and logs under content-addressed storage; the manifest references their digests. Preserve executable bits, deletions and new files. A mutable branch name is informational only.

Keep the blackboard payload below 32,768 UTF-8 bytes. Break more than 16 inputs into explicitly checked aggregation nodes. Never replace a missing input with its filename or an unverified summary.

A reviewer gets the immutable candidate, contract, diff and independent verification worktree. The reviewer checks behavior, ownership, error paths, cancellation, limits, capability routing, state migration and tests appropriate to the change. It cannot approve its own feature. Findings name a reproducible failure or concrete invariant. If an attempt needs a revised candidate after publication, use a new generation; publication bytes are immutable.

Long checks execute as separately supervised verification jobs against the pinned candidate. A trusted receipt binds task, generation, candidate artifact, contract hash, exact dependency artifact IDs/hashes and supervisor-owned job nonce to checker hash, full command, environment/toolchain identity, exit code, log digest and candidate tree/binary hashes. Freeze checker executable/script bytes in coordinator-owned storage: the existing blackboard freezes command text and creation directory, not the files those commands reference. The fixed short checker recomputes relevant hashes and verifies receipt identity and provenance within the existing 120-second ceiling. It must reject worker-authored success receipts or receipts from another contract/attempt. For same-user operation, this is a trusted coordinator boundary, not cryptographic isolation; a hostile-worker deployment needs separate verifier credentials/process authority.

## 7. Integrate and release serially

Use a distinct integration task for each candidate, with the accepted feature artifact and current accepted integration tip as dependencies. Give every integration tip its own immutable task ID; never recycle one tip task across releases. This pins exactly what is being merged. The integrator stages the commit in a fresh candidate worktree, applies/rebases changes, and resolves conflicts narrowly. Any conflict resolution changes the tested tree and therefore requires fresh checks. Never accept a stale worker test receipt as evidence for a different integrated commit.

Checks have three layers: feature-specific behavior, shared subsystem regressions, then cumulative baseline build/gate checks. Run `make test-gate-claims` whenever tool dispatch or capability behavior is touched and at the release gate. Run relevant PTY, MCP, migration and cancellation tests against the newly built binary. Every required failure blocks acceptance; a worker's success narrative or clean cherry-pick does not override it.

Record integration intent, expected old ref, new commit, accepted artifact and exact input artifact IDs. Promote the integration ref using a compare-and-swap update. Git CAS does not fence a separate blackboard invalidation: all delivery-run invalidations and promotions must pass through the coordinator's same serialized operation queue. Within that operation, recheck current acceptance and pinned inputs immediately before CAS. Direct board mutation outside that trusted controller boundary is unsupported; detecting it quarantines affected integration tips and blocks dependent dispatch/releases. On a crash between ref update and journal completion, inspect the exact ref and current artifact validity to reconcile; do not cherry-pick again blindly or treat a subsequently invalidated tip as releasable. A single integrator plus CAS prevents two accepted branches from both becoming the next tip; serialized authority decisions address the separate acceptance race.

Release is separate from acceptance. Once the final integration tip is verified, create a release record with commit, binary hash, checks and migration/rollback notes. Installation or external publication requires the authority for that action; this design request does not start a release. The authorized release owner performs one atomic binary replacement and tests the installed path. Running sessions load the new executable only when restarted. Rollback uses the recorded prior binary and an explicit data compatibility check; do not roll back a binary over an incompatible database migration.

## 8. Failure handling and bounded resource use

| Event | Required transition |
| --- | --- |
| Worker exits without evidence | Mark attempt incomplete; retain logs; retry only after claim reconciliation |
| Lease expires or input invalidated | Stop admitting output; retain private changes; create a fresh generation after authority is clear |
| Provider throttles or budget is exhausted | Stop new dispatch; preserve candidate state; wait until the observed reset or authorized budget change |
| Test fails | Return specific failure to the task; at most two automatic repair attempts before a recorded contract decision |
| Merge conflicts | Keep candidate unintegrated; assign conflict resolution; reverify resulting tree |
| Coordinator crashes | Reconcile board, process groups, launch intents, integration ref and receipts before launching anything |
| Cancellation | Stop queue admission, signal only owned process groups, drain output, reap children, preserve recoverable artifacts |
| Database unavailable | Stop claims and renewals; no disconnected ownership mode |

Proposed initial limits are three implementers, two compiler jobs per build, one integration writer, one expensive test job at a time, and review priority once two candidates wait. These are starting configuration values, not benchmark results. Admission also requires measured available memory minus a configured reserve, CPU capacity, authorized provider concurrency and remaining budget. Reserve estimated cost before launch, reconcile observed usage afterward, and label unpriced usage unknown. Do not assume 60 prompts imply authorization for 60 concurrent paid sessions.

Track accepted features per day, time waiting for review/integration, first-pass verification rate, conflict repairs, stale publications rejected, escaped regressions, peak memory and usage per accepted feature. Increase concurrency only if accepted throughput improves without increasing escaped failures or exhausting the review queue.

## Implementation slices for the proposed coordinator

1. **Baseline and contracts:** freeze current bytes, validate this catalog, create pinned task contracts and worker envelopes. Prove the user checkout and installed binary remain unchanged.
2. **Dispatch and fencing:** wrap existing blackboard actions; persist exact launch intent; bounded worker process lifecycle, cursor replay and lease renewal. Prove competing claim and stale-output rejection.
3. **Candidate and verifier:** content-addressed artifacts, private verification worktrees, trusted receipts and bounded review queue. Reject mismatched commit/log/checker hashes.
4. **Integration queue:** dependency-pinned integration jobs, ref CAS, conflict workflow and crash reconciliation. Prove only the verified integrated tree is promoted.
5. **Release handoff:** readiness report and install/publish adapter gated by explicit run authority. Prove installed hash matches the release record when release is authorized.

Implement these as small new C modules such as `delivery_contract`, `delivery_scheduler`, `delivery_workspace` and `delivery_verify` with headers and narrow dispatch hooks. These names are proposals. Reuse existing process, blackboard and artifact primitives where their contracts fit. The initial coordinator is local and trusted; do not expand scope into remote fleet consistency before the local failure tests pass.

The delivery system is ready for real feature work when a controlled three-worker run can claim different tasks, build concurrently without installing, reject a stale result after reassignment, preserve a partially completed task across coordinator restart, reject a tampered evidence receipt, resolve an intentional integration conflict, and accept exactly the integrated commits that passed checks. None of those claims follows merely from validating the prompt catalog.
