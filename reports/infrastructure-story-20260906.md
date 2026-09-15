# DSCO infrastructure review: the package is the product

Reviewed 2026-09-06. Scope: local source, packaging, integration contracts, telemetry and training paths across GraphSub, agents.erl, Tool Management, dsco-router and dsco-cli. This is a bounded architecture/product review, not a production-readiness or security certification. No deployment or GPU training was performed.

## The story

Distributed Systems is building an installable execution system for AI agents: shared context, supervised workers, discoverable capabilities, model routing and a native harness, with execution evidence feeding evaluation and model training.

A model is one replaceable compute component. The enduring product is the system that lets a customer put models to work, connect tools, retain state, inspect failures, control expenditure and improve the result without rebuilding the application around each new provider.

The strongest potential advantage is not the number of tools or agents. It is ownership of the complete execution-and-feedback path: what task was attempted, which context was available, which model and tools were chosen, what happened, what it cost, and whether the result actually met the objective. With permission and reliable labels, that evidence becomes training material and reusable evaluations.

The inspected repositories contain substantial parts of this system. They do not yet establish the full five-component, one-installer customer experience. The next integration milestone should make one complete path dependable rather than expand the advertised surface.

## What the components contribute

| Component | Evidence-backed role | Boundary of this review |
|---|---|---|
| GraphSub | Rust graph/storage/query infrastructure with API and agent-coordination surfaces. Its manifest includes memory mapping, persistent storage backends and native/Python library targets. | Core graph code must be distinguished from the separate agent adapter: that adapter keeps agents and pheromones in maps, and several endpoints remain placeholders. No scale, latency or distributed-consistency claim was tested. |
| agents.erl | Erlang/OTP supervision, dynamic agent processes, registries, scheduling and a timeline with disk append/replay. | Restarting a process is not proof of resumable task execution. Cross-component adapters and distributed agent-state recovery were not established. Some learning-related functions are explicit no-ops. |
| Tool Management | Python/FastAPI capability catalog and execution service, remote MCP backend discovery, persistent tool storage, durable task queue and multiple API surfaces. | It is more than a static tools list. However, full tenant/auth/side-effect behavior was not exercised; direct service execution is a separate boundary from the C harness gate. |
| dsco-router | Python model-routing service with provider adapters, API/auth/billing surfaces, telemetry and Chimera decision-model/LM assets. | Training and promotion plumbing exists; competitive model quality and a promoted live checkpoint were not verified. The router service is distinct from dsco-cli's local C router. |
| dsco-cli harness | Native C execution environment with model/tool interfaces, MCP, worker orchestration, explicit tool-execution gating and local Chronicle capture. | Version/help and focused offline tests were exercised. The full service stack, fresh install, capability-gate suite and deployment defaults were not validated. |

### Existing connections

- The harness includes a real Tool Management HTTP client, bounded discovery, execution entry points, connection/time budgets and circuit-breaker state: `dsco-cli/src/toolmgmt.c:1-118`; interface in `include/toolmgmt.h`.
- The harness knows `dsco-router` as a provider with its own base URL and model mapping: `dsco-cli/src/llm.c:258-262,283,301`. That is a concrete provider adapter, not proof the current router service is healthy.
- GraphSub client code exists in C, including registration, heartbeat, memory and tool-result methods: `dsco-cli/src/graphsub_client.c:175-275`. Client/server compatibility still needs repair and end-to-end tests.
- Targeted searches in `agents.erl/apps` did not find GraphSub, Tool Management or dsco-router-specific integration references. This is bounded negative evidence, not a claim that no integration exists anywhere.

## Material integration findings

### 1. The existing installer paths do not establish the requested package

`dsco-cli/npm/dsco/scripts/install.js:40-49,74-108` downloads/checks a dsco binary. `Formula/dsco.rb:16-35` builds/installs dsco. `scripts/install_atomic.sh` publishes a completed executable atomically; it is not a complete platform installer.

