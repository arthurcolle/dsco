# Jev compaction shadow experiment

Status: implemented and tested in candidate `build/dsco-jev-experiment`; **not installed or enabled in the normal DSCO binary**.

## Scope

`src/jev_compaction.c` observes `context_evict` (including automatic context-budget recovery). After the existing archival pass, it asks Jev for archive/retain recommendations on up to eight eligible exchanges. It never changes selection, summaries, archives, recall handles, or conversation messages. Jev is a decision model, not a prose summarizer.

This first rubric uses structural metadata only: exchange age, archive byte length, tool-call count, and result count, plus synthetic IDs. No message text, tool names, arguments, outputs, original IDs, paths, hashes, or recall keys are sent. Consequently this does **not** measure semantic importance or summarization quality. It also observes only candidates the incumbent visited, not an unbiased sample of all history.

## Enable / disable

Build the isolated experimental executable:

```sh
make -j4 TARGET=build/dsco-jev-experiment build/dsco-jev-experiment
```

For an explicitly opted-in session, with `TYPESAFE_API_KEY` already exported:

```sh
DSCO_JEV_COMPACTION=shadow ./build/dsco-jev-experiment
```

Normal capability grants still apply. Any value other than exactly `shadow`, or an unset variable, disables the experiment. Unset it and start a fresh process to roll back the experiment. Existing running processes and the installed executable are not modified. The process-wide attempt limit does not reset at conversation reset.

No requests are made until an eligible context-eviction pass occurs. At most one attempt per process, up to eight questions, no retries; connect timeout 2 seconds and total curl timeout 4 seconds. The synchronous observer adds latency when opted in. Response size is bounded at 32 KiB. The adapter requests `jev-latest` and records its resolved model; promotion needs pinned/re-evaluated model behavior.

## Transport and authority

The native C adapter uses DSCO's existing `curl_raw` tool through `tools_execute_for_tier`, inheriting the calling tier. No new tool or gate exemption is introduced. The existing transport requires curl >= 8.3 for environment variable expansion; tested here with curl 8.7.1. Credentials are expanded inside curl, not embedded into shell arguments or tool audit input. Curl configuration is disabled (`-q`), HTTPS is required, and redirects are not followed. No shell startup file is read by the runtime adapter.

Secret capability denial, missing key, tool allowlist denial, network denial, the lethal-trifecta guard, timeout, HTTP failure, and malformed output all leave incumbent compaction intact. The existing curl_raw wrapper does not expose detailed HTTP errors; non-JSON failures surface as `invalid_response`, and explicit dispatch denial surfaces as `transport_denied_or_failed`.

**Important side effect:** credential access and network response taint are recorded normally. An enabled experiment can therefore affect subsequent egress permissions in that session. Do not bypass that guard for this experiment. Default-off mode does not touch the credential or perform nested dispatch.

## Observation

Enabled calls add `jev_shadow` to the context-eviction result and emit Chronicle event `experiment.jev_compaction`. An observed receipt includes rubric, resolved model, candidate count, archive recommendation count, agreement count, candidate/actual archive bitmasks, input tokens, measured dispatch latency, and `applied:false`. Small output buffers can omit the detailed result receipt; Chronicle receives the full bounded receipt where enabled. No claim is made that agreement establishes correctness.

## Verification

Offline unit and integration coverage:

```sh
make test-jev-compaction test_context_eviction
python3 tests/test_context_recovery_binary.py --binary build/dsco-jev-experiment --mode tui
python3 tests/test_context_recovery_binary.py --binary build/dsco-jev-experiment --mode tui --jev missing-key
python3 tests/test_context_recovery_binary.py --binary build/dsco-jev-experiment --mode tui --jev net-denied
```

A deliberately paid, metadata-only live check (one request per test process):

```sh
python3 tests/test_context_recovery_binary.py --binary build/dsco-jev-experiment --mode tui --jev live
```

