# Startup, motion, and progressive learning

The interactive ANSI welcome (`dsco --tui`) animates by default.
`DSCO_TUI_ANIM=0` disables the splash animation; reduced-motion settings override
all animation requests. Piped input/output gets a complete static wordmark.
Live prompt/status effects remain opt-in with `DSCO_TUI_ANIM=1`, and native
phase/tool motion uses its existing independent policy. `dsco_banner_render_cells(out, 0)` renders one static frame, including
the graphics-protocol path, without generating a flipbook or sleeping.

Native phase accents ease out over 160 ms. A new phase starts from the current
displayed accent, including an interrupted transition. Repeated identical
model, usage, budget, and clock publications leave the compositor clean.

The self-improvement loop advances on instrumented task observations. Each
consolidation consumes a revision once; each pattern contributes only when its
occurrence count advances. Unchanged failing-tool metrics and unrelated new
work cannot reinforce an old failure pattern. Recommendations retain stable
IDs as their explanations refresh, and repeated acknowledgement is idempotent.
`self_improve` and `self_assess` calls, including redundant-call reports, remain
traceable but contribute no learning evidence.

Inspect this state through:

```json
{"action":"progress"}
```

The `self_improve` tool returns `dsco.self_improve.progress.v1`, with observed and
consolidated revisions, consolidation count, pending evidence, recommendation count,
and strategy weights. Explicit invocation initializes observation for MCP-only
processes without automatically reading private history. Existing interactive
startup retains cross-session prior loading.

History uses `dsco.self_improve.history.v2`. Saving writes and syncs a unique
temporary file before an atomic rename. Loading is bounded, rejects incomplete
JSON and invalid numeric ranges, restores all six weights, and runs once per
session. Historical counters document the saved session; old observations are
not replayed as new evidence. Concurrent processes retain last-writer-wins
history semantics. Independent held-out evaluation is still required to
establish quality; these learned weights are advisory. Skill promotion and
source-code mutation remain separate workflows.

Before restoring default splash animation on 2026-10-05, the 2026-10-03 offline
PTY measured ANSI input readiness at 81.768 ms with the
default static splash, 1684.496 ms with explicit animation, and 85.940 ms with
explicit animation plus reduced motion. This is one local comparison of splash
policies, not a cross-host guarantee. The default transmitted no animation
chunks, while explicit animation transmitted 3679 continuation chunks.

The populated 1920×1080 native frame retained pixel hash `61052abcceaebcf4`.
Before/after raster medians were 5.014/5.575 ms, with p95 5.603/6.056 ms; no raster
speedup is claimed. At zoom 0.65, retained composer patches measured 0.5 ms p95
across both tested viewports. The fixed 240→160 ms phase duration and suppression
of 400 unchanged HUD publications are checked deterministically.

Run the offline regressions:

```sh
make test_self_improve_progress test_self_improve_mcp test_native_motion
make test_native_compositor test_native_zoom test_native_composer_perf test_native_render_perf
make test_tui_splash test_tui_swarm_composer test-gate-claims
```

The learning regression uses ASAN/UBSAN and isolated history. The real MCP test
executes 56 governed tool calls, verifies fresh failure observations, stable
recursive consolidation, and an on-disk checkpoint with zero provider requests.
The startup suite covers graphics detection/opt-out, ANSI fallback, piped input,
reduced motion, explicit animation, and clean quit. The composer fixture covers
Unicode, focus, dragging, zoom, four resizes, draft retention, and mouse cleanup.
Evidence and exact binary/source hashes are in `build/progressive-optimization.json`.