The parent `unified-platform.sh:15-22` hardcodes an old `~/dsco/graphsub` location and names graphsub, agents.erl, macro, native-mcp-core and telemetry. Its dependency check expects language build toolchains (`:37-48`). This is an older development orchestration surface, not evidence that the requested GraphSub/agents.erl/Tool Management/router/harness package installs on a clean machine.

The parent `INTEGRATION_TEST.sh:14-21,62-81,183-225` checks an older composition, treats 400/405 as evidence that some endpoints exist, and prints manual workflow instructions. It does not prove the five-part customer journey.

### 2. GraphSub has a concrete adapter mismatch and placeholder success paths

The C client posts an agent's identifier as `agent_id` (`dsco-cli/src/graphsub_client.c:233-249`). The inspected Rust registration handler reads `id` and generates a UUID when absent (`dsco-graphsub/src/api/agent_routes.rs:66-72`). Its heartbeat handler then requires the correct `agent_id` (`:116-145`). Without another translating layer, registration does not preserve the caller's identity as expected.

This same Rust adapter declares in-memory maps (`:23-27`), and its `memory_sync` and `tool_result` handlers return success while retaining TODO comments for persistence (`:497-535`). Its task routing returns `status: queued` with a placeholder routing policy (`:473-495`). A successful HTTP response is not evidence of durable storage or an actual queued job.

These are findings about this adapter, not a dismissal of GraphSub's separate graph/storage engine.

### 3. Erlang durability is more real than a superficial ETS read suggests

The timeline maintains ETS indexes, but also restores events from disk at initialization (`agents.erl/apps/agents/src/timeline_event_store.erl:139-145`). It persists before making an event visible in ETS or to subscribers (`:292-319`). Persistence appends NDJSON and calls `file:sync` (`:527-550`); startup replays records directly into indexes (`:552-595`).

A delegated review incorrectly inferred that ETS made this store memory-only. Coordinator inspection rejected that finding. The disk path is implemented. Its replay silently skips malformed lines and some read failures; operational error visibility merits follow-up. Full agent checkpoint recovery and cluster replication remain separate, unverified claims.

The OTP root supervision and transient dynamic-child semantics are implemented in `agents_sup.erl:19-50` and `agent_supervisor.erl:34-58,106-122`. Meanwhile `intelligent_query_router.erl:514-520` contains no-op analytics/feedback/model-update functions. Do not market those names as a functioning learning loop.

### 4. Telemetry exists, but privacy defaults and labels are product decisions

Chronicle contains SQLite tables for sessions, turns, LLM/tool calls, traces, token ledger and training examples (`dsco-cli/src/chronicle.c:293-356`). LLM records contain request/response blob references, usage and latency (`:926-953`); training examples have consent/sync-policy fields (`:1138-1175`).

Important: capture is enabled unless explicitly disabled in the inspected code (`:407-413`), and request/response blobs can retain content. A consent field is not proof that every downstream dataset/export consumer enforces it. Customer-facing defaults must clearly separate operational metadata, sensitive content capture, local learning and shared training.

The older dataset path defines `completion_success` from `turn_done` or non-error HTTP completion (`scripts/chimera_router/build_dataset.py:532-579,1229-1236`). Those are execution-health proxies, not independently verified task success. The newer feedback/dataset path has explicit label masks, revision snapshots, provenance and secret-rejection tests; that is a stronger foundation, but does not erase the older label semantics.

## The model-training story is already concrete

There are two distinct tracks worth describing accurately:

