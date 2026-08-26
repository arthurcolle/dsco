# Chimera advanced routing curriculum

## Mission

Build and operate a managed, cross-provider routing system that improves the
**accepted outcome per dollar** for a customer request while preserving hard
authority, privacy, spend, latency, and provider-account constraints.

This is not a curriculum for training a general-purpose foundation model. It
is the operating program for training, evaluating, promoting, and supervising
the candidate-conditioned **Chimera router** that decides:

```text
request + workspace authority + current provider/model offers + budget
    -> admissible candidates
    -> direct, cascade, or bounded parallel/synthesis plan
    -> explicit dispatch by the managed router
    -> immutable receipt + delayed outcome labels
    -> next training snapshot
```

The public router, paid workspace, provider credentials, and production ledger
are part of the system. The training loop must not be detached from product
economics.

## Non-negotiable boundaries

1. **No raw customer prompt or answer becomes training data by default.**
   Features, hashes, structured outcome labels, and explicitly governed
   evaluation data may enter the pipeline; raw content does not.
2. **Training never spends customer money.** Dataset construction, scoring,
   evaluation, and planning are local/offline. Only the explicit execution
   layer may invoke a provider.
3. **A route is not a truth label.** Chosen models have observed outcomes;
   unchosen candidates remain unknown, not failures.
4. **Every promotion is reproducible and reversible.** Pin catalog,
   telemetry high-water marks, feedback revision, artifact hashes, evaluation
   report, and deployment identity before a canary is eligible.
5. **Customer authority outranks model score.** BYOK/direct lanes, regional
   limits, ZDR requirements, credential availability, budget, tool support,
   and explicit provider/model policy filter candidates before learning ranks
   them.
6. **Hypercube/fusion is the evidence warehouse, not the route model.** It
   improves data coverage, cohort analysis, and post-mortems; it does not make
   a score or authorize dispatch.
7. **No automatic self-promotion.** Chimera may recommend a candidate
   artifact. A named operator reviews a passing report and explicitly promotes
   it through shadow and canary stages.

## Target operating architecture

```text
                        CONTROL PLANE
workspace / account / credit / provider authority / routing policy
                                  |
                                  v
                       ADMISSION + HARD FILTERS
  capabilities | context | price | latency | privacy | region | tool support
                                  |
                                  v
                          CHIMERA LEARNER
     operational ensemble + semantic-quality ensemble + uncertainty
                                  |
                                  v
                        BOUNDED SWARM PLANNER
              direct | cascade | parallel + judge | refusal
                                  |
                                  v
                          EXECUTION LAYER
  managed capacity | OpenRouter | direct provider account | provider fallback
                                  |
                                  v
                   RECEIPT + DELAYED OUTCOME LEDGER
 cost | latency | provider result | verifier | accepted outcome | corrections
                                  |
                                  v
                   FUSION / HYPERCUBE EVIDENCE WAREHOUSE
  pinned catalog + route decisions + outcome revisions + cohort / margin views
```

## Program artifacts and ownership

| Layer | Existing primary surface | Required output | Owner |
|---|---|---|---|
| Catalog + data contract | `build_dataset.py`, `features.py` | pinned dataset `.npz` + JSON sidecar | data/routing operator |
| Operational learner | `ensemble.py`, `model.py` | operational ensemble `.npz` | routing operator |
| Semantic learner | `ensemble.py` quality output | quality ensemble `.npz` | routing operator |
| Evaluation | `evaluate.py` | machine-readable promotion report | independent reviewer |
| Planning | `planner.py` | declarative route plan + hard-limit proof | runtime owner |
| Feedback | `feedback.py`, `serve.py /v1/outcome` | immutable revisions + high-water snapshot | product/runtime owner |
| Evidence warehouse | `fusion.py`, `FUSION.md` | content-addressed Parquet/DuckDB snapshot | data steward |
| Hosted integration | `dsco-router/.../routing/chimera.py` | shadow/canary router release | production operator |

## 12-week advanced curriculum

Each week ends with an evidence artifact. Do not advance because a notebook
looks plausible; advance when the stated gate passes.

### Phase I — Establish a trustworthy learning boundary (weeks 1–2)

#### Week 1 — System orientation and route anatomy

**Objective:** Explain a real customer request from admission through billing
receipt and distinguish learned decisions from enforcement.

Study:

