# Harness JSON skip optimization — 2026-09-05

Local arm64, Apple clang 21.0.0, checkout HEAD 29390f9 with pre-existing working changes preserved. Change: `src/json_util.c` uses a 32-iteration scalar prefix for short strings, then the existing NUL-bounded libc-backed span scanner for long strings while skipping JSON values (top-level, object, array). Escape and malformed-input behavior remains unchanged. No gate, tool dispatch, provider, or identity changes.

## Measurement

Same benchmark source and `cc -O3 -march=native -Iinclude` flags, linked separately against saved pre-change and candidate json_util.c. Seven samples after warmup per case; medians below, nanoseconds per operation. Offline synthetic fixtures, not whole-turn or network latency. Response fixture includes text and a tool-use block; late-fields reads both an integer and boolean after a nested string. Escaped fixtures insert a newline every 32 characters. The 16-byte escaped-labelled fixture has no inserted newline and is effectively a duplicate short-string control.

| Case | Before ns | After ns | Speedup |
|---|---:|---:|---:|
| 16 B late fields, plain | 61.32 | 59.97 | 1.02x |
| 16 B response, plain | 411.99 | 407.07 | 1.01x |
| 128 B late fields, plain | 216.10 | 134.10 | 1.61x |
| 128 B response, plain | 599.90 | 512.40 | 1.17x |
| 4 KiB late fields, plain | 4675.20 | 2347.30 | 1.99x |
| 4 KiB late fields, escaped | 4642.00 | 2752.10 | 1.69x |
| 4 KiB response, plain | 6333.40 | 4055.20 | 1.56x |
| 4 KiB response, escaped | 7480.90 | 5557.80 | 1.35x |
| 64 KiB late fields, plain | 74211.00 | 35307.00 | 2.10x |
| 64 KiB late fields, escaped | 73466.00 | 42705.00 | 1.72x |
| 64 KiB response, plain | 95042.00 | 56106.00 | 1.69x |
| 64 KiB response, escaped | 109239.00 | 80139.00 | 1.36x |

Reverse-order repeat confirmed 1.69–2.11x late-field and 1.38–1.69x response speedups for 4–64 KiB; short controls ranged 1.02–1.04x. Repeat ran concurrently with verification, so first isolated series is primary. Initial all-libc candidate regressed short fixtures ~10–15%; it was replaced, not retained.

Reproduce candidate: `make bench-json-skip`. Saved baseline and raw samples/logs: `build/harness-perf/` (local, ignored build artifacts). To compare another baseline, compile `bench/bench_json_skip.c` against that baseline's json_util.c with identical flags.

## Verification

- `make -j6 test`: 5,280/5,280 passed; rebuilt live dsco.
- `make test-json-skip`: ASan passes 337,134 truncated prefixes, each checked at exact heap and guard-page boundaries, plus valid plain/escaped/Unicode nested fixtures.
- `make test-json-scan-bounds`: existing ASan decoder/scan bounds regression passes.
- `make test-gate-claims`: live MCP JSON-RPC gate checks 9 passed, 0 failed/skipped.
- `./dsco --help`: passed.
- `git diff --check`: passed.

## Provenance / rollback / limits

- Baseline json_util.c SHA-256: `38b564b9da76e15adc7369811d060f8b217ed88193644edaf57c6a84622209f1`.
- Candidate json_util.c SHA-256: `6b3addbbdec82c46125acf1c5c972553b1f6669580311040c6f3d85917f102bc`.
- Benchmark SHA-256: `c95467cf1297edb2b0d7eaae19cf6476c13f254b1826ce67fce4a397b9c2e28b`.
- Regression SHA-256: `378cb5ccefae16dcc6fe7d330372e9898a1f7c9fe6b9a5db89fa62b19070bdb8`.
- Baseline was rebuilt and rerun successfully. Roll back only this patch/helper and its three call sites, plus the new test/benchmark targets/files if desired; do not reset the already-dirty Makefile or other user work. No commit or external deployment performed.
- CPU microbenchmarks establish only the measured parser gain; Linux/libc variants, dense escapes, and whole-agent latency remain unmeasured.
- One native read-only audit worker completed/collected (no children active), subscription lane, estimated inference $0.02368284; actual billed cost and coordinator usage unknown. It identified a separate byte-at-a-time ChatGPT SSE path; that suggestion remains unimplemented/unverified and was not counted toward this outcome.
