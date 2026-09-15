"""
CGQP: Counterexample-Guided two-queue Planner.

Two queues:
  - Hypothesis queue: candidate hypotheses with Bayesian posterior weights.
  - Experiment queue: candidate experiments, selected by expected information
    gain (EIG) against the current hypothesis posterior.

Deterministic: given the same sequence of outcomes, results are identical
across runs (no randomness; ties broken by stable ids).

Stdlib only.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple


@dataclass
class Hypothesis:
    hid: str
    weight: float  # posterior probability mass (unnormalized until normalize())
    predict: Callable[[str], object]  # experiment_id -> predicted outcome

    def __repr__(self) -> str:
        return f"H({self.hid}, w={self.weight:.6f})"


@dataclass
class Experiment:
    eid: str
    run: Callable[[], object]  # deterministic outcome generator
    cost: float = 1.0
    used: bool = False

    def __repr__(self) -> str:
        return f"E({self.eid})"


class HypothesisQueue:
    """Maintains hypotheses with Bayesian posterior weights."""

    def __init__(self, hypotheses: List[Hypothesis]):
        if not hypotheses:
            raise ValueError("HypothesisQueue requires at least one hypothesis")
        if len({h.hid for h in hypotheses}) != len(hypotheses):
            raise ValueError("Hypothesis ids must be unique")
        if any(not math.isfinite(h.weight) or h.weight < 0 for h in hypotheses):
            raise ValueError("Hypothesis weights must be finite and non-negative")
        self._by_id: Dict[str, Hypothesis] = {h.hid: h for h in hypotheses}
        self.normalize()

    def normalize(self) -> None:
        total = sum(h.weight for h in self._by_id.values())
        if total <= 0:
            raise ValueError("Total hypothesis weight collapsed to zero")
        for h in self._by_id.values():
            h.weight /= total

    def items(self) -> List[Hypothesis]:
        # Stable deterministic order: sorted by hid.
        return sorted(self._by_id.values(), key=lambda h: h.hid)

    def weights(self) -> Dict[str, float]:
        return {h.hid: h.weight for h in self._by_id.values()}

    def best(self) -> Hypothesis:
        return max(self.items(), key=lambda h: (h.weight, h.hid))

    def update(self, experiment: Experiment, outcome: object,
               likelihood_fn: Callable[[Hypothesis, Experiment, object], float]) -> None:
        """Bayesian update: w_h ∝ w_h * P(outcome | h, experiment)."""
        for h in self._by_id.values():
            lik = likelihood_fn(h, experiment, outcome)
            if not math.isfinite(lik) or not 0 <= lik <= 1:
                raise ValueError("Likelihood must be a finite probability in [0, 1]")
            h.weight *= lik
        self.normalize()

    def entropy(self) -> float:
        ent = 0.0
        for h in self._by_id.values():
            if h.weight > 0:
                ent -= h.weight * math.log2(h.weight)
        return ent

    def proof_margin_reached(self, threshold: float) -> bool:
        return self.best().weight >= threshold


class ExperimentQueue:
    """Holds candidate experiments and selects by expected information gain."""

    def __init__(self, experiments: List[Experiment]):
        if len({e.eid for e in experiments}) != len(experiments):
            raise ValueError("Experiment ids must be unique")
        if any(not math.isfinite(e.cost) or e.cost <= 0 for e in experiments):
            raise ValueError("Experiment costs must be finite and positive")
        self._by_id: Dict[str, Experiment] = {e.eid: e for e in experiments}

    def available(self) -> List[Experiment]:
        return sorted(
            (e for e in self._by_id.values() if not e.used),
            key=lambda e: e.eid,
        )

    def mark_used(self, eid: str) -> None:
        self._by_id[eid].used = True

    def expected_information_gain(
        self,
        experiment: Experiment,
        hq: HypothesisQueue,
        likelihood_fn: Callable[[Hypothesis, Experiment, object], float],
        outcome_space: Callable[[Experiment], List[object]],
    ) -> float:
        """
        EIG(e) = H(hypotheses) - E_outcome[H(hypotheses | outcome)]

        Computed exactly via enumeration over the (finite) outcome space,
        using each hypothesis's predicted marginal outcome probability.
        Deterministic: no sampling.
        """
        prior_entropy = hq.entropy()
        hyps = hq.items()
        outcomes = outcome_space(experiment)
        if not outcomes:
            return 0.0

        likelihoods = {
            (h.hid, i): likelihood_fn(h, experiment, outcome)
            for i, outcome in enumerate(outcomes)
            for h in hyps
        }
        for likelihood in likelihoods.values():
            if not math.isfinite(likelihood) or not 0 <= likelihood <= 1:
                raise ValueError("Likelihood must be a finite probability in [0, 1]")
        for h in hyps:
            mass = sum(likelihoods[(h.hid, i)] for i in range(len(outcomes)))
            if not math.isclose(mass, 1.0, rel_tol=1e-9, abs_tol=1e-12):
                raise ValueError("Outcome space likelihoods must sum to 1 per hypothesis")

        # P(outcome) = sum_h w_h * P(outcome | h, e)
        expected_posterior_entropy = 0.0
        for i, outcome in enumerate(outcomes):
            liks = {h.hid: likelihoods[(h.hid, i)] for h in hyps}
            p_outcome = sum(h.weight * liks[h.hid] for h in hyps)
            if p_outcome <= 0:
                continue
            # posterior weights under this hypothetical outcome
            post = {h.hid: h.weight * liks[h.hid] / p_outcome for h in hyps}
            ent = 0.0
            for w in post.values():
                if w > 0:
                    ent -= w * math.log2(w)
            expected_posterior_entropy += p_outcome * ent

        eig = prior_entropy - expected_posterior_entropy
        # Numerical noise guard.
        if abs(eig) < 1e-12:
            eig = 0.0
        return eig

    def select_best(
        self,
        hq: HypothesisQueue,
        likelihood_fn: Callable[[Hypothesis, Experiment, object], float],
        outcome_space: Callable[[Experiment], List[object]],
    ) -> Optional[Tuple[Experiment, float]]:
        """
        Select the available experiment maximizing EIG per unit cost.
        Ties broken deterministically by experiment id (lexicographic).
        Returns None if no experiments remain.
        """
        candidates = self.available()
        if not candidates:
            return None
        scored = []
        for e in candidates:
            eig = self.expected_information_gain(e, hq, likelihood_fn, outcome_space)
            score = eig / e.cost
            scored.append((score, e.eid, e, eig))
        scored.sort(key=lambda t: (-t[0], t[1]))
        best_score, best_eid, best_exp, best_eig = scored[0]
        return best_exp, best_eig


@dataclass
class StepRecord:
    experiment_id: str
    outcome: object
    eig: float
    weights_after: Dict[str, float]


class CGQPlanner:
    """
    Orchestrates the counterexample-guided two-queue loop:
      1. Pick experiment maximizing EIG.
      2. Run it (deterministic outcome).
      3. Bayesian-update hypothesis weights.
      4. If an outcome contradicts a hypothesis (likelihood == 0), that
         hypothesis is a falsified counterexample and its weight collapses
         to zero on normalization.
      5. Halt when best hypothesis weight >= proof_margin, or no experiments
         remain, or max_steps reached.
    """

    def __init__(
        self,
        hypotheses: List[Hypothesis],
        experiments: List[Experiment],
        likelihood_fn: Callable[[Hypothesis, Experiment, object], float],
        outcome_space: Callable[[Experiment], List[object]],
        proof_margin: float = 0.99,
        max_steps: int = 50,
    ):
        if not math.isfinite(proof_margin) or not 0 < proof_margin <= 1:
            raise ValueError("proof_margin must be in (0, 1]")
        if max_steps < 0:
            raise ValueError("max_steps must be non-negative")
        self.hq = HypothesisQueue(hypotheses)
        self.eq = ExperimentQueue(experiments)
        self.likelihood_fn = likelihood_fn
        self.outcome_space = outcome_space
        self.proof_margin = proof_margin
        self.max_steps = max_steps
        self.history: List[StepRecord] = []

    def step(self) -> Optional[StepRecord]:
        picked = self.eq.select_best(self.hq, self.likelihood_fn, self.outcome_space)
        if picked is None:
            return None
        experiment, eig = picked
        outcome = experiment.run()
        self.eq.mark_used(experiment.eid)
        self.hq.update(experiment, outcome, self.likelihood_fn)
        rec = StepRecord(
            experiment_id=experiment.eid,
            outcome=outcome,
            eig=eig,
            weights_after=self.hq.weights(),
        )
        self.history.append(rec)
        return rec

    def run(self) -> Hypothesis:
        for _ in range(self.max_steps):
            if self.hq.proof_margin_reached(self.proof_margin):
                break
            rec = self.step()
            if rec is None:
                break
        return self.hq.best()

    def converged(self) -> bool:
        return self.hq.proof_margin_reached(self.proof_margin)