1. **Decision models.** `dsco-cli/scripts/chimera_router/train.py:67-204` builds bootstrap ensembles with outcome/cost heads, train-only normalization, calibration and saved artifacts. `evaluate.py:43-124` compares candidate and baseline on a test split, calculates uncertainty intervals, and checks Brier/ECE/coverage promotion criteria. This is implemented training/evaluation machinery, not proof of economic lift in production.
2. **Generative models.** `dsco-router/scripts/train_chimera_qwen38.py:18-55` defines full-model/LoRA SFT commands over a prepared corpus. Execution is conditional on `--execute` (`:57-78`). `modal_chimera_train.py` contains GPU training-job execution. `scripts/gate_chimera_lm.py:25-58` defines a candidate gate against same-data baselines using structured-output validity, tool-name validity, perplexity and warm-latency evidence. `routing/chimera_lm.py:28-39` marks the registered LM experiment promotion-required and disabled by default.

No GPU job was launched, no checkpoint was promoted, and no fresh evidence established that a DSCO model beats an external baseline. Training-script existence and passing plumbing tests do not establish model competitiveness.

The competitive thesis is nevertheless coherent: route to the best available provider now; use authorized, outcome-labeled experience to train task/decision models; move traffic only when held-out quality and cost justify it. Customer-owned local datasets and explicitly permissioned shared datasets can support different offerings without requiring ambient access to customer content.

## The customer story to tell

**Distributed Systems packages the infrastructure needed to run and improve AI agents: shared context, supervised execution, tools, model routing and observability, under customer control.**

The intended experience is one installation and one operational identity. A developer connects credentials, runs a workload, sees its model and tool calls in a single trace, understands the cost and failure, and can export eligible examples for evaluation or training. A better model can replace part of the compute without replacing the surrounding system.

That promise needs an acceptance test, not just an architecture diagram:

1. Clean machine installs pinned, verifiable component artifacts without a developer checkout.
2. A supervised agent performs an actual harness/tool/router/GraphSub workflow with shared run identifiers.
3. A deliberate interruption proves the documented restart/resume semantics without duplicating a side effect.
4. One trace links the input, model/version, tool/schema, graph changes, cost and independently checked result.
5. An authorized example exports with provenance and consent controls; a candidate can be evaluated, selected and rolled back.

These are proposed release criteria, not outcomes verified by this review. They retain the package as the product; one demonstration workload tests the integration rather than redefining the product as a narrow application.

## Verification performed

- `./dsco --version` and `./dsco --help`: runnable binary, reported version 1.1.0, build 29390f9.
- Trace-KG export/store tests: **12 passed** (1.089 seconds).
- Chimera dataset/feedback/fusion/planner/routing/model tests: **60 passed** (0.152 seconds) using the existing Homebrew Python/NumPy environment.
- Router Chimera LM/gate/corpus/scaling tests: **12 passed** (0.55 seconds) using the existing router virtual environment.
- An initial Chimera discovery run had five import errors due to a missing NumPy dependency in that interpreter. The corrected package-qualified run passed all 60 tests; no dependency installation was needed.
- Total successful focused checks: **84 tests**. This is not the entire suite or an end-to-end stack test.
- Three native DSCO review workers were launched. One returned a brief; two were stopped after exceeding the bounded review window. Final collection reported zero active workers. Worker outputs were treated as evidence, not accepted wholesale.
- No application source, configuration, governance policy, deployment or customer data was modified by the coordinator. Review artifacts and normal runtime/swarm bookkeeping were created.

## Provenance and remaining uncertainty

Observed repository HEADs: dsco-cli `29390f9`; agents.erl `c00e4e8`; dsco-router `f31ab27`; GraphSub complex `d88ed75`, nested Rust repository `52ade08`; dsco-autobot `f2788d5`. Local working-tree changes were present, so HEAD identifiers alone do not identify all inspected bytes; the companion receipt hashes selected source evidence.

This review establishes a credible integrated-product thesis and specific integration risks. It does not establish clean-install success, cross-service authentication/tenant isolation, full disaster recovery, production endpoint health, model quality gains, unit economics or customer demand. The immediate gap is a verified product boundary across existing components—not absence of infrastructure or absence of training code.
