#!/usr/bin/env python3
"""Focused integration tests for ensemble and semantic-quality routing."""

from __future__ import annotations

import unittest

import numpy as np

from .ensemble import QUALITY_TASK_FEATURE_VERSION, RouterEnsemble
from .features import (
    DEFAULT_MODEL_DIM,
    DEFAULT_TASK_DIM,
    MODEL_FEATURE_VERSION,
    TASK_FEATURE_VERSION,
    model_features,
)
from .model import CandidateRouter
from .route import route_request


def _operational_member(bias: float) -> CandidateRouter:
    member = CandidateRouter(
        DEFAULT_TASK_DIM,
        DEFAULT_MODEL_DIM,
        ("completion_success", "provider_failure", "tool_error"),
        rank=2,
        seed=3,
        head_types=("probability", "probability", "probability"),
    )
    for name in member.parameter_names:
        getattr(member, name).fill(0.0)
    member.task_train_min.fill(-1.0)
    member.task_train_max.fill(1.0)
    member.model_train_min.fill(-1.0)
    member.model_train_max.fill(1.0)
    member.output_bias[:] = (bias, -3.0, -3.0)
    member.training_metadata = {
        "artifact_role": "operational_router",
        "task_feature_version": TASK_FEATURE_VERSION,
        "model_feature_version": MODEL_FEATURE_VERSION,
    }
    return member


def _quality_member(good: dict, bad: dict, scale: float) -> CandidateRouter:
    member = CandidateRouter(
        1,
        DEFAULT_MODEL_DIM,
        (
            "general_intelligence_quality",
            "coding_quality",
            "agentic_quality",
        ),
        rank=2,
        seed=7,
        task_feature_names=("numeric:bias",),
        head_types=("probability", "probability", "probability"),
    )
    for name in member.parameter_names:
        getattr(member, name).fill(0.0)
    member.task_train_min.fill(-1.0)
    member.task_train_max.fill(1.0)
    member.model_train_min.fill(-1.0)
    member.model_train_max.fill(1.0)
    good_vector = model_features(good, include_benchmark_priors=False).astype(
        np.float64
    )
    bad_vector = model_features(bad, include_benchmark_priors=False).astype(np.float64)
    direction = good_vector - bad_vector
    denominator = float(np.dot(direction, direction))
    if denominator <= 0.0:
        raise AssertionError("semantic fixture vectors unexpectedly match")
    member.model_skip[:, :] = (direction[:, None] / denominator) * scale
    member.training_metadata = {
        "artifact_role": "catalog_quality_predictor",
        "task_feature_version": QUALITY_TASK_FEATURE_VERSION,
        "model_feature_version": MODEL_FEATURE_VERSION,
    }
    return member


