# 60 independent dsco-cli feature prompts

Each numbered section below is a complete standalone prompt. Copy one section into its own implementation session. Proposed commands are specifications, not claims of existing interfaces.

# 01 — Recover unsent drafts after interruption

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/tui.c`, `src/agent.c`, `src/native_composer.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed draft recovery surface that preserves an unsent composer across clean restart and unexpected process exit. Save text, cursor position, workspace identity, and a monotonically increasing revision using bounded asynchronous persistence. On reopening the same workspace, show a recoverable draft indicator and let the user restore or discard it explicitly. Never submit a recovered draft automatically. Keep separate drafts for concurrent sessions, distinguish submitted text from unsent text atomically, and handle truncated files without losing the newest valid record. Respect an explicit persistence opt-out. Existing in-process composer handoff support is the integration foundation, not proof that durable recovery exists. Ensure background Tool Management loading cannot postpone keystroke handling or draft restoration.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/draft_store.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Use owned PTYs to type multiline UTF-8 text, kill the process, restart with isolated state, and restore identical bytes and cursor position. Verify a submitted draft does not reappear, two sessions do not overwrite each other, corrupt state degrades visibly, and slow persistence leaves typing responsive.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 02 — Edit queued follow-up messages

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/agent.c`, `src/tui.c`, `src/native_composer.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Extend the existing bounded followup_queue with a proposed /queue interface for listing, editing, removing, and reordering messages that have not started execution. Assign stable message IDs and display queued versus already-consumed state. Editing should reuse the composer, retain the previous draft, and atomically replace only the selected queued revision. A message consumed while its edit view is open must produce an explicit stale-edit result rather than editing an active turn. Keep queue capacity enforcement and show clear feedback when full. Preserve FIFO order unless the user explicitly reorders entries. Queue manipulation is steering and must never create approval or expanded authority. Do not introduce a second scheduler or change inference cancellation semantics.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/followup_editor.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Drive the live binary through an owned PTY with a deterministic stalled response fixture. Enqueue three messages, edit the middle message, remove the first, and verify exactly-once delivery in the resulting order. Cover full capacity, stale edits, UTF-8, and simultaneous completion while typing.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 03 — Stage large pasted input before submission

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/tui.c`, `src/native_composer.c`, `src/input_budget.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Add a proposed paste staging interaction for bracketed pastes exceeding a configurable byte or line threshold. Show a compact preview with exact byte count, line count, and the existing conservative context estimate; retain the complete content in a bounded local staging object. Let the user expand, edit, attach as a snapshot, or discard the paste before submission. Enter inside a bracketed paste must remain content, and incomplete paste markers must have a recoverable timeout path. Preserve surrounding typed text and cursor placement. Existing paste handling and context estimation should be reused. Keep small pastes behaving as they do today, avoid provider requests for previews, and never silently truncate oversized text. Treat any attachment conversion as a proposed integration that must use the repository's actual attachment path.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/paste_staging.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Use PTYs for multiline pastes, split delimiters, embedded terminal controls, Unicode, and a paste beyond the configured maximum. Verify preview counts against original bytes, explicit submission sends exactly one turn, discard preserves surrounding text, and asynchronous status output does not corrupt the draft.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 04 — Bookmark and revisit terminal evidence

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/tui.c`, `src/project.c`, `src/buffer_store.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement proposed /bookmark add, list, jump, and remove commands over existing transcript or buffer identities. Users should bookmark a message or tool-result boundary with a short label, then revisit its archived content without scrolling through an entire session. Persist stable content references and source session identity, not raw terminal row numbers. Show when live scrollback has wrapped and load archived content if available; report an unavailable source honestly when it is not. Search labels locally and support a plain-text listing outside a graphical terminal. Adding a bookmark must not pin unlimited content into the model context, inject instructions, or reexecute a tool. Reuse the existing buffer store and session surfaces after inspecting their ownership and retention contracts.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/transcript_bookmarks.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Generate enough local fixture output to wrap the project ring, bookmark before and after wrapping, and verify navigation resolves the intended content. Restart with isolated storage and confirm labels persist. Test duplicate labels, missing archives, ANSI-rich results, and a PTY jump that preserves the active composer draft.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 05 — Search prompt history without losing the draft

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/tui.c`, `src/agent.c`, `src/session_memory.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Ship a proposed incremental history picker for previously submitted user prompts, scoped to the current workspace by default. Display matched excerpts with timestamps and workspace labels; support an explicit all-workspaces mode and bounded result pagination. Selecting a result must place it in the composer for editing, never submit it. Escape restores the exact prior draft and cursor. Deduplicate identical prompts in the picker while preserving their history records, and support Unicode-aware matching without blocking input on a large history file. Inspect existing readline and cell-editor history paths so the feature adds one shared index instead of incompatible histories. Do not search assistant messages, hidden instructions, credential stores, or unrelated files. Honor existing history disable settings and avoid inventing new automatic retention behavior.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/history_search.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Use an isolated history fixture containing repeated prompts, long Unicode entries, and two workspaces. In a live PTY search, select, edit, cancel, and submit, checking exact bytes and workspace isolation. Measure responsiveness with at least ten thousand history records and verify history-disabled mode creates no index.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 06 — Provide an accessible linear terminal mode

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/tui.c`, `src/native_composer.c`, `src/main.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Add a proposed accessible terminal mode with a documented explicit switch and sensible non-TTY behavior. Render conversation events as stable linear text, announce busy, waiting, failed, and completed transitions once, and expose keyboard help without relying on color, animation, cursor-position reports, or graphical terminal extensions. Keep the existing shared editor authoritative where interactive editing is available. Suppress repeated spinner chatter while preserving tool names, result summaries, and actionable errors. Support plain-text command output suitable for assistive technology and shell capture. Inspect current no-color, native compositor, and terminal fallback controls first; extend the missing user-visible accessibility contract rather than adding contradictory flags. This task does not redesign themes, remove the rich UI, or alter tool execution policy.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/terminal_accessibility.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Run the built binary in an owned PTY and with redirected output under TERM=dumb and NO_COLOR. Assert no cursor movement or image protocol sequences in linear mode, one announcement per state transition, readable error text, and working input while background services load. Include a keyboard-only transcript documenting navigation and draft preservation.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 07 — Freeze attachment content at submission

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/context_fabric.c`, `src/agent.c`, `src/tui.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed attachment preview and submission snapshot for local text files and selected line ranges. Before sending, show the resolved path, included range, byte count, and a conservative size estimate. At submission, store immutable content using existing ctxkeys and record the source path plus digest so subsequent file edits cannot silently change what a turn referenced. Detect a change between preview and submission and refresh the preview or require an explicit refreshed selection. Bound file size and handle binary files, unreadable files, empty ranges, symlinks, and files outside the workspace according to current read capabilities. Preserve source provenance as data, never as higher-priority instructions. Reuse existing mention parsing if present and keep the initial feature limited to textual attachments.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/attachment_snapshot.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Test with a temporary repository where an attached file changes before and after submission. Inspect the captured request and persisted snapshot to prove exact version identity. Cover UTF-8 ranges, spaces in paths, deletion, an oversize file, and a live PTY preview that does not block typing or make an inference request.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 08 — Fork a session from a chosen turn

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/session_memory.c`, `src/agent.c`, `src/main.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed session fork command that creates a new session identity from a selected completed turn while keeping the original transcript immutable. Record parent session and fork-point identifiers, copy or reference only the bounded conversation prefix, and show lineage when inspecting either session. Preserve source provenance and distinguish historical tool receipts from executable pending actions. Never replay tools, inherit approvals as fresh authorization, or claim filesystem rollback occurred. Reject fork points inside incomplete tool exchanges and explain whether missing archive data prevents a faithful fork. Inspect the existing resume and autosave formats before choosing a versioned extension. The initial slice is local conversational branching, not Git branch creation, alternate model comparison, or automatic state reconciliation.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/session_branch.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Construct a deterministic three-turn session with one tool exchange, fork after the second turn, and verify exact prefix, unique identity, lineage, and untouched original storage. Confirm no tool is reexecuted and no third-turn content leaks. Restart both sessions, test invalid boundaries and damaged archives, and exercise the proposed command against the built binary.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 09 — Preview workspace drift before resuming

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/main.c`, `src/agent.c`, `src/session_memory.c`, `src/project.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Extend current resume behavior with a proposed local preview showing the saved objective, last completed turn, unresolved actions, workspace root, and files whose recorded digests differ from current disk. Distinguish modified, deleted, moved when demonstrable, and unavailable paths. Show the saved commit and current commit when Git metadata exists, without treating a clean Git status as proof that content matches. Let the user resume with an explicit drift note injected as lower-priority context, or leave the session unopened. Keep the first implementation read-only until the resume choice; do not reset files or replay prior commands. Record a bounded touched-file manifest through current session persistence, respecting existing privacy and retention settings.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/resume_preview.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Create isolated saved sessions, then modify, delete, and recreate recorded files and move the checkout. Verify exact drift categories and clear unavailable-state reporting. Resume through the live binary and inspect the resulting context to confirm historical claims are qualified. Confirm inspecting the preview changes neither files nor the saved session and performs no network request.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 10 — Export a structured session handoff capsule

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/session_memory.c`, `src/context_fabric.c`, `src/main.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Ship a proposed local handoff capsule export and import flow containing the objective, accepted constraints, decisions, completed evidence, open work, and source references. Produce a versioned machine-readable document plus a concise readable rendering. Build the initial capsule from explicit recorded fields and user edits; do not invent decisions or silently make paid summarization calls. Import should preview provenance and workspace mismatches and attach the capsule as untrusted historical context, never as system instructions. Bound capsule size and preserve links to external evidence without automatically copying arbitrary files. Inspect current memory tiers and resume handling so this becomes a portable, reviewable handoff slice instead of a competing session database. Export remains local unless the user independently asks to share it.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/session_capsule.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Export a fixture session with conflicting outdated and current decisions, edit an allowed field, and import into a fresh isolated session. Verify schema validation, provenance, deterministic rendering, size bounds, and that malicious instruction text stays ordinary context. Exercise actual binary export/import and demonstrate a clear error for an unsupported version without modifying existing state.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 11 — Inspect and recover compacted context

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/context_eviction.c`, `src/context_fabric.c`, `src/agent.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Expose a proposed compaction inspection view over existing eviction archives. For each compaction, show which complete exchanges were archived, retained summaries or stubs, before-and-after estimated size, and retrievable ctxkeys. Allow the user to expand an archived exchange locally and request its bounded rehydration for a subsequent turn. Warn when the resulting assembled context exceeds the current budget and require a smaller selection rather than silently dropping protected user content. Keep tool-call/result pairing intact and never turn historical calls into executable actions. The implementation must inspect current eviction and archive recall logic before adding metadata. Do not replace the compaction algorithm, provider token accounting, or storage architecture; the deliverable is explainability and controlled recovery for existing compaction.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/compaction_inspector.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Force compaction with deterministic local exchanges including large tool results, then inspect and rehydrate a selected exchange through the live binary. Verify content hashes, exact call/result pairing, budget rejection, missing-archive handling, and immutable history. Confirm protected user messages remain intact and viewing an archive never starts inference or executes its recorded tool.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 12 — Preview the assembled context budget

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/input_budget.c`, `src/context_fabric.c`, `src/agent.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed preflight context breakdown that reports estimated contributions from user text, instructions, conversation, pinned context, attachments, and tool descriptions before a request is sent. Reconcile the displayed total against the exact assembled request using the existing conservative estimator, and label estimates explicitly rather than presenting provider billing tokens. Let the user inspect the largest optional items and remove or unpin selected optional references through existing mechanisms. Show any reduction the input budget layer would apply and a clear reason if admission fails. Keep the preview local and responsive, with no provider call or tool catalog refresh required. Do not introduce another tokenizer or alter protected-content rules. Any proposed command or panel name must fit the actual CLI and TUI dispatch found on disk.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/context_budget_view.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Capture deterministic assembled requests with mixed instructions, tool schemas, attachments, and history. Compare preview totals to the current estimator, verify reported reductions match the transmitted payload, and cover rejection where only protected content remains. Exercise a live preview with a stalled Tool Management endpoint and prove typing and preview completion remain responsive.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 13 — Expose source freshness for recalled context

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/context_fabric.c`, `src/session_memory.c`, `src/context_eviction.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Add proposed source freshness metadata to local-file context references: capture source path, digest, observed timestamp, and workspace identity when a snapshot is stored. When displaying or recalling a reference, distinguish unchanged, changed, missing, and uncheckable source state. Provide an explicit refresh action that creates a new immutable snapshot linked to the previous one; never mutate the old content-addressed object. Preserve valid historical citations even when the current file changed. Freshness checks must be bounded and lazy, use direct file reads where permitted, and avoid scanning entire repositories on startup. Support older metadata with an unknown state rather than guessing. This feature should extend the broker's provenance model and current archive recall path, not build a background file watcher or alter semantic ranking.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/context_freshness.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Store a file, edit it without changing its size, restore its timestamp, and confirm digest-based drift detection. Cover deletion, symlink target changes, permission errors, and older records. Refresh through the built binary and prove both versions remain retrievable by their original keys and no check modifies the source file.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 14 — Explain active workspace instructions

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/setup.c`, `src/project.c`, `src/agent.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed read-only instruction explanation command for the current workspace and an optional target path. Show exactly which AGENTS.md files and other existing project instruction sources were loaded, their scope, ordering, content digest, and any truncation or budget exclusion. Explain which source applies to a nested target without inventing a universal semantic conflict resolver. Distinguish discovered files from actually injected content. Inspect the real loader and its hierarchical behavior first; if a basic listing exists, add target-path scope resolution and injection receipts as the missing slice. Paths and file text are data, and viewing instructions must not execute embedded commands. Keep this local, avoid broad filesystem discovery outside the loader's legitimate scope, and preserve established instruction priority.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/instruction_explain.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Use a temporary repository with root, nested, sibling, missing, and symlinked instruction files. Compare explanation output with the actual captured request for each cwd and target. Cover edited files between runs, excluded oversized content, and a non-repository directory. Exercise the built binary without inference and verify the command changes no instruction files or runtime policy.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 15 — Track explicit project decisions and supersession

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/session_memory.c`, `src/context_fabric.c`, `src/project.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Add a proposed project decision ledger where the user can record a decision, rationale, scope, evidence references, and an optional decision it supersedes. Assign stable IDs and distinguish active, superseded, and withdrawn entries. Retrieval should show the current decision with a concise lineage, preserve historical records, and never promote inferred assistant suggestions into accepted decisions automatically. Provide local list, show, add, and supersede operations using existing project identity and persistence primitives. Bound text and support explicit export for review. If a general memory write tool exists, reuse it internally while adding decision-specific validation and presentation. Do not create governance doctrines, approvals, or new authority; ledger contents remain user-supplied project context and may be corrected.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/decision_ledger.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Record two decisions in one project, supersede one, reopen storage, and verify active retrieval and full lineage. Test two separate projects, invalid supersession IDs, cycles, truncated writes, and concurrent read/update behavior. Exercise the live CLI and inspect resulting context to ensure an outdated decision is labeled historical and an unaccepted assistant suggestion never becomes an active entry.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 16 — Assemble local evidence bundles for completed work

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/context_fabric.c`, `src/chronicle.c`, `src/session_memory.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed evidence bundle command that packages explicitly selected local test receipts, command outputs, artifact references, and a user-written claim into a portable directory with a manifest. Include source session, relative paths, content digests, command metadata when available, and a clear distinction between recorded output and independently verified facts. Validate selected paths and handle duplicate names deterministically. Copy only selected artifacts; do not sweep environment variables, credentials, or an entire working directory. Verify a bundle offline by checking schema, required files, and hashes, and explain missing evidence without marking the claim proven. Reuse existing chronicle and ctxkey references. This is a local developer handoff artifact, not automated release approval or external publication.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/evidence_bundle.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Create a bundle from isolated deterministic test receipts and a small generated artifact, verify it through the live binary, then tamper with and remove entries to prove verification fails precisely. Cover paths with spaces, duplicate basenames, missing sources, and oversized selection limits. Confirm the manifest cannot escape its root and creating a bundle leaves source artifacts unchanged.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 17 — Show workspace changes since a session checkpoint

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/project.c`, `src/session_memory.c`, `src/context_fabric.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement a proposed read-only workspace change digest between an explicitly recorded session checkpoint and current disk. Track a bounded user-selected file set or existing touched-file set, storing snapshot digests and optionally small textual baselines through the context fabric. Report added, modified, deleted, and unchanged files with optional bounded text diffs; distinguish untracked files from changes relative to the checkpoint. Explain when content was not captured and only identity comparison is available. Include checkpoint time and workspace identity. Do not stage, commit, restore, or delete files, and do not assume Git HEAD is the session baseline. Reuse current project and context storage after inspecting their scope contracts. Keep initial collection explicit, without repository-wide watchers or automatic large-file ingestion.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/workspace_digest.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Create a temporary dirty Git repository before checkpointing, then modify selected tracked and untracked files, delete one, and leave another unchanged. Compare digest output to actual bytes and prove preexisting dirty work is not misattributed. Exercise the binary across restart, missing baselines, binary files, and size caps, confirming the index and working files remain byte-identical.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 18 — Navigate compiler diagnostics from build output

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/pty_session.c`, `src/buffer_store.c`, `src/buffer_cli.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Ship a proposed local diagnostics view that recognizes common Clang and GCC file, line, column, severity, and message records in explicitly selected existing PTY or buffer output. Preserve the original output byte offsets and command exit status, group repeated diagnostics, and navigate to a bounded source excerpt using current buffer facilities. Resolve relative paths against the command's recorded cwd and label missing or changed source files. Offer machine-readable export and readable summaries. Parsing must be incremental with bounded memory and handle ANSI sequences, chunk boundaries, multiline notes, and paths containing spaces. Do not automatically run suggested fixes, restart builds, or claim every textual error line is a compiler diagnostic. Reuse existing process ownership rather than adding another command execution engine.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/build_diagnostics.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Run a small intentionally failing C build through the live binary's actual process surface in a temporary project. Verify detected locations, note relationships, original output references, and exit status. Cover split UTF-8 chunks, colored diagnostics, successful builds, malformed lines, and deleted sources. Demonstrate navigation preserves the composer draft and invokes no editor or command automatically.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 19 — Save and launch explicit local terminal recipes

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/pty_session.c`, `src/project.c`, `src/main.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Implement proposed project-scoped named terminal recipes containing an executable, explicit argv array, cwd relative to the project, terminal size, and bounded lifetime. Provide create, inspect, list, and explicit run operations that dispatch through the existing PTY manager. Display the resolved invocation and return the real session ID so existing read, write, status, and close controls work unchanged. Never introduce an implicit shell, persist resolved credential values, start recipes on launch, or pretend PTYs survive dsco restart. Validate paths and schema versions, provide clear errors for missing executables and moved projects, and allow simple descriptions for human recall. If command presets already exist, connect them to the PTY ownership contract and add missing validation rather than maintaining duplicate configuration systems.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/terminal_recipes.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Create an isolated recipe for a tiny local program, run it through the live binary, read output, resize, and close using the returned session ID. Verify argv containing spaces and shell metacharacters arrives literally, capabilities are enforced, and shutdown owns cleanup. Test invalid cwd, excessive lifetime, concurrent recipe launches, and that listing recipes spawns no process.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 20 — Capture reproducible local bug-report packets

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your assigned isolated worktree of that repository. Deliver one bounded, user-visible feature. First read applicable AGENTS.md instructions and inspect `src/chronicle.c`, `src/pty_session.c`, `src/project.c`, related headers, dispatch, and tests. All interfaces named below are proposed, not claims that commands already exist. If the basic capability is present, identify and implement the missing behavior in this specification instead of duplicating it.

