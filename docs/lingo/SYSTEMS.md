# Lingo across DSCO, Autobot, GraphSub and Chimera

Lingo runs one LuaJIT script against these existing systems through DSCO's governed tool dispatcher. Modules share the same invocation, arguments, tool-call budget, execution tier and session taint. They do not start a new DSCO process for each operation.

The [core language reference](../LINGO.md) defines the object, calculation, scenario and dependency semantics. This document defines its callable system bindings.

| Import | Implemented role |
|---|---|
| `lingo` | Types, objects, calculations, scenarios, JSON and raw tool receipts |
| `lingo.platform` | Shared typed Service/Object/Observation/Policy/Task/Run/Artifact/Model/Agent classes over explicit inputs |
| `lingo.workspace` | Publish source-bound stored-world artifacts to GraphSub; reopen by exact node ID and SHA |
| `lingo.dsco` | Scriptable handles for the current DSCO tool registry, discovery, checked calls and explicit JSON decoding |
| `lingo.autobot` | Explicit Tool Management discovery and invocation of the exact registered remote tool |
| `lingo.workflow` | Immutable ordered workflows, bounded Autobot execution, durable receipt inspection and key reconciliation |
| `lingo.graphsub` | Native GraphSub live attachment, health, dynamic schemas, node pages and object reads |
| `lingo.operator` | Compatibility name for the same GraphSub module |
| `lingo.chimera` | Immutable routing preferences, actual decisions and guarded execution of eligible direct plans |

All modules are embedded at build time. `require` accepts these exact names. Importing a module has no network effect. The CLI starts a lightweight script host: remote tools become available through explicit discovery without eagerly starting plugins, MCP servers, browser profiles or IPC. Restricted tool profiles and nested capability checks still apply.

## One script connecting the systems

[cross-system.lingo](../../examples/lingo/cross-system.lingo) executes this sequence:

1. Read an input file through DSCO's `read_file` tool.
2. Attach to GraphSub and read its dynamic-schema catalog and a node page.
3. Discover a specifically requested Autobot tool and invoke it with the file observation and graph IDs.
4. Ask the actual Chimera router to select a bounded route using explicit preferences.
5. Compose the observations and receipts, create a DSCO buffer, verify its content, and save a JSON artifact.

The example expects a known text-transform tool whose documented input is `{text=...}`. It requires an exact original remote tool ID or discovered DSCO name; it never executes the first search result merely because its score is highest. Service observations remain independent live reads. The saved file is a local artifact, with no implication that a distributed transaction committed.

The [platform program](../../examples/lingo/platform-observations.lingo) imports
these actual response envelopes into shared typed objects, computes Task eligibility,
explores a policy scenario and reopens the same world. The service verifier runs this
stage against freshly captured results in two separate native processes. See
[shared classes](../LINGO_PLATFORM.md), [stored worlds](WORLDS.md), and the
[retained service and foundation evidence](../../reports/lingo-foundation-20260910/services/services-verification.json).

With the three services already configured and a known pure `local:uppercase` tool registered in the selected Autobot service:

```sh
./dsco lingo run examples/lingo/cross-system.lingo \
  '{"input":"README.md","output":"/tmp/lingo-summary.json","autobot_query":"uppercase","autobot_tool":"local:uppercase"}'
```

Select a new output path. The example uses the normal buffer/save semantics and does not add a distributed rollback or an implicit overwrite policy.

## DSCO scripting

```lua
local d = require("lingo.dsco")
local read = d.tool("read_file")
local receipt = read:call {path=args.input, limit=20}
assert(receipt.ok, receipt.result)
return receipt
```

`d.tool(name)` creates an immutable handle; it does not execute the tool or grant authority. Handles have `name`, `:call(inputs)`, `:run(inputs)` and `:json(inputs)`. The equivalent functions are `d.call(name, inputs)`, `d.run(name, inputs)` and `d.json(name, inputs)`.

