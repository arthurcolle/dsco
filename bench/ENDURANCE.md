# DSCO endurance evaluations

For ongoing evaluations, use the [continuous discovery workflow](../docs/CONTINUOUS_MODEL_DISCOVERY.md).
`model_campaign.py prepare` selects from refreshed catalogs and previous measured
outcomes; the fixed manifests below are historical reproducibility fixtures.
`model_campaign.py record` feeds outcomes back into future selection.

Run the installed native DSCO binary on small, independently graded jobs. One
model can author test cases; a separate executable oracle must validate those
expectations before another model uses them. A model saying “done” is never the
grader. `endurance_grade.py` supplies the event-ledger oracle used in the October
6 pilot: model-authored cases, 300 seeded generated sequences, 27 invalid inputs,
and input immutability checks. The grader and its original test data live outside
the candidate workspace. Workspaces are isolation for reproducibility, not an OS
security sandbox.

`endurance_campaign.py` adds pacing and durable accounting to `harness_matrix.py`:

- Explicit attempt, active-time, and reference-cost admission limits.
- A single-writer lock and atomic state updates, with reservations saved before
  model invocation. Completed attempts are not rerun on resume.
- Interrupted attempts with uncommitted outcomes stop for inspection. Committed
  results can be reconciled after a controller crash without another model call.
- Consecutive verification failures trigger bounded backoff and then a stop.
- Subscription-only receipt checks by default; disabled fallback flags are
  required. Missing receipts, unknown cost, or route changes stop the campaign.
- Hashes bind the manifest, executable, runner, fixtures, and absolute grader
  files. Changed inputs require a new campaign directory.
- Child and grader deadlines use monotonic time; unread stdin cannot block the
  deadline loop. Process-group cleanup includes descendants ignoring SIGTERM.
- Unknown costs remain unknown in summaries. Reference inference estimates are
  distinct from subscription fees and provider-reported charges.

## Commands

From this repository, rerun the offline failure checks:

```sh
python3 tests/test_endurance_campaign.py -v
python3 tests/test_headless_tool_guard_binary.py --binary ~/.local/bin/dsco
make test-gate-claims
```

The measured pilot and frozen inputs are in `reports/endurance-20261006/`.
This bounded six-attempt command is the pilot; repeating it resumes the same
campaign and makes no new calls once completed:

```sh
DSCO_ALLOW_SECRETS=0 python3 bench/endurance_campaign.py \
  reports/endurance-20261006/repair-frozen.json \
  --output reports/endurance-20261006/campaign-frozen \
  --max-attempts 6 --reference-allowance-usd 1.5 \
  --max-active-seconds 1800 --interval-seconds 2
```

For a new, slower evaluation campaign, the Luna-only manifest has a $0.03
reference allowance per attempt. This example admits at most 48 attempts,
spaces them by 30 minutes after each completion, and reserves at most $1.44 in
reference inference allowances. It does not promise $1.44 in actual billing:

```sh
DSCO_ALLOW_SECRETS=0 python3 bench/endurance_campaign.py \
  reports/endurance-20261006/luna-campaign.json \
  --output reports/endurance-day-01 \
  --max-attempts 48 --reference-allowance-usd 1.44 \
  --max-active-seconds 86400 --interval-seconds 1800
```

For seven or thirty days, multiply the three limits by 7 or 30, and use a new
output directory. No model runs during the pacing interval. These are operator
commands, not already-running day/month experiments. Long campaigns need the
host awake and the controller running. Active-time accounting survives normal
interruptions; abrupt machine loss can lose elapsed time since the last saved
heartbeat, while the saved attempt reservation and unknown-outcome stop remain.

Inspect `campaign.state.json` for the heartbeat, active attempt, reservations,
failure streak, and stop reason. `summary.json` records per-model success,
latency, token/cache usage, RSS samples, and labeled costs. Per-attempt logs and
grader receipts are retained below `attempts/`. Exit 0 means the admitted attempt
count completed with every result verified. Exit 2 means a configured limit or
error stopped the campaign; inspect the reason. Exit 130 means interruption.
The runner does not silently reopen a terminal campaign or retry an uncertain
effect; inspect it and explicitly start a new campaign when appropriate.

## Evidence limits

A short pilot establishes these observed task results and exercised recovery
paths. It does not establish week/month uptime, provider availability, subscription
capacity, interactive TUI continuity, or general coding quality. This campaign
uses fresh DSCO processes per job; it does not test a single months-long context.
Fresh processes bound context growth and cumulative process-local resource leaks.
Future task suites should add distinct workloads and harder model-authored tests,
while retaining independent executable grading.

Reference allowances are admission accounting, not a provider-enforced billing
cap. DSCO checks its inference budget between responses, so an in-flight response
can exceed the remaining allowance. Subscription receipt checks detect an
unexpected lane after a response; they are not a remote billing firewall. The
native no-progress guard catches consecutive failures and identical observations;
the outer attempt deadline also bounds alternating or superficially active loops
in these campaigns. Deliberately detached processes can escape a process group;
this runner is not a container or a general-purpose sandbox.
