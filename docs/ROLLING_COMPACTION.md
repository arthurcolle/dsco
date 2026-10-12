# Rolling compaction and provider continuity

Context maintenance updated 2026-10-03; verification: `reports/compaction-completion-20261003/`. Provider continuity observations and the older install/rollback details below are the 2026-09-26 snapshot, not a fresh account/access check.

## Context maintenance

`src/request_context.c` is the shared interactive/headless request boundary. Before dispatch, each request is measured against both a model-capacity trigger (at most **48,000 estimated tokens or 50%** of input capacity, tightened for recent bursts) and a serialized-wire working-set target. The wire target defaults to **0 (off)**; model-context pressure remains active when automatic compaction is enabled. Both directly address cumulative small results that never cross a per-result spill threshold. Set `DSCO_REQUEST_TARGET_BYTES` to `65536..67108864`, or to `0`/`off` for immediate rollback. This is a soft performance target: protected recent content is never discarded merely to satisfy it. Archival crosses `tools_execute_for_tier()` and continues while actual request size decreases and pressure remains. No fixed pass count limits recovery.

Completed exchanges are persisted and read back before being replaced. Old assistant prose is also bounded, rather than retaining unlimited reasoning/text after removing tool arguments. Contiguous tool and prose receipt runs are folded into verified parent archives; parent indexes retain child keys. Split parallel result messages and multi-block text/replay responses are supported, with every block field serialized. Both manual modes require archival; there is no destructive history-trimming fallback. The 2026-10-03 real-binary fixture archived 650 responses, restarted, traversed the indexes through governed context_recall, and independently verified all 650 exact originals in the persistent store.

Human instructions, pending tool batches and the latest two messages remain intact. Errors preserve their `is_error` field in archives. Interactive pre-request, spending and overflow maintenance no longer destructively trims history before archival. Explicit automatic-compaction opt-out remains supported. JEV is optional shadow telemetry, not a dependency for recovery.

Every admission now records pre/post wire bytes, the active target, archive attempts/passes, and whether the target was satisfied. Every archive result reports why it was a no-op (`protected_tail_or_empty`, `no_complete_tool_exchange`, `unsafe_or_incomplete`, `below_min_bytes`, or a storage/compaction failure) plus candidate counters. This makes ineligible retained history measurable instead of opaque.

The wire target is an optional performance policy. Watch `wire_target_triggered`, `wire_target_satisfied`, archive frequency, cache-hit ratio, and provider latency together. More archives trade cache-prefix continuity for smaller transfers; roll back with `DSCO_REQUEST_TARGET_BYTES=0` if cache misses or recall overhead outweigh the byte reduction. In the offline 56-request fixture, a 131,072-byte target reduced maximum serialized input from **273,508 to 126,364 bytes (53.8%)**, with 9 rather than 3 archive cycles. This is an isolated transfer-volume result, not a live end-to-end latency claim.

`llm.request.created` also carries content-safe whole-request and stable-prefix fingerprints, previous request length, and common-prefix bytes. These fields share the LLM span with response cache usage, so a cold miss can now be separated into stable instruction/schema churn, early history rewrite, or an unchanged retry without retaining raw prompt text in product telemetry.

## Provider continuity

`src/provider_continuity.c` selects native subscription lanes. Operator opt-in `DSCO_CROSS_PROVIDER_FALLBACK=1` permits cross-provider recovery despite the initial provider selection. `DSCO_CONTINUITY_PROVIDERS` narrows the eligible providers. Models retain their subscription provider binding rather than being implicitly routed onto metered API credentials; auth class is rechecked before fallback. Interactive fallback adopts the destination model's context capacity.

Enabled locally: `anthropic,openai-codex,kimi-code`, with `DSCO_AUTO_FALLBACK=1` and `DSCO_AUTO_COMPACT=1`. Live bounded streaming probes returned HTTP 200 for these three. Sakana returned 429/no active subscription; Z.AI returned 429/expired plan. Neither is included in the enabled chain. The probe checked text streaming, not a live tool-use conversation on every provider.

Both interactive and headless local HTTP fixtures prove Codex credential failure -> Kimi recovery, preserved objective/tool history, and no repeated write. Opt-out is tested. Fallbacks remain finite, not indefinite retries.

## Verification

`make test-continuity-regression` covers policy/archive unit tests, headless endurance, interactive/headless failover, and opt-out. The current retained-output fixture processes 56 requests and 990,000 bytes of individually sub-24-KiB tool arguments, preserves the latest exchange and final answer, and checks the configured wire bound. No paid inference is used by these fixtures.

Separate real-binary archive-recall fixture verifies exact archived arguments and saved final answer. Nine live-binary capability-gate checks passed. The installed executable also passed the interactive failover fixture.

Installed: `~/.local/bin/dsco`.
SHA256: `5b818611d61b89e966e4edad2ef09d89cba3041fb4f261d755adc33d877eecde`.
Rollback binary and private config backup: `~/.dsco/backups/continuity-20260926T030635/`. Restore the binary using `scripts/install_atomic.sh`; restore only the named continuity settings if unrelated config changes have occurred since backup.

Limitations: existing processes retain their loaded executable and environment; no hot patch or interruption was attempted. Human-only history is not semantically summarized. Protected content can still exceed a model's capacity. Additional specialized request paths outside the interactive/headless loops are not claimed covered. One bounded read-only audit worker exhausted its budget without a usable final audit; coordinator completed verification. Existing unrelated checkout changes are included in the build.
