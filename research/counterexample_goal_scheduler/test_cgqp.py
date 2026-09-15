"""Deterministic test suite for CGQP. Stdlib unittest only."""
import math
import unittest

from cgqp import (
    CGQPlanner,
    Experiment,
    ExperimentQueue,
    Hypothesis,
    HypothesisQueue,
)


def coin_likelihood(h: Hypothesis, e: Experiment, outcome: object) -> float:
    """h.predict(e.eid) returns P(outcome=1) for a Bernoulli experiment."""
    p1 = h.predict(e.eid)
    return p1 if outcome == 1 else (1 - p1)


def coin_outcome_space(e: Experiment):
    return [0, 1]


class TestHypothesisQueue(unittest.TestCase):
    def test_normalize_sums_to_one(self):
        hq = HypothesisQueue([
            Hypothesis("A", 2.0, lambda e: 0.5),
            Hypothesis("B", 3.0, lambda e: 0.5),
        ])
        total = sum(hq.weights().values())
        self.assertAlmostEqual(total, 1.0, places=12)

    def test_empty_hypothesis_set_rejected(self):
        with self.assertRaises(ValueError):
            HypothesisQueue([])

    def test_duplicate_hypothesis_ids_rejected(self):
        with self.assertRaises(ValueError):
            HypothesisQueue([
                Hypothesis("same", 0.5, lambda e: 0.5),
                Hypothesis("same", 0.5, lambda e: 0.5),
            ])

    def test_best_returns_highest_weight(self):
        hq = HypothesisQueue([
            Hypothesis("A", 0.1, lambda e: 0.5),
            Hypothesis("B", 0.9, lambda e: 0.5),
        ])
        self.assertEqual(hq.best().hid, "B")

    def test_deterministic_tiebreak_by_id(self):
        hq = HypothesisQueue([
            Hypothesis("Z", 0.5, lambda e: 0.5),
            Hypothesis("A", 0.5, lambda e: 0.5),
        ])
        # equal weights -> best() must pick lexicographically larger id per
        # the (weight, hid) max key, deterministically every run.
        self.assertEqual(hq.best().hid, "Z")

    def test_update_collapses_falsified_hypothesis(self):
        # H_fair predicts p(1)=0.5, H_double_heads predicts p(1)=1.0
        hq = HypothesisQueue([
            Hypothesis("fair", 0.5, lambda e: 0.5),
            Hypothesis("double_heads", 0.5, lambda e: 1.0),
        ])
        e = Experiment("flip1", run=lambda: 0)  # outcome=0 (tails)
        # double_heads predicts P(0)=0, a hard counterexample.
        hq.update(e, 0, coin_likelihood)
        self.assertAlmostEqual(hq.weights()["double_heads"], 0.0, places=12)
        self.assertAlmostEqual(hq.weights()["fair"], 1.0, places=12)

    def test_negative_likelihood_rejected(self):
        hq = HypothesisQueue([Hypothesis("A", 1.0, lambda e: 0.5)])
        e = Experiment("x", run=lambda: 1)
        with self.assertRaises(ValueError):
            hq.update(e, 1, lambda h, e_, o: -0.1)

    def test_likelihood_above_one_rejected(self):
        hq = HypothesisQueue([Hypothesis("A", 1.0, lambda e: 0.5)])
        e = Experiment("x", run=lambda: 1)
        with self.assertRaises(ValueError):
            hq.update(e, 1, lambda h, e_, o: 1.1)

    def test_entropy_zero_when_certain(self):
        hq = HypothesisQueue([
            Hypothesis("A", 1.0, lambda e: 0.5),
            Hypothesis("B", 0.0, lambda e: 0.5),
        ])
        self.assertAlmostEqual(hq.entropy(), 0.0, places=12)

    def test_proof_margin_reached(self):
        hq = HypothesisQueue([
            Hypothesis("A", 0.995, lambda e: 0.5),
            Hypothesis("B", 0.005, lambda e: 0.5),
        ])
        self.assertTrue(hq.proof_margin_reached(0.99))
        self.assertFalse(hq.proof_margin_reached(0.999))


