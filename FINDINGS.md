# Findings

## 2026-07-05 consolidation pass

- `test_runner_tsan` is blocked by a pre-existing data race in `output_guard`, outside the touched subsystems. Reproduction: `TSAN_OPTIONS='halt_on_error=0 exitcode=66' ./test_runner_tsan > /tmp/test_runner_tsan_report.log 2>&1`. Report:
  - read: `stream_thread`, `output_guard.c:340`
  - write: `output_guard_reset`, `output_guard.c:406`
  - global: `g_og`
  - summary: `ThreadSanitizer: data race output_guard.c:340 in stream_thread`
- `make test_runner_tsan` / `make test_runner_asan` under `-O0` expose existing warning noise in `src/tui.c` for unused static helpers (`tui_alloc_with_retry`, `tui_error_report`) and the existing macOS deprecation warning for `posix_spawn_file_actions_addchdir_np`. These warnings are not from the consolidation changes.
- `src/construct.c` and `src/prompt_pool.c` are untracked modules. They were audited and minimally fixed for I1/I2 issues, and both compile standalone, but they are not wired into the tracked Makefile build graph.
