# Counterexample-Guided Queue Policy

This is a held research prototype produced by a live DSCO autonomous run. It maintains a weighted
hypothesis queue and selects finite-outcome experiments by exact expected information gain per
cost. Bayesian updates eliminate hypotheses contradicted by observed evidence, and a configurable
posterior margin supplies a deterministic stopping rule.

Run it with only the Python standard library:

```sh
python3 -m unittest -v test_cgqp.py
python3 demo.py
```

The tests cover deterministic convergence, hard counterexamples, differentiated probe selection,
probability validation, cost-aware ordering, and exhaustion without false convergence.

## Disposition

Do not wire this into `goal_queue` yet. The coin demo's probes are exchangeable, so it proves the
Bayesian update and deterministic EIG calculation but does not measure scheduling advantage. The
additional differentiated-probe test proves the selector prefers information over lexical order,
but arbitrary coding tasks still lack calibrated hypotheses, finite outcome spaces, likelihoods,
and a replay benchmark tying posterior confidence to acceptance evidence.

A production experiment should derive priors and likelihoods from Chronicle traces, compare EIG
ordering against FIFO and fixed priority on frozen task replays, and retain `goal_queue`'s hard
evidence gate. Posterior confidence may prioritize work; it must never substitute for verified root
acceptance or expand tool authority.
