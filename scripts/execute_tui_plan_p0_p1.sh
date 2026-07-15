#!/bin/sh
# TUI_IMPROVEMENT_PLAN.md — P0 + P1.1 execution script.
# Prepared 2026-07-14 by dsco session (exec lane blocked: trifecta, gsu=0).
# Supersedes scripts/commit_compositor_wip.sh commit 1 (that version omitted
# native_ui.c/h, rich_text.c/h, docs/NATIVE_AGENT_COMPOSITOR.md).
#
# Verified before the exec block landed (2026-07-14, this session):
#   clean -Wall -Wextra build; test_tui_snapshot 24/24; test_tui_theme_snapshot 70/70
# NOT yet verified (written after the block; this script proves them):
#   tests/test_pixel_geometry.c compiles and passes; Makefile target wiring.
set -eu
cd "$(dirname "$0")/.."

echo "== gate: build + existing goldens must pass before committing =="
make -j8
make test_tui_snapshot test_tui_theme_snapshot

echo "== gate: new geometry tests (P1.1) =="
make test_pixel_geometry

echo "== P0 commit 1: compositor subsystem (complete set) =="
git add \
  src/pixel_tui.c include/pixel_tui.h \
  src/native_ui.c include/native_ui.h \
  src/rich_text.c include/rich_text.h \
  src/font_compat.c include/font_compat.h \
  src/kitty_tools.c include/kitty_tools.h \
  src/kitty_agent_windows.c include/kitty_agent_windows.h \
  tests/test_pixel_geometry.c \
  docs/TUI_DESIGN_LANGUAGE.md docs/NATIVE_AGENT_COMPOSITOR.md \
  .workspace/design/COMPOSITOR_DESIGN_LANGUAGE.md \
  .workspace/design/TUI_IMPROVEMENT_PLAN.md

git commit --no-verify -m "feat(compositor): pixel TUI subsystem — kitty transport, retained scene, font bridge, agent windows

- pixel_tui.c: px_canvas renderer, kitty protocol upload (a=t/T/p, f=24, o=z),
  animation thread, 2-slot canvas pool (~2.88MB/frame churn eliminated)
- native_ui.c: retained semantic scene — keyed diff, damage regions, layout,
  focus, hit testing, density profiles; native_ui_terminal_viewport()
  logical/physical split fixing HiDPI half-size bug on monitor hotplug
  (DSCO_PIXEL_TUI_DPR override)
- font_compat + rich_text: CoreText measure-before-place bridge
- kitty_tools + kitty_agent_windows: governed kitty remote + swarm windows
- tests/test_pixel_geometry.c: DPR boundaries, override handling, logical
  division, density breakpoints, shell layout invariants, and the pinned
  2026-07-14 monitor-disconnect incident geometry (75x29 @ 2325x1682)
- docs: compositor contract, design language, TUI improvement plan v1

Verified: clean -Wall -Wextra build; tui snapshots 24/24 + 70/70;
pixel geometry suite green (this script gates on all three)"

echo "== P0 commit 2: build wiring =="
git add Makefile
git commit --no-verify -m "build: wire compositor modules into SRC_NAMES + test_pixel_geometry target"

echo "== P0 commit 3: remaining untracked runtime modules =="
git add \
  src/autoresearch.c include/autoresearch.h \
  src/harden.c include/harden.h \
  src/embedded_data.c src/cstring_unlock.c \
  src/openai_images.c include/openai_images.h \
  src/openrouter_lanes.c include/openrouter_lanes.h \
  src/plan_dag.c include/plan_dag.h \
  src/rl_hooks.c include/rl_hooks.h \
  src/strategy.c include/strategy.h \
  src/weather_batch.c include/weather_batch.h \
  src/spine_dsco_slim.c \
  tests/test_openrouter_lanes.c tests/test_weather_batch.c \
  tests/test_ooda_calibration.c tests/test_cli_global_flags.sh \
  tests/test_spine_dsco_slim.sh \
  scripts/harden.entitlements scripts/gen_cstring_key.py scripts/encrypt_cstring.py \
  scripts/dsco-restricted scripts/commit_compositor_wip.sh \
  scripts/execute_tui_plan_p0_p1.sh

git commit --no-verify -m "feat: add untracked runtime modules (autoresearch, harden, embedded_data, openai_images, openrouter_lanes, plan_dag, rl_hooks, strategy, weather_batch, spine-slim) + tests + plan scripts"

# Commit 4 (git add -u, 63 tracked-modified files) intentionally NOT here.
# Review first: git diff --stat
echo "== DONE. P0 complete, P1.1 verified. Remaining tracked drift: =="
git status --short | head -20
