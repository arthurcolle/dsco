#!/usr/bin/env python3
"""Budget-aware declarative orchestration plans for Chimera Router.

The planner never invokes a model, a verifier, or a network service.  It turns
the candidate-conditioned ranking from :mod:`route` into one of three bounded
plan templates:

* one direct model call;
* a cheap draft, deterministic local verification, then conditional escalation;
* two parallel candidates followed by a judge when measured risk and expected
  quality gain justify all three calls.

The default is deliberately one direct call.  Multi-call plans require both an
explicit call allowance and an explicit cost ceiling.  All cost and latency
figures are estimates from the same live descriptors used by the router; a
caller must still explicitly execute the returned stages.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass, fields
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple, Union

try:
    from .ensemble import RouterEnsemble
    from .model import CandidateRouter
    from .route import ROUTE_SCHEMA_VERSION, route_request
except ImportError:  # Direct execution.
    from ensemble import RouterEnsemble
    from model import CandidateRouter
    from route import ROUTE_SCHEMA_VERSION, route_request


PLAN_SCHEMA_VERSION = 2


def _finite_float(value: Any, name: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError("planning policy %s must be numeric" % name) from exc
    if not math.isfinite(result):
        raise ValueError("planning policy %s must be finite" % name)
    return result


def _as_bool(value: Any) -> bool:
    if isinstance(value, str):
        normalized = value.strip().casefold()
        if normalized in {"1", "true", "yes", "on"}:
            return True
        if normalized in {"0", "false", "no", "off"}:
            return False
    return bool(value)


def _string_tuple(value: Any) -> Tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        return (value,)
    if isinstance(value, Sequence):
        return tuple(str(item) for item in value if item is not None)
    raise ValueError("local_verifiers must be a string or a sequence")


@dataclass(frozen=True)
class PlanningPolicy:
    """Hard plan limits plus conservative plan-selection thresholds."""

    strategy: str = "auto"
    max_calls: int = 1
    max_expected_cost_usd: Optional[float] = None
    max_worst_case_cost_usd: Optional[float] = None
    max_expected_latency_s: Optional[float] = None
    max_worst_case_latency_s: Optional[float] = None
    latency_preference: str = "balanced"
    allow_cascade: bool = True
    allow_parallel: bool = True
    min_cascade_savings_fraction: float = 0.10
    min_cascade_savings_usd: float = 0.0
    max_cascade_quality_drop: float = 0.10
    parallel_risk_threshold: float = 0.30
    min_parallel_expected_gain: float = 0.05
    local_verifiers: Tuple[str, ...] = ()
    local_verifier_latency_s: float = 0.0

    @classmethod
    def from_mapping(cls, value: Optional[Mapping[str, Any]]) -> "PlanningPolicy":
        raw: Dict[str, Any] = dict(value or {})
        aliases = {
            "plan_type": "strategy",
            "mode": "strategy",
            "budget_usd": "max_expected_cost_usd",
            "max_expected_cost": "max_expected_cost_usd",
            "max_worst_cost_usd": "max_worst_case_cost_usd",
            "latency_sla_s": "max_expected_latency_s",
            "max_latency_s": "max_expected_latency_s",
            "max_worst_latency_s": "max_worst_case_latency_s",
            "verifiers": "local_verifiers",
        }
        for source, target in aliases.items():
            if source in raw and target not in raw:
                raw[target] = raw[source]
        if "latency_sla_ms" in raw and "max_expected_latency_s" not in raw:
            raw["max_expected_latency_s"] = (
                _finite_float(raw["latency_sla_ms"], "latency_sla_ms") / 1000.0
            )

        accepted = {field.name for field in fields(cls)}
        kwargs = {name: raw[name] for name in accepted if name in raw}
        strategy = str(kwargs.get("strategy", cls.strategy)).strip().casefold()
        if strategy not in {"auto", "direct", "cascade", "parallel"}:
            raise ValueError(
                "planning policy strategy must be auto/direct/cascade/parallel"
            )
        kwargs["strategy"] = strategy
        latency_preference = (
            str(kwargs.get("latency_preference", cls.latency_preference))
            .strip()
            .casefold()
        )
        if latency_preference not in {"low", "balanced", "quality"}:
            raise ValueError(
                "planning policy latency_preference must be low/balanced/quality"
            )
        kwargs["latency_preference"] = latency_preference

        if "max_calls" in kwargs:
            try:
                max_calls = int(kwargs["max_calls"])
            except (TypeError, ValueError, OverflowError) as exc:
                raise ValueError(
                    "planning policy max_calls must be an integer"
                ) from exc
            if max_calls < 0 or max_calls > 16:
                raise ValueError("planning policy max_calls must be in [0, 16]")
            kwargs["max_calls"] = max_calls

        numeric = (
            "max_expected_cost_usd",
            "max_worst_case_cost_usd",
            "max_expected_latency_s",
            "max_worst_case_latency_s",
            "min_cascade_savings_fraction",
            "min_cascade_savings_usd",
            "max_cascade_quality_drop",
            "parallel_risk_threshold",
            "min_parallel_expected_gain",
            "local_verifier_latency_s",
        )
        for name in numeric:
            if name not in kwargs or kwargs[name] is None:
                continue
            converted = _finite_float(kwargs[name], name)
            if converted < 0.0:
                raise ValueError("planning policy %s must be non-negative" % name)
            kwargs[name] = converted
        for name in ("allow_cascade", "allow_parallel"):
            if name in kwargs:
                kwargs[name] = _as_bool(kwargs[name])
        if "local_verifiers" in kwargs:
            kwargs["local_verifiers"] = _string_tuple(kwargs["local_verifiers"])
        return cls(**kwargs)


def _merge_planning_policy(
    request: Mapping[str, Any], overlay: Optional[Mapping[str, Any]]
) -> PlanningPolicy:
    merged: Dict[str, Any] = {}
    for key in ("planning_policy", "planner", "orchestration_policy"):
        nested = request.get(key)
        if isinstance(nested, Mapping):
            merged.update(nested)
    for name in (
        "strategy",
        "plan_type",
        "max_calls",
        "max_expected_cost_usd",
        "max_worst_case_cost_usd",
        "max_expected_latency_s",
        "max_worst_case_latency_s",
        "latency_preference",
        "allow_cascade",
        "allow_parallel",
        "min_cascade_savings_fraction",
        "min_cascade_savings_usd",
        "max_cascade_quality_drop",
        "parallel_risk_threshold",
        "min_parallel_expected_gain",
        "local_verifiers",
        "local_verifier_latency_s",
    ):
        if name in request:
            merged[name] = request[name]
    if overlay:
        merged.update(overlay)
    return PlanningPolicy.from_mapping(merged)


def _candidate_quality(candidate: Mapping[str, Any]) -> Tuple[float, str]:
    benchmark = candidate.get("benchmark_quality")
    if benchmark is not None:
        return min(max(float(benchmark), 0.0), 1.0), "live_benchmark"
    prior = candidate.get("quality_prior")
    if prior is not None:
        return (
            min(max(float(prior), 0.0), 1.0),
            str(candidate.get("quality_source") or "router_quality_prior"),
        )
    predictions = candidate.get("predictions")
    if isinstance(predictions, Mapping):
        for name in ("quality", "reward", "completion_success", "success"):
            value = predictions.get(name)
            if value is not None:
                return min(max(float(value), 0.0), 1.0), "router:%s" % name
    return 0.5, "unknown_prior"


def _candidate_risk(candidate: Mapping[str, Any]) -> float:
    quality, _ = _candidate_quality(candidate)
    failure = float(candidate.get("failure_probability") or 0.0)
    support = max(
        float(
            candidate.get("combined_uncertainty")
            if candidate.get("combined_uncertainty") is not None
            else candidate.get("support_distance") or 0.0
        ),
        0.0,
    )
    uncertainty = support / (1.0 + support)
    return min(max(max(failure, 1.0 - quality, 0.35 * uncertainty), 0.0), 1.0)


def _candidate_cost(candidate: Mapping[str, Any]) -> Optional[float]:
    value = candidate.get("expected_cost_usd")
    return None if value is None else max(float(value), 0.0)


def _candidate_latency(candidate: Mapping[str, Any]) -> Optional[float]:
    value = candidate.get("expected_latency_s")
    return None if value is None else max(float(value), 0.0)


def _candidate_cost_or_inf(candidate: Mapping[str, Any]) -> float:
    cost = _candidate_cost(candidate)
    return math.inf if cost is None else cost


def _provider(model_id: str) -> str:
    normalized = str(model_id).removeprefix("openrouter/")
    return normalized.split("/", 1)[0].casefold() if "/" in normalized else "unknown"


def _dedupe_candidates(
    candidates: Sequence[Mapping[str, Any]], limit: int
) -> List[Mapping[str, Any]]:
    result: List[Mapping[str, Any]] = []
    seen = set()
    for candidate in candidates:
        model_id = str(candidate.get("model_id"))
        if model_id in seen:
            continue
        seen.add(model_id)
        result.append(candidate)
        if len(result) >= limit:
            break
    return result


def _sum_known(values: Sequence[Optional[float]]) -> Optional[float]:
    if any(value is None for value in values):
        return None
    return float(sum(float(value) for value in values if value is not None))


def _candidate_summary(candidate: Mapping[str, Any]) -> Dict[str, Any]:
    quality, source = _candidate_quality(candidate)
    return {
        "model": candidate.get("model_id"),
        "utility": candidate.get("utility"),
        "quality": quality,
        "quality_source": source,
        "risk": _candidate_risk(candidate),
        "expected_cost_usd": _candidate_cost(candidate),
        "expected_latency_s": _candidate_latency(candidate),
        "support_distance": candidate.get("support_distance"),
    }


def _model_stage(
    stage_id: str,
    role: str,
    candidate: Mapping[str, Any],
    *,
    when: str,
    parallel_group: Optional[str] = None,
) -> Dict[str, Any]:
    quality, quality_source = _candidate_quality(candidate)
    stage: Dict[str, Any] = {
        "id": stage_id,
        "kind": "model_call",
        "role": role,
        "model": candidate.get("model_id"),
        "when": when,
        "estimated_cost_usd": _candidate_cost(candidate),
        "estimated_latency_s": _candidate_latency(candidate),
        "estimated_quality": quality,
        "quality_source": quality_source,
        "failure_probability": candidate.get("failure_probability"),
        "request_patch": {
            "model": candidate.get("model_id"),
            "provider": {
                "allow_fallbacks": False,
                "require_parameters": True,
            },
        },
    }
    if parallel_group is not None:
        stage["parallel_group"] = parallel_group
    return stage


def _verifier_checks(
    request: Mapping[str, Any],
    route_decision: Mapping[str, Any],
    policy: PlanningPolicy,
) -> List[str]:
    checks = ["provider_success", "nonempty_response", "finish_reason_allowed"]
    route_policy = route_decision.get("policy")
    parameters = set()
    if isinstance(route_policy, Mapping):
        raw = route_policy.get("required_parameters") or ()
        if isinstance(raw, str):
            parameters.add(raw.casefold())
        elif isinstance(raw, Sequence):
            parameters.update(str(value).casefold() for value in raw)
    if parameters.intersection(
        {"structured_outputs", "response_format", "json_schema"}
    ):
        checks.extend(["json_parse", "json_schema_valid"])
    if parameters.intersection({"tools", "tool_choice"}):
        checks.extend(["tool_call_schema_valid", "tool_result_success"])
    supplied = request.get("deterministic_verifiers")
    if isinstance(supplied, str):
        checks.append(supplied)
    elif isinstance(supplied, Sequence):
        checks.extend(str(value) for value in supplied if value is not None)
    checks.extend(policy.local_verifiers)
    # Preserve order while making the contract deterministic.
    return list(dict.fromkeys(value.strip() for value in checks if value.strip()))


def _plan_violations(plan: Mapping[str, Any], policy: PlanningPolicy) -> List[str]:
    violations: List[str] = []
    calls = int(plan.get("worst_case_calls") or 0)
    if calls > policy.max_calls:
        violations.append("max_calls_exceeded")
    expected_cost = plan.get("expected_cost_usd")
    if policy.max_expected_cost_usd is not None:
        if expected_cost is None:
            violations.append("expected_cost_unknown")
        elif float(expected_cost) > policy.max_expected_cost_usd + 1.0e-12:
            violations.append("max_expected_cost_usd_exceeded")
    worst_cost = plan.get("worst_case_cost_usd")
    if policy.max_worst_case_cost_usd is not None:
        if worst_cost is None:
            violations.append("worst_case_cost_unknown")
        elif float(worst_cost) > policy.max_worst_case_cost_usd + 1.0e-12:
            violations.append("max_worst_case_cost_usd_exceeded")
    expected_latency = plan.get("expected_latency_s")
    if policy.max_expected_latency_s is not None:
        if expected_latency is None:
            violations.append("expected_latency_unknown")
        elif float(expected_latency) > policy.max_expected_latency_s + 1.0e-12:
            violations.append("max_expected_latency_s_exceeded")
    worst_latency = plan.get("worst_case_latency_s")
    if policy.max_worst_case_latency_s is not None:
        if worst_latency is None:
            violations.append("worst_case_latency_unknown")
        elif float(worst_latency) > policy.max_worst_case_latency_s + 1.0e-12:
            violations.append("max_worst_case_latency_s_exceeded")
    return violations


def _base_plan(
    plan_type: str,
    roles: Mapping[str, Any],
    stages: Sequence[Mapping[str, Any]],
    *,
    expected_calls: float,
    worst_case_calls: int,
    expected_cost_usd: Optional[float],
    worst_case_cost_usd: Optional[float],
    expected_latency_s: Optional[float],
    worst_case_latency_s: Optional[float],
    expected_quality: float,
    risk_score: float,
    triggers: Sequence[Mapping[str, Any]],
    fallback_behavior: Mapping[str, Any],
    reasons: Sequence[str],
) -> Dict[str, Any]:
    return {
        "plan_type": plan_type,
        "roles": dict(roles),
        "stages": [dict(stage) for stage in stages],
        "expected_calls": float(expected_calls),
        "worst_case_calls": int(worst_case_calls),
        "expected_cost_usd": expected_cost_usd,
        "worst_case_cost_usd": worst_case_cost_usd,
        "expected_latency_s": expected_latency_s,
        "worst_case_latency_s": worst_case_latency_s,
        "expected_quality": float(expected_quality),
        "risk_score": float(risk_score),
        "triggers": [dict(trigger) for trigger in triggers],
        "fallback_behavior": dict(fallback_behavior),
        "selection_reasons": list(reasons),
    }


def _direct_plan(
    candidate: Mapping[str, Any], suggested_replans: Sequence[str]
) -> Dict[str, Any]:
    quality, _ = _candidate_quality(candidate)
    cost = _candidate_cost(candidate)
    latency = _candidate_latency(candidate)
    return _base_plan(
        "direct",
        {"primary": candidate.get("model_id")},
        [_model_stage("call-primary", "primary", candidate, when="always")],
        expected_calls=1.0,
        worst_case_calls=1,
        expected_cost_usd=cost,
        worst_case_cost_usd=cost,
        expected_latency_s=latency,
        worst_case_latency_s=latency,
        expected_quality=quality,
        risk_score=_candidate_risk(candidate),
        triggers=(
            {
                "event": "provider_or_transport_error",
                "action": "stop_and_return_for_replan",
            },
        ),
        fallback_behavior={
            "automatic_model_fallbacks": [],
            "on_failure": "stop_and_return_for_replan",
            "suggested_replan_models": list(suggested_replans),
            "on_budget_exhaustion": "stop",
        },
        reasons=("safe default: one router-ranked feasible model call",),
    )


def _cascade_plans(
    candidates: Sequence[Mapping[str, Any]],
    request: Mapping[str, Any],
    route_decision: Mapping[str, Any],
    policy: PlanningPolicy,
) -> Tuple[List[Dict[str, Any]], List[str]]:
    reasons: List[str] = []
    if not policy.allow_cascade:
        return [], ["cascade_disabled"]
    if policy.max_calls < 2:
        return [], ["cascade_requires_max_calls_at_least_2"]
    if policy.max_expected_cost_usd is None and policy.max_worst_case_cost_usd is None:
        return [], ["cascade_requires_explicit_cost_budget"]

    checks = _verifier_checks(request, route_decision, policy)
    plans: List[Dict[str, Any]] = []
    for escalation_rank, escalation in enumerate(candidates):
        escalation_cost = _candidate_cost(escalation)
        escalation_latency = _candidate_latency(escalation)
        if escalation_cost is None:
            continue
        escalation_quality, _ = _candidate_quality(escalation)
        draft_options: List[Tuple[float, float, Dict[str, Any]]] = []
        for draft in candidates:
            if draft.get("model_id") == escalation.get("model_id"):
                continue
            draft_cost = _candidate_cost(draft)
            if draft_cost is None or draft_cost >= escalation_cost:
                continue
            draft_latency = _candidate_latency(draft)
            draft_quality, _ = _candidate_quality(draft)
            escalation_probability = min(max(_candidate_risk(draft), 0.05), 0.95)
            expected_quality = (
                1.0 - escalation_probability
            ) * draft_quality + escalation_probability * escalation_quality
            quality_drop = max(escalation_quality - expected_quality, 0.0)
            if quality_drop > policy.max_cascade_quality_drop:
                continue
            expected_cost = draft_cost + escalation_probability * escalation_cost
            worst_cost = draft_cost + escalation_cost
            savings = escalation_cost - expected_cost
            savings_fraction = savings / max(escalation_cost, 1.0e-12)
            if savings < policy.min_cascade_savings_usd:
                continue
            if savings_fraction < policy.min_cascade_savings_fraction:
                continue
            expected_latency = (
                None
                if draft_latency is None or escalation_latency is None
                else draft_latency
                + policy.local_verifier_latency_s
                + escalation_probability * escalation_latency
            )
            worst_latency = _sum_known(
                (
                    draft_latency,
                    policy.local_verifier_latency_s,
                    escalation_latency,
                )
            )
            risk = min(
                (1.0 - escalation_probability) * _candidate_risk(draft)
                + escalation_probability * _candidate_risk(escalation),
                1.0,
            )
            plan = _base_plan(
                "cascade",
                {
                    "draft": draft.get("model_id"),
                    "local_verifier": "deterministic",
                    "escalation": escalation.get("model_id"),
                },
                (
                    _model_stage("call-draft", "draft", draft, when="always"),
                    {
                        "id": "verify-draft",
                        "kind": "local_verifier",
                        "role": "deterministic_verifier",
                        "model": None,
                        "when": "after:call-draft",
                        "checks": checks,
                        "on_pass": "accept_draft",
                        "on_fail": "call-escalation",
                        "estimated_cost_usd": 0.0,
                        "estimated_latency_s": policy.local_verifier_latency_s,
                    },
                    _model_stage(
                        "call-escalation",
                        "escalation",
                        escalation,
                        when="verify-draft:failed",
                    ),
                ),
                expected_calls=1.0 + escalation_probability,
                worst_case_calls=2,
                expected_cost_usd=expected_cost,
                worst_case_cost_usd=worst_cost,
                expected_latency_s=expected_latency,
                worst_case_latency_s=worst_latency,
                expected_quality=expected_quality,
                risk_score=risk,
                triggers=(
                    {
                        "event": "draft_provider_error",
                        "action": "call-escalation",
                    },
                    {
                        "event": "any_deterministic_check_failed",
                        "checks": checks,
                        "action": "call-escalation",
                    },
                    {
                        "event": "all_deterministic_checks_passed",
                        "action": "accept_draft",
                    },
                    {
                        "event": "escalation_error",
                        "action": "stop_and_return_for_replan",
                    },
                ),
                fallback_behavior={
                    "automatic_model_fallbacks": [],
                    "draft_failure": "use_declared_escalation",
                    "escalation_failure": "stop_and_return_for_replan",
                    "on_budget_exhaustion": "stop_before_escalation",
                },
                reasons=(
                    "draft is cheaper than the router-ranked escalation model",
                    "deterministic verification gates every conditional escalation",
                    "estimated savings %.1f%% with quality drop %.3f"
                    % (100.0 * savings_fraction, quality_drop),
                ),
            )
            plan["conditional_escalation_probability"] = escalation_probability
            plan["verifier_checks"] = checks
            violations = _plan_violations(plan, policy)
            if not violations:
                # Prefer expected quality, then lower expected spend.  The small
                # escalation-rank term preserves the router's ordering on ties.
                objective = expected_quality - 1.0e-6 * escalation_rank
                draft_options.append((objective, -expected_cost, plan))
        if draft_options:
            draft_options.sort(key=lambda item: (item[0], item[1]), reverse=True)
            plans.append(draft_options[0][2])
    if not plans:
        reasons.append("no_cascade_met_budget_quality_and_savings_thresholds")
    return plans, reasons


def _parallel_plans(
    direct: Mapping[str, Any],
    candidates: Sequence[Mapping[str, Any]],
    policy: PlanningPolicy,
) -> Tuple[List[Dict[str, Any]], List[str]]:
    if not policy.allow_parallel:
        return [], ["parallel_disabled"]
    if policy.max_calls < 3:
        return [], ["parallel_requires_max_calls_at_least_3"]
    if policy.max_expected_cost_usd is None and policy.max_worst_case_cost_usd is None:
        return [], ["parallel_requires_explicit_cost_budget"]

    direct_quality, _ = _candidate_quality(direct)
    direct_risk = _candidate_risk(direct)
    if direct_risk < policy.parallel_risk_threshold:
        return [], [
            "parallel_risk_below_threshold:%.3f<%.3f"
            % (direct_risk, policy.parallel_risk_threshold)
        ]

    primary_cost = _candidate_cost(direct)
    primary_latency = _candidate_latency(direct)
    if primary_cost is None:
        return [], ["parallel_primary_cost_unknown"]
    plans: List[Dict[str, Any]] = []
    highest_gain = 0.0
    # A catalog may contain hundreds of models.  Preserve router-ranked and
    # cheap cold-start candidates without constructing the quadratic set of
    # every possible three-call plan.
    cheapest = sorted(
        (
            candidate
            for candidate in candidates
            if _candidate_cost(candidate) is not None
        ),
        key=lambda candidate: (
            float(_candidate_cost(candidate) or 0.0),
            -float(candidate.get("utility") or 0.0),
            str(candidate.get("model_id")),
        ),
    )
    by_quality = sorted(
        candidates,
        key=lambda candidate: (
            -_candidate_quality(candidate)[0],
            _candidate_cost_or_inf(candidate),
            str(candidate.get("model_id")),
        ),
    )
    second_pool = _dedupe_candidates([direct, *candidates[:32], *cheapest[:16]], 48)
    judge_pool = _dedupe_candidates([direct, *by_quality[:16], *cheapest[:16]], 32)
    for second in second_pool:
        if second.get("model_id") == direct.get("model_id"):
            continue
        second_cost = _candidate_cost(second)
        second_latency = _candidate_latency(second)
        if second_cost is None:
            continue
        second_quality, _ = _candidate_quality(second)
        diversity = (
            0.55
            if _provider(str(second.get("model_id")))
            != _provider(str(direct.get("model_id")))
            else 0.30
        )
        raw_complement = (1.0 - direct_quality) * second_quality * diversity
        for judge in judge_pool:
            judge_cost = _candidate_cost(judge)
            judge_latency = _candidate_latency(judge)
            if judge_cost is None:
                continue
            judge_quality, _ = _candidate_quality(judge)
            judge_reliability = 0.5 + 0.5 * judge_quality
            expected_gain = raw_complement * judge_reliability
            highest_gain = max(highest_gain, expected_gain)
            if expected_gain < policy.min_parallel_expected_gain:
                continue
            expected_quality = min(direct_quality + expected_gain, 1.0)
            expected_cost = primary_cost + second_cost + judge_cost
            latency_values = (primary_latency, second_latency, judge_latency)
            if any(value is None for value in latency_values):
                expected_latency = None
            else:
                assert primary_latency is not None
                assert second_latency is not None
                assert judge_latency is not None
                expected_latency = max(primary_latency, second_latency) + judge_latency
            risk = max(direct_risk - expected_gain, 0.0)
            plan = _base_plan(
                "parallel",
                {
                    "candidates": [direct.get("model_id"), second.get("model_id")],
                    "judge": judge.get("model_id"),
                },
                (
                    _model_stage(
                        "call-candidate-a",
                        "candidate_a",
                        direct,
                        when="always",
                        parallel_group="candidate-generation",
                    ),
                    _model_stage(
                        "call-candidate-b",
                        "candidate_b",
                        second,
                        when="always",
                        parallel_group="candidate-generation",
                    ),
                    _model_stage(
                        "call-judge",
                        "judge",
                        judge,
                        when="candidate-generation:at_least_one_success",
                    ),
                ),
                expected_calls=3.0,
                worst_case_calls=3,
                expected_cost_usd=expected_cost,
                worst_case_cost_usd=expected_cost,
                expected_latency_s=expected_latency,
                worst_case_latency_s=expected_latency,
                expected_quality=expected_quality,
                risk_score=risk,
                triggers=(
                    {
                        "event": "both_candidates_complete",
                        "action": "call-judge",
                    },
                    {
                        "event": "one_candidate_failed",
                        "action": "judge_surviving_candidate",
                    },
                    {
                        "event": "both_candidates_failed",
                        "action": "stop_and_return_for_replan",
                    },
                    {
                        "event": "judge_failed",
                        "action": "stop_and_return_unresolved_candidates",
                    },
                ),
                fallback_behavior={
                    "automatic_model_fallbacks": [],
                    "one_candidate_failure": "judge_surviving_candidate",
                    "both_candidates_failure": "stop_and_return_for_replan",
                    "judge_failure": "return_unresolved_candidates",
                    "on_budget_exhaustion": "do_not_start_plan",
                },
                reasons=(
                    "direct-route risk %.3f clears %.3f threshold"
                    % (direct_risk, policy.parallel_risk_threshold),
                    "diverse second candidate and judge add estimated quality %.3f"
                    % expected_gain,
                    "all three calls fit the declared call/cost/latency limits",
                ),
            )
            plan["expected_quality_gain"] = expected_gain
            plan["parallel_warrant_risk"] = direct_risk
            if not _plan_violations(plan, policy):
                plans.append(plan)
    if not plans:
        return [], [
            "parallel_expected_gain_or_budget_check_failed:max_gain=%.3f threshold=%.3f"
            % (highest_gain, policy.min_parallel_expected_gain)
        ]
    plans.sort(
        key=lambda plan: (
            float(plan["expected_quality"]),
            -float(plan["expected_cost_usd"]),
            str(plan["roles"]),
        ),
        reverse=True,
    )
    return plans, []


def _refusal(
    policy: PlanningPolicy,
    route_decision: Mapping[str, Any],
    reasons: Sequence[str],
    candidates: Sequence[Mapping[str, Any]],
    top_k: int,
) -> Dict[str, Any]:
    return {
        "schema_version": PLAN_SCHEMA_VERSION,
        "route_schema_version": ROUTE_SCHEMA_VERSION,
        "status": "refused",
        "declarative_only": True,
        "executes_models": False,
        "requires_explicit_execution": True,
        "plan_type": None,
        "roles": {},
        "stages": [],
        "expected_calls": 0.0,
        "worst_case_calls": 0,
        "expected_cost_usd": 0.0,
        "worst_case_cost_usd": 0.0,
        "expected_latency_s": 0.0,
        "worst_case_latency_s": 0.0,
        "triggers": [],
        "fallback_behavior": {
            "action": "do_not_execute",
            "reason": "no_plan_satisfies_hard_constraints",
        },
        "planning_policy": asdict(policy),
        "explainability": {
            "refusal_reasons": list(dict.fromkeys(reasons)),
            "router_error": route_decision.get("error"),
            "feasible_candidates": route_decision.get("feasible_count", 0),
            "rejected_candidates": route_decision.get("rejected_count", 0),
        },
        "ranked_candidates": [
            _candidate_summary(candidate) for candidate in candidates[: max(top_k, 1)]
        ],
    }


def plan_request(
    router: Union[CandidateRouter, RouterEnsemble, str],
    request: Mapping[str, Any],
    catalog: Sequence[Mapping[str, Any]],
    *,
    quality_router: Optional[Union[CandidateRouter, RouterEnsemble, str]] = None,
    policy: Optional[Mapping[str, Any]] = None,
    planning_policy: Optional[Mapping[str, Any]] = None,
    explicit_model: Optional[str] = None,
    top_k: int = 10,
    now_epoch: Optional[float] = None,
) -> Dict[str, Any]:
    """Return a bounded declarative plan without executing any stage."""

    if not isinstance(request, Mapping):
        raise TypeError("request must be a mapping")
    planner = _merge_planning_policy(request, planning_policy)
    # Ask the router for every candidate so advanced roles remain metadata-
    # conditioned and future model IDs work without planner changes.
    route_decision = route_request(
        router,
        request,
        catalog,
        quality_router=quality_router,
        policy=policy,
        explicit_model=explicit_model,
        top_k=max(len(catalog), int(top_k), 1),
        now_epoch=now_epoch,
    )
    ranked = [
        candidate
        for candidate in route_decision.get("candidates", [])
        if isinstance(candidate, Mapping)
    ]
    feasible = [candidate for candidate in ranked if candidate.get("feasible")]

    if planner.max_calls == 0:
        return _refusal(
            planner,
            route_decision,
            ("max_calls_is_zero",),
            ranked,
            top_k,
        )
    if route_decision.get("explicit_override") and route_decision.get(
        "override_violations"
    ):
        return _refusal(
            planner,
            route_decision,
            [
                "explicit_override_violates_hard_constraint:%s" % reason
                for reason in route_decision.get("override_violations", [])
            ],
            ranked,
            top_k,
        )
    if not feasible:
        return _refusal(
            planner,
            route_decision,
            (route_decision.get("error") or "no_feasible_model",),
            ranked,
            top_k,
        )

    admissible_direct: List[Dict[str, Any]] = []
    direct_rejections: List[str] = []
    for candidate in feasible:
        suggested = [
            str(other.get("model_id"))
            for other in feasible
            if other.get("model_id") != candidate.get("model_id")
        ][:3]
        plan = _direct_plan(candidate, suggested)
        violations = _plan_violations(plan, planner)
        if violations:
            direct_rejections.extend(violations)
        else:
            admissible_direct.append(plan)
    if not admissible_direct:
        reasons = list(dict.fromkeys(direct_rejections))
        if planner.max_expected_cost_usd is not None:
            reasons.append("budget_refusal:no_direct_model_within_expected_cost")
        return _refusal(planner, route_decision, reasons, ranked, top_k)

    direct_plan = admissible_direct[0]
    direct_model = str(direct_plan["roles"]["primary"])
    direct_candidate = next(
        candidate for candidate in feasible if candidate.get("model_id") == direct_model
    )
    cascade_plans, cascade_reasons = _cascade_plans(
        feasible, request, route_decision, planner
    )
    parallel_plans, parallel_reasons = _parallel_plans(
        direct_candidate, feasible, planner
    )
    cascade = cascade_plans[0] if cascade_plans else None
    parallel = parallel_plans[0] if parallel_plans else None

    selected = direct_plan
    selection_notes: List[str] = []
    if planner.strategy == "cascade":
        if cascade is not None:
            selected = cascade
        else:
            selection_notes.append("requested_cascade_not_admissible; used_safe_direct")
    elif planner.strategy == "parallel":
        if parallel is not None:
            selected = parallel
        else:
            selection_notes.append("requested_parallel_not_warranted; used_safe_direct")
    elif planner.strategy == "auto":
        if planner.latency_preference == "quality" and parallel is not None:
            selected = parallel
        elif parallel is not None and float(direct_plan["risk_score"]) >= max(
            planner.parallel_risk_threshold, 0.45
        ):
            selected = parallel
        elif cascade is not None and planner.latency_preference != "low":
            selected = cascade
        elif parallel is not None and (
            selected.get("expected_latency_s") is None
            or parallel.get("expected_latency_s") is not None
            and float(parallel["expected_latency_s"])
            <= float(selected["expected_latency_s"])
        ):
            selected = parallel

    response = {
        "schema_version": PLAN_SCHEMA_VERSION,
        "route_schema_version": ROUTE_SCHEMA_VERSION,
        "status": "ready",
        "declarative_only": True,
        "executes_models": False,
        "requires_explicit_execution": True,
        **selected,
        "planning_policy": asdict(planner),
        "explainability": {
            "requested_strategy": planner.strategy,
            "selected_plan": selected["plan_type"],
            "router_selected_model": route_decision.get("selected_model"),
            "router_candidate_count": len(ranked),
            "hard_constraints_applied_before_planning": True,
            "selection_notes": selection_notes,
            "alternatives": {
                "direct": {
                    "admissible": True,
                    "expected_cost_usd": direct_plan["expected_cost_usd"],
                    "expected_quality": direct_plan["expected_quality"],
                },
                "cascade": {
                    "admissible": cascade is not None,
                    "reasons": cascade_reasons,
                    "expected_cost_usd": (
                        None if cascade is None else cascade["expected_cost_usd"]
                    ),
                    "expected_quality": (
                        None if cascade is None else cascade["expected_quality"]
                    ),
                },
                "parallel": {
                    "admissible": parallel is not None,
                    "reasons": parallel_reasons,
                    "expected_cost_usd": (
                        None if parallel is None else parallel["expected_cost_usd"]
                    ),
                    "expected_quality": (
                        None if parallel is None else parallel["expected_quality"]
                    ),
                },
            },
        },
        "ranked_candidates": [
            _candidate_summary(candidate) for candidate in ranked[: max(top_k, 1)]
        ],
    }
    return response


__all__ = ["PLAN_SCHEMA_VERSION", "PlanningPolicy", "plan_request"]
