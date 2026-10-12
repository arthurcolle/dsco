# Native Jev model routing

Jev now has a native C model-selection path, not just notebook lane labels. This is an **explicit-opt-in local feature**, not a production fleet migration or a demonstrated quality/cost win.

## Use

```sh
# Installed opt-in launcher; existing model/provider pins remain authoritative.
dsco-jev --provider anthropic -p 'Reply with exactly READY.'

# Equivalent for this one process:
DSCO_ROUTER_POLICY=jev dsco --provider anthropic -p 'Your current task'

# Actual Jev recommendation, without dispatching a completion:
dsco --jev-route 'Explain a hash table in two sentences.' --provider anthropic
```

On a newly launched build, `/route policy jev` enables the policy in an interactive session. `/route policy fixed` disables automatic selection. `/route lock` and explicit model choices prevent Jev calls and switches; `/route unlock` deliberately clears the interactive lock. An already-running older executable cannot acquire this C implementation without a new launch; no active session is restarted by installation.

## Integrated paths

- Interactive: before the first inference for the current user input, once per input, not after every tool result.
- Headless one-shot: before completion request construction. A selected model is actually passed to the provider, not printed as a placeholder.
- ACP: current prompt uses the same adapter; `DSCO_ACP_MODEL` stays pinned and the ACP trust tier defaults to untrusted. Normal secret/network denials may therefore cause incumbent fallback.
- `--jev-route`: real selection-only diagnostic, explicitly reports `completion_dispatched:false`.

`src/jev_router.c` performs the typed request. `src/router.c::router_decide_for_task` builds a maximum-six-model shortlist from the existing native model registry and local complexity floor, capability tiers, latency hints and provider eligibility. It is NOT a sweep of every row in the external model-lab catalog. Explicit provider-family constraints may leave too few candidates; that returns to the incumbent rather than broadening the pin.

## Privacy and transport contract

Selecting the Jev policy or invoking `--jev-route` opts this process into sending **the current task text** to `https://api.typesafe.ai/v1/systemone`. No full conversation, prior tool outputs, system prompt, files, or credentials are added by the adapter. A user can still paste private text into the current task; do not opt in for data that is not authorized for that provider. Inputs over 2,048 bytes fall back rather than being silently truncated.

- Pinned model: `jev-1.13.0`; pinned rubric: `candidate-choice.v1`.
- Existing `tools_execute_for_tier("curl_raw", ...)`, caller trust tier and `CAP_SECRETS` checks. No gate exemption or taint reset.
- Credential expands inside curl, not into shell arguments/audit input. Curl config is disabled; HTTPS only, no redirects.
- Shell quoting is distinct from JSON escaping; apostrophes and shell metacharacters remain task data.
- One-second connect and two-second total timeout; 16-KiB response cap; existing 8-KiB curl command cap checked before dispatch.
- At most 32 attempts per process, no retry, including denied/failed transport attempts. After the ceiling the incumbent remains available.
- Exact candidate enum, answer count, model identity, usage type, finite probabilities and probability normalization validated. Provider output cannot introduce a new model or command.

## Existing authority remains

Jev recommends among locally filtered candidates. Model locks, provider pins, disabled cross-provider routing, escalation limits, cost-budget overrides, routability checks and tool gates remain in charge. Returned confidence is telemetry, **not a calibrated probability or authority grant**. An invalid response, missing key, denial or transport error uses the incumbent decision constrained by the same provider boundaries.

The ordinary default remains `fixed`. The `dsco-jev` launcher is the explicit mode; no global shell configuration or user-wide default is silently rewritten. Existing processes and drafts are preserved.

## Evidence and limits

Migration evidence: `.workspace/jev-native-router-migration-20260924/`.

- 11 mocked response/failure cases repeated across 220 isolated runs, plus selection, lock, provider-pin, fallback and receipt checks. These are NOT 220 distinct semantic routing-quality cases.
- Nine live-binary capability-gate checks passed.
- Existing compaction adapter and ACP protocol smoke passed.
- Live native Jev selection was observed; a separate real completion used the selected Claude Haiku model and returned `JEV_ROUTING_OK`.
- Request/resolved model, payload hash, usage, latency and validation status are exposed in the route receipt; `applied:false` denotes the selection client itself does not dispatch. CLI output separately states whether it dispatched a completion.

No held-out >=200-case model-sufficiency benchmark, calibrated abstention threshold, or matched-load p95/cost-savings claim is made. Native model-tier/price hints remain approximate and registry-bound. Jev overhead is separately visible via routing receipts and is not a provider-generation price quote. The broader promotion gates in `.workspace/harness-parity/22_JEV_DECISION_MIGRATION.md` remain unmet, not weakened.

Rollback: select fixed policy or stop using the launcher; the previous installed binary and scoped source patch are retained with hashes. Restoring a binary requires a drift check so a newer installation is not overwritten. No production deployment or universal routing policy was performed.