- `README.md`: candidate descriptors, operational/quality separation, planner
  contract, feedback semantics.
- `route.py`: one-shot selection and uncertainty output.
- `planner.py`: direct, cascade, parallel, and refusal construction.
- `dsco-router/src/dsco_router/routing/chimera.py`: hosted artifact loader and
  fallback behavior.

Build:

1. Write three request policies: managed cloud, activated BYOK, and a
   privacy/region-constrained enterprise workspace.
2. Generate a direct route and a refused route for each policy.
3. Trace which outcome was decided by learning and which was prevented by a
   hard policy.

Gate:

- Every result exposes the selected model, fallbacks, uncertainty, and the
  reason a candidate was filtered or refused.
- No routing score is allowed to override an authority or spend constraint.

#### Week 2 — Data contracts, privacy, and provenance

**Objective:** Build a reproducible training snapshot without leaking content
or silently mixing time periods.

Study:

- `build_dataset.py`, `features.py`, `feedback.py`.
- The privacy and source-provenance sections of `FUSION.md`.

Build:

1. Pin a catalog snapshot and inclusive baseline-event high-water mark.
2. Build a baseline dataset and record its SHA-256, feature versions, row
   count, candidate coverage, and temporal split boundaries.
3. If feedback exists, pin the immutable feedback revision high-water and
   logical snapshot SHA-256; otherwise record `feedback=absent` explicitly.
4. Produce a data card with sources, known blind spots, privacy treatment,
   label definitions, and excluded fields.

Gate:

- Rebuilding the exact pinned inputs yields the same logical dataset hash.
- Raw prompts, responses, keys, tool payloads, and unredacted error text are
  absent from the dataset and warehouse inputs.

### Phase II — Learn candidate quality under uncertainty (weeks 3–4)

#### Week 3 — Operational ensemble

**Objective:** Predict completion, provider failure, tool validity, historical
cost, and latency for a request/candidate pair—not merely classify a model ID.

Build:

1. Train the five-member operational ensemble with a pinned dataset.
2. Inspect calibration, confidence intervals, support distance, provider/model
   coverage, and the cheap-baseline comparison.
3. Run temporal, model-family, and provider holdout diagnostics.
4. Document where the learner is weak: cold catalog entries, sparse providers,
   changing price fields, and unobserved capability combinations.

Gate:

- The artifact is candidate-conditioned and handles a new descriptor without
  a frozen class label.
- Validation calibration and held-out performance are reported separately;
  test rows never influence fit or calibration.

#### Week 4 — Semantic quality and claim discipline

**Objective:** Keep semantic usefulness separate from “the HTTP request
finished.”

Build:

1. Train the separate semantic-quality ensemble on permitted benchmark/eval
   labels.
2. Zero benchmark-derived descriptor fields when training the quality lane to
   prevent direct label leakage.
3. Route on a lower confidence bound when fresh benchmark data is absent.
4. Define a task-evaluation ladder: deterministic verifier, structured human
   review, customer acceptance, and unknown.

Gate:

- Operational completion and semantic acceptance never share a deceptive
  combined label.
- Any public quality statement is labeled as live benchmark, learned estimate,
  customer evaluator, or unknown.

### Phase III — Turn scores into bounded multi-provider execution (weeks 5–6)

#### Week 5 — Policy-aware provider routing

**Objective:** Route across managed and direct provider lanes without treating
provider identity as cosmetic metadata.

Build:

1. Add test policies for OpenRouter-managed capacity; direct OpenAI/Anthropic;
   Moonshot/Kimi; Z.AI/GLM; and a provider-denied fallback case.
2. Verify that credential availability, source (`managed` versus `byok`),
   billing mode, scope, user policy, residency, and spend ceiling are applied
   before score ranking.
3. Create per-lane availability, latency, cost, and outcome cohorts in fusion.
4. Define provider-health states: eligible, degraded, cooling down, unavailable,
   and administratively disabled.

Gate:

- A model can win only through a lane the workspace is actually authorized to
  use.
- A provider outage changes the candidate set and receipt; it does not become a
  fabricated “low quality” training label.

#### Week 6 — Declarative swarm planning

**Objective:** Select when extra calls are justified and make worst-case spend
explicit.

Build:

1. Use `planner.py` to generate direct, cascade, and parallel/judge plans.
2. Establish domain-specific local verifiers: schema validity, test pass,
   citation coverage, extraction completeness, or human approval.
