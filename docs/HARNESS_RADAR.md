# Harness radar

The local radar inventories agent harnesses and adjacent projects, checks public primary sources, and records feature-change candidates without recurring LLM calls.

Open [the source-level post-SOTA review](../reports/harness-radar/POST_SOTA_CODE_REVIEW.md), [dashboard](../reports/harness-radar/index.html), [harness/product index](../reports/harness-radar/HARNESS_INDEX.md), [landscape comparison](../reports/harness-radar/REVIEW.md), or [complete catalog](../reports/harness-radar/CATALOG.md). The manual feature matrix has explicit documented/local/optional/extension/unknown evidence levels. Generated keyword-topic columns are not feature assertions.

## Commands

Run from the dsco-cli checkout:

```sh
python3 scripts/harness_radar.py sync
python3 scripts/harness_radar.py status
python3 scripts/harness_radar.py render
python3 scripts/harness_radar.py inventory /Users/arthurcolle --depth 8
python3 scripts/harness_radar.py diff 1
python3 scripts/harness_radar.py review 1 --status deferred --note 'Needs a DSCO reproduction fixture'
```

`diff` and `review` require an actual event ID from `CHANGES.md`; the number above illustrates syntax. Review states are pending, accepted, rejected and deferred. Accepting a change means it was triaged; it does not modify the curated feature matrix or claim runtime validation.

Force a bounded refresh of the core group:

```sh
python3 scripts/harness_radar.py sync --force --only openai-codex,anthropics-claude-code,anomalyco-opencode,can1357-oh-my-pi,earendil-works-pi,nousresearch-hermes-agent --max-requests 60 --max-api 0
```

`--force` bypasses the due-time check, including error backoff. Ordinary scheduled runs respect due times. Do not use force in the recurring job. The automatic collector does not use `gh` or stored GitHub credentials.

## Scheduling

```sh
python3 scripts/harness_radar.py install
python3 scripts/harness_radar.py uninstall
launchctl print gui/$(id -u)/com.dsco.harness-radar
```

The macOS LaunchAgent runs every 900 seconds, at low priority, with 32 source requests per tick, four concurrent network workers and at most two unauthenticated GitHub metadata API requests per tick. It runs only while the user's launchd session exists and the computer is awake. The job uses absolute interpreter, script, configuration, state and output paths. Its stdout/stderr go to `/dev/null`; operational results and source failures are persisted in SQLite and exposed by `status` and `source-status.json`. Launch failures before Python starts are visible through `launchctl print`.

After each source refresh, the same process runs the provider-free installed
harness conformance probe. It checks executable versions, the exact flags used
by DSCO's five named adapters, and the OpenCode/OMP ACP entry points. Results go
to `reports/harness-radar/interop-conformance.json`. This scheduled path never
invokes a model; `make test-agent-interop-live-invoke` is the separate real-task
gate. Real-task receipts classify authentication, rate-limit, billing, timeout,
and adapter failures and retain only output size and hash rather than model text.

The script is standalone research tooling. It does not change dsco's agent tool registry, grants, model settings, provider auth or C runtime. Downloaded material is parsed as inert data. It never executes upstream installers or instructions and never sends messages to other people.

## Refresh and resource policy

| Source | Nominal refresh |
|---|---:|
| Core six harness release feeds; Claude/omp changelogs | 15 minutes |
| Core documentation; secondary harness READMEs/releases | 6 hours |
| Long-tail READMEs/releases; commercial pages; discovery indexes | Daily |
| Licenses and GitHub metadata | Weekly |

These are due intervals, not deadlines: a bounded queue, network outages, errors and sleeping hardware can delay refresh. The core group has priority over the long tail. The dashboard reports last-success timestamps, errors and stale-source counts. A missing license filename is not proof that a project has no license. Initial broad metadata enrichment is intentionally gradual under the unauthenticated API budget.

HTTP ETags and Last-Modified validators avoid downloading unchanged content when supported. Content hashes suppress identical-body alerts even when a server returns 200. Each response is capped at 2 MB; oversized feeds remain explicit failures. Responses with 403/429 honor available rate-limit headers and back off; other failures use bounded exponential backoff. The last successful observation survives an error. One process lock prevents overlapping scheduled/manual runs.

The maximum normal schedule is 3,072 configured source requests/day while awake, including at most 192 GitHub metadata requests/day (eight/hour). Actual work is due-driven and usually lower. Redirects can add HTTP exchanges. No paid API, search service, model or embedding endpoint is called. Bandwidth and local disk/CPU are the remaining costs. This does not claim that the initial interactive research conversation was free.

SQLite stores latest normalized bodies and hashes, change diffs, review state and run receipts. Run receipts and bulky diff bodies have a 90-day retention window; source hashes and review decisions remain. Source bodies are bounded individually, not by a total database quota. Do not point the collector at unbounded numbers of sources without adjusting its budget and retention.

## Files and provenance

- `data/harness-radar/registry.json`: project identities, primary source URLs, discovery provenance, aliases and polling intervals.
- `data/harness-radar/feature-matrix.json`: curated feature observations, evidence levels and citations. Only manual review changes this file.
- `reports/harness-radar/`: generated catalog, dashboard, source status, discovery candidates, comparison and local evidence.
- `reports/harness-radar/interop-conformance.json`: latest no-token installed-harness contract receipt.
- `reports/harness-radar/interop-conformance-live.json`: latest explicitly requested real-task receipt, when run.
- `~/.local/state/dsco-harness-radar/radar.sqlite3`: cached public evidence, hashes, event diffs and operational receipts.
- `~/Library/LaunchAgents/com.dsco.harness-radar.plist`: the reversible local schedule.

Directory URLs supply discovery leads; their editorial capability scores and rankings are not copied. Project aliases collapse known transfers; unresolved renames may still produce candidates requiring review. The ACP archive parser reads only small regular `agent.json` entries in memory and never extracts files or launches their distribution commands. An ACP adapter is categorized separately from the proprietary or open runtime it controls.

`inventory` performs a bounded local directory walk without following symlinks or searching credentials, conversations, logs, or package contents broadly. It reads Git metadata for matching repositories and probes known PATH commands with `--version`. It skips many hidden/system/cache paths and Git worktree `.git` files; the output lists scope and limitations. This is not a full disk forensic census.

## Verification

```sh
python3 tests/test_harness_radar.py
```

Tests exercise baseline-versus-change handling, removal detection, conditional HTTP requests, rate-limit backoff, last-good evidence, Atom normalization, HTML escaping and registry URL constraints. Live public refreshes and a subsequent forced core refresh establish network operation. Version/help probes do not establish task performance, sandbox correctness or provider billing behavior. No C build is necessary because this task adds only standalone research tooling, data and documentation.