class TestExperimentQueueEIG(unittest.TestCase):
    def test_eig_zero_when_all_hypotheses_agree(self):
        hq = HypothesisQueue([
            Hypothesis("A", 0.5, lambda e: 0.5),
            Hypothesis("B", 0.5, lambda e: 0.5),
        ])
        eq = ExperimentQueue([Experiment("e1", run=lambda: 1)])
        eig = eq.expected_information_gain(
            eq.available()[0], hq, coin_likelihood, coin_outcome_space
        )
        self.assertAlmostEqual(eig, 0.0, places=9)

    def test_eig_positive_when_hypotheses_disagree(self):
        hq = HypothesisQueue([
            Hypothesis("fair", 0.5, lambda e: 0.5),
            Hypothesis("biased", 0.5, lambda e: 0.9),
        ])
        eq = ExperimentQueue([Experiment("flip", run=lambda: 1)])
        eig = eq.expected_information_gain(
            eq.available()[0], hq, coin_likelihood, coin_outcome_space
        )
        self.assertGreater(eig, 0.0)

    def test_select_best_deterministic_tie_break(self):
        hq = HypothesisQueue([
            Hypothesis("fair", 0.5, lambda e: 0.5),
            Hypothesis("biased", 0.5, lambda e: 0.9),
        ])
        eq = ExperimentQueue([
            Experiment("z_exp", run=lambda: 1),
            Experiment("a_exp", run=lambda: 1),
        ])
        # both experiments have identical predict() behavior via coin_likelihood
        # (same hypothesis predictions regardless of eid), so EIG ties;
        # tie-break must select lexicographically smallest eid.
        result = eq.select_best(hq, coin_likelihood, coin_outcome_space)
        self.assertIsNotNone(result)
        experiment, eig = result
        self.assertEqual(experiment.eid, "a_exp")

    def test_select_best_prefers_information_over_lexical_order(self):
        def predict_a(eid):
            return 0.5 if eid == "a_noisy" else 0.99

        def predict_b(eid):
            return 0.5 if eid == "a_noisy" else 0.01

        hq = HypothesisQueue([
            Hypothesis("A", 0.5, predict_a),
            Hypothesis("B", 0.5, predict_b),
        ])
        eq = ExperimentQueue([
            Experiment("a_noisy", run=lambda: 1),
            Experiment("z_decisive", run=lambda: 1),
        ])
        experiment, eig = eq.select_best(hq, coin_likelihood, coin_outcome_space)
        self.assertEqual(experiment.eid, "z_decisive")
        self.assertGreater(eig, 0.0)

    def test_invalid_experiment_cost_rejected(self):
        with self.assertRaises(ValueError):
            ExperimentQueue([Experiment("free", run=lambda: 1, cost=0)])

    def test_select_best_none_when_empty(self):
        hq = HypothesisQueue([Hypothesis("A", 1.0, lambda e: 0.5)])
        eq = ExperimentQueue([])
        self.assertIsNone(eq.select_best(hq, coin_likelihood, coin_outcome_space))

    def test_mark_used_excludes_from_available(self):
        eq = ExperimentQueue([Experiment("e1", run=lambda: 1)])
        eq.mark_used("e1")
        self.assertEqual(eq.available(), [])


