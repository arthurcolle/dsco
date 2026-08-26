# Native Compositor Hyper-Plan: 24 Hardening Vectors

**Status:** execution-ready plan  
**Date:** 2026-07-17  
**Scope:** DSCO native retained compositor, Kitty transport, input/focus, observability, recovery, and multi-session workspace foundations  
**Companion documents:** `docs/NATIVE_AGENT_COMPOSITOR.md`, `docs/NATIVE_COMPOSITOR_WORKSPACE_ROADMAP.md`, `.workspace/design/TUI_IMPROVEMENT_PLAN.md`  
**Governing constraint:** harden incrementally; do not rewrite `src/pixel_tui.c` or move multiple mutable agent loops into one process.

## Mission

Turn the current native compositor from a visually capable but brittle single-session framebuffer into a deterministic, bounded, recoverable, observable, backend-neutral operator surface. Hardening is complete only when correctness survives timing stalls, resize storms, malformed input, transport failures, long streams, worker crashes, and controller restart—not merely when a happy-path animation looks smooth.

## Baseline observed on disk

- `src/pixel_tui.c` still owns process-global `g_session` and the terminal lifecycle.
- Absolute monotonic animation deadlines and scheduler telemetry have an initial implementation.
- Retained `native_ui` scenes are live for masthead and composer; transcript, inspector, and portions of overlays remain immediate-mode.
- Pixel output still rasterizes a complete RGB canvas before tile comparison, so semantic damage does not yet bound all frame work.
- Kitty upload timing observes local write/flush, not terminal display acknowledgement.
- Telemetry is opt-in and primarily reported at session close; there is no live HUD or bounded Chronicle rollup.
- Targeted native-compositor tests exist, but deterministic clock injection, checked-in visual goldens, fault matrices, soak gates, and multi-session recovery tests remain incomplete.
- The working tree is mixed and dirty; integration must use narrowly scoped commits and must not absorb unrelated changes.

## Hard invariants

1. The controller alone owns the terminal, alternate screen, cursor, input routing, and Kitty image lifecycle.
2. Agent, provider, governance, and durable journal state remain authoritative; pixels never become state.
3. Every frame has bounded CPU, memory, queue depth, and transport work.
4. Latest correct state beats rendering every intermediate update.
5. One failed session, renderer, transport operation, or modal cannot freeze another session.
6. Capability decisions remain session-scoped and originate in governed execution, never in the compositor.
7. Reduced-motion and ANSI fallback remain complete operating modes.
8. Every hardening vector has a deterministic verifier and an explicit rollback boundary.

## Definition of done

| Dimension | Release gate |
|---|---|
| Frame pacing | At 30 Hz on the calibrated Retina viewport: P95 complete frame work ≤25 ms, P99 ≤33 ms; injected 100 ms stall causes no timeline drift or burst catch-up. |
| Correctness | Compact, dense, and expanded visual goldens pass across lifecycle states, DPR modes, overlays, and long-content fixtures. |
| Boundedness | 10,000 producer updates and a 30-minute stream maintain fixed queue bounds and converge to the final state. |
| Recovery | Resize, suspend/resume, Kitty write failure, worker crash, and controller restart recover without stranded terminal state or cross-session corruption. |
| Observability | Live snapshot and HUD expose pacing, queue, damage, transport, memory, fallback, and recovery reasons; Chronicle emission is bounded to ≤1 rollup/s. |
| Isolation | Eight sessions can run, switch, prompt, fail, and recover independently in one controller window. |
| Compatibility | `make`, `make test_pixel`, native compositor tests, ANSI snapshots, and PTY lifecycle smoke tests remain green. |

---

# The 24 hardening vectors

## 1. Deterministic frame-clock seam

**Failure addressed:** scheduler behavior cannot be proven reliably with wall-clock sleeps.

**Implementation**

- Add `compositor_clock.c/.h` with monotonic-now and deadline-wait callbacks.
- Production uses `clock_gettime`/condition waits; tests use a manually advanced virtual clock.
- Move deadline arithmetic out of the animation thread into pure functions.
- Preserve the current absolute-timeline policy: skip expired slots; never accumulate catch-up frames.

**Acceptance**

- Deterministically inject 1 ms, 33 ms, 100 ms, and 1 s stalls.
- After a 100 ms stall, exactly one current frame renders and the next deadline is the next absolute slot.
- No test depends on real-time sleeps.

