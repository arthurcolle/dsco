# ChatGPT SSE buffering — 2026-09-05

Optimized only chatgpt_sse_write_cb in src/provider.c: append contiguous non-CR/LF spans, not each byte. Existing modifications to provider.c preserved. No dispatch/gate changes.

Offline test compiles the actual callback extracted from source, alongside the original scalar implementation, using cc -O3 and the same json_util.c. Line processing is a deterministic test stub (records lines and recognizes [DONE]), so these numbers measure buffering/framing, NOT JSON event processing, network or whole-agent latency.

Median of five samples, local arm64, CPU clock, 20,000 calls/sample:

| Callback bytes | Before ns | After ns | Speedup |
|---|---:|---:|---:|
| 128 | 268.8 | 58.0 | 4.63x |
| 1024 | 2039.8 | 342.1 | 5.96x |
| 8192 | 16376.1 | 2466.7 | 6.64x |
| 65536 | 131182.8 | 19857.7 | 6.61x |

Tests compare return values, pending bytes, processed output and termination against the scalar reference after each chunk: every fixture chunk size, CRLF, discarded lone CR, empty lines, [DONE] with trailing data, embedded NUL, 1,000 deterministic randomized buffers and 64 KiB buffers with varying chunk sizes. Timing fixtures retain earlier inserted newlines when growing, thus include multiple lines at larger sizes.

Verification:
- python3 tests/test_chatgpt_sse_buffer.py: PASS
- make -j6 test: 5280 passed; live dsco rebuilt
- make test-gate-claims: 9 passed, 0 failed/skipped
- git diff --check: PASS

Raw logs: build/harness-perf/sse.log, sse-tests.log, sse-gates.log.
Reproduce: python3 tests/test_chatgpt_sse_buffer.py.
Rollback: replace ONLY chatgpt_sse_write_cb with REFERENCE from that test (renaming reference to chatgpt_sse_write_cb). Do not reset provider.c: it contains unrelated prior changes. Scalar reference was compiled and exercised alongside candidate. No commit/deployment. No paid provider calls for testing; coordinator usage cost unknown. Other platforms and full streaming latency remain unmeasured.
