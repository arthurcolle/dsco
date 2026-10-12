# Offline run diagnostics

`dsco-run-diagnostic` turns retained Chronicle telemetry into a compact latency/context/economics report. It performs no inference, network calls, workload replay, account mutation, or changes to source logs. It is separate from `scripts/dsco_profile.py`, which builds and symbolizes object-code instrumentation.

## Use

```sh
# Find the trace IDs recorded in a session file.
dsco-run-diagnostic --events ~/.dsco/chronicle/events/YYYY/MM/DD/session-UUID.jsonl --list

# Diagnose one trace; incomplete request/response coverage returns exit 3.
dsco-run-diagnostic --events PATH_TO_SESSION_JSONL --trace TRACE_UUID \
  --output ./run-diagnostic --strict
```

Outputs are `REPORT.md` and `diagnostic.json`, atomically replaced and read back after writing, with owner-only permissions. Select an output directory separate from the source log directory. Invalid inputs return exit 2. A still-appending partial final line is excluded and clearly marked as partial coverage. The utility does not delete older output files if a later invocation fails; always inspect its exit status.

For source-tree use: `python3 scripts/run_diagnostic.py ...`. Installed locally at `~/.local/bin/dsco-run-diagnostic`; removal of that companion command reverses the installation without affecting the native runtime. Tests: `make test-run-diagnostic` (also included in `make test`).

## What it measures

- Paired model requests, wall time, summed transport duration, request-span residual, and the union of overlapping request spans.
- Completed tool-call counts, reported duration and output volume.
- Initial/final input tokens, cumulative token categories, and serialized request bytes.
- Recorded queue waits, where available.
- Estimated inference cost separately from invoiced cost, with missing samples retained as unknown rather than zero.
- Signals for large residual waits, context growth and excessive serial model turns.

It exports selected metadata, not prompt text, tool arguments, tool results, credentials, or request-blob contents. Reports are local artifacts, not automatically approved for external publication.

## Attribution boundaries

Request-span residual is **not proof of queue wait**, CPU time, or a particular provider defect. Chronicle event timestamps have second resolution. Tool sums and overlapping model requests cannot be added as disjoint wall-time categories. Queue and inference-cost events lacking trace correlation are summarized over the selected session/time window and labeled accordingly; retries or overlap can make their counts differ from paired requests.

A successful request/tool is **not** independently verified task completion. The diagnostic deliberately leaves cost-per-verified-outcome unset. It never authorizes deployment, changes an acceptance test, reroutes providers, or grants capability.

## Initial verification and business use

Nine deterministic tests cover pairing, overlap, unknown values, invalid timing/schema, duplicate spans, session isolation, partial/malformed logs, owner-only artifacts, and output privacy. The installed command reproduced the earlier audit's independently computed 2,126-second wall time, 37 requests, 51 tools and transport/cost totals. Analysis took about 21 ms on that retained log; a separate one-request trace also passed. These timings cover retrospective analysis only, not the original audit or future workloads.

This is a reusable delivery component for the existing AI Spend & Workflow Control Diagnostic—not new revenue, an approved price change, or proof that a customer has purchased anything. Before/after customer outcomes and customer authorization remain necessary for commercial claims.

Evidence: `reports/run-diagnostic/installation.json` and the two retained sample reports.
