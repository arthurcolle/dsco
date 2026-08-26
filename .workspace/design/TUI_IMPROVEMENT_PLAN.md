# TUI / Native Compositor — Improvement Plan v1

## EXECUTION STATUS — 2026-08-11 attachment / MCP / latency repair

| Item | Verified state |
|---|---|
| Primary MCP | **Fixed and live-verified**: `localhost:2016` refusal now falls back to `https://tools.distributed.systems/mcp`; slow cold-start discovery gets a 30s budget + one bounded retry; MCP `nextCursor` pagination is followed (64-page guard). Rebuilt binary discovered and registered **2,698 tools**. `DSCO_MCP_DEBUG` is no longer suppressed by the background loader. |
| Clipboard / screencap attach | **Fixed**: clipboard captures no longer reuse `/tmp/dsco_clip_<pid>.png` (which let a second paste clobber the first before submit). Each paste receives a distinct `~/.dsco/attachments/clip_<pid>_<time>_<seq>.png`; rapid two-grab verification produced two surviving files. |
| Composer latency | **First hotspot fixed**: the `@` image picker no longer runs `opendir/readdir` on every repaint/keystroke; directory scans are cached by `(directory,prefix)` token. |
| Verification | `make` links, ad-hoc codesigns, and verifies the binary. MCP live fallback/discovery passed. Full suite: **5,217/5,222 passed**; the same five environment-sensitive provider OAuth/routing/fabric failures reproduce with the MCP patch temporarily reverted, so they are not regressions from this repair. |

**Remaining TUI performance work:** add frame-time telemetry and coalesced dirty-region
repaint for the immediate-mode composer/transcript path; cache slash-menu matching where
measurement shows value; preserve retained-compositor ownership boundaries.

---

## EXECUTION STATUS — 2026-07-15 session 3

| Item | State |
|---|---|
| DPR fix | **Landed & refactored**: logical/physical split lives in `native_ui_terminal_viewport()`; `pixel_tui.c` now rasterizes every logical shape and glyph onto the exact terminal backing dimensions before Kitty placement. Auto-detection conservatively selects 2x at cell ≥14w/24h; `DSCO_PIXEL_TUI_DPR=1..4` remains the explicit override. |
| P1.1 tests | **Passing**: `tests/test_pixel_geometry.c` now covers boundary cells, override handling, logical division, density breakpoints, shell-layout invariants, the pinned incident geometry, and multiline UTF-8 composer wrapping/viewport behavior. Makefile target `test_pixel_geometry` wired; **84/84 assertions pass** (2026-07-15). |
| P0 commits | **Not run**: the current branch contains unrelated local work, so compositor changes remain uncommitted until an explicit commit/publish request. The gated validation script is historical context, not the current execution path. |
| P1.2–P1.4, P2+ | P1.2 partially covered — a dpr flip alone changes logical w/h, which defeats `session_refresh_geometry_locked`'s early-out (verified by reading; needs the P1.2 injection-seam test to prove). P2 now has a `test_pixel` aggregate plus retained-region/backend/full-session artifact coverage (**52/52 assertions**). P3.1 landed as `px_backend`; P3.2 landed for the live masthead and composer with stable keyed scenes and semantic damage. P5.3 parity work landed early: multiline composer layout, command/image popovers, status/notices, permissions, questions, and menus now project into native retained state; pager/lock/raw TTY surfaces use explicit handoff. |
| Threshold retune question | **Resolved**: incident cell 31×58 stays at conservative 2x, and the observed odd `2325×1682` backing is uploaded exactly. Kitty no longer stretches a logical half-resolution frame. |

**To resume:** migrate one owner region at a time through `native_ui` +
`px_backend`; transcript is next. Keep `make test_pixel`, ANSI snapshots, and a
live PTY smoke green at every boundary.

---

**Date:** 2026-07-15 · **Branch:** `agent/governed-runtime-product-readiness`
**Owner:** Arthur Colle · **Executor:** DSCO Runtime
**Companion docs:** `docs/NATIVE_AGENT_COMPOSITOR.md`, `docs/TUI_DESIGN_LANGUAGE.md`,
`.workspace/design/COMPOSITOR_DESIGN_LANGUAGE.md`

## 0. Observed baseline (verified this session)