Build a proposed reproducibility packet command for an explicitly selected failed local invocation. Capture the exact executable identity, argv, cwd mapping, exit status or signal, bounded stdout/stderr references, selected nonsecret configuration names, and user-selected minimal input files in a versioned manifest. Provide a local inspection command and a generated explicit replay invocation that requires the user to run it; never replay automatically. Distinguish unavailable historical metadata from empty values and record any omissions. Preserve literal quoting safely by storing structured argv rather than reconstructing an unsafe shell string. Integrate with existing chronicle or PTY receipts. Do not collect arbitrary environment values, upload reports, bundle the whole repository, or broaden this into provider benchmarking or release automation.

Keep diffs narrow and preserve unrelated dirty work. Prefer proposed `src/repro_packet.c` and its header with small dispatch hooks and a Makefile entry; reuse existing modules when appropriate. Every tool-call path must use `tools_execute_for_tier()`; enforce capabilities, never filename locks. Keep test state and build outputs isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco` in your own worktree; never install worker binaries or invoke the auto-installing default target.

Acceptance: Capture a deterministic failing program with spaces, quotes, Unicode, and shell metacharacters in arguments. Verify the packet's structured invocation reproduces the same failure when explicitly executed in an isolated test directory. Cover signal exit, missing executable metadata, output truncation, excluded credential values, and packet tampering. Exercise live binary capture and inspection while proving neither action reruns the recorded command.

Close with implemented behavior, exact validation commands and results, changed paths, and remaining limitations. Include live-binary evidence; static inspection alone is insufficient. Run relevant regression checks and `make test-gate-claims` if tool dispatch or capability integration changed.

---

# 21 — Account-scoped offline tool catalog

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Implement an account-scoped, last-known-good Tool Management discovery cache. Users should be able to discover previously available tools while the API is slow or unavailable, with explicit freshness and origin information. Inspect current semantic discovery, external snapshots, and background initialization first; preserve their nonblocking behavior. If basic caching already exists, ship the missing account isolation and stale-result behavior instead of creating another cache.

Persist only bounded catalog metadata and schemas, never credentials or execution results. Define cache identity from canonical endpoint plus a nonsecret account identifier; an unavailable account identifier must prevent cross-account reuse. Publish snapshots atomically only after complete validation. A proposed `dsco tools cache status --json` interface should report generation, age, account scope, and refresh outcome. Discovery may return stale metadata with an explicit marker, but executing a cached tool must still contact its configured service and pass the normal gate. Document eviction and corrupt-cache recovery.

Verify the worktree binary against a controlled HTTP catalog: populate account A, restart offline and discover its tools, then switch to account B and prove A's entries are absent. Interrupt publication and corrupt one snapshot; the previous valid generation must remain usable. Stall refresh while submitting terminal input and record response latency. Include a bounded cache-size fixture and show that secret headers never enter persisted files.

Start with `src/toolmgmt.c`, `include/toolmgmt.h`, `src/tools.c`. Proposed modules: `src/tool_catalog_cache.c`, `include/tool_catalog_cache.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 22 — Generation-pinned external tool schemas

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Implement generation-pinned external tool selection so a catalog refresh cannot silently change a selected tool's contract before dispatch. Inspect the existing snapshot, registry locking, hashed names, and discovery paths. Retain the current stable external names; this feature adds coherent schema identity across selection and execution. If registry generations already exist, finish the user-visible mismatch handling and concurrent refresh proof.