3. Create a swarm policy matrix by task type with maximum calls, expected cost,
   worst-case cost, latency ceiling, and escalation trigger.
4. Execute no plan automatically in training; assert every plan remains
   `declarative_only` until the execution layer authorizes it.

Gate:

- Parallel plans require diversity, expected quality gain, explicit maximum
  calls, and both expected and worst-case cost ceilings.
- When limits cannot be met, the planner returns a refusal or safe direct plan.

### Phase IV — Close the evidence loop (weeks 7–8)

#### Week 7 — Receipts and delayed outcomes

**Objective:** Make every production outcome trainable without pretending it
is immediate or fully observed.

Build:

1. Emit one immutable route-decision record for every selected route.
2. Record completion, tool validity, actual model, billed/estimated cost,
   latency, evaluator result, label source, confidence, and censoring state.
3. Support corrections only as appended revisions.
4. Create a receipt view that reconciles provider usage, workspace credit
   movement, and operational route decision.

Gate:

- “No outcome yet” remains censored rather than failure.
- A corrected customer acceptance label is replayable as of a specific revision
  high-water mark.

#### Week 8 — Counterfactual and exploration discipline

**Objective:** Learn from deployment without rewarding the historical policy
for its own choices.

Build:

1. Preserve candidate exposures, propensities, policy ID, catalog ID, and
   outcome availability in the feedback store.
2. Use explicitly budgeted exploration only on low-risk tasks and only among
   admissible candidates.
3. Compare challenger/production routes in shadow where no extra provider call
   is needed, and in paid canaries only when the workspace policy allows it.
4. Report selection bias, missing labels, drift, and confidence intervals with
   each candidate promotion report.

Gate:

- Never relabel an unchosen candidate as a loss.
- Exploration cannot route around provider authority, customer price limits, or
  tool/region restrictions.

### Phase V — Economics, reliability, and product packaging (weeks 9–10)

#### Week 9 — Margin-aware routing

**Objective:** Optimize for customer outcomes and truthful contribution margin,
not the superficially cheapest token route.

Build:

1. Define request-level economics: collected credit, provider cost, retry
   cost, network/payment fees allocated at transaction level, support reserve,
   gross contribution, and accepted-outcome contribution.
2. Keep provider price, reported provider usage cost, internal estimate, and
   customer charge as distinct fields.
3. Create price-change and provider-failure stress scenarios.
4. Build a margin guard: a swarm plan cannot spend beyond customer budget or
   the product’s declared reserve without explicit operator intervention.

Gate:

- No claim that an artifact cost one cent unless a receipt contains the exact
  route, output, reported cost, estimate basis, and allocation method.
- Marketing price is never treated as provider cost or MRR.

#### Week 10 — Reliability and provider health

**Objective:** Distinguish model/task weakness from transient provider faults.

Build:

1. Feed bounded provider-health signals into admission and fallback order.
2. Correlate route outcomes with provider, model, status code class, latency,
   cold-start support, and worker role.
3. Design pre-response fallback only: once meaningful bytes are delivered to a
   customer, preserve the receipt and handle recovery explicitly rather than
   silently swapping a response.
4. Add incident and regression drills: rate limit, authentication failure,
   stale catalog price, invalid structured output, timeout, and judge failure.

Gate:

- Provider incidents are observable in receipts and cohort reports.
- A health rule is only promoted if it improves a held-out/replay result without
  breaking declared policy constraints.

### Phase VI — Production promotion and advanced research (weeks 11–12)

#### Week 11 — Shadow, canary, and rollback

**Objective:** Promote a route policy safely into the hosted paid router.

Build:

1. Generate the fail-closed `evaluate.py` promotion report.
2. Register the artifact, quality artifact, catalog, dataset, source high-water
   marks, report hash, and release note as one immutable release record.
3. Shadow-score live eligible requests without changing dispatch.
4. Canary a small, explicitly selected policy cohort with a known rollback
   pointer.
5. Monitor customer acceptance, hard-constraint violations, provider failure,
   spend, latency, and margin versus the incumbent.

Gate:

- Any failed required promotion check is a hold, not a conditional launch.
- Rollback changes only the active artifact/policy pointer and retains all
  decision/outcome evidence.

#### Week 12 — Advanced capstone: cross-provider evidence artifact

**Objective:** Produce a real, bounded deliverable that proves why Chimera is
worth paying for.