| Fact | Evidence |
|---|---|
| Clean `-Wall -Wextra` build; `dsco` + `spine-dsco-slim` link | make -j8, 2026-07-14 |
| `test_tui_snapshot` 24/24 · `test_tui_theme_snapshot` 70/70 | run 2026-07-14 |
| DPR/Retina fix landed in `pixel_tui.c` (3 sites, `render_device_scale()`) | this session |
| Compositor work remains in an intentionally dirty mixed tree; unrelated files must stay out of future commits | `git status --short` |
| Live masthead and composer now use retained `native_ui` scene/diff/render through `px_backend`; transcript, inspector, and live overlays remain immediate-mode | `native_masthead.c`, `native_composer.c`, `px_backend.c`, `draw_session_masthead()`, `draw_session_composer()` |
| Dedicated compositor tests cover geometry, transport, retained masthead/backend damage, session PPMs, and plan artifacts | `make test_pixel` |
| ANSI snapshots remain independent; native session artifacts cover compact/dense/expanded geometry plus all four lifecycle states | `tests/test_native_compositor.c` |
| Framebuffer tile diff + Kitty frame edits avoid full uploads for bounded damage; semantic damage now exists for masthead and composer | `session_collect_damage()`, `native_ui_diff()` |

## Phase 0 — Durability (BLOCKING, ~10 min)

Nothing else is safe until the subsystem is committed.

- **0.1** Run commits 1–3 of `scripts/commit_compositor_wip.sh` (compositor
  subsystem incl. DPR fix, Makefile wiring, untracked runtime modules).
  Commit 4 (`git add -u`, 63 tracked files) held for Arthur's review.
- **Accept:** `git status` shows no untracked compositor sources; build+tests green post-commit.

## Phase 1 — Correctness & display robustness (highest user-visible value)

- **1.1 DPR fix hardening.** Unit-test `render_device_scale()` boundaries
  (16/17/31/32 cell_w; 33/34/63/64 cell_h; env override 0/1/4/5/garbage;
  zero-geometry fallback). Table-driven, no tty needed.
- **1.2 Monitor-hotplug re-detection.** `session_refresh_geometry_locked`
  already resamples per tick; verify a dpr change alone (same cols/rows,
  changed xpixel/ypixel) triggers repaint — the current early-out compares
  logical w/h which changes with dpr, so it should, but prove it with a test
  seam that injects geometry.
- **1.3 Fractional/asymmetric scaling.** Current dpr is integer; Kitty on a
  scaled display can produce ~1.5x. Decide: keep integer (accept mild
  shrink/grow) or move `session_render_geometry` to float dpr. Default:
  integer now, note float as follow-up; do not over-engineer.
- **1.4 Terminal capability degradation.** Verify behavior when
  `pixel_tui_available()` flips mid-session (ssh, screen, TERM change):
  session must fall back to ANSI TUI without stranding the alt screen
  (`?1049l`, cursor restore).
- **Accept:** new `tests/test_pixel_geometry.c` green; manual monitor
  unplug/replug shows correct size both directions; no alt-screen strand.

## Phase 2 — Test infrastructure for the pixel path

The subsystem now has direct geometry, retained-scene, transport, plan, and
session-artifact tests. Checked-in visual goldens and resize fault injection
remain from the design-language acceptance criteria (§6).

- **2.1 Canvas snapshot harness (headless seam landed; checked-in goldens remain).** `DSCO_PIXEL_TUI_SESSION_SNAPSHOT` /
  `pixel_tui_write_plan_ppm` already emit PPMs. Add
  `tests/test_pixel_snapshot.c`: render session frames at
  compact (640×360), dense (1162×841), expanded (1600×900) for each of the
  4 states; hash-compare against checked-in golden PPMs; env knob to
  regenerate goldens.
- **2.2 Kitty transport unit tests (landed).** Capture escape output into a memstream;
  assert control-string grammar (`a=t/T/p/f/d`, `f=24`, `o=z`, chunking
  `m=` continuation at 4096, image-id lifecycle: upload → place → delete old).
  No real terminal required.
- **2.3 native_ui contract tests (landed).** Scene diff damage bounds, focus order,
  hit routing, density breakpoints — per the doc's own testing contract.
- **Accept:** `make test_pixel` is green in a CI-style run. Checked-in PPM
  goldens remain a follow-up; current artifacts are structural/hash smoke tests.

## Phase 3 — Architecture convergence: retained scene → pixel backend

`native_ui.c` now drives the live masthead, live composer, and generative
overlays through the shared pixel adapter. Transcript, inspector, and live
overlays still use the immediate painter and should migrate incrementally.

