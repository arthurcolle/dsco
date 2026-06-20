#!/usr/bin/env python3
"""
Lightweight Kalman bias estimation for forecast error sequences.
"""

from __future__ import annotations

from typing import Iterable, Tuple


def estimate_bias(errors: Iterable[float]) -> Tuple[float, float]:
    """
    Estimate current bias and uncertainty from historical errors.
    Falls back to EWMA if pykalman is unavailable.
    """
    seq = [float(e) for e in errors]
    if not seq:
        return 0.0, 2.0

    try:
        from pykalman import KalmanFilter

        kf = KalmanFilter(
            transition_matrices=[1.0],
            observation_matrices=[1.0],
            transition_covariance=0.05,
            observation_covariance=1.2,
            initial_state_mean=0.0,
            initial_state_covariance=5.0,
        )
        states, covs = kf.filter([[x] for x in seq])
        return float(states[-1, 0]), float(covs[-1, 0, 0] ** 0.5)
    except Exception:
        alpha = 0.20
        bias = 0.0
        for e in seq:
            bias = alpha * e + (1.0 - alpha) * bias
        return float(bias), 2.0