**Primary files:** new `src/compositor_clock.c`, `include/compositor_clock.h`; small hooks in `src/pixel_tui.c`; `tests/test_compositor_clock.c`.

## 2. Frame-budget governor and quality ladder

**Failure addressed:** raster/encode/upload overruns create cascading animation breaks.

**Implementation**

- Introduce a per-frame budget object with measured remaining time.
- Define reversible degradation: defer decorative animation → reduce transient effects → merge damage → full authoritative frame only when required.
- Never degrade text correctness, cursor, permission prompts, or terminal recovery.
- Record the selected quality level and reason.

**Acceptance**

- Synthetic expensive effects cannot push an unbounded queue.
- Critical surfaces still update under overload.
- Quality returns to normal after a configurable healthy-frame hysteresis window.

## 3. Single-owner render thread and invalidation mailbox

**Failure addressed:** synchronous producer repaint and shared mutation cause lock contention and inconsistent frames.

**Implementation**

- Producers publish typed invalidations; only the compositor thread mutates/render-consumes frame state.
- Replace ordinary forced repaint calls with a fixed-capacity latest-state mailbox.
- Retain synchronous paths only for startup, resize, suspend/resume, fatal recovery, and shutdown.
- Track oldest invalidation age and coalescing reason.

**Acceptance**

- 10,000 updates produce bounded memory and the final visible state.
- ThreadSanitizer/fault tests reveal no concurrent canvas or scene mutation.
- Producer lock-wait P99 remains under the declared threshold.

## 4. Backpressure and latest-state convergence

**Failure addressed:** swarm/tool/token storms can starve final state or exhaust memory.

**Implementation**

- Classify events: coalescible snapshots, batchable deltas, and non-droppable lifecycle/governance events.
- Use fixed queue limits and explicit overflow policy per class.
- Add sequence IDs and final-state convergence checks.
- Surface queue saturation rather than silently dropping critical state.

**Acceptance**

- Burst test proves bounded resident memory.
- The last token, terminal tool state, permission request, and error state always appear.
- Overflow counters identify exactly what was coalesced, deferred, or rejected.

## 5. Semantic damage drives bounded raster

**Failure addressed:** retained diff exists, but full-canvas repaint keeps CPU cost proportional to viewport size.

**Implementation**

- Feed `native_ui_diff()` regions into the raster stage.
- Clear/repaint only dirty semantic regions against a resident backing store.
- Expand damage for shadows, glyph overhang, animation bounds, and clipping ancestry.
- Fall back to full repaint on excessive fragmentation or uncertain coverage, recording the reason.

**Acceptance**

- A one-meter update touches only its bounded raster region.
- Pixel parity with a full render is exact for every fixture.
- Dirty coverage and raster time materially decline versus baseline.

## 6. Per-region surface cache

**Failure addressed:** unchanged transcript, inspector, and chrome are repeatedly shaped and rasterized.

**Implementation**

- Cache retained region surfaces by stable key, geometry, theme generation, content generation, and DPR.
- Invalidate explicitly on font/theme/DPR/backend changes.
- Bound cache memory with deterministic ownership; no opportunistic global cache.

**Acceptance**

- Repainting an animated status indicator does not reshape unchanged transcript text.
- Cache hit/miss/eviction telemetry is available.
- Cache memory remains under a viewport-derived cap.

## 7. Hot-path allocation elimination

**Failure addressed:** `malloc/realloc` spikes can interrupt animation and fail unpredictably under long streams.

**Implementation**

- Inventory every allocation reachable from frame begin to terminal flush.
- Pre-size canvas, patch, wrapping, rich-token, base64, compression, and damage arenas.
- Permit growth only at controlled safe points with explicit maximums.
- Add allocation counters in test builds.

**Acceptance**

- Steady-state frames perform zero heap allocations.
- Long Markdown/code/math fixtures do not truncate or exceed declared caps silently.
- Controlled growth failure produces a recoverable diagnostic, not corruption.

## 8. Raster/encode/upload pipeline isolation

**Failure addressed:** terminal compression and write latency block scene production and animation cadence.

**Implementation**

- Separate scene/raster, encode, and upload stages with at most one pending latest frame between stages.
- Use immutable frame descriptors and generation IDs.
- Cancel superseded non-authoritative frames before expensive encode/upload.
- Keep resize and recovery frames authoritative and non-droppable.