class TestCGQPlannerConvergence(unittest.TestCase):
    def _make_planner(self, true_bias, proof_margin=0.99, max_steps=200):
        # Three candidate hypotheses about a coin's P(heads=1).
        hyps = [
            Hypothesis("h_fair_0.5", 1 / 3, lambda e: 0.5),
            Hypothesis("h_biased_0.9", 1 / 3, lambda e: 0.9),
            Hypothesis("h_biased_0.1", 1 / 3, lambda e: 0.1),
        ]
        # Deterministic pseudo-random-free outcome generator: a fixed
        # periodic sequence approximating true_bias, so results are
        # reproducible across runs (no randomness).
        # Build a deterministic outcome cycle matching true_bias exactly
        # for common test biases (0.9 -> 9 heads per 10 flips, etc).
        if true_bias == 0.9:
            cycle = [1, 1, 1, 1, 1, 1, 1, 1, 1, 0]
        elif true_bias == 0.1:
            cycle = [0, 0, 0, 0, 0, 0, 0, 0, 0, 1]
        elif true_bias == 0.5:
            cycle = [1, 0]
        else:
            raise ValueError("unsupported test bias")

        state = {"i": 0}

        def make_runner():
            def run():
                v = cycle[state["i"] % len(cycle)]
                state["i"] += 1
                return v
            return run

        experiments = [
            Experiment(f"flip{i}", run=make_runner()) for i in range(max_steps)
        ]
        return CGQPlanner(
            hypotheses=hyps,
            experiments=experiments,
            likelihood_fn=coin_likelihood,
            outcome_space=coin_outcome_space,
            proof_margin=proof_margin,
            max_steps=max_steps,
        )

    def test_converges_to_correct_biased_hypothesis(self):
        planner = self._make_planner(true_bias=0.9)
        best = planner.run()
        self.assertEqual(best.hid, "h_biased_0.9")
        self.assertTrue(planner.converged())

    def test_converges_to_other_biased_hypothesis(self):
        planner = self._make_planner(true_bias=0.1)
        best = planner.run()
        self.assertEqual(best.hid, "h_biased_0.1")
        self.assertTrue(planner.converged())

    def test_deterministic_repeated_runs_identical(self):
        p1 = self._make_planner(true_bias=0.9)
        p2 = self._make_planner(true_bias=0.9)
        b1 = p1.run()
        b2 = p2.run()
        self.assertEqual(b1.hid, b2.hid)
        self.assertEqual(len(p1.history), len(p2.history))
        for r1, r2 in zip(p1.history, p2.history):
            self.assertEqual(r1.experiment_id, r2.experiment_id)
            self.assertEqual(r1.outcome, r2.outcome)
            self.assertAlmostEqual(r1.eig, r2.eig, places=12)

    def test_history_records_eig_and_weights(self):
        planner = self._make_planner(true_bias=0.9, max_steps=5)
        planner.run()
        self.assertGreater(len(planner.history), 0)
        first = planner.history[0]
        self.assertIsInstance(first.eig, float)
        self.assertIn("h_fair_0.5", first.weights_after)

    def test_halts_when_experiments_exhausted_without_margin(self):
        # Very strict margin, very few experiments -> should halt without
        # crashing once experiments run out, best() still callable.
        hyps = [
            Hypothesis("A", 0.5, lambda e: 0.5),
            Hypothesis("B", 0.5, lambda e: 0.5),
        ]
        experiments = [Experiment("only1", run=lambda: 1)]
        planner = CGQPlanner(
            hypotheses=hyps,
            experiments=experiments,
            likelihood_fn=coin_likelihood,
            outcome_space=coin_outcome_space,
            proof_margin=0.9999999,
            max_steps=10,
        )
        best = planner.run()
        self.assertIsNotNone(best)
        self.assertFalse(planner.converged())

    def test_single_hypothesis_trivially_converged(self):
        hyps = [Hypothesis("only", 1.0, lambda e: 0.5)]
        experiments = [Experiment("e1", run=lambda: 1)]
        planner = CGQPlanner(
            hypotheses=hyps,
            experiments=experiments,
            likelihood_fn=coin_likelihood,
            outcome_space=coin_outcome_space,
            proof_margin=0.99,
            max_steps=10,
        )
        best = planner.run()
        self.assertEqual(best.hid, "only")
        self.assertTrue(planner.converged())
        # No experiments needed since already at margin before any step.
        self.assertEqual(len(planner.history), 0)

    def test_contradictory_evidence_collapses_weight_fast(self):
        # A hypothesis that predicts impossible outcomes gets zeroed
        # immediately (hard counterexample), unlike gradual updates.
        hyps = [
            Hypothesis("possible", 0.5, lambda e: 0.5),
            Hypothesis("impossible_predicts_always_1", 0.5, lambda e: 1.0),
        ]
        experiments = [Experiment("flip", run=lambda: 0)]
        planner = CGQPlanner(
            hypotheses=hyps,
            experiments=experiments,
            likelihood_fn=coin_likelihood,
            outcome_space=coin_outcome_space,
            proof_margin=0.99,
            max_steps=1,
        )
        best = planner.run()
        self.assertEqual(best.hid, "possible")
        self.assertAlmostEqual(planner.hq.weights()["impossible_predicts_always_1"], 0.0, places=12)


if __name__ == "__main__":
    unittest.main(verbosity=2)