Assign each published external catalog generation an identifier and each schema a deterministic digest. A tool selection should carry the schema identity actually advertised to the model. Before execution, resolve that identity without holding a registry mutex across network I/O. An unchanged schema may proceed after refresh; an incompatible replacement or removed tool should return a structured rediscovery requirement without invoking the backend. Keep in-flight callbacks alive until their users finish. A proposed discovery metadata field, `schema_revision`, must be optional for existing clients and explicitly documented.

Exercise the real binary with a local catalog service that changes a required field while a call is queued. Prove the stale contract is rejected before the execution endpoint is contacted, an unchanged schema survives a generation refresh, and a fresh rediscovery succeeds. Include removal, duplicate remote IDs, and refresh failure cases. Use a repeatable concurrent fixture to demonstrate no use-after-free, lost callback, or mixed-generation result.

Start with `src/toolmgmt.c`, `src/tools.c`, `include/tools.h`. Proposed modules: `src/tool_catalog_generation.c`, `include/tool_catalog_generation.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 23 — Explicit schema capacity and hydration errors

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Make oversized or unsupported remote schemas a visible, recoverable condition instead of advertising truncated tool contracts. Inspect MCP's fixed schema buffers, Tool Management parsing, progressive schemas, and registry limits before coding. Preserve lazy retrieval and existing tool paging. If oversized schemas are already rejected correctly, implement the missing bounded hydration and operator-facing reason reporting.

Distinguish a complete executable schema, metadata-only discovery entry, and rejected schema. Retain full remote identity and a concise reason without pretending the tool is callable. Where the existing transport supports fetching an individual schema, add bounded on-demand hydration; otherwise clearly report that the server must expose a smaller contract. Set explicit input bytes, nesting, and total catalog memory limits. A proposed `dsco tools inspect NAME --json` addition should show contract availability, source size, and actionable rejection reason. Never silently weaken required fields or substitute an empty object schema.

Drive the worktree binary against local HTTP and stdio fixtures advertising schemas just below and above current capacities, deeply nested JSON, malformed schema text, and a valid neighboring tool. Prove the valid tool remains usable, oversized contracts never reach execution with partial arguments, and inspection identifies the actual failure. Measure memory growth under repeated rejected refreshes and ensure startup and unrelated input stay responsive.

Start with `src/mcp.c`, `include/mcp.h`, `src/toolmgmt.c`, `src/tools.c`. Proposed modules: `src/tool_schema_capacity.c`, `include/tool_schema_capacity.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 24 — Retry-After-aware Tool Management admission

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add endpoint-scoped admission that honors server Retry-After responses without freezing unrelated Tool Management work. Existing request retries, jitter, caller deadlines, and parallel execution are starting points, not features to rebuild. Inspect those paths and deliver the missing shared cooldown behavior, including a user-visible explanation of why a request is waiting or cannot meet its deadline.

Parse bounded delta-seconds and HTTP-date values, convert waits to monotonic deadlines, and cap unreasonable server values through documented policy. Requests sharing an authenticated endpoint should respect its cooldown; different endpoints should remain independent. Keep cancellation and shutdown responsive. Preserve stable idempotency keys across permitted retries and never retry a mutation merely because a cooldown expired. A proposed structured error should distinguish admission timeout, server throttling, and transport failure. Avoid logging authorization headers or remote bodies that may contain secrets.

Use the real `dsco tools` path against a controlled HTTP server returning a sequence of 429, 503, and success responses. Verify minimum retry timing with tolerance, maximum caller deadline, bounded concurrent retries, malformed and expired header handling, and an unaffected second endpoint. Include a mutation with unknown outcome and prove that admission logic does not create another execution. Capture server request timestamps and the terminal-visible error so acceptance proves actual transport behavior.