**Acceptance**

- Artificial 200 ms upload latency does not grow a frame queue.
- Display converges to the newest generation.
- No stale-size frame uploads after resize.

## 9. Kitty transport state machine

**Failure addressed:** partial writes, chunk errors, image-ID confusion, and interrupted uploads can strand visual state.

**Implementation**

- Model upload/place/update/delete as an explicit state machine.
- Handle short writes, `EINTR`, `EPIPE`, timeout, and partial chunk sequences.
- Assign monotonic frame/image generations and reject stale operations.
- Make cleanup idempotent.

**Acceptance**

- Memstream and fault-injection tests cover every transition and failure edge.
- A failed patch triggers one bounded authoritative recovery frame.
- Shutdown always attempts cursor/alternate-screen restoration independently of image cleanup success.

## 10. Transport capability negotiation and degradation

**Failure addressed:** terminal capability can be absent, misreported, or change mid-session.

**Implementation**

- Centralize capability probing with a cached generation and explicit reprobe triggers.
- Detect unsupported Kitty operations and downgrade patch → full image → ANSI.
- Separate terminal identity from assumed protocol support.
- Publish fallback reason and current backend.

**Acceptance**

- Simulated capability loss transitions to ANSI without stranded alternate screen.
- Re-entry to pixel mode requires an explicit safe reprobe boundary.
- No user content or draft is lost during backend transition.

## 11. Resize, DPR, and monitor-transition transaction

**Failure addressed:** resize storms and display movement can mix old geometry with new backing dimensions.

**Implementation**

- Treat geometry change as a generation transaction: freeze old uploads, compute logical/physical geometry, allocate, relayout, full-render, atomically publish.
- Debounce resize storms while preserving the final geometry.
- Test integer DPR boundaries now; isolate fractional DPR support behind the geometry API.
- Reject stale frame generations after geometry change.

**Acceptance**

- 1,000 resize events converge to the final dimensions with bounded allocations.
- Same rows/columns with changed pixel dimensions/DPR forces correct relayout.
- No out-of-bounds draw, stretched frame, stale patch, or cursor displacement.

## 12. Suspend/resume and terminal lease

**Failure addressed:** pager, secure input, raw-stdin tools, signals, or shell suspension can leave the terminal corrupted.

**Implementation**

- Introduce a terminal-lease state machine: acquire, active, suspending, handed-off, resuming, recovering, released.
- Make alternate-screen, cursor, mouse, bracketed-paste, and Kitty cleanup idempotent.
- Handle `SIGTSTP`, `SIGCONT`, `SIGINT`, `SIGTERM`, and abnormal child return.
- Serialize handoff so compositor and specialty renderer never write concurrently.

**Acceptance**

- PTY tests interrupt every lease transition.
- Terminal modes match the pre-session snapshot after normal exit and injected failure.
- Resume forces an authoritative frame and preserves transcript/composer state.

## 13. Retained transcript migration

**Failure addressed:** the dominant, high-churn surface remains partly immediate-mode and limits semantic damage.

**Implementation**

- Add `native_transcript.c/.h`; do not expand `pixel_tui.c` inline.
- Give messages, tool calls, artifacts, code blocks, and stream tails stable keys.
- Cache shaping/wrapping by width, font generation, and content generation.
- Preserve bounded scrollback and anchored scroll position during streaming.

**Acceptance**

- Appending one token damages only the active tail and any necessary reflow region.
- Historical rows are not reshaped on unrelated status animation.
- Resize preserves semantic scroll anchor and selected artifact.

## 14. Retained overlays and modal isolation

**Failure addressed:** popovers/modals can disturb underlying layout, focus, or repaint correctness.

**Implementation**

- Normalize all overlays into one retained z-stack with stable identity.
- Snapshot and restore focus per layer.
- Damage old and new overlay bounds, including scrim.
- Require permission prompts to bind visibly to session ID, capability, destination, and decision scope.

**Acceptance**

- Opening/closing nested overlays leaves transcript, draft, scroll, and focus intact.
- Permission cannot be accepted from an ambiguous or background session.
- Reduced-motion mode retains complete state transitions.

## 15. Input, paste, and Unicode hardening