- `call` preserves `{ok, tool, result, sequence}` from the core bridge.
- `run` raises if the dispatcher reported failure, otherwise returns result text and the receipt.
- `json` performs `run`, then strict JSON decoding, and returns the decoded value and receipt. The target tool owns application-level meaning; this generic function does not guess whether a domain status denotes success.

`d.discover{query, limit, offset}` returns the normal registry discovery result and receipt. Limits are 1–16, default 8; offsets are 0–1,000,000. Query strings are optional, up to 512 bytes. Local registry inspection can work offline. Any remote discovery goes through the separate network-gated Autobot discovery operation.

Existing tools retain their own schemas and semantics: file operations, buffers, plans, agents, execution receipts, workflows and other registered capabilities are available according to the current host and grants. A Lingo wrapper does not change the running agent's model by changing display metadata.

## Autobot discovery and execution

```lua
local a = require("lingo.autobot")
local found = a.discover {query="uppercase", limit=4}
local selected
for _, entry in ipairs(found.matches) do
  if entry.remote_tool_id == args.remote_tool_id then
    selected = found.tools[entry.name]
  end
end
assert(selected, "the requested remote tool was not discovered")
return selected:json {text=args.text}
```

`discover` accepts `source="tool_management"` (the default), a nonblank query of 1–512 bytes and a limit of 1–16. It returns `{matches, tools, receipt}`. `matches` contains actual remote input/output contracts; `tools` is indexed by each exact discovered DSCO name. Successful empty discovery returns empty collections. Unavailable service, invalid metadata and authentication failure raise explicit failures rather than falling back to a similar local tool.

Each handle captures its identity and copied metadata. It exposes `:describe()`, `:call(inputs)`, `:run(inputs)` and `:json(inputs)`. Every invocation is a new gated call of the registered `tm__...` leaf, using the existing Tool Management client and its execution contract. The native client derives names from the readable remote ID plus a SHA-256-derived suffix so punctuation-normalized IDs cannot overwrite another handle's target. Use the returned name or original `remote_tool_id`; do not synthesize an alias.

The Tool Management execution envelope is `{tool_id, tool_name, status, output, error}`. `run` and `json` require `status="success"` and no non-null error. HTTP 200 with `status="error"` is a failed operation. The raw `call` receipt remains available for diagnosis: the legacy external bridge's `ok` can mean that response bytes were returned, so it is not an application-success assertion.

This binding reaches whatever tools the selected authenticated catalog exposes. Autobot Engine has additional task/agent/workflow services; those are reached through their actual registered contracts when available. The verified local integration uses the real Tool Management application and its existing deterministic text callable. It does not claim a Temporal cluster or Autobot worker fleet was launched.

## Chimera preferences that affect real routing

```lua
local chimera = require("lingo.chimera")
local policy = chimera.preferences {
  quality_weight=2, benchmark_weight=1,
  cost_weight=0, latency_weight=0,
  require_known_price=true,
}
return policy:plan {
  task="Explain durable distributed coordination in one paragraph.",
  strategy="direct", max_calls=1, max_output_tokens=64,
  max_expected_cost_usd=0.20, max_worst_case_cost_usd=0.25,
}
```

The native `chimera_route` tool posts to `/v1/route` with model identity `dsco-router/chimera:latest`, the task as a user message, normalized `routing_policy` and `orchestration_policy`. This selects the existing Chimera implementation and its pinned artifacts. It does not substitute DSCO's unrelated `chimera_plan` capacity calculator or Router's generic `auto` selector.

`preferences{...}` returns immutable per-script preferences. `:inspect()` returns a defensive copy of all effective defaults and overrides. `:plan(request)` obtains a decision. The returned envelope includes `profile="chimera_plan"`, `scope="request"`, `executed=false`, observation time, normalized policy/request and the full native decision with selected model, provider request patch, orchestration and artifact identities. An infeasible constraint raises `route_refused`, which can be handled with `lingo.try`. The selected model and provider request patch must agree with the admissible orchestration role and a model-call stage. The ranking explanation can still record a different pre-budget winner; it is not the selected execution route.

