# Native compositor: continuation plan and session handoff

Prepared September 6, 2026. Resume September 7 at 10:30 a.m. America/New_York.

## Start here

The native compositor now has retained, directly manipulable panels inside its
Kitty graphics framebuffer. This is the `--native` path. `--tui` selects the text
interface. The next objective is to turn the working workspace into a dependable
daily environment for sustained agent work, with live buffers, recoverable
layouts, clear workflow state, and measured interaction performance.

Read this file, [the current controls](NATIVE_WINDOWS.md), and the
[verification report](../reports/native-windows-20260906/REPORT.md) before editing.
Do not reconstruct the work from an old roadmap or claim all native issues are
fixed. The concrete failures found during this session were corrected and
tested; the larger usability goal remains open.

### Non-negotiable user constraints

- Never delete, reset, replace, or reinitialize any user or system keychain.
  Normal credential use is allowed. Authentication trouble is not permission
  to reset credentials or keychains.
- Preserve the user's budget settings and unrelated local work.
- Inspect `/Users/arthurcolle/Dsco/dsco-cli` first. The parent `Dsco` directory
  is not the repository. The tree is heavily dirty and other sessions may be
  changing it. Do not reset, clean, stash everything, or attribute the whole
  working diff to this task.
- Every tool execution retains its caller's tier and routes through
  `tools_execute_for_tier()`. Reads of governance status remain inspectable;
  capability denials remain binding. Source edits are normal filesystem writes.
- Use owned test processes and explicitly identified test Kitty surfaces.
  Close those surfaces after validation. Never send input to a window selected
  only by its current position, a broad title match, or an assumed active pane.
- Terminal input can be physical or automated. A workflow button is task
  steering, not verified human identity or new authorization.
- Keep changes modular. Add modules and narrow dispatch hooks instead of growing
  `tools.c`, `main.c`, `agent.c`, or `pixel_tui.c` without bound.
- Continue safe, reversible implementation and verification without routine
  permission questions. Ask only for a material missing constraint or authority.

## What is implemented and verified

| Area | Current behavior |
| --- | --- |
| Retained panels | Up to 12 note, buffer snapshot, and workflow panels; stable session IDs, z order, focus, move/resize, zoom/restore, tile/cascade, hide/show/close. |
| Direct manipulation | Title dragging, cell-aware resize grip, click-on-release controls, independent scrolling, keyboard navigation, terminal-resize retiling. |
| Rendering | Panel-local clipping, opaque workspace background, protected multiline composer, shared UTF-8 word wrapping and scroll bounds, toolbar feedback, retained scene restoration. |
| Workflow steering | Continue, Revise, Retry, Inspect; pending queue and retained receipt pages; delivery at a safe agent boundary; no duplicate delivery in the exclusive-input proof. |
| Draft continuity | Draft bytes, cursor, Unicode, multiline content, partial bracketed paste, and partial paste-end marker survive idle/in-flight reader handoffs. Focused Enter returns to the draft without submitting. |
| Buffers | Attach a bounded snapshot of a persistent buffer, retain UUID/revision/sensitivity, refresh explicitly, close the view without deleting the buffer. Reads retain the execution tier. |
| Tools and prompts | Registered `native_window`; `/windows` commands; refreshed runtime tool inventory with exact tool names, Bash/Python examples and discover/load/invoke guidance. Native sessions get workflow-panel guidance. |
| Validation | Full suite, focused model/adapter/renderer/input checks, sanitizer runs, real Kitty graphics/input fixtures, real subscription-provider interaction, installed-binary capability checks. |

Launch in Kitty:

```sh
dsco --native
```

Use **Ctrl+G** or **`/windows`**. A quick manual creation command is:

```text
/windows open {"kind":"workflow","title":"Continue the native workspace","text":"Make sustained agent work comfortable and recoverable.","status":"ready","next_step":"Check the baseline and choose the first concrete improvement."}
```

## Limits and unresolved questions

These are current boundaries, not all newly discovered defects:

- Buffer panels are read-only snapshots of the first 8,191 UTF-8 bytes. They do
  not yet follow edits automatically or provide an inline editor, text selection,
  search, or a paged large-buffer view. Canonical editing uses existing buffer tools
  and editor views.
- Notes, layouts, panel IDs, pending input, and receipts belong to one native
  process/session. Persistent buffers survive; the workspace itself does not yet
  have a durable restore contract.
- `done` requires nonempty evidence but remains agent-reported. The UI does not
  independently validate evidence or bind every claim to an artifact revision.
- A separate headless MCP process cannot control another compositor's in-memory
  model. It can inspect its own empty state. Any future attachment needs explicit
  process identity, ownership, lifecycle, and gate semantics.