These tests use owned PTYs, an isolated HOME/database, synthetic conversation fixtures, and a local fake primary-model server. They do not open desktop surfaces or spend on a primary model. The live Jev option is the sole real API call. Unit fixtures cover missing/denied credentials, inherited authority, no secret in tool input, request bounds, malformed/missing/range-invalid responses, and attempt limiting. Recovery tests verify byte-exact archived arguments, recall, protected recent evidence, no repeated writes, protocol integrity, and clean exit. Existing live-binary gate claims passed 9/9.

Observed live result, 2026-09-18:

- Model: `jev-1.13.0`; 432 input tokens; 745.0 ms measured nested-dispatch latency.
- One candidate: Jev recommended retain; incumbent archived. Recommendation was **not applied**. This is useful disagreement, not evidence that either policy is semantically better.
- Archive retrieval remained exact; both fixture writes executed exactly once; latest arguments and user instructions were preserved.
- Estimated Jev input cost at $0.042/M tokens: $0.000018144, not invoice confirmation. Output tokens are free at documented pricing.
- Evidence: `build/jev-compaction-live/tui/result.json` and sibling synthetic transcripts/requests.
- Candidate SHA256: `fa37530b2cf7fc0844582f1348034ddf358c083ad56ae192a43e5a6665d3af0e`.

Independent native worker reviewed the design read-only and identified privacy, inherited-tier, curl configuration, taint, and latency concerns incorporated here. Its runtime-estimated subscription inference usage was $1.4869 despite a requested $0.40 worker budget; no further workers were launched. Coordinator cost is unknown. No claim of complete high-blast independent code review is made.

## 100,000-case industry-scale follow-up

Completed 100,000 real Jev classifications in 1,000 validated API batches over synthetic evidence snippets spanning 100 representative domain labels and 20 broad sectors. The corpus comprises 20 scenario families with 50 procedural variants per domain; it is neither exhaustive industry coverage nor 100,000 independent semantic scenarios. Unlike the native metadata-only hook, this isolated benchmark sends synthetic semantic evidence. Private conversations and production settings remain untouched.

- Raw Jev at archive probability >= 0.50: 96.977% accuracy; 2,461 retain-worthy items recommended for archival, including 2,304 protected items.
- Same threshold with deterministic protection: 99.281% accuracy; 157 incorrect archives remain. These protections must not be delegated to Jev.
- Safety-first selection on 80,000 development variants chose >= 0.90. On 20,000 same-template holdout variants: zero observed incorrect archives, but 2,338 unnecessary retentions (88.31% overall accuracy). This is not a guarantee of safety or novel-template generalization.
- Input usage 25,582,600 tokens; estimated recorded API cost $1.0744692, excluding unknown coordinator cost. An execution timeout left up to four unrecorded in-flight outcomes; their conservatively estimated extra cost bound is $0.017831352. Invoice totals unverified.
- Median 100-case batch latency 1.053 seconds; p95 1.835 seconds at concurrency four. Do not equate batch latency to single-case latency.
- All case IDs, answer schemas, exact reconstructed request hashes and response-file hashes checked. Native one-attempt bounds and installed DSCO were not changed.

Evidence and full domain/family/threshold metrics: `~/.dsco/workspace/experiments/typesafe-compaction-sweep/industry-100k-v1/REPORT.md`, `verified-analysis.json`, `evidence-manifest.json`, and versioned `summary-*.json`. Recommendation: keep the native experiment in shadow mode; test genuinely varied held-out traces and downstream task success before allowing semantic recommendations to govern retention.

## Next experiment

Create labeled, synthetic or explicitly approved redacted exchanges with retention-critical facts; compare semantic Jev recommendations with deterministic archival and downstream recall/task success. Structural-only observations cannot justify production semantic-compaction decisions. Keep full archive recovery and permission gates authoritative; evaluate fallback, confidence/abstention, latency, and cost before allowing recommendations to affect retention.