**Failure addressed:** malformed UTF-8, large paste, escape ambiguity, and grapheme boundaries can corrupt editor/render state.

**Implementation**

- Fuzz the shared editor/compositor boundary with invalid UTF-8, combining marks, emoji sequences, wide glyphs, bidi controls, and partial escape sequences.
- Bound bracketed-paste memory and show truncation/rejection explicitly.
- Keep cursor and selection in grapheme-safe positions.
- Separate backend key decoding from semantic commands.

**Acceptance**

- Fuzz corpus produces no crash, OOB access, stuck paste mode, or invalid cursor.
- Composer text round-trips exactly for accepted input.
- A paste storm cannot starve terminal recovery or permission input.

## 16. Scene identity and invariant checker

**Failure addressed:** duplicate keys, invalid parentage, clipping errors, and stale references can create nondeterministic damage.

**Implementation**

- Add debug validation for unique stable keys, acyclic trees, valid bounds, finite geometry, z-order, focusability, clip nesting, and capacity limits.
- Emit compact failure artifacts with scene generation and offending key path.
- Compile out or sample checks in release mode.

**Acceptance**

- Property tests generate valid/invalid scenes and detect every invariant class.
- Invalid scene data fails closed to a diagnostic frame rather than memory corruption.

## 17. Visual golden and differential renderer suite

**Failure addressed:** structural tests miss clipping, color, typography, spacing, and stale-pixel regressions.

**Implementation**

- Check in deterministic goldens for compact/dense/expanded layouts, four lifecycle states, overlays, long content, DPR modes, and reduced motion.
- Compare semantic scene snapshots and raster hashes.
- Render the same fixture through incremental damage and full repaint, then pixel-diff.
- Provide an explicit golden regeneration command with manifest and reviewable diff montage.

**Acceptance**

- CI fails on unexplained pixel or semantic changes.
- Incremental and full renders are byte-identical for deterministic fixtures.
- Golden updates identify changed fixtures and hashes; no silent bulk replacement.

## 18. Fault-injection matrix

**Failure addressed:** recovery paths remain untested until real failures occur.

**Implementation**

- Add deterministic injection points for allocation failure, clock stall, mutex delay, queue overflow, resize during upload, compressor failure, short write, flush failure, worker disconnect, malformed event, and shutdown mid-frame.
- Execute pairwise combinations for high-risk boundaries.
- Persist the seed and exact injected edge for reproduction.

**Acceptance**

- Every fault maps to continue, degrade, retry-once, authoritative-reset, or clean-exit policy.
- No fault causes deadlock, unbounded retry, terminal stranding, or cross-session mutation.

## 19. Live telemetry snapshot, HUD, and bounded Chronicle rollups

**Failure addressed:** end-of-session JSON cannot diagnose live jank or hangs.

**Implementation**

- Extend `pixel_tui_perf_snapshot()` with queue depth/age, damage coverage, fallback reasons, resize/recovery counts, cache stats, active motion tracks, and backend state.
- Add a toggleable retained HUD that reads snapshots without driving compositor state.
- Emit at most one Chronicle rollup per second when enabled; never emit per-frame events.
- Ensure disabled telemetry avoids clocks/locks on hot paths where currently promised.

**Acceptance**

- A live stall can be attributed to scheduler, raster, encode, upload, queueing, or recovery.
- Chronicle volume remains bounded during a 30-minute run.
- HUD itself consumes a measured, bounded frame budget and can be disabled instantly.

## 20. Performance regression harness and soak gate

**Failure addressed:** local one-off measurements do not prevent gradual regressions.

**Implementation**

- Extend `--compositor-stream-bench` with viewport/content profiles, threshold file, warmup, percentiles, and machine-readable exit code.
- Add 10-minute cadence and 30-minute memory soaks.
- Record environment calibration: terminal, dimensions, DPR, CPU, build hash, and feature flags.
- Separate deterministic headless raster numbers from terminal-dependent transport numbers.

**Acceptance**

- CI/local gate rejects P95/P99, memory slope, queue age, or long-gap regression beyond declared tolerance.
- Reports are directly comparable only when calibration fields match.
- Soak ends with zero leaked terminal resources and bounded RSS slope.

## 21. Crash-safe compositor recovery

**Failure addressed:** compositor/controller failure can destroy UX continuity or leave workers inaccessible.

**Implementation**