Start with `src/toolmgmt.c`, `include/toolmgmt.h`. Proposed modules: `src/toolmgmt_admission.c`, `include/toolmgmt_admission.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 25 — Remote mutation outcome reconciliation

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Implement explicit reconciliation for Tool Management mutations that time out after the server may have committed them. The client already uses idempotency keys and has a bounded single-attempt helper; inspect those contracts and backend response shapes before extending them. Deliver a narrow operator-visible outcome mechanism, not a general job engine or an assumption that every remote server supports lookup.

Represent confirmed success, confirmed failure, and unknown outcome separately. Preserve the execution identifier, idempotency reference, endpoint identity, and minimal nonsecret reconciliation metadata. A proposed `dsco tools reconcile EXECUTION_ID --json` command may call only an actually supported read-only status endpoint. If no lookup capability exists, return `unsupported` and the available evidence without resubmitting the mutation. Never persist argument bodies by default. Any caller-requested retry must keep the original key and obey the service's documented idempotency contract, with no automatic key replacement.

Verify the worktree binary against a local server that commits a mutation, drops the response, and exposes a read-only status result. Show exactly one side effect, an initial unknown outcome, and later confirmed success. Add unknown identifier, unsupported lookup, expired reconciliation metadata, endpoint mismatch, and denied network cases. Prove reconciliation cannot call the execution route accidentally. Include request counts and redacted machine-readable output in the closeout.

Start with `src/toolmgmt.c`, `include/toolmgmt.h`, `src/tools.c`. Proposed modules: `src/toolmgmt_reconcile.c`, `include/toolmgmt_reconcile.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 26 — Per-call MCP cancellation

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add cancellation and deadlines for an individual MCP tool call without cancelling every configured server. Inspect the current global initialization cancellation flag, stdio reads, HTTP progress callbacks, request IDs, and server ownership. Keep global startup cancellation intact. If per-call deadlines already exist, complete request-specific cancellation and late-response isolation as the missing user-visible slice.

Associate each outstanding call with its request ID, monotonic deadline, and cancellation token. Send the protocol's cancellation notification only when the negotiated server supports the relevant behavior, while always stopping local waiting within a bound. A late response must not satisfy a later request or corrupt the next response frame. Cancellation of one server's call must leave unrelated calls usable. Report cancelled, deadline exceeded, and transport broken distinctly; for mutations, cancellation must state that the remote outcome may remain unknown. Proposed timeout options should extend existing call interfaces instead of adding an unrelated executor.

Run the worktree binary through local stdio and HTTP fixtures: one call stalls, another succeeds, the stalled call is cancelled, and its response arrives late. Prove subsequent calls receive their own results and shutdown reaps owned subprocesses. Measure cancellation latency against a documented threshold. Include a server ignoring cancellation, fragmented JSON, and concurrent server activity, plus the normal capability-gate checks.

Start with `src/mcp.c`, `include/mcp.h`, `src/mcp_response.c`. Proposed modules: `src/mcp_call_control.c`, `include/mcp_call_control.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 27 — Capability-gated MCP resource reads

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Expose MCP resource discovery and bounded reads through the existing governed tool surface. Inspect current MCP capability negotiation and any resource support before implementation; finish a missing resource slice rather than duplicating tools or building a new document index. The user-visible goal is to inspect server-provided resources using their exact declared identity and provenance.

Add paginated resource listing and resource-template discovery where supported, plus a proposed `mcp_resource_read` tool that accepts a configured server and an advertised URI. Treat remote text as untrusted input. Resource URI handling must not become an arbitrary local-file or URL fetcher: dispatch through the owning MCP server, preserve its authorization context, and reject unknown server identities. Apply bounded response sizes, MIME validation, and explicit binary-content behavior. Report unsupported server capabilities clearly. Preserve URI text exactly for protocol calls while displaying a safe representation to users.

Exercise the real binary through `dsco mcp serve` with a local fixture advertising a text resource, a parameterized template, a binary response, and multiple pages. Prove the read result retains server, URI, and MIME information; reject oversized and malformed responses without destabilizing tool calls. Include a resource containing hostile instructions and verify untrusted taint is recorded before later egress is gated. Demonstrate that resource reads never bypass `tools_execute_for_tier()` and cannot directly open host paths.

Start with `src/mcp.c`, `include/mcp.h`, `src/mcp_server.c`, `src/tools.c`, `src/capability.c`. Proposed modules: `src/mcp_resources.c`, `include/mcp_resources.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 28 — Scoped MCP catalog-change refresh

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Implement server-scoped MCP catalog refresh when a configured server reports that its tools changed. Inspect protocol capability negotiation, catalog paging, external registry snapshots, and background initialization. Preserve the existing initial discovery and avoid restarting all connectors for one server's update. If notifications are already consumed, deliver the missing atomic replacement and stale-tool removal behavior.

Recognize negotiated tool-list change notifications during normal response processing. Coalesce bursts into one bounded refresh per server, retain the previous valid catalog during a failed refresh, and publish a complete replacement atomically. Remove tools that vanished from the server while preserving in-flight call ownership. A proposed catalog-status field should expose each server's refresh generation and last refresh result without emitting asynchronous text into the input area. Prevent an abusive notification stream from producing an unbounded queue or constant network traffic.

Run the worktree binary against two controlled stdio servers. Change the first server's list while its tool call is active; demonstrate that a new tool becomes discoverable, a removed tool stops being offered, and the second server remains uninterrupted. Add repeated notifications, invalid replacement schemas, failed pagination, and shutdown during refresh. Show that no partial catalog is published and input remains responsive throughout. Capture server-side list request counts to demonstrate bounded coalescing, and verify all newly discovered tools retain ordinary capability classification.

Start with `src/mcp.c`, `include/mcp.h`, `src/tools.c`. Proposed modules: `src/mcp_catalog_refresh.c`, `include/mcp_catalog_refresh.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 29 — Structured tool-output contract validation

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Implement actionable validation of structured tool results against their declared output schemas. The registry already carries output schemas; inspect native defaults, external registration, MCP structured content, and result truncation before choosing the smallest missing path. Preserve plain-text tools and avoid treating a generic default schema as a meaningful strict contract.

For tools with explicit structured contracts, validate the supported schema subset after execution and before reporting a successful structured result. Distinguish tool execution failure, invalid JSON, and schema mismatch, retaining a bounded redacted preview and a precise field path. A proposed validation mode may allow report-only behavior for compatibility, while strict mode must never turn invalid output into valid-looking data. Document unsupported schema keywords explicitly; do not silently claim full JSON Schema compliance. Contract failure must not trigger replay of a side-effecting tool.

Exercise the real binary via MCP JSON-RPC with a local tool fixture returning valid structured content, a wrong required type, missing fields, oversized output, and an error response. Verify advertised and enforced schemas agree, valid results retain their content, and invalid results include stable error categories and paths. Confirm a mutating fixture runs exactly once despite output validation failure. Add a native tool regression and default-schema compatibility case, then run capability-gate claims so the validator cannot create a parallel dispatch route.

Start with `src/tools.c`, `include/tools.h`, `src/mcp_response.c`, `src/mcp_server.c`. Proposed modules: `src/tool_output_validate.c`, `include/tool_output_validate.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 30 — Read-only capability decision preview

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add a read-only way to preview the exact capability decision for a proposed tool call. Inspect current gate decisions, taint tracking, grant precedence, governance status exemptions, and available introspection commands. Deliver a missing machine-readable explanation slice rather than introducing a second policy implementation. The preview must use the same classification and decision functions as actual execution.

A proposed `dsco tools explain-call NAME --input-json JSON --json` interface should report required capabilities, effective grant sources, relevant session taint, and the specific denial or allow reason. It must not execute the tool, contact a remote service, read credential material, mutate taint, or grant authority. Explain that a preview is conditional on current session state and is not a reusable authorization token. For sensitive arguments, expose only field names needed for reasoning. Keep read-only introspection available during a control-plane denial.

Use the actual binary and `dsco mcp serve` to compare preview and execution for ordinary reads, explicit write opt-out, denied control mutation, permitted control status, and lethal-trifecta egress. Establish taint through controlled fixtures and prove preview leaves it unchanged. Check that `DSCO_ALLOW_EXFIL=1` and explicit capability opt-outs retain their current semantics. Include an argument-dependent tool verb so classification cannot be replaced with filename or tool-name heuristics. Record matching decisions and zero side effects from preview.

Start with `src/capability.c`, `include/capability.h`, `src/tools.c`, `include/tools.h`, `src/mcp_server.c`. Proposed modules: `src/capability_explain.c`, `include/capability_explain.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 31 — External tool origin pinning

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add explicit origin identity to remote tool inspection and prevent a discovered tool from silently moving to another backend during execution. Inspect existing hashed Tool Management names, MCP namespacing, configured server descriptions, and external metadata first. Keep their identity rules; the feature is a transport-origin binding with clear user-visible mismatch handling, not another naming scheme.

Bind a registered tool to its canonical configured endpoint or stdio server identity, account scope when available, and exact remote tool identifier. Expose a redacted origin record through a proposed `dsco tools inspect NAME --json` extension. A refresh that changes transport origin must require fresh resolution before invocation; it may not silently reuse the old selection. Resolve redirects through existing credential rules and never forward a bearer credential to a changed origin. Do not claim cryptographic attestation: this is a local configuration and dispatch consistency guarantee.

Test the worktree binary with two local servers advertising identical names and schemas. Discover on A, change configuration to B, and prove a pending selection does not execute on B. Fresh discovery should produce the documented result while exact remote names remain intact. Include endpoint normalization, a cross-origin redirect, duplicate display names, and stdio command changes. Show inspection contains no headers or environment secrets, and demonstrate that origin mismatch reporting still passes through the normal governed call path.

Start with `src/toolmgmt.c`, `src/mcp.c`, `src/mcp_names.c`, `src/tools.c`, `include/tools.h`. Proposed modules: `src/tool_origin.c`, `include/tool_origin.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 32 — External tool concurrency policies

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Make external-tool concurrency honor an explicit local policy for tools that cannot safely overlap. Inspect existing read-only metadata, concurrent-safe classification, Tool Management parallel calls, and server batch paths. The desired feature is consistent execution admission for an individual external tool or integration, not a new worker pool. If serialization already exists, complete enforcement across currently uncovered remote batch paths.

