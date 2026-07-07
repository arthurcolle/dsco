# Findings

## 2026-07-05 consolidation pass

- `test_runner_tsan` is blocked by a pre-existing data race in `output_guard`, outside the touched subsystems. Reproduction: `TSAN_OPTIONS='halt_on_error=0 exitcode=66' ./test_runner_tsan > /tmp/test_runner_tsan_report.log 2>&1`. Report:
  - read: `stream_thread`, `output_guard.c:340`
  - write: `output_guard_reset`, `output_guard.c:406`
  - global: `g_og`
  - summary: `ThreadSanitizer: data race output_guard.c:340 in stream_thread`
- `make test_runner_tsan` / `make test_runner_asan` under `-O0` expose existing warning noise in `src/tui.c` for unused static helpers (`tui_alloc_with_retry`, `tui_error_report`) and the existing macOS deprecation warning for `posix_spawn_file_actions_addchdir_np`. These warnings are not from the consolidation changes.
- `src/construct.c` and `src/prompt_pool.c` are untracked modules. They were audited and minimally fixed for I1/I2 issues, and both compile standalone, but they are not wired into the tracked Makefile build graph.

## 2026-07-07 review pass

Status legend: [OPEN] unresolved · [CARRIED] from prior pass, not re-verified today · [INFRA] environment/tooling, not repo code

- [FIXED 2026-07-07] `output_guard` data race — resolved. Two distinct races were present; the flag masked the second:
  - Race A (`tripped`): naked read at `src/output_guard.c:340` vs `__sync_lock_release` at reset. Fixed: `:340` now `__atomic_load_n(&g_og.tripped, __ATOMIC_ACQUIRE)`, pairing the release.
  - Race B (per-stream state, TSAN-surfaced only after A was fixed): `output_guard_reset` (main thread) wrote `frame_len`/`repeat_count`/`repeat_bytes`/`last_norm`/`last_preview` while `stream_thread`→`process_bytes` (owner thread) read/wrote them (`output_guard.c:308` vs `:415`). Fixed by removing all cross-thread writes: reset now only bumps an atomic `reset_epoch` (`__atomic_fetch_add(..., __ATOMIC_RELEASE)`); each stream self-clears on its next chunk (owner-thread `seen_epoch` check). Every per-stream field is now single-writer; only `tripped` + `reset_epoch` are shared, both atomic.
  - Verification: `make -j4` + `make -j4 test_runner_tsan` clean; `test output_guard init/reset` PASSES under TSAN; two independent `TSAN_OPTIONS='halt_on_error=0 exitcode=66' ./test_runner_tsan` runs report ZERO ThreadSanitizer warnings (pre-fix the race fired at report line 527). `./dsco --version`/`--help` smoke-tested — output mirrors correctly. NOTE: full TSAN suite intermittently stalls on subprocess-spawning `swarm spawn` tests (varying points: ollama-scoring / Codex-OAuth) — pre-existing environmental flakiness under TSAN, NOT this change (lock-free atomics cannot deadlock; stall point is non-deterministic).
- [CARRIED] `src/tui.c` unused static helpers (`tui_alloc_with_retry`, `tui_error_report`) + macOS `posix_spawn_file_actions_addchdir_np` deprecation warning. Not re-verified this pass.
- [CARRIED] `src/construct.c` and `src/prompt_pool.c` still untracked in the Makefile build graph (compile standalone). Not re-verified this pass.
- [INFRA] PATH-installed `dsco` is a STALE build predating the argv parser fix: `dsco -m X -a -i` swallows unknown `-`-flags into the prompt (fixed behavior lives only in `./dsco`), and `dsco --interactive --autonomous` 400s with an empty model where `./dsco` supplies `DEFAULT_MODEL`. `hotswap` targets `./dsco`, not the PATH binary. Action: install the fixed repo build over the PATH `dsco`.
- [INFRA] `cloudmail-mcp` MCP server registered (154 tools) but transport NOT connected — `inbox` fails `MCP server not connected: cloudmail-mcp` (stable across retries). Likely backend under `~/protonmail-bridge` (modal_app.py / src/email_mcp) not running/reachable.
- [OPEN] `examples/max_swarm` cache-floor padding smell: `swarm.py:639` and `self_improve.py` clear the per-model cache_min by literal string repetition (`"..." * 250`). Correct (sub-floor prefix silently pays full price, enforced at `swarm.py:383`) but crude — floods reports (128KB doctrine dump) and burns redundant prefix tokens. Prefer one substantive doctrine that clears the floor over stuttering the same sentence.
- Process note: the 2026-07-05 findings were re-"mined" by the max_swarm self-improvement run rather than resolved. Findings are being READ but not CLOSED. This tracker needs a resolution step (mark fixed + commit ref), not just re-discovery.