- Persist minimal UI projection state: active session ID, per-session draft, cursor, scroll anchor, focus history, overlay-safe state, and last rendered generation.
- On restart, rebuild from Chronicle/run journals; never trust framebuffer contents.
- Use a crash marker to force terminal cleanup and authoritative redraw.
- Make recovery versioned and tolerant of older state.

**Acceptance**

- Kill -9 controller during stream; restart restores sessions, drafts, transcript projection, and authority boundaries.
- Workers continue independently where the supervisor permits.
- Corrupt UI state is discarded safely and reconstructed from authoritative sources.

## 22. Session workspace protocol and process isolation

**Failure addressed:** process-global `g_session` cannot safely host multiple governed sessions.

**Implementation**

- Add `session_workspace.c/.h` as a bounded controller module.
- Define versioned typed messages: hello, snapshot, event batch, submit, cancel, permission decision, focus, resize, health, and shutdown.
- Spawn each agent loop as an isolated supervised worker with independent Chronicle ID, capability taint, provider state, budget, and run journal.
- Workers never emit terminal control sequences.

**Acceptance**

- Eight sessions run concurrently in one controller without shared mutable provider/governance state.
- Killing or blocking one worker does not interrupt input, rendering, or progress in others.
- Protocol compatibility and malformed-message tests pass.

## 23. Multi-session focus, lifecycle, and permission safety

**Failure addressed:** one-window operation can create ambiguous focus, unread state, cancellation, and approvals.

**Implementation**

- Retain a session rail and per-session composer draft, cursor, scroll, modal stack, unread marker, and attention state.
- Make all commands target an explicit session ID.
- Require focus plus visible destination-aware context for consequential permission decisions.
- Define close, archive, detach, cancel, kill, and recover as distinct lifecycle operations.

**Acceptance**

- Rapid switching cannot send input or approval to the wrong session.
- Background permission prompts attract attention but cannot steal acceptance focus.
- Close/archive does not implicitly kill durable work; kill requires explicit governed action.

## 24. Backend parity, accessibility, and release containment

**Failure addressed:** Kitty-specific assumptions can fossilize product semantics and make rollout unsafe.

**Implementation**

- Keep semantic scene, commands, focus, accessibility labels, and session registry backend-neutral.
- Build a minimal second backend/parity adapter after core hardening—not before—to prove contract independence.
- Add reduced-motion, no-color, keyboard-only, and screen-reader-oriented semantic snapshot tests.
- Ship behind feature flags with shadow metrics, canary cohorts, automatic ANSI fallback, and a tested rollback switch.

**Acceptance**

- Kitty and the parity backend consume the same semantic fixture and expose identical command/focus/accessibility structure.
- Native failure can fall back without restarting the agent session.
- Promotion requires benchmark, fault, soak, recovery, accessibility, and rollback evidence.

---

# Dependency graph and execution waves

```text
Wave 0 — preserve + measure
  1 clock seam ─┬─> 2 frame governor
  17 goldens    ├─> 18 fault matrix
  19 telemetry  └─> 20 regression/soak

Wave 1 — bound the current compositor
  3 render ownership -> 4 backpressure -> 8 staged pipeline
  5 semantic raster -> 6 region cache -> 7 allocation elimination
  9 transport FSM -> 10 degradation -> 12 terminal lease
  11 resize transaction depends on 1 + 9

Wave 2 — complete retained correctness
  13 transcript migration -> 5 semantic raster coverage
  14 overlay isolation
  15 input/Unicode hardening
  16 scene invariant checker

Wave 3 — recovery and workspace
  21 crash recovery depends on 12 + 19
  22 session protocol depends on 3 + 4 + 21
  23 multi-session safety depends on 14 + 22

Wave 4 — promotion
  24 backend parity/release containment depends on 17–23
```

## Parallel work lanes

| Lane | Vectors | Write scope |
|---|---:|---|
| Timing and scheduling | 1, 2 | new clock/governor modules; narrow animation hooks |
| Pipeline and boundedness | 3, 4, 7, 8 | new queue/pipeline modules; no competing writes with timing lane |
| Rendering | 5, 6, 13, 16 | retained-region modules and raster adapter |
| Terminal resilience | 9, 10, 11, 12 | Kitty transport, geometry, terminal lease |
| Interaction | 14, 15 | overlays, semantic input tests |
| Verification | 17, 18, 20 | tests, fixtures, benchmark harness |
| Observability | 19 | `pixel_tui_perf` and Chronicle adapter |
| Workspace | 21, 22, 23 | new recovery/workspace modules |
| Promotion | 24 | parity tests, flags, release runbook |

