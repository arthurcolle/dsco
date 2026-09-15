# Interactive reliability and request budgets

The shared `--tui` / `--native` path defaults to the selected model's context
capacity minus output reservation and 1,024 tokens of headroom. This includes
instructions, tool schemas, conversation messages and tool observations.
An unknown context capacity falls back to 32,768 estimated tokens. An explicit
`DSCO_MAX_INPUT_TOKENS` sets a smaller independent request ceiling when desired.

```sh
dsco --tui
dsco --native
DSCO_MAX_INPUT_TOKENS=16384 dsco --tui
```

Within either interface:

```text
/input-budget
/input-budget auto
/input-budget 16384
/evict
/compact
/cost
```

`/input-budget` changes this process's request limit; it does not edit global
configuration. `auto` restores model-aware admission; numeric values are
512–2,000,000. Invalid values, including zero,
do not disable admission. Model context capacity minus output reservation and
1,024 tokens of headroom may further lower the limit. If the output reservation
itself exhausts context, reduce `DSCO_MAX_TOKENS`.

## Request admission

Every interactive initial request, model fallback rebuild and context-overflow
rebuild passes the same guard. The local estimator uses complete JSON content,
one token per three ASCII bytes, conservative non-ASCII byte accounting,
structure overhead and a 512-token serialization reserve. Image blocks use a
4,096-token estimate instead of treating base64 bytes as language tokens.

Chat Completions and native Codex Responses honor the default compact tool proxy: at most 17 directly
advertised schemas, including execution/discovery tools and selected external
tools. The remaining catalog stays available through discovery and invocation.
An explicitly forced tool keeps its exact allowed schema; a denied/disabled
forced name is rejected locally. Explicit `DSCO_OR_MAX_TOOLS` or
`DSCO_TOOL_PROXY=0` selects the older broader schema policy, but does not bypass
the request input ceiling. The proxy directly advertises `context_status`,
`context_evict`, `context_compact` and `context_recall` subject to the operator's
tool allowlist, so recovery does not require a discovery round trip.

This is **not an exact provider tokenizer or a guaranteed upper bound** for all
text/media/model combinations. Actual metered usage remains separate. The guard
provides a deterministic ceiling on the local estimate; it does not claim a
provider can never reject a request for context length.

If needed, request-local projection shortens tool observations, then historical
assistant text, with head/tail excerpts and explicit omission markers. It does
not change the saved conversation. User text, system instructions, schemas,
tool IDs and call arguments are not shortened. If that protected content still
cannot fit, **no request is sent**: the UI explains the budget boundary and
returns to editable input. Before stopping, automatic compaction (when enabled)
gets up to three governed `context_evict` passes and request rebuilds under the
same limit, continuing only while the request estimate decreases. It archives
only completed old tool exchanges; the latest two messages,
user text and pending calls remain intact. Use `/evict`, `/compact`, fewer attachments, or an explicitly
larger budget. Nothing is silently authorized by increasing this limit.

`context_evict {"keep_recent":6,"max_turns":8,"min_bytes":2048}` archives
complete old tool call/result pairs, including large arguments. Each archive
is read back before the pair is replaced with excerpts and a `ck:tool:` key.
Archive failure leaves that evidence intact. Retrieve it with
`context_recall {"key":"ck:tool:...#b:0-4096"}`; slices avoid loading a large
archive back into the request. `/evict` runs locally even after admission stops
the model, keeping the latest two messages and considering up to 64 old pairs.

`context_compact` supports `aggressive`, `keep_recent` and `max_result_chars`.
It trims old observations and can collapse old tool calls into excerpts; this
is not a semantic model summary. Use eviction first when full archival is
needed. `/compact` explicitly invokes this operation without waiting for the
model-window automatic threshold. Both commands use the capability gate.
`context_status` distinguishes provider context usage from the latest local
request estimate and its separate admission limit.

The Chronicle `request.input_budget` event records the estimate before/after,
limit, admission decision and reduction count without copying prompt contents.
The input limit is per provider request, not a cumulative per-submitted-prompt
token or dollar allowance. Existing `/budget` limits remain separate. No new
monetary ceiling was enabled by this change.

## Honest costs

- Canonical input tokens exclude the separately recorded cached tokens. Cached
  input is included when calculating context occupancy, but is not charged a
  second time at the uncached reference rate.
- Repeated cumulative usage snapshots replace prior observations instead of
  accumulating them. Historical usage is not re-priced under a newly selected
  model.
- Provider-reported cost, reference estimate, accounted resource value and
  unknown/unpriced responses are distinct. Subscription reference value is not
  an invoice or evidence of an additional charge.
- A successful response with unknown price stays successful. Unknown values
  remain null in receipts and visibly unknown in the interface; they are not
  invented as zero. Accounting persistence/delivery failures still surface.
- Prompt summaries use a prompt-local delta, not the whole session's value
  beside that prompt's turn count. Adapter attempts, including returned failed
  attempts and fallback attempts, contribute to the session ledger. Internal
  transport retries without provider usage cannot establish metered spend.

Historical receipts and restored totals were **not rewritten**. Older sessions
can still contain the previously inflated estimates and require explicit
reconciliation. See the metadata-only [observed correction](../reports/interactive-core-20260908/cost-receipt-correction.md).

## Turn and input integrity

The composer preserves queued input across submissions, does not discard early
typing during startup, and decodes UTF-8 without blocking cancellation. Both
rendering modes share this input path.

Anthropic and Responses streams require protocol completion; HTTP 200 or EOF
alone is not a successful turn. Incomplete or malformed tool calls are never
executed. Once response text, reasoning or tool activity has begun, automatic
retry/fallback cannot replay the turn. A failed prompt and visible partial text
are retained, explicitly marked incomplete, and the composer becomes editable.
Failures remain failures; they are not cosmetically relabeled as success.

Final answers and incomplete-turn markers are checkpointed before the next
prompt. Autosave writes a private temporary file and fsyncs it before replacing
the previous checkpoint, reporting failure instead of silently truncating the
old file. The latest-autosave name remains shared between sessions; this is not
per-thread durable isolation or a claim of survival against every power-loss,
filesystem, hardware or provider failure.

## Regressions

```sh
make test-interactive-core
make test_native_compositor test-self-swarm-core
make test
```

These use owned PTYs, real subprocesses, and controlled local HTTP streams.
They exercise production request construction, parsing, tool execution,
accounting, checkpointing and native raster production. They do not use paid
model calls, control an existing user terminal, or establish physical-display
latency. [Verification artifacts](../reports/interactive-core-20260908/README.md)
identify the tested executable and remaining limits.