- **3.1 Landed.** `px_backend` maps semantic tokens and retained primitives to
  transport-owned raster operations. Kitty, headless PPM, and a future native
  window can share it without exposing `pixel_tui.c` internals.
- **3.2 Landed for masthead and composer.** The live status header and command
  deck are built by `native_masthead_build()` and `native_composer_build()`
  with stable keys, accessibility labels, and fixed scene pairs for
  allocation-free previous/current diffing. The shared cell editor remains
  authoritative, and the established TUI remains untouched.
- **3.3 Partial.** Framebuffer tile patching is already live; the retained
  masthead and composer now also emit semantic damage. Next, use
  `native_ui_diff` regions to
  restrict repaint/encode work instead of scanning a newly painted full canvas.
  Kitty supports placement-relative updates;
  fall back to full frame when fragmentation exceeds threshold (native_ui
  already implements that fallback policy).
- **3.4** Migrate remaining regions incrementally (transcript, tool activity,
  inspector meters, overlays) — one region per commit, goldens updated per step.
  **Tool-history direction landed:** every governed call now owns one durable
  operation row that updates in place with a bounded result; the native default
  is `results`, with explicit `calls` and `full` density modes. A dedicated
  `native_transcript` module remains the next architectural extraction.
- **Accept:** golden parity at each step; steady-state upload bytes/frame for
  a single meter update drops materially vs full-frame (measure via memstream
  byte counts; record before/after in this doc).

## Phase 4 — Performance & telemetry

- **4.1 Partially landed.** Frame-cost instrumentation covers encode, transport,
  frame tails, scheduler wake lateness, deadline misses, and long rendered-frame
  gaps behind `DSCO_PIXEL_TUI_PERF`; zero cost remains the disabled target.
  Chronicle rollups and a live HUD remain. See
  `docs/NATIVE_COMPOSITOR_WORKSPACE_ROADMAP.md` for the multi-session controller
  and complete observability sequence.
- **4.2** Repaint coalescing audit: 0.125s min-interval + telemetry wake path
  under swarm-storm load (many `session_swarm_update` per tick); prove no
  unbounded queue and no starvation of the final state.
- **4.3** Canvas pool sizing: 2-slot pool was tuned for full-frame flow;
  re-check under partial-repaint flow from 3.3.
- **Accept:** measured numbers recorded here; no regression in snapshot tests.

## Phase 5 — Capability & UX growth (post-stabilization backlog, ordered)

- **5.1** `kitty_agent_windows` lifecycle tests + reconnect behavior when a
  companion window is closed by the user mid-swarm.
- **5.2 Landed.** Scroll-region transcript, placement re-anchor, bounded
  retained history, PageUp/PageDown, and SGR wheel scrollback are live.
- **5.3** Overlay stack and compatibility boundary (**landed early**): command
  and image-picker popovers, permission/confirmation/question/menu modals,
  transient notices, and full status projection render through the pixel
  backend. Pager, secure lock, and raw-stdin surfaces explicitly suspend and
  resume the compositor. The established TUI remains selectable with `--tui`.
- **5.4** Fractional DPR (from 1.3) if integer proves insufficient in practice.
- **5.5** Second backend spike (Metal or plain-ANSI) to prove the backend
  contract is real — smallest possible, one region.

## Sequencing & effort

```
P0 (10m) ──► P1 (½–1d) ──► P2 (1–2d) ──► P3 (2–4d, incremental) ──► P4 (1d) ──► P5 (backlog)
                 └── P2.2/2.3 can run parallel to P1 (independent files)
```

## Rules of engagement

- One region / one concern per commit; goldens updated in the same commit.
- No rewrite of `pixel_tui.c`; migration is additive and reversible per step.
- All escape-sequence behavior proven via memstream capture, not eyeballs.
- Design-language tokens (COMPOSITOR_DESIGN_LANGUAGE.md §1) are law; any new
  color/spacing goes through that doc first.
- Kitty remote-control (`tool_kitty_remote`/`tool_kitten`) stays governed
  through `tools_execute_for_tier()`; the compositor never grants authority.

## Open decisions for Arthur

1. Commit 4 of the WIP script (63 tracked-modified files) — review `git diff --stat` or let it ride as a baseline snapshot?
2. Integer vs fractional DPR (1.3) — integer default unless you see wrong sizing on a scaled display.
3. Phase 3 priority vs Wave B roadmap items — this plan assumes compositor continues as the active vector.
