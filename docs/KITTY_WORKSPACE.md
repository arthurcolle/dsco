# TUI swarm workspace

Run `dsco --tui`, including inside Kitty. Swarm workers appear as retained ANSI
cards inside the same terminal, above the composer. No Kitty remote-control
connection, split, or separate window is needed. Finished output remains readable.
Successful cards roll into a six-row summary when no active/attention cards remain;
Ctrl+G opens their history for inspection. In mixed runs, completed cards no longer
consume live pages. Active workers, failures, explicit zoom and manually positioned
cards keep their space. Failures remain visible until explicitly dismissed.

Press **Ctrl+G** to open the swarm area or switch focus between cards and input.
You can also enter `/swarm`. `/swarm hide` hides the area, and `/swarm status`
prints the existing textual summary.

| Control while cards have focus | Action |
| --- | --- |
| Tab or arrows | Select a worker |
| Shift+Arrow | Move the selected card |
| Ctrl+Arrow | Resize the selected card |
| Drag a card title | Move the card |
| Drag its bottom-right corner | Resize the card |
| Mouse wheel or PageUp/PageDown | Read the selected worker's output |
| Z | Zoom the selected card; repeat to return |
| R | Arrange cards again |
| H | Toggle live cards / all retained history (including dismissed cards) |
| X | Dismiss the selected terminal card; never cancels or hides an active worker |
| Esc or Ctrl+G | Return to the composer without losing input |

Cards open automatically when real workers start. The empty view explains that
it is waiting for workers; opening it never launches agents or makes model calls.
Moving or resizing a card does not change its worker process. The normal agent
capability gate still governs work.

Dismissal affects only presentation: status, output tail, cost and durable execution
records are unchanged. Reusing an ID for a new active run clears its dismissed flag.
The dock retains up to 64 worker entries and an 8 KiB tail per entry; at capacity it
reclaims dismissed terminal entries first, then oldest terminal history, never an
active card. If all 64 entries are active, additional cards are omitted and the
header flags the capacity limit. Counts describe retained entries, not a complete
fleet census. Full execution records remain separate from this bounded UI cache.

The optional `config/kitty/dsco.conf` profile provides Copper & Ice colors,
cursor motion, and the Arthur signature. Its Cmd+Shift+Enter shortcut opens
`dsco --tui`. Include that file at the end of `~/.config/kitty/kitty.conf`.
Remove the include and reload Kitty to undo the terminal styling.

`DSCO_THEME=copper-ice` selects the matching wordmark and optional pixel-renderer
palette. `DSCO_KITTY_SIGNATURE` supplies the identity header. External Kitty
companions remain an explicit opt-in via `DSCO_KITTY_AGENT_WINDOWS=1`; the TUI
profile disables them.

## Automatic layout and progress

Untouched cards re-tile when terminal geometry or worker slots change, including
narrow-to-wide resizes. Keyboard moves/resizes and mouse drags mark a card as
manually placed; automatic layout respects that choice. R restores automatic
placement. Odd column/row sizes are distributed without unused edge cells.

The composer posts a seven-second footer notice when work starts, every 15
seconds while workers report active status, and once when all are terminal.
Notices show active/finished/failed counts and a task. “Output received” means
an append was observed; “no new output” does not assert a stall or failure.
Hidden and idle docks stay quiet. Notices use retained footer rows and never
write over the draft. These updates run while the ANSI composer is mounted;
they are not OS notifications or a background monitoring daemon.

Additional PTY checks after building the fixture:

- `DSCO_TUI_TEST_PROGRESS=1 python3 tests/test_tui_swarm_composer.py`
- Add `DSCO_TUI_FIXTURE_STATIC=1` to verify quiet-worker notices.
- `DSCO_TUI_FIXTURE_COMPLETE=1 python3 tests/test_tui_swarm_composer.py`
  verifies successful completion shrinks the dock while preserving the draft.
- Set `DSCO_TUI_TEST_CAPTURE` to a distinct ANSI capture path for parallel runs.

## Rendering and appearance

The harness has three distinct surfaces:

- **Default ANSI TUI:** `src/tui.c` owns the composer and terminal lock;
  `src/tui_swarm_dock.c` retains worker state. Producers update state without
  writing terminal output. The composer refreshes worker changes at up to 10 Hz.
- **Optional pixel TUI:** `src/pixel_tui.c` rasterizes a retained scene and uses
  `src/kitty_graphics.c` for Kitty image uploads and damage patches. The supplied
  Kitty profile does not enable this path.
- **Optional external worker windows:** `src/kitty_agent_windows.c` manages
  explicit opt-in companions. These are not required for the inline cards.

Inline cards now skip unchanged paints entirely and emit only changed rows for
worker updates. The full snapshot renderer remains available for tests and
exports. Terminal clears, resizes, lock-screen recovery and unmounts invalidate
retained output; callers of `tui_swarm_dock_render_retained()` must invalidate
when another surface overwrites its cells. Whole-row updates preserve wide
Unicode glyphs and erase vacated card positions.

Muted borders keep peer cards quiet; ice marks selection and copper marks
keyboard focus. Completed labels are green; failure/error/timeout labels are
red. Narrow terminals use shorter control hints. Composer border animations
reuse color runs rather than resending a color escape for every cell.

Validation: `make test_tui_swarm_dock test_tui_swarm_composer test_tui_snapshot test_tui_theme_snapshot`.
For the quiet-worker regression (typing skips cards, Ctrl+L restores them), run
`DSCO_TUI_FIXTURE_STATIC=1 python3 tests/test_tui_swarm_composer.py` after building
the fixture. Local before/after benchmark evidence and source-only rollback
material: `.workspace/tui-kitty-20260906/`. These are headless/PTY checks, not a
claim of visual inspection in a live Kitty window.

## Cooperative worker progress during coordinator waits

The shared tool-runtime swarm has a recursive ownership lease. Provider streaming
progress callbacks, the bash subprocess wait loop, and the root parallel-tool
completion wait cooperatively drain/reap existing workers under that lease.
The completion wait releases its own mutex before polling. No background polling
thread or compositor-side pipe reader is created. Swarm operations holding the
lease remain their own polling owner; other callers skip busy ticks.

Ticks are throttled to at most one every 50 ms, but this is NOT a 50 ms delivery
guarantee: quiet libcurl callbacks can be about one second apart, and a long-held
lease postpones other callers. Retry sleeps, DNS stalls, unrelated blocking APIs
and legacy waits without a hook are not guaranteed fresh. Shared interior
pointers from `tools_swarm_instance()` must be used within `SWARM_PROGRESS_GUARD`;
independent `swarm_t` objects remain caller-owned, not generally thread-safe.
Agent waits release the lease between bounded polls so separately authorized
kill/status calls need not wait for the full join timeout.
Detach before destruction; forked children skip inherited tick ownership. Existing
poll-time budget enforcement is preserved rather than bypassed. No tool is
dispatched by a progress tick; tool execution still enters the capability gate.

Verify with `make test_swarm_progress` (real child pipes and loopback SSE, no
inference), then `python3 tests/test_swarm_progress_pty.py` for actual worker
output and completion in the live composer with an unchanged draft.