- Native panels currently share the framebuffer/damage-patch renderer. They are
  not separate Kitty terminal processes or independently placed protocol images.
  Actual PTYs, Kitty tabs, and OS windows use `surface`, `pty_session`,
  `buffer_view`, and `kitty_remote`.
- Physical pointer hardware, accessibility/IME breadth, very small screens, long
  sessions, and simultaneous agent/user window edits need a broader matrix.
- Workflow footer actions currently require the mouse. Panel keyboard commands
  handle arrangement/scrolling; Enter returns to the composer. Toolbar feedback
  has no expiry, so an old acknowledgement can suppress informational notices
  indefinitely. Add explicit keyboard action focus and acknowledgement lifetimes.
- The renderer estimates text columns using codepoints and an 8-pixel width;
  it does not yet promise grapheme-aware wrapping. Tiled cells can become smaller
  than fixed controls. CJK, combining marks, emoji sequences and dense layouts
  need targeted reproduction before claiming specific visual defects.
- Two visible real-model probes received additional terminal input whose source
  could not be attributed. Do not call that a proven duplicate-dispatch bug or
  blame a human. A separate exclusive owned PTY proved one Inspect delivery and
  byte-exact draft preservation. Repeat with explicit input provenance if it recurs.
- Performance has instrumentation, but this session did not establish a new
  end-to-end native workspace latency/CPU/memory baseline. Test counts are not
  evidence of smoothness under sustained load.

## First session tomorrow

1. Read `AGENTS.md`, this handoff, and the report. Confirm the current branch,
   dirty status, PATH resolution, and executable hashes. Do not rebuild before
   recording the binary the user is actually running.
2. Launch an owned `--native` session. Try a realistic workflow with two panels,
   a multiline draft, a live agent turn, and one explicit Inspect. Observe the
   human-facing result before selecting the first implementation change.
3. Establish a reproducible performance and interaction baseline using the
   existing fixtures and frame telemetry. Record device/window dimensions,
   terminal cell geometry, scale, binary hash, workload, and input transport.
4. Fix any reproducible blocker found in that pass. Otherwise start with live
   buffer freshness and an obvious Edit/Refresh path; this closes the largest
   gap between a useful view and a daily working surface.
5. Finish one complete vertical slice, including real-runtime proof and docs,
   before expanding to persistence or a new control protocol.

The order below is a default. A reproduced input-loss, crash, ownership, or gate
failure takes priority over a feature or visual polish.

## Ordered implementation plan

### 1. Measure and harden sustained interaction

Purpose: establish whether the workspace stays responsive while tools stream,
panels update, text wraps, and the user types or drags.

- Extend the existing compositor benchmark with a retained-window workload:
  1/4/12 panels, long Unicode bodies, tool-output bursts, drag/resize, tiled
  terminal resize, multiline composer growth, menus, and an idle phase.
- Record input-to-visible-frame latency, queue depth, render/encode/write time,
  patch/full-frame counts, transmitted bytes, CPU, and resident memory. Separate
  model/network waiting from compositor work.
- Inspect per-panel allocation and repaint work. Cache or reuse panel raster
  resources only after measuring the expensive path. Keep snapshot/renderer
  lock ordering intact; never do I/O or execute tools under the model mutex.
- Full paints currently rasterize transcript/rail before the opaque workspace
  covers them and redraw each panel. Measure that confirmed redundant work;
  consider occlusion skipping and revision/size/theme/scale-based raster caches.
- Add bounded repeated resize/suspend/resume/close cycles and a 30-minute soak.
  Check stale placements, lost capture, memory growth, and draft continuity.

Proposed acceptance targets, to calibrate against the baseline: no lost or
duplicated accepted input; no stale pixels after any tested lifecycle change;
input-to-visible-frame p95 below 50 ms and p99 below 100 ms in the declared local
workload; no continuous full redraw at idle; no sustained memory growth after
warmup. These are targets, not achieved measurements.

Pointers: `src/pixel_tui.c`, `src/pixel_tui_perf.c`,
`src/compositor_stream_bench.c`, `src/native_windows.c`,
`tests/test_pixel_native_windows.c`, `tests/test_native_windows_composer.py`.

### 2. Make persistent buffers feel live

Purpose: keep canonical content and its native view understandable and in sync.

- First add an explicit stale/fresh revision indicator and obvious Refresh/Edit
  controls. Edit should open the established owned editor view before inventing
  a second editor implementation.
- Preserve and show the store's truncation/next-offset metadata; the current
  adapter discards it. A partial snapshot must clearly say that more text exists.
- Introduce a bounded refresh notification or polling mechanism for visible
  buffer panels. Do not read buffers every rendered frame. Preserve scroll and
  user focus, coalesce rapid changes, and handle disappearance or closure visibly.
- Reuse the canonical buffer UUID and revision. Route every content read/write
  through the existing public tool gate. Preserve sensitive classification and
  avoid background reads after permission changes.
