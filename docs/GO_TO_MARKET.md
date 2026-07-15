# DSCO Go-to-Market Operating Kit

**Status:** founder-led commercial operating document

**Audience:** Arthur Colle / Distributed Systems, Inc.

**Primary objective:** contract $300,000 in revenue over the next 12 months through paid, bounded DSCO deployments and recurring private-deployment support.

> This document is a commercial hypothesis, not a statement of shipped product guarantees. Every customer-facing technical claim must be demonstrated from the current build and written into the applicable deployment scope.

## 1. Wedge and positioning

### Ideal customer profile

Engineering-led organizations with 10–150 technical staff that:

- operate proprietary source code, operational data, or research processes;
- already use AI coding tools but cannot safely extend them into internal systems with ambient credentials;
- have a high-context, repeated workflow consuming experienced engineering or operations time;
- can sponsor a bounded paid deployment; and
- value a controlled deployment boundary, observable actions, and reviewable outputs.

Initial segments: developer infrastructure, security, crypto/market infrastructure, data-heavy B2B SaaS, AI-native startups, and private research organizations.

### Positioning

> DSCO deploys governed AI workflows across proprietary code and internal systems inside the customer’s control boundary.

Short version:

> Automate high-context engineering work without granting an AI agent ambient authority.

### What not to lead with

- Total tool, skill, doctrine, provider, or source-line counts.
- Generic claims that DSCO is a replacement for every coding assistant.
- “Secure,” “private,” “compliant,” or “enterprise-ready” without a deployment-specific technical basis.
- Autonomous production action as the first deployment step.

### Competitive frame

Generic coding assistants can remain useful. DSCO is a fit only where a workflow crosses the boundary from individual code drafting into governed use of repositories, internal evidence, tools, and durable operating records.

## 2. Primary offer: DSCO Governed Workflow Pilot

### Commercial terms

| Item | Default |
|---|---|
| Duration | 30 calendar days |
| Fee | $25,000 |
| Payment | 50% at kickoff; 50% at delivery |
| Scope | One named workflow, agreed systems, and explicit authority boundary |
| Environment | Customer-controlled local, sandbox, or VPC deployment as agreed |
| Production authority | Excluded unless separately authorized in writing |
| Output | Workflow, runbook, evaluation evidence, final findings, continuation recommendation |

### Candidate workflows

1. Incident evidence → diagnosis → bounded tested patch proposal.
2. Issue → implementation plan → reviewable, tested change.
3. Repository migration analysis → staged execution plan → verified incremental changes.
4. Private codebase and documentation investigation with cited evidence.
5. Internal research or operational investigation that ends in a review packet, not an external action.

### Deliverables

1. Deployment and data-flow design for the agreed environment.
2. Minimum necessary repository, tool, and evidence-source configuration.
3. Documented capability boundary and human approval points.
4. One bounded workflow implementation.
5. Agreed evaluation cases and results.
6. Operator runbook and handoff session.
7. Final evidence report: outputs, verification results, limitations, and rollback/review path.

### Explicit exclusions

- Unbounded production write access.
- Unspecified integrations, data sources, or security certifications.
- Investment advice, autonomous trading, or financial execution.
- A promise that a model’s output will be correct without review.
- Broad enterprise rollout beyond the agreed workflow.

## 3. Sales motion

### 12-month revenue model

| Source | Quantity | Average contract | Revenue |
|---|---:|---:|---:|
| Workflow pilots | 4 | $25,000 | $100,000 |
| Private deployment/support agreements | 8 | $18,750 | $150,000 |
| Paid architecture/deployment assessments | 5 | $10,000 | $50,000 |
| **Total** |  |  | **$300,000** |

This is a planning model. It should be revised monthly based on actual close rates, cycle lengths, delivery cost, and retention.

### Pipeline stages

| Stage | Exit criterion | Owner action |
|---|---|---|
| Targeted | Account fits ICP and has a researched trigger | Record buyer, trigger, workflow hypothesis, and contact path |
| Contacted | High-context outreach sent | Schedule follow-up; do not spam |
| Discovery | Concrete recent workflow and accountable owner identified | Quantify current process, constraints, value, and buying path |
| Qualified | Named scope, technical access path, success measure, and budget/process | Send one-page proposal |
| Proposal | Commercial scope delivered | Run security/technical validation only as needed to close |
| Closed pilot | Kickoff payment received and scope accepted | Run the 30-day delivery plan |
| Expansion | Pilot evidence supports continuation | Propose annual private deployment/support scope |
| Disqualified | No owned pain, no access path, no buying path, or unsafe scope | Record reason and stop active pursuit |

### Weekly activity baseline

| Activity | Target |
|---|---:|
| New target accounts researched | 15–25 |
| High-context messages sent | 25–40 |
| Warm introductions requested | 5–10 |
| Discovery calls completed | 3–6 |
| Qualified proposals delivered | 1–3 |
| Customer-learning interviews | 3+ |

