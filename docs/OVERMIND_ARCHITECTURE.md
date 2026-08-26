# Overmind Architecture

**Status:** candidate reification; not canonical without principal promotion  
**Scope:** cognitive orchestration only

## Definition

Overmind is DSCO's conceptual top-level cognitive orchestration and execution-planning layer. It establishes a standard baseline for selecting reasoning methods, applying task-sensitive cognitive-bias checks, coordinating diverse candidate views, and requiring verification proportional to consequence.

Overmind is not a persona, source of authority, governance mechanism, or claim of unbiased reasoning. It cannot grant capabilities or bypass `tools_execute_for_tier()`. Governance remains implemented and enforced by the capability gate.

## Reified kernel

`include/overmind.h` and `src/overmind.c` provide a deterministic, dependency-free planning kernel. Given explicit task characteristics, the kernel produces:

- selected reasoning families: deductive, inductive, abductive, analogical, causal, probabilistic, counterfactual;
- checks: assumptions, consider-opposite, counterargument, reference class, pre-mortem, causality, calibration, independent verification;
- a bounded number of reasoning passes and independent views;
- a disposition: proceed, gather evidence, require a verifier, or abstain.

It does not execute an LLM, expose hidden chain-of-thought, select permissions, or run tools. Its output is structured orchestration metadata suitable for agent, topology, trace, or evaluation layers.

## Provenance

The cognitive vocabulary is distilled from the real project at:

`/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/`

Relevant implementations include `comprehensive_reasoning_system.py`, `debiased_reasoning_modules.py`, `debiased_applications.py`, and `specialized_teams.py`. The C kernel transfers only testable orchestration primitives; it does not inherit that project's unvalidated performance claims.

## Invariants

1. `may_expand_authority` is always false; plans violating it are invalid.
2. Consequential, irreversible, adversarial, or self-modifying work requires an independent-verification check.
3. Consequential self-change without evidence abstains.
4. Reasoning is bounded by an explicit pass limit.
5. Structured summaries disclose selected methods and checks, not private chain-of-thought.
6. A proposer cannot satisfy an independent-verifier requirement merely by re-labeling itself.
7. Promotion of this candidate remains subject to DSCO's RSI discipline and principal ratification where canonical architecture is affected.

## Relationship to existing systems

- `task_profile.c` profiles topology fit; Overmind profiles cognitive method and verification requirements.
- `ooda.c` tracks Observe → Orient → Decide → Act; Overmind provides a plan for the Orient/Decide boundary, with verification remaining explicit.
- `swarm.c` and `topology.c` execute worker structures; Overmind can request independent views but does not launch them itself.
- `capability.c` and `tools_execute_for_tier()` remain the authority boundary.
- `agent_event.c` can eventually record structured plan summaries after a separately reviewed integration hook.

## Current acceptance evidence

`tests/test_overmind.c` deterministically checks baseline selection, uncertain prediction, causal planning, bounded compute, abstention on unevidenced self-change, independent verification, and the no-authority-expansion invariant.

This slice intentionally stops before integration into `agent.c`: the checkout already contains extensive unrelated modifications, and a forced hot-path integration would increase merge and regression risk. The kernel and focused test make the architecture concrete while keeping promotion reversible.
