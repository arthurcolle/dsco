#!/usr/bin/env python3
"""Adaptive *numerical lead generator* for possible off-line zeta zeros.

This program cannot certify a counterexample. It searches, refines, reruns at
higher precision, and emits a JSON lead. A separate rigorous interval/argument-
principle checker must prove that a zero exists in an off-line box before the
Lean bridge can be instantiated.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import mpmath as mp


@dataclass(frozen=True)
class Point:
    sigma: float
    t: float


@dataclass
class Evaluation:
    sigma: str
    t: str
    abs_zeta: str
    precision_bits: int
    policy: str


def zeta_abs(p: Point) -> float:
    """Fast ranking value; infinity is used for failed evaluations."""
    try:
        value = abs(mp.zeta(mp.mpc(p.sigma, p.t)))
        result = float(value)
        return result if math.isfinite(result) else math.inf
    except (ValueError, ZeroDivisionError, OverflowError):
        return math.inf


def global_points(rng: random.Random, count: int, t_min: float, t_max: float,
                  margin: float) -> Iterable[Point]:
    """Uniform exploration on both sides, never inside the excluded line band."""
    for _ in range(count):
        left = rng.random() < 0.5
        if left:
            sigma = rng.uniform(margin, 0.5 - margin)
        else:
            sigma = rng.uniform(0.5 + margin, 1.0 - margin)
        yield Point(sigma, rng.uniform(t_min, t_max))


def mutate_points(rng: random.Random, elites: list[Point], count: int,
                  t_min: float, t_max: float, margin: float,
                  sigma_scale: float, t_scale: float) -> Iterable[Point]:
    if not elites:
        yield from global_points(rng, count, t_min, t_max, margin)
        return
    for _ in range(count):
        base = rng.choice(elites)
        sigma = min(1.0 - margin, max(margin,
                    base.sigma + rng.gauss(0.0, sigma_scale)))
        # Reflect proposals out of the critical-line exclusion band.
        if abs(sigma - 0.5) < margin:
            sigma = 0.5 + math.copysign(margin, sigma - 0.5 or 1.0)
        t = min(t_max, max(t_min, base.t + rng.gauss(0.0, t_scale)))
        yield Point(sigma, t)


def refine(seed: Point, bits: int) -> tuple[mp.mpc, mp.mpf] | None:
    """Numerically solve ζ(s)=0 from two nearby complex starting values."""
    with mp.workprec(bits):
        z = mp.mpc(seed.sigma, seed.t)
        delta = mp.mpc(mp.mpf("1e-4"), mp.mpf("1e-4"))
        try:
            root = mp.findroot(mp.zeta, (z - delta, z + delta),
                               solver="secant", tol=mp.power(2, -bits // 2),
                               maxsteps=100)
            residual = abs(mp.zeta(root))
            return root, residual
        except (ValueError, ZeroDivisionError, OverflowError):
            return None


def validate(root: mp.mpc, precisions: list[int]) -> list[Evaluation]:
    """Re-solve independently at each precision; do not merely re-evaluate digits."""
    out: list[Evaluation] = []
    seed_re = mp.nstr(root.real, 80)
    seed_im = mp.nstr(root.imag, 80)
    for bits in precisions:
        with mp.workprec(bits):
            seed = mp.mpc(seed_re, seed_im)
            delta = mp.mpc(mp.power(2, -bits // 4), mp.power(2, -bits // 4))
            try:
                z = mp.findroot(mp.zeta, (seed - delta, seed + delta),
                                solver="secant", tol=mp.power(2, -bits // 2),
                                maxsteps=100)
                residual = abs(mp.zeta(z))
                policy = "independent_refinement"
            except (ValueError, ZeroDivisionError, OverflowError):
                z, residual, policy = seed, abs(mp.zeta(seed)), "refinement_failed"
            digits = max(20, int(bits * math.log10(2)))
            out.append(Evaluation(mp.nstr(z.real, digits), mp.nstr(z.imag, digits),
                                  mp.nstr(residual, 20), bits, policy))
    return out


def digest(record: dict) -> str:
    body = json.dumps(record, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--generations", type=int, default=20)
    ap.add_argument("--population", type=int, default=256)
    ap.add_argument("--elite", type=int, default=16)
    ap.add_argument("--t-min", type=float, default=10.0)
    ap.add_argument("--t-max", type=float, default=1000.0)
    ap.add_argument("--line-margin", type=float, default=1e-5)
    ap.add_argument("--working-bits", type=int, default=160)
    ap.add_argument("--output", type=Path, default=Path("runs/latest.json"))
    args = ap.parse_args()
    if not (0 < args.line_margin < 0.25 and args.t_min < args.t_max):
        ap.error("invalid margin or height interval")

    rng = random.Random(args.seed)
    mp.mp.prec = args.working_bits
    elites: list[Point] = []
    best_trace: list[dict] = []
    sigma_scale, t_scale = 0.08, max(1.0, (args.t_max - args.t_min) / 20)

    for generation in range(args.generations):
        explore = args.population if generation == 0 else args.population // 4
        proposals = list(global_points(rng, explore, args.t_min, args.t_max,
                                       args.line_margin))
        proposals += list(mutate_points(
            rng, elites, args.population - explore, args.t_min, args.t_max,
            args.line_margin, sigma_scale, t_scale))
        ranked = sorted(((zeta_abs(p), p) for p in proposals), key=lambda x: x[0])
        elites = [p for score, p in ranked[:args.elite] if math.isfinite(score)]
        if not elites:
            continue
        score, point = ranked[0]
        best_trace.append({"generation": generation, "sigma": point.sigma,
                           "t": point.t, "abs_zeta": score})
        sigma_scale *= 0.82
        t_scale *= 0.82

    refined: list[dict] = []
    for seed in elites:
        answer = refine(seed, args.working_bits)
        if answer is None:
            continue
        root, residual = answer
        off_line_distance = abs(root.real - mp.mpf("0.5"))
        ladder = validate(root, [args.working_bits, args.working_bits * 2,
                                 args.working_bits * 4])
        refined.append({
            "seed": asdict(seed),
            "root": {"sigma": mp.nstr(root.real, 60), "t": mp.nstr(root.imag, 60)},
            "abs_zeta_at_working_precision": mp.nstr(residual, 30),
            "off_line_distance": mp.nstr(off_line_distance, 30),
            "passes_numerical_screen": bool(
                off_line_distance > args.line_margin and
                residual < mp.power(2, -args.working_bits // 3)),
            "precision_ladder": [asdict(x) for x in ladder],
            "status": "UNVERIFIED_NUMERICAL_LEAD"
        })

    record = {
        "schema": "dsco.rh-counterexample-search.v1",
        "created_unix": time.time(),
        "parameters": vars(args) | {"output": str(args.output)},
        "best_trace": best_trace,
        "refined": refined,
        "soundness": {
            "is_proof": False,
            "warning": "Small |zeta(s)| and numerical root convergence do not prove zeta(s)=0.",
            "promotion_requirement": "Rigorous zero-count/isolation in a box disjoint from Re(s)=1/2, followed by Lean-checkable bridge evidence."
        }
    }
    record["sha256_without_digest"] = digest(record)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    print(f"wrote {args.output} ({len(refined)} refined leads)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