Support a proposed policy with bounded per-integration concurrency and an optional serialization key derived from explicitly configured nonsecret argument fields. Unknown external operations should retain conservative existing defaults. Remote read-only hints may inform scheduling but must never grant capabilities or override local mutation classification. Waiting calls need cancellation and deadline behavior; do not hold the global registry lock while queued or performing I/O. Every member of a remote batch must pass its own gate before dispatch, and a server-level batch endpoint must not hide denied members.

Run the real binary against a controlled integration whose mutation handler records overlapping calls. Show that calls for the same serialized resource never overlap, independent resources can proceed when policy allows, and denied calls never reach the server. Cover timeout while queued, cancellation, malformed keys, and bounded queue capacity. Exercise direct invocation and batch invocation with equivalent policy. Record maximum observed concurrency and prove the policy cannot turn a control-plane or lethal-trifecta denial into a scheduling-only warning.

Start with `src/tools.c`, `include/tools.h`, `src/toolmgmt.c`, `src/capability.c`. Proposed modules: `src/tool_concurrency_policy.c`, `include/tool_concurrency_policy.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 33 — Capability-constrained provider selection

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add explicit request capability constraints to provider selection and fallback. Inspect current router capability tiers, model metadata, explicit provider selection, and route explanation before coding. Preserve existing quality and cost policies. Deliver the missing ability to exclude a candidate for a concrete required feature, rather than assigning every model a new vague score.

A proposed routing constraint structure should express requirements such as tool calls, image input, structured output, context capacity, and local-only transport. Resolve capabilities from existing registry metadata or bounded validated overrides, retaining unknown as unknown. Hard requirements must not be relaxed during retries or fallback; if no candidate qualifies, return a clear unsatisfied-constraint result before transport. Distinguish model family from actual provider, endpoint, credential lane, and supported transport behavior. Extend the existing route-explanation surface with rejected-candidate reasons instead of creating a parallel diagnostic command.

Verify the worktree binary with controlled provider endpoints offering different capability sets. Submit requests requiring tools and structured output, force the preferred endpoint to fail, and prove fallback stays inside the constraint set. Include an unknown capability, explicit provider pin, local-only requirement, and no eligible candidate. Use endpoint request logs to prove excluded providers were never called. Add a regression showing unconstrained requests keep their prior routing behavior, and report the actual chosen provider identity rather than only the model alias.

Start with `src/router.c`, `include/router.h`, `src/provider.c`, `src/provider_profiles.c`, `src/llm.c`. Proposed modules: `src/route_constraints.c`, `include/route_constraints.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 34 — Billing-lane-preserving fallback policy

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add a request-level policy that prevents automatic fallback from crossing an operator's allowed billing lanes. Inspect actual provider identities, account profiles, subscription exhaustion handling, and fallback implementation. Subscription-backed products and direct paid API providers must retain separate identities even when they expose similar models. If lane restrictions already exist in one path, complete the same invariant across fallback and explicit-model dispatch.

A proposed policy should express allowed provider products and whether marginal-cost API fallback is permitted. Apply it before resolving credentials or starting transport for every candidate. Exhausted or unavailable subscriptions should return an actionable blocked result when paid fallback is disallowed; never reinterpret a similarly named API credential as authorization. Preserve existing user-selected provider pins and configured defaults unless the new policy is explicitly set. Expose allowed and rejected lanes through the existing route explanation, without showing account secrets or claiming subscription inference has no accounting value.

Exercise the actual binary against fake subscription executors and local API endpoints using isolated credential references. Force subscription exhaustion, missing credentials, and transport failure. Prove a disallowed paid endpoint receives zero requests, an explicitly allowed API lane works, and provider identity remains accurate in the response. Include two account profiles and verify one profile's policy does not bleed into the other. Capture the exact invocation and redacted route result; do not launch paid inference or use real account credentials for acceptance.

Start with `src/provider.c`, `src/provider_pool.c`, `src/auth_lanes.c`, `src/router.c`. Proposed modules: `src/billing_lane_policy.c`, `include/billing_lane_policy.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 35 — Safe streamed-provider failover boundary

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Make provider failover aware of whether a response has already produced externally visible content or tool intent. Inspect current retry loops, streaming result structures, tool-call assembly, interrupt handling, and fallback routing. Preserve existing retry policy for failures before output. Deliver a bounded state machine that prevents a broken stream from silently causing duplicate assistant content or repeated tool side effects.

Track transport started, response accepted, visible output committed, and tool dispatch committed as distinct milestones. Automatic fallback may occur only within a documented safe boundary; after visible output, return an explicit interrupted-response result or use an existing supported continuation path without replaying committed tool calls. Do not infer that a missing final usage frame means the request never ran. Partial token fragments and incomplete tool JSON must not become executable arguments. Keep provider identity and cost treatment attached to each attempted request.

Run the worktree binary against controlled streaming endpoints that disconnect before headers, before visible content, mid-text, mid-tool arguments, and after a tool result is accepted. Prove early failures can fail over, later failures do not duplicate text or dispatch the tool twice, and incomplete calls never execute. Include user cancellation and a second healthy turn after failure. Record endpoint attempt counts, emitted content, and tool side-effect counts. Restrict edits to small hooks around the existing stream lifecycle; do not rewrite the agent loop.

Start with `src/provider.c`, `src/llm.c`, `src/provider_pool.c`, `src/agent.c`. Proposed modules: `src/provider_failover_state.c`, `include/provider_failover_state.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 36 — Explicit local-provider capability probes

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add an explicit, bounded capability probe for configured local inference endpoints so routing need not guess from a model name. Inspect local endpoint detection, model discovery, provider profiles, and existing connection checks. Reuse those components. The feature should distinguish advertised support from behavior verified by a small probe, while leaving ordinary startup free of unsolicited inference.

A proposed `dsco providers probe NAME --local --json` command should first verify the resolved endpoint is local under existing policy, then run a tiny operator-requested matrix for streaming, tool-call formatting, structured JSON, and usage reporting. Each check must have strict token, response-size, and time bounds. Store endpoint identity, model revision when available, probe version, timestamp, and individual outcomes in an isolated cache. Unknown and unsupported are distinct. Invalidate results when endpoint or model identity changes; do not use a successful probe to bypass capabilities, authentication, or billing restrictions.

Test the real binary against local fixtures that correctly implement one feature, falsely advertise another, omit usage, and hang. Verify bounded completion, accurate per-feature status, cache invalidation, and zero probe requests during normal help or startup. Demonstrate that routing can consume a fresh verified result without treating stale evidence as current. Include malformed model metadata and a configured nonlocal URL, which must be rejected before a probe is sent. Use only controlled endpoints for development acceptance.

Start with `src/local_llm.c`, `include/local_llm.h`, `src/provider.c`, `src/provider_profiles.c`. Proposed modules: `src/local_provider_probe.c`, `include/local_provider_probe.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 37 — Account-scoped quota reset handling

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Normalize provider quota exhaustion and reset timing so one exhausted account does not disable unrelated accounts or become eligible too early. Inspect existing `exhausted_until` persistence, retry headers, provider identities, and account-profile plumbing. Keep the provider pool and circuit breaker; this feature supplies a precise quota state model where current paths rely on ambiguous timestamps.

Represent quota scope, observed time, reset source, confidence, and unknown reset separately from transient transport failure. Accept only documented provider reset fields or validated adapter output. Convert waiting behavior to monotonic deadlines while retaining wall-clock timestamps for display. Scope persisted quota state by provider product and account principal, never raw credentials. A proposed extension to existing provider status should explain whether availability comes from an authoritative reset, a conservative backoff, or a fresh successful request. Clock jumps and malformed resets must not trigger retry storms.

Exercise the actual binary with two isolated account profiles and a local provider fixture returning epoch, duration, absent, malformed, and past reset values. Exhaust one profile and prove the other remains selectable. Advance a controllable test clock to demonstrate correct re-entry without real waiting, then make a successful request to clear the appropriate state. Test restart persistence, corrupt state, and authentication failure separately from quota exhaustion. Capture real route decisions and endpoint counts, preserving subscription versus direct API identity throughout.

Start with `src/provider_pool.c`, `include/provider_pool.h`, `src/provider.c`, `src/llm.c`, `src/auth_lanes.c`. Proposed modules: `src/provider_quota.c`, `include/provider_quota.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 38 — Conservative inference cost admission

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Add a conservative preflight cost bound for requests operating under an explicit monetary cap. Inspect current session and daily budget arithmetic, pricing lookup, token estimation, and router downgrades. Keep existing accounting authoritative. Deliver the missing distinction between a usable upper estimate and unknown pricing, rather than treating unknown cost as free or building a separate billing system.

Compute admission from estimated input, configured maximum output, applicable cache assumptions, and the actual provider billing lane. Clearly label estimates and reported cost. A proposed strict-budget policy should reject or require a caller-supplied conservative bound when pricing is unknown; ordinary unlimited behavior should retain compatibility. If output limits can be reduced without violating the request contract, return the allowed limit explicitly before dispatch. Account for retry attempts under the same cap and never silently choose a different paid lane to fit an estimate. Expose the decision through existing budget or route explanation surfaces.

