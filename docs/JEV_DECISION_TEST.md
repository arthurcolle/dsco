# One-command Jev decision smoke test

```sh
cd ~/dsco/dsco-cli
./scripts/test_jev.sh
```

The default run sends six small synthetic cases through the existing governed
`jev_decide` tool: true/false arithmetic, easy/strong lane selection, abstention,
and a routing-injection attempt. It prints answers, probabilities/confidence,
latency, and PASS/FAIL against fixed smoke-test labels. It does not generate
completions, execute recommendations, change provider defaults, or install code.
These labels test an explicit routing rubric; they are not measured model
sufficiency or a production promotion evaluator.

## Live verification (2026-09-26)

With Arthur's explicit approval of a **test-process-only** secrets grant, the
live suite passed **6/6** cases: both arithmetic checks, easy/strong routing,
abstention, and the injection case. Receipts report 1,969 input tokens and
1,645.627 ms summed Jev request latency (not total script runtime). Invoice
cost is unknown. No downstream model completion was dispatched.
Evidence: `reports/jev-test/run-20260926T225550Z-qve3bK/summary.json`.
The seven offline harness tests also pass. Six examples are not a general
quality, calibration, or injection-resistance guarantee.

The approved invocation was:

```sh
DSCO_ALLOW_SECRETS=1 ./scripts/test_jev.sh
```

This grant was scoped to that invocation; the parent environment remains unset
and neither the script nor saved configuration was changed to enable secrets.
The initial ungranted run remains recorded at
`reports/jev-test-verified-20260926T2240/summary.json` (`secrets_denied`).
The existing `tool_jev_decide` wrapper passes a NULL tier to the Jev client,
which defaults to `standard`, even in a trusted shell. Subsequent live runs
still need an operator-authorized process-local grant unless this behavior is
separately repaired and reviewed. All other capability denials remain binding.

## Try your own question

```sh
./scripts/test_jev.sh 'Does proving a new theorem require deep reasoning?'
```

This returns a yes/no decision and `p_yes`. Custom questions are **unscored**,
not automatically declared correct. An uncertain probability is not factual
verification. Treat confidence as telemetry, not established calibration.
Do not submit sensitive text unless it is approved for TypeSafe.

## Exercise the native router, without a completion

```sh
./scripts/test_jev.sh --route 'Explain a hash table in two sentences.'
```

This calls `./dsco --jev-route` and accepts only an actual Jev selection or
abstention with `completion_dispatched:false`. It preserves model locks and
provider pins; `model_locked`, `insufficient_input`, missing keys, denials,
and other fallbacks are errors, not Jev successes.

The native selector is **not** the hosted gateway. The current
`--provider dsco-router` path has a shortlist/provider-family mismatch and
can return `insufficient_input` before contacting Jev. This script does not
repair that integration or claim a gateway completion occurred. See
[JEV_ROUTING.md](JEV_ROUTING.md) for native router boundaries.

## Setup, reports, and limits

- Uses checkout `./dsco` by absolute path; `DSCO_BIN` can override it for tests.
- Requires `jq`, `python3`, configured `TYPESAFE_API_KEY`, and the existing
  secret/network/exec permissions. It never exports allow flags, clears taint,
  overrides trust, sources an env file as shell code, or retries a denial.
- The pinned Jev adapter calls TypeSafe System One. A suite has six calls;
  custom question/router modes have one. Each CLI invocation has a 20-second
  outer timeout; the C client also bounds its HTTP request. On transport or
  validation errors, stop immediately without retries. An HTTP timeout may
  still represent billable remote work; its usage is unknown.
- Reports: a fresh `reports/jev-test/run-<UTC>-<random>/` with raw stdout/stderr,
  normalized responses, `results.jsonl`, and `summary.json`. Directory mode 700;
  files are private to the user. Custom input is not separately saved by the
  script, but DSCO's ordinary audit behavior still applies. No key is printed.
- `--out NEW_DIRECTORY` chooses another report directory without overwriting
  an existing one. Request hashes/usage/latency come from client receipts.
  The summary's token count includes **reported** usage only; missing usage is
  not proof of zero billing. `cost_usd:null` means invoice cost is unknown.
- Exit codes: **0** expected labels/valid unscored result; **1** one or more
  wrong labels; **2** setup, denied, timeout, invalid response or router fallback.
- This is a smoke test, not a calibrated quality/cost benchmark. No production
  routing, capability gate, or promotion policy changes are made. Rollback is
  simply to stop using/remove this standalone script; the runtime is untouched.

Offline harness checks (no API traffic):

```sh
bash -n scripts/test_jev.sh
shellcheck scripts/test_jev.sh
python3 tests/test_jev_script.py
```
