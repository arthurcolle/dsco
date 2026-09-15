# Lingo workflows and release evidence

A Lingo workflow is an immutable description of work that Autobot executes. The program specifies named tools and how their results feed later steps; Autobot owns execution records and idempotency. Lingo's world then turns recorded results into a typed, explainable decision. GraphSub can retain that world, and the [workbench](WORKBENCH.md) can vary its policy without executing the workflow again.

The [release desk](../../examples/lingo/release-desk.lingo) applies this to a concrete question: **does the current DSCO integration have current evidence for its Autobot, Chimera and GraphSub contracts?** A prior report can say PASS while referring to an older binary. That is a real evidence defect. The workflow records it, runs focused validators against the current candidate, and produces a new decision from the resulting receipts.

## Run the complete local proof

From the DSCO CLI checkout:

```sh
python3 scripts/verify_lingo_release.py --binary ./dsco --report-dir reports/lingo-release-20260910
```

The [verifier](../../scripts/verify_lingo_release.py) creates an actual Autobot app, persistent tool/execution registries, an MCP worker and a native GraphSub engine on unused loopback ports with fresh local storage. Autobot authentication stays enabled with generated credentials. It registers the worker in Autobot's persistent tool store; no execution route or result is replaced with a fixture. Child environments omit provider keys. The workflow does not run inference or deploy anything.

The retained reports include the composition receipt, exact source/binary digests, original and refreshed evidence, GraphSub artifact address, reconstructed world, idempotent replay, key reconciliation, and policy comparisons. It restarts Autobot before reconciling the original execution and kills/restarts GraphSub before reopening the exact release artifact. The verifier then shuts down its owned services. Its final local session remains directly usable without them:

```sh
./dsco lingo open examples/lingo/release-desk.lingo --restore reports/lingo-release-20260910/release.session.json
```

At the prompt:

```text
set 1 "historical"
why
set 1 "refreshed"
set 2 0
set 2 120000
set 3 1000000000
reset
inspect evidence:refreshed
quit
```

Historical evidence yields `HOLD_STALE` when its recorded binary or source hashes differ, or when it lacks coverage for a declared candidate input. Refreshed passing evidence yields `PASS`. A validation-time budget below the recorded duration yields `HOLD_VALIDATION_BUDGET`; an assumed future review time can yield `HOLD_AGE`. These operations change a local scenario over the same observations. They do not collect new source state or call workers.

## The actual work

The [worker](../../scripts/lingo_release_worker.py) exposes three bounded MCP tools. Workspace roots, executable paths, commands and output directories belong to its host configuration; workflow inputs cannot replace them.

| Stage | Work and retained evidence |
| --- | --- |
| `lingo_release:collect` | Hash the current DSCO binary, relevant native/Lua modules, Autobot composition sources, Router code and pinned routing artifacts, the GraphSub binary, and validators. Read the existing historical integration report and compare its binary digest. |
| `lingo_release:verify` | Run the existing actual three-service integration verifier, MCP/source/output-schema comparison, four focused Router budget/model-identity tests, and the real GraphSub kill/restart/reconstruction proof. Retain report digests, case counts, outcomes, elapsed time and the actual route-plan observation. |
| `lingo_release:seal` | Re-read the candidate fingerprints and require the verified worker state. Emit a compact evidence record bound to the unchanged candidate. |

Each stage consumes an actual predecessor result. Collection creates an opaque run ID. Later steps receive that ID through explicit mappings and resolve their evidence in the worker's retained state, so they cannot forge a successful predecessor by submitting their own report. The workflow has a 120-second deadline; fixed child validators have shorter deadlines. Dependency or validator failures are reported, without substituting synthetic success.

The existing validators remain individually usable:

```sh
python3 scripts/verify_lingo_services.py --binary ./dsco --report-dir /tmp/lingo-services
/usr/bin/python3 tests/test_lingo_systems_contract.py ./dsco --live-report /tmp/lingo-services/systems-services-tests.json --report /tmp/lingo-contracts.json
../dsco-router/.venv/bin/python -m pytest -q ../dsco-router/tests/test_chimera_plan_identity.py
python3 scripts/verify_lingo_workspace.py ./dsco --report-dir /tmp/lingo-recovery
```