Verify the real binary against a local endpoint with a fixed test pricing table. Cover a request just within budget, one exceeding the bound, unknown pricing, cached input assumptions, and zero reported cost. Prove rejected requests produce zero HTTP calls and accepted requests use the admitted output limit. Add a retry case showing its projected spend remains covered. Reconcile reported usage after completion and explain unavoidable estimate uncertainty without claiming the client can guarantee an external provider's final invoice.

Start with `include/cost_budget.h`, `src/inference_cost.c`, `src/cost_model.c`, `src/model_pricing.c`, `src/router.c`. Proposed modules: `src/inference_admission.c`, `include/inference_admission.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 39 — Process-local concurrent spend reservations

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Prevent concurrent inference requests inside one dsco process from each spending the same remaining budget. Inspect existing budget preflight and inference-cost recording, plus any reservation code already present. This feature is a process-local atomic admission primitive and user-visible reservation accounting; do not create a distributed ledger, swarm scheduler, or cross-process lock service.

Reserve a conservative estimated amount before each eligible request begins transport. Admission must consider settled spend plus outstanding reservations. Settle exactly once from authoritative reported usage when available, release unused reservation, and retain an explicit uncertain amount when a request may have incurred cost but returned no usage. Cancellation before transport can release immediately; cancellation after submission must follow the uncertainty rule. A proposed budget-status extension should separate spent, reserved, uncertain, and remaining values. Use stable request IDs and bounded storage, and ensure duplicate completion callbacks cannot credit budget twice.

Exercise the worktree binary with two concurrent controlled requests whose estimates individually fit but jointly exceed a small cap. Prove only the admissible request reaches the endpoint and that released capacity becomes available after settlement. Include early cancellation, disconnect after submission, duplicate settlement, and a lower-than-estimated reported cost. Use a deterministic barrier in the fixture to force the race rather than relying on timing luck. Confirm single-request behavior remains unchanged and document that the guarantee applies only to one process's budget authority.

Start with `include/cost_budget.h`, `src/inference_cost.c`, `src/agent.c`, `src/router.c`. Proposed modules: `src/cost_reservation.c`, `include/cost_reservation.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 40 — Versioned pricing evidence for decisions

Work in `/Users/arthurcolle/Dsco/dsco-cli` or your explicitly assigned isolated worktree. Attach explicit pricing evidence to cost estimates and routing decisions so a background refresh cannot silently change the basis of an in-flight request. Inspect the existing cached vendor pricing, model catalog refresh, provider-specific lookup, and inference-cost measurement. Keep their calculations and refresh mechanisms; add a coherent snapshot identity and freshness policy where those are missing.

Publish validated pricing generations atomically with source identity, retrieval time, currency, units, and schema version. A request should retain the generation used for admission until settlement. Preserve reported provider charges as reported even when they differ from the estimate; do not recompute historical estimates using today's prices. A proposed `dsco pricing inspect PROVIDER MODEL --json` interface should expose current values, age, and whether they came from live, cached, or built-in data. Unknown values must remain unknown, and stale-price handling should be an explicit policy under strict budgets.

Use the actual binary with controlled pricing fixtures that change between admission and completion. Prove the original estimate retains its generation while a later request uses the new one. Test malformed prices, negative values, currency mismatch, truncated cache writes, and offline restart. Confirm a failed refresh preserves the prior valid snapshot and does not delay terminal input. Compare reported usage settlement against the preserved estimate, showing both sources in machine-readable evidence without claiming estimated totals are the provider's final bill.

Start with `src/model_pricing.c`, `include/model_pricing.h`, `src/inference_cost.c`, `src/cost_frontier.c`, `src/model_catalog_refresh.c`. Proposed modules: `src/pricing_snapshot.c`, `include/pricing_snapshot.h`.

Read applicable AGENTS.md instructions and inspect dirty state first. Preserve unrelated work. Put substantial new logic in a new C module and header with a Makefile entry; use small hooks in existing files. Route every tool-call path through `tools_execute_for_tier()`. Preserve capability grants, explicit opt-outs, control denials, and lethal-trifecta enforcement. Use isolated state, build, and fixture paths. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install binaries from a worker. Finish with changed paths, exact checks, binary evidence, limitations, and any backend-dependent behavior clearly identified.

---

# 41 — Durable retry schedules and poison-task inspection

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/ipc.c`, `src/durable_agents.c`, `include/ipc.h`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Durable tasks already have ownership generations, but repeated failures need an explicit, inspectable retry schedule. Add an opt-in policy with attempt ceiling, bounded exponential delay, deterministic jitter seed, next eligible time, and terminal exhausted reason. Preserve directed-task routing and existing stale-completion rejection. Store attempt history separately from the current task row; restarting the CLI must retain delays and consumed attempts. Proposed `agents retries` inspection should explain why an item is waiting or exhausted. Requeue requires a deliberate existing-authority operation and creates a new fenced attempt without erasing previous evidence. Do not infer that an uncertain external effect is safe to retry: classify that case as reconciliation required.

Implement new capability in proposed `src/task_retry.c` and `include/task_retry.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Use two competing local processes against a temporary database, scripted failures, and an injectable test clock. Prove one claim per generation, no claim before eligibility, bounded attempts across restart, and stale completion rejection after requeue. Check malformed policies and clock rollback without long sleeps.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 42 — Event-driven scheduling of accepted blackboard dependencies

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/blackboard.c`, `src/durable_agents.c`, `src/ipc.c`, `src/plan_dag.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

The blackboard already derives readiness from accepted dependencies; it does not automatically activate workers. Implement a bounded, opt-in local scheduler that consumes its durable event cursor and dispatches newly ready contracts to configured durable identities. Keep blackboard acceptance authoritative and IPC responsible for activation routing. Persist the task-to-activation mapping and reconcile it after restart before creating another activation. A worker must acquire a fresh blackboard claim before doing work, and dispatch itself must not imply ownership or acceptance. Proposed `agents schedule-board` options should name the board, roster, maximum concurrent activations, idle exit, and state directory. Quiescence must wait outside inference, with bounded event batching and useful blocked-dependency diagnostics.

Implement new capability in proposed `src/board_scheduler.c` and `include/board_scheduler.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Run a diamond dependency graph using deterministic worker subprocesses. Verify parallel independent roots, no downstream launch until accepted inputs exist, bounded concurrency, restart after dispatch before cursor commit, and invalidation while a worker runs. Assert no duplicate accepted publication and no inference during idle observation.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 43 — Actionable execution recovery plans with uncertainty preserved

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/execution_recovery.c`, `src/execution_kernel.c`, `include/execution_recovery.h`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Execution recovery currently projects CRC-validated WAL evidence without replaying tools. Extend that read-only surface with a machine-readable recovery plan grouping attempts into completed, definitely unstarted, failed without confirmed effects, and effect uncertain. Include execution identifiers, last trustworthy event, missing evidence, and a specific recommended next inspection. Proposed `execution recovery-plan` output should have a schema version and stable reason codes. Optional operator annotations may live in a separate sidecar referencing journal hashes; they must never rewrite receipts or promote unknown effects into success. A plan is evidence for deciding what to do, not authority to repeat work. Preserve current recovery exit-code meanings or document a clearly additive command contract.

Implement new capability in proposed `src/recovery_plan.c` and `include/recovery_plan.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Construct real journal fixtures by running bounded local governed operations, then truncate or corrupt copied journals at admission, start, and completion boundaries. Prove every uncertain started operation remains non-retryable by default, clean evidence remains stable, and generating the plan neither mutates journal bytes nor launches tools.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 44 — Durable cancellation requests and deadline completion receipts

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/durable_agents.c`, `src/ipc.c`, `src/swarm.c`, `src/supervisor.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Process cleanup exists, but users need durable answers when cancellation races task completion or worker restart. Add cancellation request records scoped to task generation, with requested, delivered, and terminal outcome states. Proposed `agents cancel-task` supports a reason and a bounded grace interval; optional task deadlines use the same mechanism. The owner observes requests cooperatively, then the supervisor applies existing owned-process termination rules if necessary. Completion and cancellation race through one fenced transition: report which won instead of claiming both. Preserve partial artifact references as incomplete evidence and prevent a cancelled generation from silently restarting. Status should distinguish request acknowledgement from proof that owned processes exited.

Implement new capability in proposed `src/task_cancel.c` and `include/task_cancel.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Use local workers that finish normally, ignore TERM, fork an owned child, and exit during cancellation. Restart the coordinator after recording a request. Prove requests survive, unrelated PIDs are untouched, descendants are reaped within a measured bound, duplicate requests are idempotent, and a stale generation cannot cancel replacement work.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 45 — Recipient-specific durable mailbox acknowledgements

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/ipc.c`, `include/ipc.h`, `src/durable_agents.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Existing IPC messages have a read marker; reading a broadcast must not consume another recipient's delivery. Add an opt-in durable mailbox contract with per-recipient delivery records, sender idempotency keys, explicit acknowledgement after handling, and retryable visibility timeouts. Freeze the broadcast recipient set at publication and document how later agents participate. Proposed `agents mailbox` inspection should expose pending, leased, acknowledged, and exhausted deliveries without marking them read. Preserve compatibility for existing receive callers through a deliberate adapter, and cap recipient expansion and retained payload size. Distinguish at-least-once delivery from exactly-once effects: an acknowledgement cannot make arbitrary downstream actions idempotent.