Weights are relative scoring coefficients, not percentages that must sum to one. Zeroing `quality_weight` does not also zero `benchmark_weight`. Inspect the complete policy when comparing preferences. The [preference comparison example](../../examples/lingo/chimera-preferences.lingo) explicitly sets both and demonstrates refusal and immutability.

The tested operational checkpoint has no separate semantic-quality head, so its `quality_weight` currently weights the completion-success estimate. `benchmark_weight` separately weights benchmark-derived quality priors. Neither is a measured guarantee about the next answer. Candidate cost, latency and quality thresholds constrain estimates, not actual billing or an SLA. In this Router version, provider allow/deny lists match the model author's prefix (such as `anthropic` or `openai`), not a billing or subscription transport lane such as `openai-codex`.

| Preference family | Fields |
|---|---|
| Objective coefficients | `success_weight`, `quality_weight`, `benchmark_weight`, `preference_weight`, `cost_weight`, `latency_weight`, `failure_weight`, `uncertainty_weight`, `quality_lcb_z`, `missing_metadata_penalty` |
| Candidate constraints | `max_cost_usd`, `max_latency_s`, `max_failure_probability`, `quality_floor`, `min_context`, `required_region` |
| Exact inclusion/exclusion | `allowed_models`, `denied_models`, `allowed_providers`, `denied_providers` |
| Capability requirements | `required_modalities`, `required_output_modalities`, `required_parameters`, `allow_additional_output_modalities` |
| Data/routing eligibility | `zdr_required`, `require_known_price`, `include_expired`, `allow_batch`, `allow_router_models` |

Lists contain at most 25 strings, each 1–200 bytes. Numeric coefficients and cost/latency limits are finite, nonnegative and bounded to 1,000,000; failure probability and quality floor are 0–1. `min_context` is an integer up to 10,000,000. These are the binding's admitted fields; their runtime interpretation belongs to the selected Router and pinned model/catalog data.

Planning accepts `task`, `max_output_tokens`, `strategy`, `max_calls`, `max_expected_cost_usd`, `max_worst_case_cost_usd`, `latency_preference`, `max_expected_latency_s`, `max_worst_case_latency_s`, `allow_cascade`, `allow_parallel`. Task text is 1–16,384 bytes, output tokens 1–4,096, calls 1–3. Expected/worst-case cost bounds are at most $0.20/$0.25. Strategies are `auto`, `direct`, `cascade`, `parallel`. Defaults allow up to three planned calls; use direct/one-call constraints when that is the intended plan.

Planning estimates costs and chooses routes without issuing provider inference requests. It does not persist account defaults or change an existing DSCO session's model. Model identity alone does not prove an inference ran or that an artifact was promoted. The local tested manifest's promotion status remains `hold`.

### Execute the selected plan

An eligible plan returned by the updated Router exposes `plan:execute()`:

```lua
local chimera=require("lingo.chimera")
local plan=chimera.preferences{require_known_price=true}:plan{
  task="Return a concise explanation of this supplied source: ...",
  strategy="direct",max_calls=1,max_output_tokens=512,
  max_expected_cost_usd=0.01,max_worst_case_cost_usd=0.01,
  allow_cascade=false,allow_parallel=false,
}
local completion=plan:execute()
```

This is an explicit effect through `chimera_execute`. The request and selected plan are captured privately; editing the displayed observation does not edit the executable request. Execution takes no overrides. The Router compares request/plan hashes before dispatch and refuses a changed plan. It reserves a principal-scoped execution ID in its existing run journal and refuses reuse. The profile issues one direct OpenRouter call, disables hosting-provider fallback, and makes no automatic retry. A fresh request requires a new plan. Older Router instances can still return planning observations but cannot supply the guarded execution contract.

The completion carries execution/run identity, requested and provider-reported model identity, measured elapsed time and provider-reported usage when available. Missing usage remains unknown. The $0.01 limits are admission estimates based on known prices, not guaranteed final billing. Output tokens are bounded to 512; native HTTP waits at most 55 seconds and Router dispatch at most 45 seconds. A timeout, malformed completion or server failure can leave an unknown remote outcome. Task acceptance must inspect the actual content separately from transport completion.

