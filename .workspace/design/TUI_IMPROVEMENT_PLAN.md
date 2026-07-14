# TUI / Native Compositor — Improvement Plan v1

## EXECUTION STATUS — 2026-07-14 session 2

| Item | State |
|---|---|
| DPR fix | **Landed & refactored**: logical/physical split now lives in `native_ui_terminal_viewport()` (native_ui.c); `pixel_tui.c` consumes it via `render_device_scale()`/`session_render_geometry()`. Thresholds: 2x at cell ≥14w/24h, 3x at ≥27w/38h, env override `DSCO_PIXEL_TUI_DPR` 1–4. |
| P1.1 tests | **Written**: `tests/test_pixel_geometry.c` (~40 assertions: boundary cells 13/14/26/27w + 23/24/37/38h, override 0/-2/5, logical division, density breakpoints, shell-layout invariants, pinned incident geometry 75×29@2325×1682→3x). Makefile target `test_pixel_geometry` wired. **Not yet compiled/run** — exec lane blocked. |
| P0 commits | **Blocked on exec** (immune: lethal-trifecta, gsu=0). Gated script prepared: `scripts/execute_tui_plan_p0_p1.sh` — builds, runs all three test suites, then commits 1–3. Supersedes `commit_compositor_wip.sh` commit 1 (which omitted native_ui/rich_text/compositor doc). |
| P1.2–P1.4, P2+ | Not started; P1.2 partially covered — a dpr flip alone changes logical w/h, which defeats `session_refresh_geometry_locked`'s early-out (verified by reading; needs the P1.2 injection-seam test to prove). |
| Threshold retune question | Incident cell 31×58 was physically 2x + large font but classifies 3x. Behavior pinned in test with comment; retune is a conscious future decision. Visual result is still correct-size (placement stretches), so not urgent. |

**To resume:** run `sh scripts/execute_tui_plan_p0_p1.sh` from a session with exec authority (or manually). It is gated — nothing commits unless build + 24/24 + 70/70 + geometry suite all pass.

---

**Date:** 2026-07-14 · **Branch:** `emergency/wip-snapshot-20260707` @ `b0b0284`
**Owner:** Arthur Colle · **Executor:** DSCO Runtime
**Companion docs:** `docs/NATIVE_AGENT_COMPOSITOR.md`, `docs/TUI_DESIGN_LANGUAGE.md`,
`.workspace/design/COMPOSITOR_DESIGN_LANGUAGE.md`

## 0. Observed baseline (verified this session)

| Fact | Evidence |
|---|---|
| Clean `-Wall -Wextra` build; `dsco` + `spine-dsco-slim` link | make -j8, 2026-07-14 |
| `test_tui_snapshot` 24/24 · `test_tui_theme_snapshot` 70/70 | run 2026-07-14 |
| DPR/Retina fix landed in `pixel_tui.c` (3 sites, `render_device_scale()`) | this session |
| Compositor sources (~7.5K LOC) **untracked** — zero git protection | `git status` |
| `pixel_tui.c` uses `native_ui` for shell layout ONLY — scene/diff/render/focus/hit-test unused | grep: 1 call site |
| No dedicated tests for pixel_tui, native_ui, kitty_tools, kitty_agent_windows | `ls tests/` |
| Existing snapshot tests cover the ANSI/cell TUI, not the pixel path | grep in tests |
| Every repaint re-encodes + re-uploads the FULL framebuffer (no damage-driven partial upload) | `session_repaint` |

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

The subsystem is ~7.5K LOC with zero direct tests. The design docs already
ratify acceptance criteria (§6 of COMPOSITOR_DESIGN_LANGUAGE.md); implement them.

- **2.1 Canvas snapshot harness.** `DSCO_PIXEL_TUI_SESSION_SNAPSHOT` /
  `pixel_tui_write_plan_ppm` already emit PPMs. Add
  `tests/test_pixel_snapshot.c`: render session frames at
  compact (640×360), dense (1162×841), expanded (1600×900) for each of the
  4 states; hash-compare against checked-in golden PPMs; env knob to
  regenerate goldens.
- **2.2 Kitty transport unit tests.** Capture escape output into a memstream;
  assert control-string grammar (`a=t/T/p/f/d`, `f=24`, `o=z`, chunking
  `m=` continuation at 4096, image-id lifecycle: upload → place → delete old).
  No real terminal required.
- **2.3 native_ui contract tests.** Scene diff damage bounds, focus order,
  hit routing, density breakpoints — per the doc's own testing contract.
- **Accept:** `make test_pixel` target; all green in CI-style run; goldens committed.

## Phase 3 — Architecture convergence: retained scene → pixel backend

`native_ui.c` (retained scene, keyed diff, damage regions) and `pixel_tui.c`
(immediate-mode frame painter) are parallel systems joined only at
`native_ui_agent_shell_layout()`. The compositor doc's whole thesis —
semantic scene → diff → backend ops — is not yet what the pixel path does.

- **3.1** Implement `native_ui_backend_t` for px_canvas (fill/stroke rect,
  text via font_compat, icon, clip push/pop). Small file, `src/px_backend.c`.
- **3.2** Migrate ONE region (status header) from direct painting to
  scene-driven rendering. Prove identical golden output. Minimal diff;
  do not rewrite `pixel_tui.c` wholesale.
- **3.3** Damage-driven partial repaint: use `native_ui_diff` damage rects to
  re-encode only dirty regions. Kitty supports placement-relative updates;
  fall back to full frame when fragmentation exceeds threshold (native_ui
  already implements that fallback policy).
- **3.4** Migrate remaining regions incrementally (transcript, tool activity,
  composer, meters) — one region per commit, goldens updated per step.
- **Accept:** golden parity at each step; steady-state upload bytes/frame for
  a single meter update drops materially vs full-frame (measure via memstream
  byte counts; record before/after in this doc).

## Phase 4 — Performance & telemetry

- **4.1** Frame-cost instrumentation: encode µs, zlib bytes, upload bytes,
  paints/s — into Chronicle/observability (Wave B #12 synergy) behind an env
  flag; zero cost when off.
- **4.2** Repaint coalescing audit: 0.125s min-interval + telemetry wake path
  under swarm-storm load (many `session_swarm_update` per tick); prove no
  unbounded queue and no starvation of the final state.
- **4.3** Canvas pool sizing: 2-slot pool was tuned for full-frame flow;
  re-check under partial-repaint flow from 3.3.
- **Accept:** measured numbers recorded here; no regression in snapshot tests.

## Phase 5 — Capability & UX growth (post-stabilization backlog, ordered)

- **5.1** `kitty_agent_windows` lifecycle tests + reconnect behavior when a
  companion window is closed by the user mid-swarm.
- **5.2** Scroll-region transcript with placement re-anchor already exists;
  add scrollback for the pixel transcript (currently render-window only).
- **5.3** Overlay stack: command palette + permission surface as `native_ui`
  overlay roles rendered through the px backend (depends on Phase 3).
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