Implement new capability in proposed `src/mailbox_delivery.c` and `include/mailbox_delivery.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Run three receiver processes and publish directed and broadcast messages through the real CLI. Kill one after delivery but before acknowledgement, restart it, and verify redelivery only to the unfinished recipient. Check duplicate sender keys, acknowledgement by the wrong agent, expired delivery tokens, and legacy receive behavior against a temporary database.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 46 — Content-addressed large artifacts for worker handoffs

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/blackboard.c`, `src/swarm.c`, `include/swarm.h`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Blackboard candidates already preserve immutable UTF-8 payloads up to 32 KiB. Extend handoffs with an optional local content-addressed store for larger binary artifacts while keeping small manifests inside those existing candidates. A manifest records digest, byte length, media type, producing task generation, and relative object reference. Publication streams into a private temporary file, verifies the digest, and atomically seals the object before exposing its reference. Consumers resolve only under the configured store and verify bytes before use; mutable external paths cannot masquerade as pinned artifacts. Proposed `artifacts inspect` should report missing or corrupt objects without materializing executable content. Initial scope is local storage and explicit references, not network distribution or automatic retention deletion.

Implement new capability in proposed `src/artifact_store.c` and `include/artifact_store.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Publish an artifact larger than 32 KiB, consume it from a second worktree, and verify byte identity. Inject truncated writes, duplicate publication, symlink escape, digest mismatch, and missing objects. Existing text-only blackboard contracts and stale-generation checks must continue to work unchanged.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 47 — Fair bounded admission across concurrent swarm groups

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/swarm.c`, `src/swarm_scale.c`, `src/swarm_accounting.c`, `src/swarm_telemetry.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Swarm capacity and cost reservation already exist; multiple groups also need predictable admission when one group floods the queue. Add an opt-in weighted fair admission policy over existing physical worker slots, with per-group concurrency ceilings, a global queued-task cap, and aging to prevent starvation. Preserve existing budget accounting and never reinterpret unknown costs as free capacity. Admission receipts should identify queued versus launched work, the limiting resource, and queue age. Proposed policy configuration is local and immutable for a running scheduling epoch; changing it takes effect at a documented boundary. Cancellation releases reservations exactly once, and group slot reclamation remains separate from task acceptance.

Implement new capability in proposed `src/swarm_admission.c` and `include/swarm_admission.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Drive three deterministic groups with unequal weights, long and short local workers, and a small slot limit. Verify no physical oversubscription, bounded queue memory, progress for the smallest group, correct reservation reconciliation after launch failure and cancellation, and unchanged legacy admission when the policy is disabled. Report observed start order and wait distributions.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 48 — Declared resource reservations for cooperative worker edits

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/blackboard.c`, `src/ipc.c`, `src/swarm.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Isolated worktrees avoid accidental file overwrites, but workers still need visible coordination around shared integration resources. Add advisory reservations for explicit logical keys such as public-header interfaces, generated registries, and integration targets. Reservations reference a blackboard task generation, owner, expiry, and exclusive or shared intent. Proposed `agents reservations` output explains conflicts and renewal history; acquiring a set must be atomic and deterministically ordered to avoid deadlock. Expired generations cannot renew or release another worker's reservation. This is collaboration metadata, never a filename security lock or replacement for capability gating. Do not prevent ordinary authorized source edits; instead make conflicting intent visible before dispatch and include reservation evidence in handoff manifests.

Implement new capability in proposed `src/work_reservations.c` and `include/work_reservations.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Use competing local processes to request overlapping and disjoint key sets. Verify atomic all-or-nothing acquisition, shared-read compatibility, expiry/reclaim fencing, and bounded wait reporting. Demonstrate that two isolated worktrees can proceed on disjoint modules while an overlapping interface reservation is reported accurately and underlying filesystem permissions remain unchanged.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 49 — Evidence-based worker quarantine and controlled re-entry

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/swarm_telemetry.c`, `src/swarm.c`, `src/durable_agents.c`, `src/ipc.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Current health snapshots expose worker progress and costs. Extend them with a local scheduler quarantine state for repeated startup crashes, malformed result envelopes, or independently failed acceptance checks. Keep reason categories separate and never quarantine merely because a heartbeat is temporarily late. A configured threshold and bounded observation window determine admission suspension; active work follows existing cancellation policy rather than being killed implicitly. Proposed `agents health-history` shows supporting events, consecutive counts, cooldown, and the next permitted diagnostic activation. Successful diagnostic re-entry must be recorded, and an operator can clear quarantine through existing authority without deleting history. Quarantine applies to a worker configuration identity, not unrelated agents sharing a model name.

Implement new capability in proposed `src/worker_quarantine.c` and `include/worker_quarantine.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Simulate repeated startup failures, intermittent recovery, malformed results, and a slow healthy worker. Verify the configured identity stops receiving new tasks at the threshold, unrelated workers continue, state survives coordinator restart, cooldown does not launch inference by itself, and one bounded successful diagnostic activation restores eligibility with an auditable transition.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 50 — Verified map-reduce coverage and disagreement reports

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/swarm.c`, `src/orchestrator.c`, `src/blackboard.c`, `src/swarm_scale.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Swarm runs already persist result envelopes and support reduction. Add a reducer input contract that pins accepted artifacts for an explicit expected shard set and records coverage before synthesis. Required shards, optional shards, duplicate identities, failed attempts, and superseded generations must be distinguished. Proposed reduction manifests include exact input hashes, excluded-input reasons, and disagreement groups keyed by a declared claim identifier. A reducer may produce a partial draft when configured, but it cannot label missing required coverage complete. Keep prose synthesis separate from deterministic coverage checking, and preserve the actual configured reducer identity. Build the smallest local path that wraps existing reduction rather than replacing orchestration.

Implement new capability in proposed `src/swarm_reduction.c` and `include/swarm_reduction.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Run deterministic workers producing matching, conflicting, duplicate, failed, and late shard outputs. Verify only current accepted artifacts enter the final input set, conflicting claims remain visible, missing required shards block complete status, and invalidation during reduction rejects stale acceptance. Record raw worker envelopes, the pinned manifest, coverage diagnostics, and final checker receipts.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 51 — Checker-environment attestations for accepted worker evidence

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/blackboard.c`, `src/execution_layer.c`, `scripts/release_manifest_verify.py`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Blackboard already freezes checker command text and accepts current candidates after actual checks. Extend receipts to attest the concrete verification environment: candidate and dependency hashes, repository commit or dirty-diff digest, checker executable digest, declared dependency-lock hashes, platform, exit status, and bounded captured output hashes. The goal is detecting when a passing check belongs to different bytes or an unrecorded environment. Proposed attestation policy may require specified fields; missing data must produce incomplete evidence rather than invented defaults. Exclude credentials and unrelated environment variables. Do not claim hermeticity or independent human review merely because metadata was captured. Existing contracts remain valid unless they explicitly require the new policy.

Implement new capability in proposed `src/check_attestation.c` and `include/check_attestation.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Verify the same candidate in two controlled worktrees, change the checker executable while retaining its path, and mutate a declared lockfile. Confirm attestations detect each difference, an incomplete required field blocks policy acceptance, and concurrent upstream invalidation still defeats a passing stale check. Validate receipts through a separate read-only verifier.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 52 — Offline declarative runtime scenario runner

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `scripts/bench_sota.py`, `tests/test_execution_spine_mcp.py`, `src/main.c`, `src/event_stream.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

The repository has useful individual runtime tests, but users need a portable scenario contract for validating agent behavior consistently. Add a proposed `dsco scenario run` command accepting a versioned local manifest with fixture setup, shell-free argv, bounded stdin, expected exit classes, artifact assertions, and event-sequence predicates. Keep execution deterministic by default and require explicit configuration for any provider-backed case. The existing `src/eval.c` is a mathematical expression evaluator and must not become this harness. Reuse subprocess and event infrastructure through small hooks, with one isolated state directory per case. Emit machine-readable outcomes distinguishing assertion failure, timeout, unsupported environment, and harness error, plus raw evidence references.

Implement new capability in proposed `src/scenario_runner.c` and `include/scenario_runner.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Execute scenarios against the newly built binary for successful local work, rejected capability use, malformed input, a hung child, and incorrect terminal-event order. Verify deadlines include child cleanup, fixture cleanup cannot escape its directory, and a failing assertion yields a failing process status. Re-run a saved manifest and compare stable fields while retaining honest timing differences.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 53 — Crash-boundary fault-injection corpus for durable work

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/execution_kernel.c`, `src/execution_recovery.c`, `src/event_stream.c`, `tests/test_durable_boot_fencing.py`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Durability claims need reproducible crash cases tied to real runtime boundaries. Add a bounded offline fault corpus and narrowly scoped test hooks for admission committed, process launched, output persisted, candidate published, and acceptance committed. Hooks must be inactive in normal builds or require an unmistakable dedicated test setting, and must never depend on timing sleeps to hit a boundary. A proposed `scenarios faults` suite records the chosen injection point, seed, child process status, journal hashes, and recovery classification. Reuse existing fencing and outbox logic; the feature is a replayable diagnostic workload that demonstrates which invariants survived each interruption, not a new execution engine.