class RouteV2Tests(unittest.TestCase):
    def setUp(self) -> None:
        architecture = {
            "input_modalities": ["text"],
            "output_modalities": ["text"],
        }
        self.good = {
            "id": "future-labs/compiler-2041",
            "name": "Compiler 2041",
            "description": (
                "A software engineering, coding, tool-use, structured JSON, "
                "reasoning and autonomous workflow model."
            ),
            "context_length": 131072,
            "architecture": architecture,
            "supported_parameters": ["tools", "structured_outputs"],
            "pricing": {"prompt": "0.000001", "completion": "0.000001"},
        }
        self.bad = {
            "id": "future-labs/audio-2041",
            "name": "Audio 2041",
            "description": "A speech transcription, voice and audio rendering model.",
            "context_length": 131072,
            "architecture": architecture,
            "supported_parameters": ["tools", "structured_outputs"],
            "pricing": {"prompt": "0.000001", "completion": "0.000001"},
        }
        operational_members = [_operational_member(-0.2), _operational_member(0.2)]
        self.operational = RouterEnsemble(
            operational_members,
            metadata={
                "artifact_role": "operational_router",
                "task_feature_version": TASK_FEATURE_VERSION,
                "model_feature_version": MODEL_FEATURE_VERSION,
            },
        )
        quality_members = [
            _quality_member(self.good, self.bad, 2.0),
            _quality_member(self.good, self.bad, 2.4),
        ]
        self.quality = RouterEnsemble(
            quality_members,
            metadata={
                "artifact_role": "catalog_quality_predictor",
                "task_feature_version": QUALITY_TASK_FEATURE_VERSION,
                "model_feature_version": MODEL_FEATURE_VERSION,
            },
        )

    def test_semantic_quality_routes_unbenchmarked_future_model(self) -> None:
        decision = route_request(
            self.operational,
            {
                "prompt": "Implement a compiler pass and return structured JSON.",
                "estimated_prompt_tokens": 100,
                "estimated_completion_tokens": 100,
            },
            [self.bad, self.good],
            quality_router=self.quality,
            policy={
                "cost_weight": 0.0,
                "latency_weight": 0.0,
                "failure_weight": 0.0,
                "success_weight": 0.0,
                "preference_weight": 0.0,
                "benchmark_weight": 1.0,
                "uncertainty_weight": 0.0,
            },
        )
        self.assertEqual(decision["selected_model"], self.good["id"])
        self.assertEqual(decision["router_kind"], "ensemble")
        self.assertEqual(decision["ensemble_members"], 2)
        self.assertTrue(decision["quality_router"])
        selected = decision["candidates"][0]
        self.assertEqual(selected["quality_source"], "semantic_quality_ensemble")
        self.assertIsNone(selected["benchmark_quality"])
        self.assertGreater(
            selected["catalog_quality_prediction"],
            decision["candidates"][1]["catalog_quality_prediction"],
        )
        self.assertLessEqual(
            selected["catalog_quality_lcb"], selected["catalog_quality_prediction"]
        )
        self.assertGreater(selected["epistemic_uncertainty"], 0.0)

    def test_live_benchmark_remains_authoritative_over_learned_prior(self) -> None:
        observed = dict(self.bad)
        observed["benchmarks"] = {
            "artificial_analysis": {
                "intelligence_index": 99,
                "coding_index": 99,
                "agentic_index": 99,
            }
        }
        decision = route_request(
            self.operational,
            {"prompt": "Write code", "estimated_completion_tokens": 50},
            [self.good, observed],
            quality_router=self.quality,
            policy={
                "cost_weight": 0.0,
                "latency_weight": 0.0,
                "failure_weight": 0.0,
                "success_weight": 0.0,
                "preference_weight": 0.0,
                "benchmark_weight": 1.0,
                "uncertainty_weight": 0.0,
            },
        )
        self.assertEqual(decision["selected_model"], observed["id"])
        self.assertEqual(
            decision["candidates"][0]["quality_source"], "live_catalog_benchmark"
        )

    def test_quality_artifact_contract_is_strict(self) -> None:
        invalid = RouterEnsemble(
            [_quality_member(self.good, self.bad, 1.0)],
            metadata={
                "artifact_role": "catalog_quality_predictor",
                "task_feature_version": "wrong",
                "model_feature_version": MODEL_FEATURE_VERSION,
            },
        )
        with self.assertRaisesRegex(ValueError, "quality router task feature mismatch"):
            route_request(
                self.operational,
                {"prompt": "code"},
                [self.good],
                quality_router=invalid,
            )

    def test_plain_text_route_rejects_media_generators_with_auxiliary_text(
        self,
    ) -> None:
        music = dict(self.bad)
        music.update(
            {
                "id": "google/lyria-future",
                "name": "Lyria Future Music Generator",
                "description": "Generates songs and 48kHz audio from a prompt.",
                "architecture": {
                    "input_modalities": ["text", "image"],
                    "output_modalities": ["text", "audio"],
                },
                "pricing": {"prompt": "0", "completion": "0"},
            }
        )
        decision = route_request(
            self.operational,
            {"prompt": "Implement and test a C parser."},
            [music, self.good],
            quality_router=self.quality,
            top_k=2,
        )
        self.assertEqual(decision["selected_model"], self.good["id"])
        rejected = next(
            row for row in decision["candidates"] if row["model_id"] == music["id"]
        )
        self.assertFalse(rejected["feasible"])
        self.assertIn("unexpected_output_modality:audio", rejected["rejection_reasons"])

        opted_in = route_request(
            self.operational,
            {"prompt": "Generate a song."},
            [music],
            quality_router=self.quality,
            policy={"allow_additional_output_modalities": True},
        )
        self.assertEqual(opted_in["selected_model"], music["id"])

    def test_nested_openrouter_virtual_model_requires_explicit_opt_in(self) -> None:
        nested = dict(self.good)
        nested.update(
            {
                "id": "openrouter/free",
                "name": "OpenRouter Free Router",
                "description": "A dynamic router over free models.",
                "pricing": {"prompt": "0", "completion": "0"},
            }
        )
        decision = route_request(
            self.operational,
            {"prompt": "Implement and test a C parser."},
            [nested, self.good],
            quality_router=self.quality,
            top_k=2,
        )
        self.assertEqual(decision["selected_model"], self.good["id"])
        rejected = next(
            row for row in decision["candidates"] if row["model_id"] == nested["id"]
        )
        self.assertIn("nested_router_not_allowed", rejected["rejection_reasons"])

        opted_in = route_request(
            self.operational,
            {"prompt": "Use OpenRouter's own router."},
            [nested],
            quality_router=self.quality,
            policy={"allow_router_models": True},
        )
        self.assertEqual(opted_in["selected_model"], nested["id"])


if __name__ == "__main__":
    unittest.main()