## 4. Discovery qualification

A pilot must satisfy all five conditions:

| Condition | Required evidence |
|---|---|
| Pain | A recent, concrete workflow example—not a feature wish list |
| Value | Time, risk, latency, quality, or throughput impact the buyer recognizes |
| Owner | A responsible champion and decision path |
| Access | A feasible pilot environment and minimum input/tool access |
| Scope | A measurable 30-day outcome and explicit non-goals |

Questions:

1. What engineering or operational process repeatedly consumes expensive expert time?
2. Walk through the last real instance from trigger to completion.
3. Which repositories, evidence sources, and tools are required?
4. Where do current AI tools help, and where do they fail?
5. What must an automated system never be allowed to do?
6. What would a successful pilot prove in measurable terms?
7. Who owns the result and who approves budget?

## 5. Customer-facing scripts

### First outreach

**Subject:** Governed AI automation for `[Company]`’s engineering workflows

> Hi `[Name]`,
>
> I’m Arthur Colle, founder of Distributed Systems, Inc. We build DSCO, a local-first runtime for AI workflows that need to operate across proprietary code and internal systems under explicit tool permissions.
>
> I noticed `[specific factual trigger]`. Teams in that position often find generic coding agents useful for drafting, but difficult to extend safely once work requires repository-wide context, internal evidence, credentials, or reviewable actions.
>
> We are running a small number of fixed-scope 30-day Governed Workflow Pilots: one real workflow deployed inside an agreed boundary, measured against a baseline, with an operator runbook and evidence report.
>
> If `[workflow hypothesis]` is active at `[Company]`, would a 20-minute technical walkthrough be useful?
>
> —Arthur

### Discovery-call opening

> DSCO is for teams that want AI automation to use real tools and internal context without granting ambient authority and relying on prompt instructions as the control model. We deploy one bounded workflow at a time. Before demonstrating anything, I want to determine whether there is a workflow here with enough pain, value, and operational constraints to justify a pilot.

### Direct close

> Based on the workflow you described, I propose a 30-day DSCO Governed Workflow Pilot for `[workflow]`. We will define the baseline, install only the minimum required access, configure the authority boundary, implement and evaluate the workflow, and deliver a runbook plus evidence report. The fixed fee is $25,000: half at kickoff and half on delivery. Does that structure match how you would evaluate this, and who needs to review the one-page scope?

## 6. Demo: incident to reviewable fix

The flagship demo must run from a clean, reproducible fixture in under ten minutes. It should demonstrate only behavior actually supported by the current build.

1. Present an issue/incident fixture as untrusted input.
2. Show the declared project and tool boundary.
3. Investigate the local repository and explain the proposed change.
4. Produce a normal reviewable diff.
5. Run relevant verification and surface failures honestly.
6. Show a denied out-of-boundary action, if the current capability gate supports the exact example.
7. Produce an artifact bundle: evidence, plan, diff, tests, permissions used, limitations, and review/rollback path.

The final question is:

> Does your team have a valuable workflow that needs an agent to use real tools and context while remaining inside an explicit, inspectable authority boundary?

## 7. Delivery cadence

| Time | Activity | Evidence |
|---|---|---|
| Before kickoff | Scope, data flow, access boundary, baseline, success criteria | Signed scope and technical checklist |
| Days 1–5 | Environment configuration and fixture collection | Installation/connection record |
| Days 6–15 | Workflow implementation and bounded test runs | Intermediate run artifacts |
| Days 16–23 | Evaluation, fault handling, operator review | Results against agreed cases |
| Days 24–30 | Handoff, runbook, final results, continuation proposal | Final evidence package |

## 8. Product decisions from revenue evidence

Treat each recurring qualified objection as a potential product requirement only after recording:

- customer and opportunity stage;
- workflow affected;
- buyer impact and urgency;
- whether it blocked a paid contract;
- minimal technical change required;
- testable acceptance criterion.

Current roadmap items with direct buyer-objection value include durable execution, headless operation, evaluation, observability, and an event/callback spine. Do not represent planned capabilities as shipped in a sale.

## 9. Immediate 14-day execution checklist

- [ ] Verify the live DSCO demo path against a clean fixture.
- [ ] Record a 2–3 minute demo and an 8–10 minute technical demo.
- [ ] Create a one-page pilot proposal from `docs/DSCO_WORKFLOW_PILOT_PROPOSAL.md`.
- [ ] Create a 100-account target list with buyer, trigger, and workflow hypothesis.
- [ ] Send 20 high-context messages and request five warm introductions.
- [ ] Run at least five discovery conversations.
- [ ] Log every objection and every promised technical claim.
- [ ] Deliver at least one paid assessment or pilot proposal.

## 10. Operating rule

A commercial claim is ready only when it connects:

**named buyer pain → bounded DSCO workflow → explicit authority boundary → measurable result → verifiable evidence.**
