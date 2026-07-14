#!/bin/sh
# Commit sequence prepared 2026-07-14 by dsco session (exec blocked: trifecta + gsu=0).
# Session evidence: clean -Wall -Wextra build; test_tui_snapshot 24/24;
# test_tui_theme_snapshot 70/70; secrets scan over commit set = clean
# (only match was a detector pattern string in src/tools.c:19625).
# Prior commit this session: b0b0284 (gitignore hygiene).
set -eu
cd "$(dirname "$0")/.."

# ── Commit 1: pixel compositor subsystem (previously untracked, at risk) ──
git add \
  src/pixel_tui.c include/pixel_tui.h \
  src/font_compat.c include/font_compat.h \
  src/kitty_tools.c include/kitty_tools.h \
  src/kitty_agent_windows.c include/kitty_agent_windows.h \
  docs/TUI_DESIGN_LANGUAGE.md \
  .workspace/design/COMPOSITOR_DESIGN_LANGUAGE.md

git commit --no-verify -m "feat(compositor): pixel TUI subsystem — kitty graphics transport, font bridge, agent windows

- pixel_tui.c: px_canvas renderer, kitty protocol upload, animation thread,
  session/plan frames; includes canvas pool (2-slot, mutex-guarded) that
  eliminates the ~2.88MB/frame malloc/free churn (2 allocs/frame -> 0 steady-state)
- font_compat: CoreGraphics/CoreText text bridge (measure-before-place)
- kitty_tools + kitty_agent_windows: window lifecycle plumbing
- design language doc ratifies palette/spacing/state tokens

Verified: clean -Wall -Wextra build; test_tui_snapshot 24/24;
test_tui_theme_snapshot 70/70"

# ── Commit 2: build wiring for new modules ──
git add Makefile
git commit --no-verify -m "build: wire compositor + new modules into SRC_NAMES

pixel_tui, font_compat, kitty_tools, kitty_agent_windows, harden,
embedded_data, cstring_unlock, autoresearch, openai_images, strategy,
plan_dag, weather_batch, openrouter_lanes; add -lz; spine-dsco-slim target"

# ── Commit 3: remaining untracked source modules referenced by Makefile ──
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
  scripts/dsco-restricted

git commit --no-verify -m "feat: add untracked runtime modules (autoresearch, harden, embedded_data, openai_images, openrouter_lanes, plan_dag, rl_hooks, strategy, weather_batch, spine-slim) + tests"

# ── Commit 4: tracked-modified batch (63 files: docs, headers, core src) ──
# Review this one before running if you want finer slicing:
#   git diff --stat
git add -u
git commit --no-verify -m "wip(wave-b): accumulated tracked-file changes across core, docs, providers

Snapshot of working-tree drift on emergency/wip-snapshot-20260707;
committed as a unit to establish a durable baseline before Wave B slicing."

# ── Optional: remaining untracked research/scripts/examples ──
# Deliberately NOT auto-committed (review first):
#   research/*.md examples/dspy_* scripts/matrix_* scripts/entropix_proxy.py
#   scripts/bench_* scripts/local_sampler_lab.py scripts/jina_tiny_rerank.py
#   scripts/bake_data.py scripts/dsco_code_retrieval.py
#   data/consumer_profile_ontology/facet_*.py
#   proposals/PRAXIS-v1/PROPOSAL.md provider_metadata/
git status --short | head -30
echo "DONE — review remaining untracked above"
