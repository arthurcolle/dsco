#!/usr/bin/env bash
# Conservative changed-test selector. Runs targeted tests when obvious, otherwise full test.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

changed="$(git diff --name-only --diff-filter=ACMRTUXB HEAD -- src include tests Makefile 2>/dev/null || true)"
if [[ -z "$changed" ]]; then
  changed="$(git diff --name-only --cached --diff-filter=ACMRTUXB -- src include tests Makefile 2>/dev/null || true)"
fi

run_full=false
targets=()
add_target() { local t="$1"; [[ " ${targets[*]} " == *" $t "* ]] || targets+=("$t"); }

while IFS= read -r f; do
  [[ -z "$f" ]] && continue
  case "$f" in
    src/stateful_atoms.c|include/stateful_atoms.h|tests/test_stateful_atoms.c) add_target test_stateful_atoms ;;
    src/plan_cache.c|include/plan_cache.h|tests/test_plan_cache.c) add_target test_plan_cache ;;
    src/plan_optimizer.c|include/plan_optimizer.h|tests/test_plan_optimizer.c) add_target test_plan_optimizer ;;
    src/recovery.c|include/recovery.h|tests/test_recovery.c) add_target test_recovery ;;
    src/session_memory.c|include/session_memory.h|tests/test_session_memory.c) add_target test_session_memory ;;
    src/control_flow.c|include/control_flow.h|tests/test_control_flow.c) add_target test_control_flow ;;
    src/avian.c|include/avian.h|tests/test_avian.c) add_target test_avian ;;
    src/learned_cost.c|include/learned_cost.h|tests/test_learned_cost.c) add_target test_learned_cost ;;
    src/math_fastpath.c|include/math_fastpath.h|tests/test_math_corpus.c) add_target test_math_corpus ;;
    src/tui.c|include/tui.h|tests/test_tui_*) add_target test_tui_snapshots ;;
    *) run_full=true ;;
  esac
done <<< "$changed"

if [[ ${#targets[@]} -eq 0 || "$run_full" == true ]]; then
  exec make test
fi

exec make "${targets[@]}"