The worker uses private report and test-database paths, a restricted child environment, and no pytest cache. Reports distinguish executed binary identity from observed source identity: hashing a checkout alongside a binary is not a compiler attestation. The explicit dependency list is the scope of this review, not a claim that every repository behavior was tested.

## Define and execute work

The release program contains this ordinary Lua declaration:

```lua
local wf = require("lingo.workflow")
local release = wf.create{
  id="integration-release-evidence",
  name="Integration release evidence",
  version="1",
  timeout_seconds=120,
  steps={
    {id="collect", tool="lingo_release:collect", mode="passthrough"},
    {id="verify", tool="lingo_release:verify", mode="passthrough",
      input_from="collect", input_mapping={run_id="run_id"}},
    {id="seal", tool="lingo_release:seal", mode="passthrough",
      input_from="verify", input_mapping={run_id="run_id"}},
  },
}

local receipt, host_receipt = release:execute{
  inputs={},
  idempotency_key="my-release-attempt-001",
  trace_id="my-release-review-001",
}
assert(receipt.status == "completed", "inspect the failed execution")
```

Creating or inspecting a specification is pure. `spec:inspect()` returns a copy. `execute`, `wf.read(execution_id)` and `wf.reconcile(idempotency_key)` use the native `autobot_workflow` tool and the existing capability gate. The host chooses Autobot through `TOOLS_API_URL` and `TOOLS_API_TOKEN`; the workflow cannot choose an arbitrary service URL. `DSCO_TOOLMGMT=0` disables this integration.

The supported Autobot execution profile is `lingo.v1`. A specification has 1–16 unique steps and a 1–120 second deadline. Steps use `passthrough` or bounded `map`. `input_from` must refer to an earlier step; otherwise the previous output is selected. A mapping names exact top-level keys, with `$` representing the whole selected input. This is data wiring, not an expression evaluator. The profile has no embedded Python, condition strings, provider transformations or automatic retries. Its backend validates the profile independently of the Lua facade.

The returned owner receipt includes `execution_profile`, `request_hash`, `execution_id`, `workflow_id`, `status`, `output`, `step_results`, `duration_ms`, `trace_id` and `idempotent_replay`. The second return value is the native DSCO tool receipt. An application failure remains a failed owner receipt that the program must inspect; a transport failure throws instead of fabricating an execution outcome.

Use the same original key and inputs when recovering an uncertain response. Reconciliation reads Autobot's retained execution by key. Identical submissions reuse the record; changing the request while reusing its key produces a conflict before workers run. Idempotency lasts for the retained execution record's lifetime. A missing record is not proof that no earlier effect occurred. A deadline or lost response likewise does not roll back completed tools. Completed-record recovery is tested; this surface does not automatically resume interrupted steps.

## From receipts to a reviewable world

The release program declares immutable `Evidence` and `Execution` objects plus a mutable `Release` policy. Historical and refreshed evidence remain separate objects. Its derived decision first requires evidence for the current candidate and passing checks, then applies observation-age and verification-duration limits. Relaxing those latter policy limits cannot turn evidence for the wrong binary into evidence for the current one.

`workspace.publish(world)` saves stored values through the native GraphSub binding. Derived decisions and caches are absent from the stored image. The verifier opens the returned artifact in another DSCO process and requires the same decision and source-bound value address. It then opens the live desk, changes policies with zero worker calls, saves the desk explicitly, and restores it with network and writes disabled.

The GraphSub artifact retains compact evidence and report digests; full validator logs remain in the report directory. The artifact address refers to the verifier's private engine storage, which is removed on cleanup. The retained `world.json` and local session are the portable outputs of this proof. Use an explicitly configured persistent GraphSub service when a long-lived remote artifact is required.

`PASS` means the stated evidence obligations passed for the captured candidate under the selected review policy. It does not promote Chimera models, deploy the platform, authorize provider calls, or prove that outside state has remained unchanged since collection. Refresh is an explicit new workflow execution; policy comparison and saved-desk restoration reuse the captured evidence.
