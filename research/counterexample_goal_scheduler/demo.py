"""
Demo: CGQP converging on the true bias of a coin.

Three competing hypotheses about P(heads):
  h_fair   -> 0.5
  h_biased_hi -> 0.9
  h_biased_lo -> 0.1

The "true" coin is biased at 0.9, realized deterministically via a fixed
repeating outcome cycle (9 heads per 10 flips) so the demo is perfectly
reproducible: no randomness anywhere.

At each step the planner picks the experiment (coin flip) that currently
maximizes expected information gain (here all flips are interchangeable,
so EIG only depends on the current posterior, not on which flip index is
used) -- this is still exercised explicitly for demonstration.
"""
from cgqp import CGQPlanner, Experiment, Hypothesis


def coin_likelihood(h, e, outcome):
    p1 = h.predict(e.eid)
    return p1 if outcome == 1 else (1 - p1)


def coin_outcome_space(e):
    return [0, 1]


def main():
    hyps = [
        Hypothesis("h_fair_0.5", 1 / 3, lambda e: 0.5),
        Hypothesis("h_biased_hi_0.9", 1 / 3, lambda e: 0.9),
        Hypothesis("h_biased_lo_0.1", 1 / 3, lambda e: 0.1),
    ]

    # True coin: 9 heads per 10 flips, deterministic cycle (no randomness).
    cycle = [1, 1, 1, 1, 1, 1, 1, 1, 1, 0]
    state = {"i": 0}

    def make_runner():
        def run():
            v = cycle[state["i"] % len(cycle)]
            state["i"] += 1
            return v
        return run

    experiments = [Experiment(f"flip{i}", run=make_runner()) for i in range(30)]

    planner = CGQPlanner(
        hypotheses=hyps,
        experiments=experiments,
        likelihood_fn=coin_likelihood,
        outcome_space=coin_outcome_space,
        proof_margin=0.99,
        max_steps=30,
    )

    print("=== CGQP Demo: identifying a biased coin ===")
    print(f"Initial hypothesis weights: {planner.hq.weights()}")
    print(f"Initial entropy: {planner.hq.entropy():.4f} bits")
    print(f"Proof margin threshold: {planner.proof_margin}")
    print()

    step_num = 0
    while not planner.hq.proof_margin_reached(planner.proof_margin):
        rec = planner.step()
        if rec is None:
            print("No experiments remain; halting without proof margin.")
            break
        step_num += 1
        weights_str = ", ".join(f"{k}={v:.6f}" for k, v in sorted(rec.weights_after.items()))
        print(
            f"Step {step_num:2d}: experiment={rec.experiment_id:<8} "
            f"outcome={rec.outcome}  EIG={rec.eig:.6f}  "
            f"entropy_after={planner.hq.entropy():.4f}"
        )
        print(f"          weights: {weights_str}")

    print()
    best = planner.hq.best()
    print(f"Converged: {planner.converged()}")
    print(f"Best hypothesis: {best.hid} (weight={best.weight:.8f})")
    print(f"Total experiments used: {len(planner.history)}")
    print(f"Final entropy: {planner.hq.entropy():.8f} bits")

    assert planner.converged(), "Demo expected convergence within budget"
    assert best.hid == "h_biased_hi_0.9", "Demo expected correct hypothesis identification"
    print()
    print("Assertions passed: planner converged to the correct hypothesis deterministically.")


if __name__ == "__main__":
    main()