Parallel writes must remain isolated by module or worktree. Integration is serialized through the acceptance gates below.

# Promotion gates

## Gate A — deterministic baseline

Required vectors: **1, 17, 19**

- Clock-driven scheduler tests pass.
- Existing appearance is captured in reviewed goldens.
- Baseline frame/queue/transport metrics are recorded.

## Gate B — bounded single-session compositor

Required vectors: **2–12, 18, 20**

- Work and memory are bounded under storms and stalls.
- Resize, transport failure, suspend/resume, and terminal degradation recover.
- Performance and soak thresholds pass.

## Gate C — retained semantic completion

Required vectors: **13–16**

- Transcript and overlays are retained and damage-bounded.
- Input/Unicode and scene invariants pass fuzz/property tests.
- Incremental/full differential images are exact.

## Gate D — recoverable multi-session workspace

Required vectors: **21–23**

- Eight isolated workers operate in one window.
- Controller restart rehydrates sessions safely.
- Focus, lifecycle, and permission routing are unambiguous.

## Gate E — contained release

Required vector: **24** plus all preceding gates

- Backend-neutral semantic parity demonstrated.
- Accessibility/reduced-motion contracts pass.
- Canary, fallback, and rollback procedures are tested.

# Commit and rollback discipline

Each vector is promoted as a small series, not one mega-patch:

1. test seam or characterization test;
2. additive module and narrow integration hook;
3. deterministic verifier and fault cases;
4. documentation/telemetry update;
5. benchmark evidence and rollback note.

A commit may cover one vector or one independently testable slice. Never combine unrelated repository cleanup with compositor hardening. Do not use `git add -u` in the mixed working tree. Every promotion record must list changed files, test commands, before/after metrics, known residual risk, and the exact revert boundary.

# Standard verification matrix

Run the applicable subset after every vector and the full matrix at each gate:

```bash
make -j8
make test_pixel
make test_native_compositor
make test_pixel_geometry
make test_kitty_graphics
make test_tui_snapshot
make test_tui_theme_snapshot
git diff --check
```

Additional required suites as implemented:

- deterministic clock tests;
- incremental-vs-full pixel differential tests;
- PTY terminal-lease and signal tests;
- transport short-write/failure tests;
- allocation and queue-bound tests;
- Unicode/input fuzz corpus;
- malformed workspace-protocol tests;
- controller/worker crash-recovery tests;
- 10-minute frame-cadence soak;
- 30-minute bounded-memory soak;
- eight-session isolation and permission-routing scenario.

# Evidence ledger template

For each vector, append a promotion record containing:

| Field | Required evidence |
|---|---|
| Trigger | Observed failure, missing invariant, or baseline limitation |
| Expected delta | Falsifiable behavioral or performance improvement |
| Authority/blast radius | Files, terminal state, process/session boundaries |
| Candidate | Commit/content hash and complete diff |
| Verification | Exact commands, fixture/seed, and results |
| Independent check | Differential test, fault test, reviewer, or separate executor |
| Containment | Feature flag, shadow path, or canary scope |
| Rollback | Exact revert and last-known-good artifact |
| Result | Measured delta and residual risk |

## Immediate critical path

Execute in this order:

1. Preserve the current mixed-tree compositor delta as an isolated patch/artifact without committing unrelated files.
2. Implement vector 1 so the newly landed absolute scheduler can be proven rather than merely observed.
3. Capture vector 17 goldens before further visual changes.
4. Expand vector 19 telemetry with live snapshots and explicit fallback/recovery reasons.
5. Implement vectors 3 and 4 to eliminate synchronous repaint pressure and bound update storms.
6. Harden vectors 9, 11, and 12 before deeper visual migration; terminal correctness outranks polish.
7. Migrate transcript via vector 13, then activate semantic raster bounding in vector 5.
8. Run Gate B/C soak and fault matrices before beginning the multi-session controller.
9. Build vectors 21–23 as new modules with isolated workers; never generalize `g_session` into shared multi-agent threads.
10. Promote only through vector 24's canary, fallback, and rollback gate.
