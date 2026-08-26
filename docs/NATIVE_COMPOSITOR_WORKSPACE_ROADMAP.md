# Native Compositor: Smoothness, Telemetry, and Multi-Session Workspace

**Status:** implementation roadmap grounded in the 2026-07-17 source tree

## Observed constraints

- `pixel_tui.c` owns one process-global `g_session`; it cannot host multiple live sessions.
- The compositor renders a complete RGB canvas before tile diffing, so semantic damage does not yet bound raster cost.
- Kitty transport is write/flush observed, not display-acknowledged.
- Motion values are time-based and retargetable, but frame scheduling previously drifted after slow frames.
- Telemetry is opt-in and emitted only when a session closes; there is no live operator HUD.
- The current supervised process is one interactive agent loop. Multi-session UX therefore needs a controller plus isolated worker processes, not multiple provider loops sharing one mutable `agent.c` state.

## Product target

One native workspace window hosts many independently governed DSCO sessions. A persistent session rail shows state, model, unread activity, cost, context, queue depth, tool activity, and health. Switching sessions never interrupts workers. The active session owns the central transcript/composer; background sessions continue through IPC and durable journals.

## Architecture

```text
native workspace controller
  ├─ compositor + input router (single terminal/window owner)
  ├─ session registry (stable session_id, lifecycle, unread, metrics)
  ├─ worker 1: dsco headless/session protocol
  ├─ worker 2: dsco headless/session protocol
  └─ worker N: dsco headless/session protocol
          └─ Chronicle + durable run journal remain authoritative
```

Workers never write terminal escape sequences. They publish typed events and accept typed commands over a local Unix socket or the existing IPC substrate. The workspace controller alone owns the alternate screen, Kitty image lifecycle, focus, and composer.

## Delivery sequence

### P0 — Frame integrity

1. Use absolute monotonic frame deadlines and skip missed slots rather than accumulating scheduler drift. **Initial implementation landed.**
2. Record wake lateness, missed deadlines, rendered-frame gaps, and long gaps. **Initial implementation landed.**
3. Add a deterministic scheduler test seam with an injected clock/wait function.
4. Define a frame budget: at 30 Hz, P95 end-to-end frame work under 25 ms and P99 under 33 ms on the current Retina viewport.
5. Replace forced synchronous producer repaints with queued invalidation except startup, suspend/resume, resize, and fatal recovery.

### P1 — Live observability

1. Add a compositor telemetry snapshot API that does not require session shutdown.
2. Publish one bounded Chronicle event per second when enabled, not one event per frame.
3. Add a toggleable HUD with FPS, frame P95/P99, scheduler misses, queue age, patch/full ratio, dirty coverage, upload bytes/s, retained/transient memory, and active motion tracks.
4. Add counters for resize, suspend/resume, patch fallback reason, transport failure, scene overflow, motion-track exhaustion, capture backlog, and stale-frame recovery.
5. Extend `--compositor-stream-bench` with pass/fail thresholds and a machine-readable regression exit code.

### P2 — Bounded work per frame

1. Migrate transcript, tool deck, inspector, and overlays to retained regions.
2. Feed `native_ui_diff()` damage into raster so unchanged regions are copied/reused instead of fully repainted.
3. Keep per-region cached surfaces; composite only dirty regions into the resident backing store.
4. Remove hot-path `malloc/realloc` by sizing canvas, patch, rich-token, and wrapped-line arenas ahead of use.
5. Separate raster, compression, and terminal upload stages with a latest-frame mailbox; never build an unbounded frame queue.

### P3 — Session protocol and registry

1. Create `session_workspace.c/.h` as a new capability module; do not grow `pixel_tui.c` or `main.c` inline.
2. Define versioned messages: session hello/snapshot/event, user submit/cancel, permission decision, focus, resize, and shutdown.
3. Spawn each session as an isolated supervised worker with its own Chronicle session ID, run directory, provider state, capability taint, and budget.
4. Rehydrate registry entries from Chronicle/run journals after controller restart.
5. Apply backpressure: snapshots coalesce, transcript deltas batch, terminal lifecycle events never drop.

### P4 — One-window workspace UX

1. Add retained `session-rail`, `session-tab`, and `workspace-overview` semantic roles.
2. Keyboard contract: `Cmd/Ctrl+N` new session, `Cmd/Ctrl+W` close/archive, `Ctrl+Tab` next, `Ctrl+Shift+Tab` previous, `Cmd/Ctrl+1..9` direct focus, command palette for all session actions.
3. Preserve independent composer draft, cursor, scroll, focus history, modal stack, and unread marker per session.
4. Overview mode shows cards for all sessions with lifecycle, latest operation, elapsed time, cost/context, and attention-required state.
5. Permission prompts identify the originating session and cannot be accepted from an unfocused ambiguous surface.

### P5 — Native window backend

After the retained workspace and worker protocol are stable, add the macOS Metal window as a second backend. Do not couple multi-session semantics to Kitty. Backend parity requires identical session registry, focus, commands, accessibility labels, and snapshots.

## Acceptance gates

| Gate | Requirement |
|---|---|
| Motion | No scheduler drift after an injected 100 ms stall; timeline resumes on the next absolute slot without burst rendering. |
| Frame pacing | P95 frame ≤25 ms, P99 ≤33 ms at default 30 Hz on the calibrated viewport; zero unexplained long gaps in a 10-minute stream. |
| Backpressure | 10,000 producer updates create bounded memory and eventually display the final state. |
| Recovery | Kill/restart controller while three workers run; all sessions reappear with drafts, transcript, state, and authority boundaries intact. |
| Isolation | One worker crash, provider timeout, or permission modal does not freeze or corrupt another session. |
| UX | Create, switch among, and close/archive at least eight sessions without opening another terminal tab. |
| Governance | Every session retains independent capability taint, grants, budget, audit lineage, and destination-aware approvals. |
| Compatibility | `make test_pixel`, ANSI snapshots, and terminal suspend/resume remain green. |

## Non-goals

- Multiple agent loops in threads inside one mutable process.
- Parsing visible transcript text to infer session state.
- A tab strip backed by terminal tabs or Kitty remote control.
- Per-frame Chronicle writes.
- Removing the ANSI fallback before native parity is measured.