Choose one paid-capable workload: a cited research brief, code-change review,
structured extraction pack, policy analysis, or tool-using workflow.

Required capstone receipt:

1. Customer/workspace policy and budget.
2. Eligible provider lanes and why any lane was excluded.
3. Candidate scores, uncertainty, selected direct/cascade/parallel plan, and
   maximum spend.
4. Actual calls, provider/model identities, retries, latency, and costs.
5. Verifier/human acceptance result and any failure boundary.
6. Customer charge, provider cost, allocation assumptions, contribution, and
   remaining credit.
7. Dataset/feedback effect: what can be safely learned from it and what remains
   unknown.

Gate:

- The receipt is internally reconcilable and does not fabricate unavailable
  provider lanes, measured quality, or costs.
- The product can show a customer why it routed that way without exposing
  another customer’s content, credentials, or policy.

## Operating cadence after graduation

### Per request

- enforce admission and budget;
- score only admissible candidates;
- retain decision + route receipt;
- capture provider/verification outcomes without raw-content retention;
- record explicit customer outcome when available.

### Daily

- refresh/validate the model/provider catalog;
- inspect provider health, rejects, billing deltas, and public-demo spend;
- review held credit and stale route outcomes;
- confirm no credential, privacy, or policy regression.

### Weekly

- build a pinned fusion snapshot;
- review uncertainty, sparse coverage, price drift, provider incidents, accepted
  outcome contribution, and customer cohort performance;
- decide whether there is enough new governed feedback to justify retraining.

### Per training candidate

1. Freeze sources and high-water marks.
2. Build data and train operational + quality artifacts.
3. Run temporal/cold-start/policy evaluations.
4. Produce promotion report.
5. Shadow, canary, compare, then explicitly promote or hold.

## Promotion scorecard

Promotion requires all applicable gates to pass. Do not replace this with a
single composite score.

| Dimension | Required evidence |
|---|---|
| Reproducibility | pinned dataset/catalog/feedback snapshot and hashes |
| Privacy | no forbidden raw-content or credential fields |
| Policy | zero hard-constraint bypasses in replay and canary |
| Calibration | validation-only calibration with test reported separately |
| Generalization | temporal and cold-start holdout integrity |
| Value | improvement against declared baseline on the relevant accepted-outcome/economic metric |
| Reliability | no unacceptable degradation in failure, latency, or fallback behavior |
| Economics | actual/estimated provider cost and customer-credit settlement reconcile |
| Rollback | documented active-artifact pointer and rollback procedure |
| Product truth | user-visible route receipt accurately distinguishes fact, estimate, and unavailable data |

## First implementation queue

The curriculum starts with these concrete changes, in order:

1. **Wire the router’s trained-artifact configuration into the hosted
   deployment** and surface artifact/catalog status in an operator-only health
   view. Today the hosted loader falls back when those variables are absent.
2. **Create one activation-safe route receipt schema** spanning `dsco-router`
   request records and the Chimera feedback store.
3. **Populate real route decisions and delayed outcomes** from managed-cloud
   traffic before any promotion claim. The fusion snapshot currently has broad
   operational evidence but no fresh route-decision/outcome-revision lane for
   the new hosted control plane.
4. **Promote a shadow-only Chimera artifact first.** It may explain and compare
   routes but cannot change paid dispatch.
5. **Run a bounded canary** only after the promotion report passes and a
   rollback record exists.
6. **Add swarm execution behind the declarative planner** with per-workspace
   call/spend ceilings, pre-response fallback, and an immutable multi-provider
   receipt.

## Source map

| Need | Source |
|---|---|
| Dataset construction | `build_dataset.py` |
| Features + descriptor contract | `features.py` |
| Single-router trainer | `train.py` |
| Current operational + quality ensemble | `ensemble.py` |
| Evaluation + promotion gate | `evaluate.py` |
| Route selection | `route.py` |
| Bounded plan generation | `planner.py` |
| Delayed-outcome ledger | `feedback.py` |
| Local scoring/planning service | `serve.py` |
| Fusion/Hypercube warehouse | `fusion.py`, `FUSION.md` |
| Hosted artifact integration | `../../../dsco-router/src/dsco_router/routing/chimera.py` |
| Separate trajectory-model research | `/Users/arthurcolle/dsco-penumbra/rl/chimera.py` |
