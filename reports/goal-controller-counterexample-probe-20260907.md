# Goal Controller Counterexample Probe — 2026-09-07

## Purpose

Use DSCO's new two-queue goal controller to produce a novel scheduling policy, then treat the live
run itself as a counterexample search against the controller. Acceptance required a real artifact,
independent tests, bounded execution, and evidence strong enough to decide whether the policy
belonged in the production scheduler.

## Live Run

The first Haiku attempt created only a plan. A second Sonnet run produced a standard-library Python
prototype for counterexample-guided experiment selection:

- `research/counterexample_goal_scheduler/cgqp.py`
- `research/counterexample_goal_scheduler/test_cgqp.py`
- `research/counterexample_goal_scheduler/demo.py`

The run made 32 provider requests and stopped at the controller's default limit instead of the
requested 20-turn limit. Chronicle recorded 23 `goal_queue` calls, of which 12 failed (52.2%). The
reported provider-cost estimate was $0.7116222 on a subscription lane. The generated artifact was
valid and its original 20 tests passed, but the controller did not reach its terminal state.

Baseline instance: `chrysalis-1788828588-86557-524917`  
Chronicle session: `3835af3a-8464-4c45-aff1-ad48ddd85159`

## Counterexamples Found and Fixed

1. Auto-promoted prompts bypassed `DSCO_GOAL_MAX_TURNS` and `DSCO_GOAL_TOKEN_BUDGET`. Fresh goals
   now apply startup limits whether they originate from `DSCO_GOAL`, automatic promotion, or the
   `/goal` command.
2. The C handler limited evidence to 511 bytes and reasons to 255 bytes while the advertised schema
   omitted both bounds. The wire schema now carries the exact limits and explains the byte contract.
3. Rejections returned generic errors, leading the model to request full status and retry blindly.
   Errors now return the precise cause, current revision, current lease, and field limits.
4. Every successful mutation returned the full tree. Mutations now return a compact stateful receipt;
   explicit `status` remains the full diagnostic view.
5. A planning parent could consume its final lease to decompose again, run correctly in memory, and
   then fail persistence validation after restart. Planning tasks now reserve a later review lease,
   default to a bounded eight-lease recovery allowance, and are tested across a crash boundary.
6. Native OpenAI-compatible providers injected a legacy goal prompt that said `self_exit` completed
   the objective. Every native provider path now uses the canonical leased-task context and the
   controller's evidence-gated completion contract.

## Policy Result

The prototype computes exact finite-outcome expected information gain per cost, performs Bayesian
updates, collapses falsified hypotheses, and halts at a posterior threshold. The preserved research
version adds probability and cost validation plus a differentiated-probe test showing that an
informative probe beats lexicographic queue order.

It is held out of the production scheduler. The demo's coin flips are exchangeable, and general
coding tasks do not yet provide calibrated hypotheses, outcome spaces, or likelihoods. Posterior
confidence also cannot replace concrete root acceptance evidence. The next valid experiment is a
frozen Chronicle replay comparing FIFO, fixed priority, and EIG ordering using trace-derived priors;
promotion requires lower verified cost or latency without lower task success.

## Verification

- `make test`: 5,327/5,327 passed.
- `make test-goal-queue`: AddressSanitizer flow, recovery, strict inputs, and persistence passed.
- Research policy suite: 24/24 passed; demo converged deterministically in eight observations.
- `make test-gate-claims`: 9/9 capability claims passed.
- `make docs-check` and `git diff --check`: passed.
- `make test-goal-controller-binary`: local OpenAI-compatible replay made one provider request at
  `DSCO_GOAL_MAX_TURNS=1`, entered two-queue mode through the default action classifier, exited 1
  with `goal turn limit reached`, and transmitted schema limits of 511/255.
- Source and installed `/Users/arthurcolle/.local/bin/dsco` SHA-256:
  `5516dc64e29a130dcf052b5a8a44eb097d10a57a960e33e868eb9bb54dc7c326`.