Implement new capability in proposed `src/fault_scenarios.c` and `include/fault_scenarios.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

For each supported boundary, run a real local fixture process, inject termination, restart against copied isolated state, and assert no stale completion becomes current, committed events remain replayable, and uncertain effects stay uncertain. Include one corrupted journal and one full storage failure. Report unsupported injection points honestly and retain minimized failing cases as standalone manifests.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 54 — Matched run comparisons with outcome and cost provenance

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `scripts/bench_sota.py`, `scripts/matrix_router_bench.py`, `src/headless_accounting.c`, `src/swarm_accounting.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Existing benchmarks and accounting produce measurements, but an operator needs defensible before-and-after comparisons. Add a proposed `runs compare` command consuming two immutable run manifests with case identities, binary hashes, fixture hashes, permissions, resource bounds, outcome evidence, timings, and cost provenance. Pair cases by declared identity rather than row position; reject or visibly segregate mismatched environments. Report completion quality, acceptance failures, latency distributions, and monetary totals with unknown amounts preserved. Keep subscription usage, provider-reported charges, and estimates separate. Support paired bootstrap intervals using a recorded seed, while refusing confident aggregate conclusions from insufficient paired samples. This feature compares recorded runs and does not automatically purchase new inference.

Implement new capability in proposed `src/run_compare.c` and `include/run_compare.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Feed synthetic manifests with known paired outcomes, missing cases, duplicate IDs, changed fixtures, and unknown costs, then compare two actual local deterministic scenario runs. Verify mismatches cannot silently enter paired statistics, seeded intervals reproduce, medians handle empty inputs correctly, and raw cases remain traceable from every reported aggregate.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 55 — Runtime responsiveness budgets with regression evidence

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `scripts/bench_sota.py`, `src/event_loop.c`, `src/swarm_progress.c`, `src/tool_telemetry.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

The offline benchmark harness already has startup thresholds. Extend responsiveness coverage to event-loop service gaps, queued-work admission, child-output drain lag, and scheduler notification latency under concurrent work. A proposed budget manifest declares metric definitions, measurement windows, percentile targets, sample minimums, and environment metadata. Instrument only required boundaries with monotonic timestamps and bounded counters; avoid synchronous logging on hot paths. Separate provider wait time from local service delay, and mark unavailable measurements unknown. Reports should identify which user-visible operation missed its budget and link to a representative event interval. Threshold checks must not claim a performance improvement without a matched baseline.

Implement new capability in proposed `src/latency_budget.c` and `include/latency_budget.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Run local workers that stream heavily while timers and control requests continue. Inject a deliberate fixture-only service stall and prove the budget catches it; verify the healthy run meets a declared conservative threshold. Measure observer overhead with instrumentation enabled and disabled, check bounded memory under sustained output, and preserve raw samples for independent percentile recomputation.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 56 — Read-only fleet event timeline and precise cursor queries

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/event_stream.c`, `include/event_stream.h`, `src/execution_events.c`, `src/durable_agents.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

The runtime already captures an ordered durable event outbox and supports replay. Add a read-only query projection for operators who need to isolate one task across many workers. Proposed `events query` accepts an explicit database, sequence interval, source/event filters, and bounded page size; `events timeline` groups related task and attempt records while preserving their original sequence numbers. Capture a fixed high-water mark per query session so ongoing writers cannot make pagination skip or duplicate committed events. Surface malformed payloads and capture health separately from ordinary empty results. Do not mutate exporter cursors, prune data, or imply that socket delivery means consumer acknowledgement.

Implement new capability in proposed `src/event_query.c` and `include/event_query.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Run concurrent local event producers while paging a query snapshot. Verify exact membership through the captured high-water mark, no duplicates after cursor resume, stable original order, bounded memory for a large database, and clear behavior for unknown schema versions. Compare queried event hashes with direct read-only database evidence and existing replay output.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 57 — Causal lineage across tasks, attempts, and worker events

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/execution_events.c`, `src/execution_kernel.c`, `src/event_stream.c`, `src/ipc.c`, `src/swarm.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

An ordered event stream establishes observation order, but parallel worker diagnosis also needs explicit causality. Add optional run, task, attempt, parent-attempt, and cause-event identifiers to the relevant existing envelopes, with schema-versioned backward compatibility. Preserve identifiers through durable activation and local subprocess boundaries using narrowly scoped metadata. Proposed `events lineage` builds a bounded causal graph and labels missing parents, cycles, and legacy unlinked records instead of fabricating relationships from timestamps. A retry gets a fresh attempt identity while retaining its predecessor relationship. Keep event sequence and causal parent separate, since concurrent commits can interleave. Scope the first slice to native local workers and their governed execution records.

Implement new capability in proposed `src/causal_lineage.c` and `include/causal_lineage.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Execute a fork-and-join fixture with two workers, one retry, and one cancelled branch. Verify the reconstructed graph links each execution to the correct attempt, contains no fabricated cross-worker edges, survives restart, and handles intentionally omitted legacy metadata. Check identifier size limits and cycle diagnostics using malformed copied events without corrupting the live database.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 58 — Offline fleet incident bundle with cross-worker evidence joins

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `src/swarm_telemetry.c`, `src/event_stream.c`, `src/execution_recovery.c`, `src/blackboard.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Single-run traces exist; a fleet incident needs an offline joined view of tasks, workers, accepted artifacts, and unresolved execution effects. Add a proposed `agents incident-export` command taking explicit state paths and a bounded incident window. Create a self-contained manifest plus readable report that references copied evidence by digest, preserves original task generations and event sequences, and distinguishes snapshots captured at different high-water marks. Include missing sources and capture failures prominently. Export only declared evidence categories, scrub known credential fields, and avoid copying entire home directories or raw provider payloads by default. The report must make worker failure, acceptance failure, and observation gaps separately inspectable.

Implement new capability in proposed `src/fleet_incident.c` and `include/fleet_incident.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Create a local three-worker incident with one crash, one stale publication, and one successful task. Export while another worker continues writing, then inspect the bundle offline with original databases unavailable. Verify every included digest, cross-worker links, bounded export size, explicit snapshot boundaries, and absence of seeded secret canaries. No upload or external notification should occur.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 59 — Release readiness from exact merged-artifact evidence

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `scripts/release_hardened.py`, `scripts/release_manifest_verify.py`, `src/blackboard.c`, `src/execution_layer.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Signed release manifests and blackboard acceptance exist, but a release needs proof about the exact combined artifact. Add a proposed `release readiness` command that consumes a candidate binary, source identity, required feature contracts, integration-check receipts, and supported-platform evidence. Check that worker acceptance references were incorporated into the candidate revision and that required tests ran on that exact binary or an explicitly justified source-equivalent build. Emit ready, incomplete, or failed with concrete missing evidence; a valid signature alone cannot establish correctness. Keep platform compilation, platform runtime checks, and untested targets distinct. The first slice prepares a local review packet and never publishes, installs, tags, or changes release authority.

Implement new capability in proposed `src/release_readiness.c` and `include/release_readiness.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Build a small isolated candidate and supply passing integration receipts, then replace its bytes, omit a required contract, invalidate an upstream artifact, and substitute receipts from another revision. Each case must become non-ready for the precise reason. Verify an intact complete packet succeeds through a separate verifier and retains all supporting hashes and commands.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.

---

# 60 — Deterministic local release staging with reproducibility reports

Work in `/Users/arthurcolle/Dsco/dsco-cli` or the isolated worktree assigned for this feature. Read applicable `AGENTS.md` instructions and inspect current code before editing. Start at `scripts/release_hardened.py`, `scripts/release_manifest_verify.py`, `Makefile`, `src/main.c`. If part already exists, ship the missing bounded extension and explain the observed gap; do not duplicate an existing subsystem. All interfaces named below are proposed until verified on disk.

Release packaging already copies and audits binaries. Extend it with a deterministic local staging contract that takes explicit immutable inputs, normalizes archive ordering and timestamps, records toolchain and build flags, and emits a reproducibility report comparing two isolated builds. Proposed `release stage` creates versioned candidate directories atomically and refuses to overwrite a different candidate under the same identity. Separate inherently variable signatures or platform metadata from the reproducible payload and explain any remaining byte differences. A local staged-channel pointer may be prepared as a review artifact, but promotion, installation, signing with production credentials, and network publication remain separate authorized operations. Keep the source and user-installed binary untouched.

Implement new capability in proposed `src/release_stage.c` and `include/release_stage.h`, with small dispatch hooks and a Makefile entry where needed. Preserve unrelated dirty work. Coordinate shared-file changes; keep builds, databases, sockets, fixtures, and reports isolated. Build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install a worker binary. Every tool-call path must use `tools_execute_for_tier()` and preserve capability gates.

Stage the same fixture inputs twice under different directory names and compare payload hashes. Repeat after changing a source file and after introducing an unstable timestamp to prove differences are detected. Inject packaging failure before the final rename and verify no partial candidate appears complete. Validate archive paths, manifest hashes, and a local help smoke against the packaged binary.

Finish with the implemented behavior, exact reproduction commands, binary and evidence paths, relevant regression results, and remaining limitations. Verify the real local runtime path; compilation alone is insufficient.
