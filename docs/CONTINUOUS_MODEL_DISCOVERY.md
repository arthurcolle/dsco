# Continuous model discovery and evaluation

DSCO's evaluation inputs come from refreshed catalogs. A frozen benchmark run
remains reproducible, while the next evaluation selects from current discovery
and prior measured outcomes. Model IDs do not need to be edited into the selector.

## Discovery running on this machine

The macOS LaunchAgent `com.dsco.model-lab-watch` runs at load and every 300 seconds
while the user's session is active. It executes `scripts/model_lab_watch.py`.
Catalog refreshes make metadata requests, not inference requests.

Sources include the native provider registry's catalog endpoints, the Codex
account catalog from `codex debug models`, public gateways, and recently created
Hugging Face Hub repositories. Hub discovery follows the documented public API
and pagination, uses conditional HTTP cache headers, and retains unfinished
pagination across polls. It always checks the newest page. Bootstrap covers the
preceding 24 hours plus boundary overlap; each pass admits at most ten pages.
Coverage and pending cursors are reported rather than implying a full Hub census.
Hub publication does not supply an inference endpoint or prove tool capability.

Sources: [Hugging Face Hub API](https://huggingface.co/docs/hub/en/api) and
[OpenRouter model catalog](https://openrouter.ai/docs/api/api-reference/models/list-all-models-and-their-properties).

Inspect the current data in `reports/model-labs/watch/`:

- `candidates.sqlite`: persistent model metadata, feed state and task-specific
  evaluation history, keyed by provider and exact model ID. Unknown publishers
  enter this index without changing the curated lab registry.
- `candidates.summary.json`: source health, observation time, coverage and counts.
- `evaluation-queue.json`: current catalog candidates. This is not a list of
  qualified or entitled accounts; selection applies task-specific evaluations.
- `LATEST.md`: human-readable source changes and coverage.

Failed feeds keep their last successful observations. Their models cannot enter
a fresh evaluation through that failed feed. Partial or incremental listings do
not declare absent models removed. Successful full listings can mark records
unlisted while preserving history. Source observation times survive saved-snapshot
imports; generating a new report does not make old catalog evidence fresh.

```sh
python3 scripts/model_lab_watch.py
python3 scripts/model_lab_watch_schedule.py status
python3 scripts/model_lab_watch_schedule.py install --interval-seconds 300
```

Rollback the periodic job with `python3 scripts/model_lab_watch_schedule.py
uninstall`. The previously installed daily plist is retained in
`reports/model-discovery-20261006/before/installed-watch.plist`.

## Evaluate current candidates

Use an existing harness manifest as the task, provider and budget template. Its
model names are replaced during selection. Each allowed provider needs an explicit
native adapter policy; different account/budget policies cannot be collapsed into
one provider template. Selecting a provider for evaluation is separate from
indexing its models.

```sh
python3 bench/model_campaign.py prepare \
  reports/endurance-20261006/repair-frozen.json \
  --provider openai-codex --limit 4 \
  --output reports/my-fresh-evaluation.json

DSCO_ALLOW_SECRETS=0 python3 bench/endurance_campaign.py \
  reports/my-fresh-evaluation.json --output reports/my-fresh-evaluation-run \
  --max-attempts 4 --reference-allowance-usd 1 \
  --max-active-seconds 1200 --interval-seconds 2

python3 bench/model_campaign.py record reports/my-fresh-evaluation.json \
  --run reports/my-fresh-evaluation-run
```

Preparation refreshes missing, failed or stale selected catalogs; the default
maximum source age is 900 seconds. It fails if no fresh unevaluated candidates
remain, rather than substituting a remembered model name. Selection prioritizes
unmeasured/expired and changed candidates, then the account catalog's priority
and reported release date. This is an exploration order, not a quality ranking.
Hidden models, explicit absence of tools, known non-text output, and Hub-only
publications are excluded from the worker evaluation queue but retained in the
index. Unknown capability must be established by the bounded test.

The `.selection.json` sidecar preserves source identity, catalog metadata digest,
task identity and selection time. The run freezes its chosen models. Completing
`record` binds the outcome to model metadata, tasks, fixtures, grader bytes,
executable, environment policy, and budget. It records successful runs separately
from correct artifacts and retains cost/latency evidence. Provider/model receipt
mismatches cannot qualify a result. Inference lane receipts remain distinct from
catalog inclusion and credential availability.

Successful and failed evaluations suppress immediate repeat testing of the same
model/task/policy combination for 24 hours. Changed model metadata or test inputs
requires another evaluation; expired outcomes can be retested. Thus later
preparations move through the candidate pool instead of repeatedly selecting the
same names. Increasing a failed model's budget is a new policy evaluation, not
retroactive success. Runtime defaults are not promoted merely from publication
date or catalog presence.

The periodic job performs discovery only. Model evaluation runs require explicit
provider templates and resource limits, so thousands of new catalog entries do
not trigger thousands of paid inference jobs. Current subscriptions remain the
default for the supplied examples. Reference-priced budgets are not invoices or
provider-enforced billing caps; an in-flight response may overshoot the child
budget before the next admission check.

## Verification

```sh
python3 tests/test_model_discovery_pipeline.py -v
python3 tests/test_endurance_campaign.py -v
make test-gate-claims
```

Live discovery/selection/qualification evidence and the preserved original files
are in `reports/model-discovery-20261006/`. Auth failures and unavailable endpoints
remain explicit coverage gaps. This index is not a census of every model worldwide
and does not attest to every listed model's availability, quality or licensing.
