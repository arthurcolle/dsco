#!/usr/bin/env python3
"""Deterministic regression tests for Chimera Router v2 planning."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, Iterable

import numpy as np

try:
    from .features import (
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        model_feature_names,
        task_feature_names,
    )
    from .model import CandidateRouter
    from .planner import PLAN_SCHEMA_VERSION, PlanningPolicy, plan_request
    from .serve import RouterState
except ImportError:  # Direct execution from this directory.
    from features import (
        MODEL_FEATURE_VERSION,
        TASK_FEATURE_VERSION,
        model_feature_names,
        task_feature_names,
    )
    from model import CandidateRouter
    from planner import PLAN_SCHEMA_VERSION, PlanningPolicy, plan_request
    from serve import RouterState


OUTCOMES = (
    "completion_success",
    "provider_failure",
    "tool_error",
    "cost_usd",
    "latency_s",
)


def _router() -> CandidateRouter:
    router = CandidateRouter(
        task_dim=512,
        model_dim=128,
        outcome_names=OUTCOMES,
        rank=4,
        seed=17,
        task_feature_names=task_feature_names(512),
        model_feature_names=model_feature_names(128),
        head_types=(
            "probability",
            "probability",
            "probability",
            "continuous",
            "continuous",
        ),
    )
    for name in router.parameter_names:
        getattr(router, name).fill(0.0)
    router.output_bias[:3] = np.asarray([1.4, -2.2, -2.2])
    router.outcome_mean[3:] = np.asarray([0.002, 0.5])
    router.training_metadata.update(
        {
            "task_feature_version": TASK_FEATURE_VERSION,
            "model_feature_version": MODEL_FEATURE_VERSION,
        }
    )
    return router


def _model(
    model_id: str,
    quality: float,
    *,
    prompt_price: float,
    completion_price: float,
    latency_s: float = 0.2,
    parameters: Iterable[str] = (),
) -> Dict[str, Any]:
    return {
        "id": model_id,
        "context_length": 131072,
        "architecture": {
            "input_modalities": ["text"],
            "output_modalities": ["text"],
        },
        "supported_parameters": list(parameters),
        "pricing": {
            "prompt": str(prompt_price),
            "completion": str(completion_price),
        },
        "latency_p95_s": latency_s,
        "benchmarks": {"artificial_analysis": {"intelligence_index": 100.0 * quality}},
    }


def _request(**extra: Any) -> Dict[str, Any]:
    request: Dict[str, Any] = {
        "prompt": "Give a deterministic answer.",
        "estimated_prompt_tokens": 1000,
        "estimated_completion_tokens": 500,
    }
    request.update(extra)
    return request


class PlanningPolicyTests(unittest.TestCase):
    def test_policy_validation_and_latency_alias(self) -> None:
        policy = PlanningPolicy.from_mapping(
            {"max_calls": 3, "latency_sla_ms": 750, "mode": "parallel"}
        )
        self.assertEqual(policy.strategy, "parallel")
        self.assertEqual(policy.max_calls, 3)
        self.assertEqual(policy.max_expected_latency_s, 0.75)
        with self.assertRaisesRegex(ValueError, "strategy"):
            PlanningPolicy.from_mapping({"strategy": "spend-everything"})


class PlannerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.router = _router()

    def test_safe_default_is_one_declarative_direct_call(self) -> None:
        catalog = [
            _model(
                "vendor/direct",
                0.90,
                prompt_price=0.0000005,
                completion_price=0.000001,
            )
        ]
        plan = plan_request(self.router, _request(), catalog)
        self.assertEqual(plan["status"], "ready")
        self.assertEqual(plan["plan_type"], "direct")
        self.assertEqual(plan["worst_case_calls"], 1)
        self.assertTrue(plan["declarative_only"])
        self.assertFalse(plan["executes_models"])
        self.assertTrue(plan["requires_explicit_execution"])
        self.assertFalse(
            plan["stages"][0]["request_patch"]["provider"]["allow_fallbacks"]
        )

    def test_budget_refusal_never_returns_a_spend_plan(self) -> None:
        catalog = [
            _model(
                "vendor/over-budget",
                0.90,
                prompt_price=0.000001,
                completion_price=0.000002,
            )
        ]
        plan = plan_request(
            self.router,
            _request(),
            catalog,
            planning_policy={"max_expected_cost_usd": 0.0005},
        )
        self.assertEqual(plan["status"], "refused")
        self.assertIsNone(plan["plan_type"])
        self.assertEqual(plan["stages"], [])
        self.assertEqual(plan["fallback_behavior"]["action"], "do_not_execute")
        self.assertIn(
            "budget_refusal:no_direct_model_within_expected_cost",
            plan["explainability"]["refusal_reasons"],
        )

    def test_expected_latency_limit_selects_a_faster_feasible_candidate(self) -> None:
        catalog = [
            _model(
                "vendor/slow-high-quality",
                0.95,
                prompt_price=0.0000005,
                completion_price=0.000001,
                latency_s=1.5,
            ),
            _model(
                "vendor/fast",
                0.82,
                prompt_price=0.0000005,
                completion_price=0.000001,
                latency_s=0.15,
            ),
        ]
        plan = plan_request(
            self.router,
            _request(),
            catalog,
            planning_policy={
                "max_expected_latency_s": 0.25,
                "latency_preference": "low",
            },
        )
        self.assertEqual(plan["plan_type"], "direct")
        self.assertEqual(plan["roles"]["primary"], "vendor/fast")
        self.assertLessEqual(plan["expected_latency_s"], 0.25)

    def test_cascade_has_local_verifier_and_conditional_escalation(self) -> None:
        catalog = [
            _model(
                "premium/solver",
                0.95,
                prompt_price=0.000005,
                completion_price=0.000010,
                latency_s=1.0,
                parameters=("structured_outputs",),
            ),
            _model(
                "economy/draft",
                0.85,
                prompt_price=0.0000002,
                completion_price=0.0000008,
                latency_s=0.1,
                parameters=("structured_outputs",),
            ),
        ]
        plan = plan_request(
            self.router,
            _request(required_parameters=["structured_outputs"]),
            catalog,
            planning_policy={
                "strategy": "cascade",
                "max_calls": 2,
                "max_expected_cost_usd": 0.003,
                "local_verifiers": ["caller_unit_tests"],
            },
        )
        self.assertEqual(plan["status"], "ready")
        self.assertEqual(plan["plan_type"], "cascade")
        self.assertEqual(plan["roles"]["draft"], "economy/draft")
        self.assertEqual(plan["roles"]["escalation"], "premium/solver")
        self.assertLess(plan["expected_cost_usd"], 0.003)
        self.assertGreater(plan["worst_case_cost_usd"], 0.003)
        self.assertEqual(plan["worst_case_calls"], 2)
        verifier = plan["stages"][1]
        self.assertEqual(verifier["kind"], "local_verifier")
        self.assertIn("json_schema_valid", verifier["checks"])
        self.assertIn("caller_unit_tests", verifier["checks"])
        self.assertEqual(verifier["on_fail"], "call-escalation")

    def test_parallel_requires_risk_gain_and_declared_three_call_budget(self) -> None:
        catalog = [
            _model(
                "alpha/candidate-a",
                0.75,
                prompt_price=0.0000005,
                completion_price=0.000001,
                latency_s=0.3,
            ),
            _model(
                "beta/candidate-b",
                0.72,
                prompt_price=0.0000005,
                completion_price=0.000001,
                latency_s=0.25,
            ),
        ]
        plan = plan_request(
            self.router,
            _request(),
            catalog,
            planning_policy={
                "strategy": "parallel",
                "max_calls": 3,
                "max_expected_cost_usd": 0.004,
                "parallel_risk_threshold": 0.20,
                "min_parallel_expected_gain": 0.04,
            },
        )
        self.assertEqual(plan["plan_type"], "parallel")
        self.assertEqual(plan["worst_case_calls"], 3)
        self.assertEqual(len(plan["roles"]["candidates"]), 2)
        self.assertIsNotNone(plan["roles"]["judge"])
        self.assertEqual(plan["stages"][0]["parallel_group"], "candidate-generation")
        self.assertEqual(plan["stages"][1]["parallel_group"], "candidate-generation")
        self.assertEqual(
            plan["stages"][2]["when"],
            "candidate-generation:at_least_one_success",
        )

    def test_parallel_request_falls_back_to_direct_when_risk_is_low(self) -> None:
        catalog = [
            _model(
                "alpha/reliable",
                0.97,
                prompt_price=0.0000005,
                completion_price=0.000001,
            ),
            _model(
                "beta/also-reliable",
                0.95,
                prompt_price=0.0000005,
                completion_price=0.000001,
            ),
        ]
        plan = plan_request(
            self.router,
            _request(),
            catalog,
            planning_policy={
                "strategy": "parallel",
                "max_calls": 3,
                "max_expected_cost_usd": 0.004,
            },
        )
        self.assertEqual(plan["plan_type"], "direct")
        self.assertIn(
            "requested_parallel_not_warranted; used_safe_direct",
            plan["explainability"]["selection_notes"],
        )
        reasons = plan["explainability"]["alternatives"]["parallel"]["reasons"]
        self.assertTrue(
            any("parallel_risk_below_threshold" in item for item in reasons)
        )

    def test_live_unseen_candidate_is_plannable_without_a_fixed_id_head(self) -> None:
        catalog = [
            _model(
                "legacy/known",
                0.50,
                prompt_price=0.0,
                completion_price=0.0,
            ),
            _model(
                "future-lab/model-2041",
                0.90,
                prompt_price=0.0000005,
                completion_price=0.000001,
            ),
        ]
        plan = plan_request(self.router, _request(), catalog)
        self.assertEqual(plan["plan_type"], "direct")
        self.assertEqual(plan["roles"]["primary"], "future-lab/model-2041")
        self.assertEqual(plan["ranked_candidates"][0]["model"], "future-lab/model-2041")

    def test_router_state_exposes_plan_contract_without_execution(self) -> None:
        catalog = [
            _model(
                "vendor/service-model",
                0.88,
                prompt_price=0.0000005,
                completion_price=0.000001,
            )
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "router.npz"
            catalog_path = root / "catalog.json"
            self.router.save(
                str(artifact),
                {
                    "task_feature_version": TASK_FEATURE_VERSION,
                    "model_feature_version": MODEL_FEATURE_VERSION,
                },
            )
            catalog_path.write_text(json.dumps({"data": catalog}), encoding="utf-8")
            state = RouterState(artifact, catalog_path)
            health = state.health()
            self.assertEqual(health["plan_schema_version"], PLAN_SCHEMA_VERSION)
            plan = state.plan(
                {
                    "request": _request(),
                    "planner": {"max_calls": 1},
                }
            )
            self.assertEqual(plan["plan_type"], "direct")
            self.assertFalse(plan["executes_models"])
            self.assertTrue(plan["router_sha256"])
            self.assertTrue(plan["catalog_sha256"])
            self.assertIn('"plan_type": "direct"', json.dumps(plan, sort_keys=True))


if __name__ == "__main__":
    unittest.main(verbosity=2)