### Execute a workflow and inspect its evidence

`lingo.workflow.create{...}` defines up to 16 steps using `passthrough` or `map`, explicit earlier-step inputs and exact field mappings. `specification:execute{inputs=...,idempotency_key=...}` submits to the existing Autobot owner. `read(execution_id)` retrieves its record; `reconcile(idempotency_key)` performs a read without resubmitting. Definitions and inputs bind the retained key; changing either conflicts. Map steps admit at most 32 items and the run at most 128 remote calls. There are no automatic retries or resumable Lua stacks. See [WORKFLOWS.md](WORKFLOWS.md) for the full contract and runnable release evidence desk.

## Connections, authority and reproducibility

| System | Host configuration |
|---|---|
| GraphSub | `GRAPHSUB_HOST`, default `http://127.0.0.1:7879`; optional `GRAPHSUB_API_KEY` |
| Autobot | Existing `TOOLS_API_URL` / `TOOL_MANAGEMENT_API_URL` and `TOOLS_API_TOKEN` / `AUTH_TOKEN` / configured local credential fallback |
| Chimera | `CHIMERA_HOST`, default `http://127.0.0.1:8788`; `CHIMERA_API_KEY`, falling back to `DSCO_ROUTER_API_KEY` |

Scripts cannot override endpoints or supply credentials in these APIs. Native GraphSub and Chimera requests accept origins, verify remote HTTPS and disable redirects and proxies; plaintext is limited to loopback. Autobot retains the existing Tool Management transport, with discovery bounded to a ten-second total deadline and a 1 MiB raw response before its 128 KiB normalized-result limit. GraphSub and Chimera use three-second connect/ten-second total deadlines and 128 KiB response bodies. Oversized, malformed and precision-unsafe structured results fail explicitly.

Every real interaction passes through the capability gate. `DSCO_ALLOW_NET=0` denies remote operations; host-scoped network grants are checked against the actual configured destination before connecting, including discovered Autobot execution. Cloud destination policy also applies when the cloud runtime is active. Write restrictions still apply to buffer saves. A prior credential read plus untrusted ingress remains visible across modules to the same-session flow guard. Calculations and scenarios reject external interactions from every module. Collect observations first, calculate or compare scenarios second, then act explicitly.

Individual discovered Autobot tools retain the existing client timeout (`TOOLS_API_TIMEOUT`, default 60 seconds). Read requests can retry within their deadline. Mutating individual tool requests are not retried unless the existing client has an explicit idempotency key. The workflow binding instead requires a caller key and makes one HTTP attempt, with a server deadline of 1–120 seconds and five additional seconds for the response. Workflow read/reconcile requests have a 15-second deadline. Its configured origin must be HTTPS or numeric loopback HTTP. A timeout is not proof that a remote effect did not happen; no binding promises exactly-once effects. GraphSub browse and Chimera planning also issue one HTTP attempt per call.

To reproduce the complete integration using the actual sibling repositories and their existing built binaries/environments:

```sh
python3 scripts/verify_lingo_services.py \
  --report-dir reports/lingo-systems-local
```

The launcher creates fresh local service state, registers the existing Autobot uppercase callable in the real Tool Management app, generates temporary authentication credentials, runs the combined script, verifies Router execution/billing tables remain empty, and stops its own service processes. It requires the native GraphSub release binary plus Autobot and Router `.venv` environments. Missing dependencies fail explicitly. Endpoint/path overrides are available in `--help`.

Run `make test-lingo`, `make test-lingo-operator`, `make test-lingo-systems` and `make test-gate-claims`. The systems test distinguishes adversarial transport fixtures from optional actual-service proof. See [retained cross-system evidence](../../reports/lingo-systems-20260909/REVIEW.md) and the [GraphSub browse contract](../operator/LIVE_BROWSE.md) for tested identities and remaining boundaries.