- Then add selection, copy, find, and paged viewing. For inline editing, reuse
  revision-checked writes, atomic save and existing conflict behavior. Never
  silently overwrite an externally edited buffer.

Acceptance: an external edit becomes visibly stale and can be refreshed without
losing draft/focus; content and displayed revision agree; conflicting saves are
explicit; sensitive reads remain denied when secrets are disabled; closing a
view retains content; large content is paged without invalid UTF-8 boundaries.

Pointers: `src/native_window_tool.c`, `src/buffer_store.c`, `src/buffer_view.c`,
`src/buffer_ui.c`, `src/buffer_cli.c`, `include/native_windows.h`.

### 3. Save and restore workspaces safely

Purpose: resume useful work after normal exit, crash, or a new terminal session.

- Add a versioned workspace snapshot module. Persist layout, titles, buffer
  bindings, visibility, focus, and workflow references using atomic replacement.
- Normalize/clamp geometry on restoration to a different viewport. Define
  ordering when buffers are unavailable or a session is already attached.
- Keep sensitive content out of the layout store; persist references only unless
  an existing authorized secure store explicitly handles the content.
- Separate saved layout from executable pending actions. Never replay a
  Continue/Retry automatically because a window was restored. Receipt history
  and action recovery need explicit durable semantics.
- Make save/load/reset-layout discoverable. Reset-layout must affect only this
  workspace's arrangement and never authentication, buffers, or keychains.

Acceptance: abrupt process termination preserves the last complete snapshot;
restore works at smaller/larger dimensions; malformed or future-version files
fail safely; missing buffers are explained; no queued action executes on restore.

Dependencies: stable session identity and buffer references. Implement before
multi-session attachment or a broad workspace daemon.

### 4. Make outcomes and communication actionable

Purpose: let the user see what happened, inspect proof, and steer the next step
without deciphering transcript internals.

- Keep outcome, current step, evidence, and next action short and up to date.
  Replace stale next-step text once work is complete.
- Bind evidence to artifact paths/revisions, test receipts, or execution IDs
  where available. Show agent-reported completion separately from independently
  checked results. Inspect should read actual evidence.
- Give Revise a small draft-preserving instruction composer, and display queued,
  delivered, running, and finished states for workflow actions.
- Add accessible keyboard activation and useful empty states. Provide clear
  feedback for queue saturation and unavailable operations.
- Give feedback a sequence and bounded lifetime, keeping warnings/errors
  prominent. Do not permanently mask a later informational notice with an old
  acknowledgement. Test all four actions without requiring the mouse.
- Reuse existing durable execution/chronicle structures after inspecting their
  current contracts; do not create a second unconnected job system.

Acceptance: one button press produces one identifiable action; its status is
visible; Inspect checks current proof; Revise preserves the existing draft;
completion updates the next action; no panel action expands task authority.

Pointers: `src/native_windows.c`, `src/agent.c`, `src/tui.c`,
`src/tool_grounding.c`, `src/chronicle.c`, `src/execution_layer.c`.

### 5. Add richer arrangements and controlled attachment

Purpose: organize notes, tasks, editor views, and terminal surfaces into a
coherent workspace without confusing their lifetimes or ownership.

- Add split layouts, drag-to-snap previews, panel tabs, and a window switcher.
  Preserve a deliberate user arrangement when the agent updates content.
- Use semantic operations (split, attach, focus, detach) with stable target IDs.
  Return verified post-operation geometry and ownership in tool receipts.
- Evaluate separate Kitty image placements only if measured framebuffer costs
  justify them. Define image/placement ID allocation, z order, cleanup, clipping,
  fallback, and suspend/resume before changing transport.
- If another process must attach to a native session, build an explicit local
  endpoint with session identity, capability checks, bounded requests, restart
  detection, and stale-handle rejection. Do not bypass the tool execution gate
  by calling model functions directly from an RPC handler.
- Treat PTY/tab/OS-window creation as separate owned resources with explicit
  cleanup and retained-buffer semantics.

Acceptance: re-windowing preserves content, selection, draft, and task state;
stale IDs cannot target a replacement session; a second client cannot acquire
control implicitly; hidden/closed views leave no orphaned placements or processes.

### 6. Keep capabilities discoverable throughout execution

Purpose: prevent the previous false “I have no weather/web tools” response from
returning after compaction, profile switches, or provider serialization.

- Retain the refreshed inventory of actual wire tools plus discoverable names.
  Include exact Bash, Python, discovery, loading, and invocation syntax only when
  the relevant route is actually present.
- Replay the user's original weather request against the literal installed
  binary and selected provider, with the native workspace active. Verify a real
  lookup follows discovery and the reply uses the result.
- Cover normal, compact, cheap/worker profiles, after tool results, and after
  compaction. Test an unavailable route and a denied route distinctly.
- Show discoverability in the UI: searchable tools with capabilities and schema
  retrieval, without flooding the conversation with thousands of full schemas.

Acceptance: the exact repro performs a current lookup or reports the concrete
failure; it does not claim absence from an unsearched catalog. Every invocation
retains normal execution gates. No performance claim without before/after data.

Pointers: `src/tool_grounding.c`, `src/provider.c`, `src/toolmgmt.c`,
`tests/test_tool_grounding.c`, `tests/test_tool_grounding_requests.c`,
`tests/test_codex_compact_tools.py`.

### 7. Finish typography and compact-screen behavior

- Build fixtures at 80x24, 120x36 and 160x48 with 1/6/12 panels, multiple font
  sizes, CJK, combining marks, emoji sequences, long words and explicit blank lines.
- Use the existing text renderer's measured advance or a shared grapheme-aware
  layout API when the fixture proves the current codepoint estimate insufficient.
  The renderer and scrolling model must continue to agree on row boundaries.
- Define a compact layout or overflow control when a tile cannot fit its title
  and workflow buttons. Preserve keyboard reachability for every operation.

Acceptance: no split grapheme clusters, missing final rows, unreachable controls,
or composer overlap in the declared fixture matrix. Treat those outcomes as new
acceptance work, not defects already proven by the current tests.

## Verification and release discipline

Record the initial state before a build:

```sh
cd /Users/arthurcolle/Dsco/dsco-cli
git status --short
git branch --show-current
type -a dsco
shasum -a 256 ./dsco /Users/arthurcolle/.local/bin/dsco
```

Build and focused checks (these are separate commands; stop and diagnose failures):

```sh
make -j6
make test_native_windows test_native_window_tool test_tool_grounding
make test_pixel_native_windows test_pixel_scene_lifetime test_native_windows_composer
make test_tool_grounding_requests
make test-gate-claims
make docs-check
codesign --verify --strict /Users/arthurcolle/.local/bin/dsco
```

`make` currently signs and atomically refreshes the PATH binaries. Verify that
behavior on disk before assuming a future build is installed. If generated docs
drift, inspect the changes, run the appropriate generator or `make docs`, check
again, then rebuild if baked inputs changed.

Run `make test` for a substantial integrated change. Use normal and ASan/UBSan
checks for model/parser/lifecycle changes. The previous TUI sanitizer run
instrumented the TUI and fixture, not every linked library; do not call it a
whole-program sanitizer proof.

Owned Kitty visual fixture, after inspecting its current script:

```sh
python3 tests/test_native_windows_visual.py \
  --binary /Users/arthurcolle/.local/bin/dsco \
  --output reports/native-windows-next/visual
```

For a real provider proof, inspect
`reports/native-windows-20260906/live_dsco_pty.py` first. It creates owned state,
uses the actual subscription provider and native byte stream, and sends a real
model request. Do not silently change the user's budget or credentials to make
a test pass. A synthetic Kitty-capable PTY proves input/agent integration;
combine it with an actual Kitty rendering test for display proof.

Archive binary hashes, commands, tool receipts, final screenshots and cleanup
results together. Label fixture vs real model, physical vs injected input, and
measured vs proposed performance. Do not overwrite prior evidence.

## Suggested work ownership

When parallel work is useful, use non-overlapping ownership:

| Workstream | Owns | Integration boundary |
| --- | --- | --- |
| Native model and persistence | `native_windows` and a new workspace-store module | Snapshot, event and restoration contracts |
| Renderer and interaction | `pixel_tui` plus focused fixtures | Shared layout/wrap helpers; no tool I/O under locks |
| Buffers and tool adapter | `native_window_tool`, bounded buffer integration | Public gate, revisions, sensitivity |
| Root integrator | Small agent/TUI/registry hooks, docs, build/install proof | Final source freeze, runtime validation and handoff |

Agree on the shared API first. Freeze source before the final build. Do not let
several agents rebuild/install or operate the same live Kitty window concurrently.

## Ready-to-paste next-session prompt

> Continue the native compositor work in /Users/arthurcolle/Dsco/dsco-cli.
> Read AGENTS.md, docs/NATIVE_COMPOSITOR_NEXT.md, docs/NATIVE_WINDOWS.md, and
> reports/native-windows-20260906/REPORT.md. Preserve the dirty tree and budget
> settings. Never reset or delete keychains. Verify the current installed binary
> and an owned native session before changing code. Start with measured sustained
> interaction and live-buffer freshness; complete one verified vertical slice.
> Keep tools genuinely available and advertise their exact names throughout
> execution. Use concise workflow panels with evidence and a useful next step.

The older `.workspace/harness-parity/00_MASTER.md` is useful architectural
background, but its July status board is not a current inventory of this work.
