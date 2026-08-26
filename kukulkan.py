#!/usr/bin/env python3
"""
Kukulkan v2 — library-backed weather market intelligence engine.

Replaces hand-rolled subsystems with battle-tested libraries:
  Herbie         → NWP data (HRRR/GFS/NAM) with AWS/Google/NOMADS fallback
  scoringrules   → closed-form CRPS for EMOS training loss
  python-metar   → full METAR remarks parser (SLP, peak wind, 6/24h T, precip)
  pykalman       → EM-learned Kalman filter for real-time bias tracking
  River          → ADWIN drift detection for model upgrade shifts
  quantile-forest→ nonparametric distributional forecasts
  pyextremes     → GEV/GPD for tail bucket probabilities
  pyvinecopulib  → 20-city joint temperature dependence
  dirichletcal   → multi-class bucket calibration on the simplex
  MAPIE          → conformal prediction intervals
  pvlib          → clear-sky GHI for cloud correction
  pykalshi       → Kalshi WebSocket streaming + RSA-PSS auth

Plus: Multinomial Kelly criterion (arxiv 2603.13581) in 30 lines.
"""

import math
import warnings
from datetime import datetime, timedelta, timezone
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np

from nwp_pipeline import KALSHI_CITIES

warnings.filterwarnings("ignore", category=FutureWarning)


# ══════════════════════════════════════════════════════════════════════
#  MULTINOMIAL KELLY — arxiv 2603.13581 (March 2026)
#  Closed-form for mutually exclusive bucket markets.
# ══════════════════════════════════════════════════════════════════════

def multinomial_kelly(probs: list[float], prices: list[float],
                      max_frac: float = 0.25) -> list[float]:
    """
    Optimal Kelly fractions for mutually exclusive bucket contracts.

    Given N mutually exclusive outcomes with true probabilities p_i and
    market prices (implied probabilities) q_i, the optimal fraction of
    bankroll to allocate to each bucket is:

        x_i = p_i - c * q_i

    where c = (1 - sum(p_A)) / (1 - sum(q_A)) and A is the set of
    buckets where x_i > 0.

    This is the FULL multinomial Kelly, not the conservative
    independent-binary approximation. It's MORE aggressive because
    it accounts for the constraint that exactly one bucket wins.

    Args:
        probs:    calibrated probabilities per bucket (should sum to ~1)
        prices:   market prices per bucket in [0, 1] (should sum to ~1 + vig)
        max_frac: cap per-bucket allocation (default 25%)

    Returns:
        list of fractions to bet on each bucket (0 = skip, >0 = buy YES)
    """
    n = len(probs)
    assert len(prices) == n
    p = np.array(probs, dtype=np.float64)
    q = np.array(prices, dtype=np.float64)

    # Greedy one-pass: iteratively remove buckets with negative allocation
    active = np.ones(n, dtype=bool)
    x = np.zeros(n, dtype=np.float64)
    for _ in range(n):
        p_a = p[active].sum()
        q_a = q[active].sum()
        denom = 1.0 - q_a
        if abs(denom) < 1e-9:
            denom = -1e-9 if denom <= 0 else 1e-9
        c = (1.0 - p_a) / denom
        x = np.where(active, p - c * q, 0.0)
        neg = active & (x <= 0)
        if not neg.any():
            break
        active[neg] = False

    # Final allocation with cap
    fracs = np.clip(x, 0.0, max_frac)
    return fracs.tolist()


def kelly_ev(probs, prices, fracs):
    """Expected log-growth rate of a multinomial Kelly allocation."""
    g = 0.0
    for p_i, q_i, f_i in zip(probs, prices, fracs):
        if f_i > 0 and q_i > 0:
            payout = f_i / q_i  # win payout per unit risked
            g += p_i * math.log(1.0 + payout - sum(fracs))
    return g


# ══════════════════════════════════════════════════════════════════════
#  HERBIE — multi-source NWP data
# ══════════════════════════════════════════════════════════════════════

def fetch_herbie_t2m(lat: float, lon: float, model: str = "hrrr",
                     fxx: int = 0, cycle: Optional[datetime] = None) -> Optional[float]:
    """
    Fetch INSTANTANEOUS 2-m temperature from HRRR/GFS/NAM via Herbie.
    Returns temperature in °F at nearest grid point at forecast hour fxx.

    WARNING: This is a snapshot, NOT a daily max. For daily high temperature,
    use fetch_herbie_daily_max() which scans across forecast hours or uses
    the native TMAX GRIB field.
    """
    try:
        from herbie import Herbie
        # Herbie wants naive datetime
        dt = cycle or datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
        dt = dt.replace(tzinfo=None)
        H = Herbie(dt, model=model, fxx=fxx, verbose=False)
        ds = H.xarray("TMP:2 m above ground")
        t2m_k = _nearest_grid_value(ds, "t2m", lat, lon)
        return round((t2m_k - 273.15) * 9 / 5 + 32, 1)
    except Exception:
        return None


def _nearest_grid_value(ds, var_name: str, lat: float, lon: float) -> float:
    """
    Read nearest-gridpoint values from Herbie/xarray datasets.

    Handles both regular lat/lon grids and 2D curvilinear lat/lon grids.
    """
    # Fast path: standard 1D latitude/longitude coordinates.
    for lon_target in (lon, lon % 360):
        try:
            return float(ds[var_name].sel(latitude=lat, longitude=lon_target, method="nearest").values)
        except Exception:
            pass

    # Fallback: 2D coordinates (common in HRRR/RAP/NAM grids).
    lat_name = "latitude" if "latitude" in ds else "lat"
    lon_name = "longitude" if "longitude" in ds else "lon"
    lat_arr = np.asarray(ds[lat_name].values)
    lon_arr = np.asarray(ds[lon_name].values)
    field = np.asarray(ds[var_name].squeeze().values)

    if lat_arr.ndim != 2 or lon_arr.ndim != 2:
        raise ValueError("Unsupported coordinate geometry for nearest-grid lookup")

    # Robust angular distance: normalize longitudinal difference to [-180, 180].
    dlon = ((lon_arr - lon + 180.0) % 360.0) - 180.0
    dist2 = (lat_arr - lat) ** 2 + dlon ** 2
    iy, ix = np.unravel_index(np.nanargmin(dist2), dist2.shape)
    return float(field[iy, ix])


def fetch_herbie_daily_max(lat: float, lon: float, model: str = "gfs",
                           cycle: Optional[datetime] = None) -> Optional[float]:
    """
    Fetch forecast DAILY MAX temperature via Herbie.

    GFS/NAM: Uses native TMAX:2m GRIB field (6-hour max windows).
             Takes max(fxx=12, fxx=18, fxx=24) to cover 06Z-00Z.
    HRRR:    No TMAX field. Scans TMP:2m at fxx=12..21 and takes max.

    This is what Kalshi daily high markets settle on.
    """
    from herbie import Herbie

    if cycle:
        # Herbie expects naive UTC cycle times.
        if cycle.tzinfo:
            cycle = cycle.astimezone(timezone.utc)
        base_dt = cycle.replace(minute=0, second=0, microsecond=0, tzinfo=None)
        candidate_cycles = [base_dt]
    else:
        now = datetime.now(timezone.utc)
        model_lag_h = {"hrrr": 2, "rap": 2, "gfs": 5, "nam": 5}
        model_cycles = {
            "hrrr": list(range(24)),
            "rap": list(range(24)),
            "gfs": [0, 6, 12, 18],
            "nam": [0, 6, 12, 18],
        }
        ref = now - timedelta(hours=model_lag_h.get(model, 5))
        cycles = sorted(model_cycles.get(model, [0, 12]))
        cycle_hour = next((h for h in reversed(cycles) if h <= ref.hour), cycles[-1])
        day_back = 1 if cycle_hour > ref.hour else 0
        base_dt = (ref - timedelta(days=day_back)).replace(
            hour=cycle_hour, minute=0, second=0, microsecond=0, tzinfo=None
        )
        step_h = 1 if model in ("hrrr", "rap") else 6
        candidate_cycles = [base_dt - timedelta(hours=i * step_h) for i in range(4)]

    if model in ("gfs", "nam"):
        # Native TMAX field: 6-hour max periods
        # fxx=12 covers 06-12Z, fxx=18 covers 12-18Z, fxx=24 covers 18-00Z
        for dt in candidate_cycles:
            best = None
            for fxx in [12, 18, 24]:
                try:
                    H = Herbie(dt, model=model, fxx=fxx, verbose=False)
                    ds = H.xarray("TMAX:2 m above ground")
                    tmax_k = _nearest_grid_value(ds, "tmax", lat, lon)
                    tmax_f = round((tmax_k - 273.15) * 9 / 5 + 32, 1)
                    if best is None or tmax_f > best:
                        best = tmax_f
                except Exception:
                    continue
            if best is not None:
                return best
        return None
    else:
        # HRRR/RAP: scan instantaneous T2m across afternoon hours
        max_fxx = 18 if model == "hrrr" else 21
        for dt in candidate_cycles:
            best = None
            for fxx in range(12, max_fxx + 1):
                try:
                    H = Herbie(dt, model=model, fxx=fxx, verbose=False)
                    ds = H.xarray("TMP:2 m above ground")
                    t_k = _nearest_grid_value(ds, "t2m", lat, lon)
                    t_f = round((t_k - 273.15) * 9 / 5 + 32, 1)
                    if best is None or t_f > best:
                        best = t_f
                except Exception:
                    continue
            if best is not None:
                return best
        return None


_NWS_POINT_CACHE: dict[tuple[float, float], dict[str, str]] = {}


def _c_to_f(temp_c: Optional[float]) -> Optional[float]:
    if temp_c is None:
        return None
    return round(float(temp_c) * 9.0 / 5.0 + 32.0, 1)


def _nws_json(url: str) -> Optional[dict]:
    import json
    import urllib.request

    headers = {
        "User-Agent": "kukulkan/2.0 (weather-trading)",
        "Accept": "application/geo+json",
    }
    try:
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=12) as resp:
            return json.loads(resp.read())
    except Exception:
        return None


def _nws_point_urls(lat: float, lon: float) -> Optional[dict[str, str]]:
    key = (round(lat, 4), round(lon, 4))
    if key in _NWS_POINT_CACHE:
        return _NWS_POINT_CACHE[key]
    data = _nws_json(f"https://api.weather.gov/points/{lat},{lon}")
    if not data:
        return None
    props = data.get("properties", {})
    urls = {
        "grid": props.get("forecastGridData", ""),
        "hourly": props.get("forecastHourly", ""),
    }
    if not urls["grid"] and not urls["hourly"]:
        return None
    _NWS_POINT_CACHE[key] = urls
    return urls


def _parse_valid_interval(valid_time: str) -> Optional[tuple[datetime, datetime]]:
    """
    Parse NWS validTime strings like:
      2026-04-10T00:00:00+00:00/PT1H
    """
    import re

    try:
        start_s, _, dur_s = valid_time.partition("/")
        start = datetime.fromisoformat(start_s.replace("Z", "+00:00"))
        hours = 0.0
        m = re.match(r"PT(?:(\d+)H)?(?:(\d+)M)?", dur_s)
        if m:
            h = int(m.group(1)) if m.group(1) else 0
            mins = int(m.group(2)) if m.group(2) else 0
            hours = h + mins / 60.0
        if hours <= 0:
            hours = 1.0
        end = start + timedelta(hours=hours)
        return start, end
    except Exception:
        return None


def fetch_nbm_daily_max(lat: float, lon: float, target_date: datetime) -> Optional[float]:
    """
    Pull NWS gridded maxTemperature for the target date.
    This is effectively NBM-driven guidance at the gridpoint.
    """
    urls = _nws_point_urls(lat, lon)
    if not urls or not urls.get("grid"):
        return None
    data = _nws_json(urls["grid"])
    if not data:
        return None
    values = (
        data.get("properties", {})
        .get("maxTemperature", {})
        .get("values", [])
    )
    if not values:
        return None

    target_utc = target_date.date()
    candidates = []
    for row in values:
        c_val = row.get("value")
        vt = row.get("validTime")
        if c_val is None or not vt:
            continue
        interval = _parse_valid_interval(vt)
        if not interval:
            continue
        start, end = interval
        if start.astimezone(timezone.utc).date() <= target_utc <= end.astimezone(timezone.utc).date():
            f_val = _c_to_f(c_val)
            if f_val is not None:
                candidates.append(f_val)
    if not candidates:
        return None
    return round(max(candidates), 1)


def fetch_lamp_daily_max_proxy(lat: float, lon: float, target_date: datetime) -> Optional[float]:
    """
    Hourly-updating short-range proxy for LAMP-like behavior.
    Uses NWS hourly forecast temperatures and takes target-day max.
    """
    urls = _nws_point_urls(lat, lon)
    if not urls or not urls.get("hourly"):
        return None
    data = _nws_json(urls["hourly"])
    if not data:
        return None
    periods = data.get("properties", {}).get("periods", [])
    if not periods:
        return None

    target_utc = target_date.date()
    vals = []
    for p in periods:
        t = p.get("temperature")
        t_unit = p.get("temperatureUnit")
        start_s = p.get("startTime")
        if t is None or not start_s:
            continue
        try:
            start = datetime.fromisoformat(start_s.replace("Z", "+00:00"))
        except Exception:
            continue
        if start.astimezone(timezone.utc).date() != target_utc:
            continue
        if t_unit == "C":
            vals.append(_c_to_f(float(t)))
        else:
            vals.append(float(t))

    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    return round(max(vals), 1)


def fetch_ifs_daily_max(lat: float, lon: float, cycle: Optional[datetime] = None) -> Optional[float]:
    """
    ECMWF IFS daily max via Herbie.
    Uses mx2t3 (3-hour running max 2m temp) when available.
    """
    from herbie import Herbie

    if cycle:
        dt0 = cycle.astimezone(timezone.utc).replace(minute=0, second=0, microsecond=0, tzinfo=None)
        candidate_cycles = [dt0]
    else:
        now = datetime.now(timezone.utc) - timedelta(hours=8)
        cycle_hour = 12 if now.hour >= 12 else 0
        dt0 = now.replace(hour=cycle_hour, minute=0, second=0, microsecond=0, tzinfo=None)
        if cycle_hour > now.hour:
            dt0 -= timedelta(days=1)
        candidate_cycles = [dt0, dt0 - timedelta(hours=12), dt0 - timedelta(hours=24)]

    for dt in candidate_cycles:
        best = None
        for fxx in (12, 18, 24, 30, 36):
            try:
                H = Herbie(dt, model="ifs", fxx=fxx, verbose=False)
                ds = H.xarray("2t")
                var = "mx2t3" if "mx2t3" in ds else "t2m"
                t_k = _nearest_grid_value(ds, var, lat, lon)
                t_f = round((t_k - 273.15) * 9.0 / 5.0 + 32.0, 1)
                if best is None or t_f > best:
                    best = t_f
            except Exception:
                continue
        if best is not None:
            return best
    return None


def fetch_t850_tmax_proxy(lat: float, lon: float, cycle: Optional[datetime] = None) -> Optional[float]:
    """
    T850→Tmax proxy (low-weight support member).
    """
    try:
        from herbie import Herbie
    except Exception:
        return None

    dt = cycle.astimezone(timezone.utc) if cycle and cycle.tzinfo else (cycle or datetime.now(timezone.utc))
    dt = dt.replace(minute=0, second=0, microsecond=0, tzinfo=None)
    for lag in (0, 6, 12, 18, 24):
        use_dt = dt - timedelta(hours=lag)
        try:
            H = Herbie(use_dt, model="gfs", fxx=18, verbose=False)
            ds = H.xarray("TMP:850 mb")
            t850_k = _nearest_grid_value(ds, "t", lat, lon)
            t850_f = (t850_k - 273.15) * 9.0 / 5.0 + 32.0
            # Empirical translation: surface max tends to run above T850
            proxy = 0.65 * t850_f + 34.0
            return round(proxy, 1)
        except Exception:
            continue
    return None


def fetch_additional_model_highs(lat: float, lon: float, target_date: datetime) -> dict[str, float]:
    """
    Add independent model signals beyond BUFKIT core members.
    """
    extras: dict[str, float] = {}

    nbm = fetch_nbm_daily_max(lat, lon, target_date)
    if nbm is not None:
        extras["nbm"] = nbm

    lamp = fetch_lamp_daily_max_proxy(lat, lon, target_date)
    if lamp is not None:
        extras["lamp"] = lamp

    ecmwf = fetch_ifs_daily_max(lat, lon)
    if ecmwf is not None:
        extras["ecmwf"] = ecmwf

    sounding = fetch_t850_tmax_proxy(lat, lon)
    if sounding is not None:
        extras["sounding"] = sounding

    return extras


def apply_intraday_trajectory_adjustment(
    metar_history: list[dict], now_utc: datetime, lon: float,
    base_mean: float, base_std: float
) -> dict:
    """
    Fit morning trajectory and adjust forecast intraday.
    """
    if not metar_history:
        return {"status": "no_obs", "applied": False}

    tz_offset = round(lon / 15.0)
    today_midnight_utc = now_utc.replace(hour=0, minute=0, second=0, microsecond=0) - timedelta(hours=tz_offset)

    today_obs_hours = []
    today_obs_temps = []
    min_temp = None
    for m in reversed(metar_history):
        if m.get("temp_f") is None or m.get("time") is None:
            continue
        t = m["time"]
        if not hasattr(t, "hour"):
            continue
        obs_utc = now_utc.replace(hour=t.hour, minute=t.minute, second=0, microsecond=0)
        if obs_utc > now_utc:
            obs_utc -= timedelta(days=1)
        if obs_utc < today_midnight_utc:
            continue
        local_hour = (t.hour + tz_offset) % 24 + t.minute / 60.0
        today_obs_hours.append(local_hour)
        today_obs_temps.append(m["temp_f"])
        if min_temp is None or m["temp_f"] < min_temp:
            min_temp = m["temp_f"]

    latest_local = today_obs_hours[-1] if today_obs_hours else 0.0
    if len(today_obs_hours) < 3 or min_temp is None:
        return {
            "status": "insufficient_obs",
            "n_today_obs": len(today_obs_hours),
            "latest_local_hr": round(latest_local, 1),
            "applied": False,
        }
    if latest_local <= 6.0:
        return {
            "status": "pre_sunrise",
            "n_today_obs": len(today_obs_hours),
            "latest_local_hr": round(latest_local, 1),
            "applied": False,
        }

    try:
        tmax_est = fit_tmax_from_morning(today_obs_hours, today_obs_temps, t_min=min_temp)
        weight = min(0.55, max(0.0, (latest_local - 6.0) / 10.0))
        adj_mean = base_mean * (1.0 - weight) + tmax_est * weight
        adj_std = max(2.0, base_std * (1.0 - 0.5 * weight))
        return {
            "status": "ok",
            "applied": weight > 0,
            "n_today_obs": len(today_obs_hours),
            "latest_local_hr": round(latest_local, 1),
            "tmax_est": round(tmax_est, 1),
            "weight": round(weight, 2),
            "mean": round(adj_mean, 1),
            "std": round(adj_std, 1),
        }
    except Exception:
        return {"status": "fit_error", "applied": False}


# ══════════════════════════════════════════════════════════════════════
#  METAR — full remarks parser via python-metar
# ══════════════════════════════════════════════════════════════════════

def parse_metar(raw: str) -> dict:
    """
    Parse a raw METAR string into a structured dict.
    Extracts 13+ remark fields our regex parser misses:
      SLP, peak wind, 6/24h max/min T, precipitation, pressure tendency.
    """
    try:
        from metar import Metar as MetarParser
        m = MetarParser.Metar(raw)
        def _val(attr):
            v = getattr(m, attr, None)
            return v.value() if v is not None else None

        result = {
            "station": m.station_id,
            "time": m.time,
            "temp_c": _val("temp"),
            "temp_f": round(_val("temp") * 9 / 5 + 32, 1) if m.temp else None,
            "dewpoint_c": _val("dewpt"),
            "dewpoint_f": round(_val("dewpt") * 9 / 5 + 32, 1) if m.dewpt else None,
            "wind_dir": _val("wind_dir"),
            "wind_speed_kt": _val("wind_speed"),
            "wind_gust_kt": _val("wind_gust"),
            "visibility_mi": _val("vis"),
            "pressure_mb": _val("press"),
            "sky": m.sky_conditions(),
            "weather": [str(w) for w in m.weather] if m.weather else [],
            "remarks": m.remarks() if callable(m.remarks) else str(m.remarks),
            # Remark fields our old parser missed:
            "slp_mb": _val("press_sea_level"),
            "max_temp_6hr_c": _val("max_temp_6hr"),
            "min_temp_6hr_c": _val("min_temp_6hr"),
            "max_temp_24hr_c": _val("max_temp_24hr"),
            "min_temp_24hr_c": _val("min_temp_24hr"),
            "precip_1hr_in": _val("precip_1hr"),
            "precip_3hr_in": _val("precip_3hr"),
            "precip_6hr_in": _val("precip_6hr"),
            "precip_24hr_in": _val("precip_24hr"),
        }
        # Convert 6/24h extremes to °F
        for key in ["max_temp_6hr_c", "min_temp_6hr_c", "max_temp_24hr_c", "min_temp_24hr_c"]:
            val = result[key]
            if val is not None:
                result[key.replace("_c", "_f")] = round(val * 9 / 5 + 32, 1)
        return result
    except Exception as e:
        return {"error": str(e), "raw": raw}


# ══════════════════════════════════════════════════════════════════════
#  SCORINGRULES — proper CRPS for EMOS training
# ══════════════════════════════════════════════════════════════════════

def crps_gaussian(obs: np.ndarray, mu: np.ndarray, sigma: np.ndarray) -> np.ndarray:
    """
    Closed-form CRPS for Gaussian predictive distribution.
    Delegates to scoringrules if available, else falls back to pure math.
    """
    try:
        import scoringrules as sr
        return sr.crps_normal(obs, mu, sigma)
    except ImportError:
        # Fallback: Gneiting & Raftery 2007
        z = (obs - mu) / np.maximum(sigma, 1e-6)
        from scipy.stats import norm
        return sigma * (z * (2 * norm.cdf(z) - 1) + 2 * norm.pdf(z) - 1 / np.sqrt(np.pi))


def train_emos_scoringrules(ens_means: np.ndarray, ens_spreads: np.ndarray,
                            obs: np.ndarray) -> tuple[float, float, float, float]:
    """
    Fit EMOS parameters [a, b, c, d] by minimizing mean CRPS.
    Uses scipy.optimize.minimize with scoringrules as the loss.

    μ = a + b * ens_mean
    σ² = max(c + d * ens_spread, 0.25)
    """
    from scipy.optimize import minimize

    def loss(params):
        a, b, c, d = params
        mu = a + b * ens_means
        var = np.maximum(c + d * ens_spreads, 0.25)
        sigma = np.sqrt(var)
        return float(np.mean(crps_gaussian(obs, mu, sigma)))

    res = minimize(loss, x0=[0.0, 1.0, 4.0, 0.5], method="Nelder-Mead",
                   options={"maxiter": 1000, "xatol": 1e-6, "fatol": 1e-8})
    a, b, c, d = res.x
    return float(a), float(b), max(float(c), 0.25), max(float(d), 0.0)


# ══════════════════════════════════════════════════════════════════════
#  PYKALMAN — EM-learned Kalman filter for bias tracking
# ══════════════════════════════════════════════════════════════════════

def kalman_bias_tracker(forecast_errors: list[float]) -> tuple[float, float]:
    """
    Track forecast bias in real-time with a Kalman filter.
    Auto-learns process noise Q and observation noise R from data via EM.

    Returns (current_bias_estimate, uncertainty).
    """
    try:
        from pykalman import KalmanFilter
        obs = np.array(forecast_errors).reshape(-1, 1)
        kf = KalmanFilter(
            transition_matrices=[1],
            observation_matrices=[1],
            initial_state_mean=0,
            initial_state_covariance=10,
            em_vars=["transition_covariance", "observation_covariance"],
        )
        kf = kf.em(obs, n_iter=10)
        state_means, state_covs = kf.filter(obs)
        return float(state_means[-1, 0]), float(np.sqrt(state_covs[-1, 0, 0]))
    except ImportError:
        # Fallback: simple EMA
        alpha = 0.15
        bias = 0.0
        for e in forecast_errors:
            bias = alpha * e + (1 - alpha) * bias
        return bias, 2.0


# ══════════════════════════════════════════════════════════════════════
#  RIVER — online drift detection
# ══════════════════════════════════════════════════════════════════════

def detect_drift(errors: list[float]) -> dict:
    """
    Detect model bias drift using ADWIN (Adaptive Windowing).
    Returns {"drifted": bool, "n_detections": int, "current_mean": float}.
    """
    try:
        from river.drift import ADWIN
        adwin = ADWIN(delta=0.002)
        detections = 0
        for e in errors:
            adwin.update(e)
            if adwin.drift_detected:
                detections += 1
        return {
            "drifted": detections > 0,
            "n_detections": detections,
            "current_mean": adwin.estimation if hasattr(adwin, "estimation") else np.mean(errors[-30:]),
        }
    except ImportError:
        return {"drifted": False, "n_detections": 0, "current_mean": np.mean(errors[-30:]) if errors else 0.0}


# ══════════════════════════════════════════════════════════════════════
#  QUANTILE FOREST — nonparametric distributional forecasts
# ══════════════════════════════════════════════════════════════════════

def train_qrf(X_train: np.ndarray, y_train: np.ndarray,
              n_estimators: int = 200) -> object:
    """Train a Quantile Regression Forest. Returns fitted model."""
    from quantile_forest import RandomForestQuantileRegressor
    qrf = RandomForestQuantileRegressor(n_estimators=n_estimators, random_state=42, n_jobs=-1)
    qrf.fit(X_train, y_train)
    return qrf


def qrf_bucket_probs(qrf, X: np.ndarray, buckets: list[tuple[float, float]],
                     n_quantiles: int = 99) -> list[float]:
    """
    Compute bucket probabilities from QRF predictive distribution.
    Estimates CDF at bucket boundaries from quantile predictions.
    """
    quantiles = np.linspace(0.01, 0.99, n_quantiles)
    preds = qrf.predict(X, quantiles=quantiles.tolist())  # shape (1, n_quantiles)
    if preds.ndim == 2:
        preds = preds[0]

    probs = []
    for lo, hi in buckets:
        p_lo = np.searchsorted(preds, lo) / len(preds)
        p_hi = np.searchsorted(preds, hi) / len(preds)
        probs.append(max(0.0, p_hi - p_lo))

    total = sum(probs) or 1.0
    return [p / total for p in probs]


# ══════════════════════════════════════════════════════════════════════
#  PYEXTREMES — EVT for tail bucket probabilities
# ══════════════════════════════════════════════════════════════════════

def fit_evt_tail(daily_highs: list[float], threshold_pctile: float = 0.95,
                 target_temp: float = 100.0) -> dict:
    """
    Fit a GPD to exceedances above a threshold for tail probability estimation.
    Answers: "What's P(T > target_temp) this time of year?"
    """
    try:
        import pandas as pd
        from pyextremes import get_extremes, get_return_periods

        series = pd.Series(daily_highs, index=pd.date_range("2000-01-01", periods=len(daily_highs), freq="D"))
        threshold = np.percentile(daily_highs, threshold_pctile * 100)
        extremes = get_extremes(series, method="POT", threshold=threshold, r="24h")

        if len(extremes) < 10:
            return {"p_exceed": np.mean(np.array(daily_highs) > target_temp), "method": "empirical"}

        # Return period for target temperature
        rp = get_return_periods(series, extremes, return_period_size="365.25D", alpha=0.05)
        # Exceedance probability = 1 / return_period (per year) / 365.25 (per day)
        closest_idx = np.argmin(np.abs(rp.index.values - target_temp))
        rp_years = float(rp.iloc[closest_idx]["return period"])
        p_per_day = 1.0 / (rp_years * 365.25) if rp_years > 0 else 0.0

        return {
            "p_exceed": round(p_per_day, 6),
            "return_period_years": round(rp_years, 1),
            "threshold": round(threshold, 1),
            "n_extremes": len(extremes),
            "method": "GPD-POT",
        }
    except Exception as e:
        empirical = np.mean(np.array(daily_highs) > target_temp) if daily_highs else 0.0
        return {"p_exceed": float(empirical), "method": "empirical", "error": str(e)}


# ══════════════════════════════════════════════════════════════════════
#  PYVINECOPULIB — joint city dependence
# ══════════════════════════════════════════════════════════════════════

def fit_vine_copula(city_temps: dict[str, list[float]]) -> object:
    """
    Fit an R-vine copula to multi-city daily temperature data.
    Models "if NYC busts warm, does PHL too?" — auto-selects from 190 bivariate copulas.

    Args:
        city_temps: {city_key: [daily_high_temps]} — all lists same length

    Returns:
        fitted pyvinecopulib.Vinecop object
    """
    try:
        import pyvinecopulib as pv
        from scipy.stats import rankdata

        keys = sorted(city_temps.keys())
        n = len(city_temps[keys[0]])
        # Transform to pseudo-observations (rank → uniform)
        u = np.column_stack([rankdata(city_temps[k]) / (n + 1) for k in keys])

        cop = pv.Vinecop(len(keys))
        cop.select(u)
        return cop
    except ImportError:
        return None


def conditional_prob(copula, city_idx: int, target_quantile: float,
                     conditions: dict[int, float]) -> float:
    """
    P(city_idx exceeds target_quantile | other cities at given quantiles).
    Uses vine copula for conditional sampling.
    """
    if copula is None:
        return target_quantile  # uninformative prior
    try:
        n_sim = 5000
        sim = copula.simulate(n_sim)
        # Filter simulations matching conditions (within tolerance)
        mask = np.ones(n_sim, dtype=bool)
        for idx, val in conditions.items():
            mask &= np.abs(sim[:, idx] - val) < 0.05
        if mask.sum() < 50:
            return target_quantile
        return float(np.mean(sim[mask, city_idx] > target_quantile))
    except Exception:
        return target_quantile


# ══════════════════════════════════════════════════════════════════════
#  DIRICHLETCAL — multi-class bucket calibration
# ══════════════════════════════════════════════════════════════════════

def calibrate_bucket_probs(raw_probs_train: np.ndarray, labels_train: np.ndarray,
                           raw_probs_test: np.ndarray) -> np.ndarray:
    """
    Calibrate bucket probabilities using Dirichlet calibration.
    Natively handles mutually exclusive outcomes without OvR hacks.

    Args:
        raw_probs_train: (n_train, n_buckets) uncalibrated probabilities
        labels_train:    (n_train,) true bucket indices
        raw_probs_test:  (n_test, n_buckets) uncalibrated probabilities

    Returns:
        (n_test, n_buckets) calibrated probabilities
    """
    try:
        from dirichletcal.calib.fulldirichlet import FullDirichletCalibrator
        cal = FullDirichletCalibrator(reg_lambda=1e-3)
        cal.fit(raw_probs_train, labels_train)
        return cal.predict_proba(raw_probs_test)
    except ImportError:
        # Fallback: temperature scaling
        return raw_probs_test


# ══════════════════════════════════════════════════════════════════════
#  MAPIE — conformal prediction intervals
# ══════════════════════════════════════════════════════════════════════

def conformal_interval(model, X_calib: np.ndarray, y_calib: np.ndarray,
                       X_test: np.ndarray, alpha: float = 0.1) -> tuple[np.ndarray, np.ndarray]:
    """
    Wrap any sklearn-compatible model with MAPIE for coverage-guaranteed intervals.
    Returns (lower_bound, upper_bound) arrays at (1 - alpha) confidence.
    """
    try:
        from mapie.regression import MapieRegressor
        mapie = MapieRegressor(model, method="plus", cv="prefit")
        mapie.fit(X_calib, y_calib)
        _, intervals = mapie.predict(X_test, alpha=alpha)
        return intervals[:, 0, 0], intervals[:, 1, 0]
    except ImportError:
        # Fallback: naive ±2*RMSE
        preds = model.predict(X_test)
        residuals = y_calib - model.predict(X_calib)
        rmse = np.sqrt(np.mean(residuals ** 2))
        return preds - 2 * rmse, preds + 2 * rmse


# ══════════════════════════════════════════════════════════════════════
#  PVLIB — clear-sky GHI for cloud correction
# ══════════════════════════════════════════════════════════════════════

def cloud_attenuation(lat: float, lon: float, dt: datetime) -> float:
    """
    Compute clear-sky GHI ratio for temperature correction.
    Returns attenuation factor [0, 1] where 1 = perfectly clear.
    Useful for: "NWP says 85°F clear-sky but it's cloudy → knock 3°F off."
    """
    try:
        import pandas as pd
        from pvlib.location import Location

        loc = Location(lat, lon, tz="UTC")
        times = pd.date_range(dt, periods=1, freq="h")
        cs = loc.get_clearsky(times, model="ineichen")
        ghi_clear = float(cs["ghi"].iloc[0])
        return ghi_clear / max(ghi_clear, 1.0)  # normalize; actual obs comparison elsewhere
    except Exception:
        return 1.0


# ══════════════════════════════════════════════════════════════════════
#  IEM CLI — pre-parsed NWS settlement data (free JSON API)
# ══════════════════════════════════════════════════════════════════════

def fetch_iem_cli(station: str, date: Optional[datetime] = None) -> Optional[dict]:
    """
    Fetch pre-parsed NWS CLI settlement data from IEM.
    Station should be the CLI station ID (e.g. "MDW", "NYC", "PHX").
    IEM expects the full ICAO with K prefix and uses a date-range endpoint.
    """
    import json
    import sqlite3
    import urllib.request

    dt = date or (datetime.now(timezone.utc) - timedelta(days=1))
    date_str = dt.strftime("%Y-%m-%d")

    # Prefer locally ingested CLI verification (fresh NWS-backed values).
    try:
        from calibration import CALIB_DB

        cli_id = station.upper().lstrip("K")
        city_key = next(
            (ck for ck, cfg in KALSHI_CITIES.items() if str(cfg[7]).upper() == cli_id),
            None,
        )
        if city_key:
            conn = sqlite3.connect(str(CALIB_DB))
            row = conn.execute(
                """
                SELECT obs_high_f, obs_low_f, date
                FROM verification
                WHERE city = ? AND model = 'cli' AND date = ?
                LIMIT 1
                """,
                (city_key, date_str),
            ).fetchone()
            conn.close()
            if row and row[0] is not None:
                return {
                    "station": station,
                    "date": row[2],
                    "high_f": row[0],
                    "low_f": row[1],
                    "source": "calibration_db",
                }
    except Exception:
        pass

    icao = station if station.startswith("K") else f"K{station}"

    # IEM CLI endpoint — use the date range format which is more reliable
    url = (f"https://mesonet.agron.iastate.edu/json/cli.py"
           f"?station={icao}&fmt=json&sdate={date_str}&edate={date_str}")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "kukulkan/2.0"})
        with urllib.request.urlopen(req, timeout=10) as r:
            data = json.loads(r.read())
        results = data.get("results", [])
        # Filter to the exact date we want
        for r in results:
            if r.get("valid") == date_str:
                avg_wind_kts = r.get("avg_wind_speed_kts")
                max_wind_kts = r.get("max_wind_speed_kts")
                return {
                    "station": r.get("station"),
                    "date": r.get("valid"),
                    "high_f": r.get("high"),
                    "low_f": r.get("low"),
                    "precip_in": r.get("precip"),
                    "snow_in": r.get("snow"),
                    "avg_wind_mph": round(avg_wind_kts * 1.15078, 1) if avg_wind_kts is not None else None,
                    "max_wind_mph": round(max_wind_kts * 1.15078, 1) if max_wind_kts is not None else None,
                    "high_normal": r.get("high_normal"),
                    "low_normal": r.get("low_normal"),
                    "high_record": r.get("high_record"),
                    "low_record": r.get("low_record"),
                    "source": "iem",
                }
        # If exact match not found, return first result only when the year is plausible.
        if results:
            r = results[0]
            valid = r.get("valid")
            if isinstance(valid, str):
                try:
                    if abs(int(valid[:4]) - int(date_str[:4])) > 1:
                        return None
                except Exception:
                    pass
            return {
                "station": r.get("station"),
                "date": valid,
                "high_f": r.get("high"),
                "low_f": r.get("low"),
                "high_normal": r.get("high_normal"),
                "low_normal": r.get("low_normal"),
                "high_record": r.get("high_record"),
                "low_record": r.get("low_record"),
                "_date_mismatch": True,
                "source": "iem",
            }
        return None
    except Exception:
        return None


# ══════════════════════════════════════════════════════════════════════
#  PARTON-LOGAN DTC — diurnal temperature cycle model
# ══════════════════════════════════════════════════════════════════════

def parton_logan_dtc(t_min: float, t_max: float, hour: float,
                     sunrise: float = 6.0, sunset: float = 18.0) -> float:
    """
    Parton-Logan diurnal temperature cycle model.
    Sine curve during daytime, exponential decay at night.

    Fit Tmax from morning observations via least-squares on the rising limb.
    By noon, the posterior on Tmax is extremely tight — intraday edge lives here.

    Args:
        t_min, t_max: daily min/max temperature (°F)
        hour: hour of day (0-24, fractional OK)
        sunrise, sunset: approximate sunrise/sunset hours

    Returns:
        estimated temperature at given hour
    """
    day_len = sunset - sunrise
    night_len = 24.0 - day_len
    t_lag = 1.5  # thermal lag — max temp occurs ~1.5h after solar noon

    if sunrise <= hour <= sunset:
        # Daytime: sine curve
        phase = math.pi * (hour - sunrise) / (day_len + 2 * t_lag)
        return t_min + (t_max - t_min) * math.sin(phase)
    else:
        # Nighttime: exponential decay toward t_min
        if hour > sunset:
            hours_after_sunset = hour - sunset
        else:
            hours_after_sunset = (24 - sunset) + hour
        t_sunset = t_min + (t_max - t_min) * math.sin(math.pi * day_len / (day_len + 2 * t_lag))
        decay = math.exp(-2.2 * hours_after_sunset / night_len)
        return t_min + (t_sunset - t_min) * decay


def fit_tmax_from_morning(obs_hours: list[float], obs_temps: list[float],
                          t_min: float, sunrise: float = 6.0, sunset: float = 18.0) -> float:
    """
    Estimate Tmax from morning observations by fitting Parton-Logan rising limb.
    The key intraday signal: by 10-11 AM local, Tmax posterior is very tight.
    """
    from scipy.optimize import minimize_scalar

    def cost(t_max_guess):
        return sum((parton_logan_dtc(t_min, t_max_guess, h, sunrise, sunset) - t) ** 2
                   for h, t in zip(obs_hours, obs_temps))

    res = minimize_scalar(cost, bounds=(t_min, t_min + 60), method="bounded")
    return round(res.x, 1)


# ══════════════════════════════════════════════════════════════════════
#  PYKALSHI — WebSocket streaming
# ══════════════════════════════════════════════════════════════════════

def create_kalshi_client(api_key: Optional[str] = None, private_key_path: Optional[str] = None):
    """
    Create authenticated Kalshi client with auto-reconnect and RSA-PSS auth.
    Falls back to public (unauthenticated) if no credentials.
    """
    try:
        from pykalshi import KalshiClient
        if private_key_path and api_key:
            # Support both legacy and newer pykalshi constructors.
            try:
                return KalshiClient.create_with_rsa(api_key, private_key_path)
            except Exception:
                try:
                    return KalshiClient(api_key_id=api_key, private_key_path=private_key_path)
                except Exception:
                    pass
        if api_key:
            try:
                return KalshiClient(api_key=api_key)
            except Exception:
                try:
                    return KalshiClient(api_key_id=api_key)
                except Exception:
                    pass
        return KalshiClient()  # public only
    except ImportError:
        return None


# ══════════════════════════════════════════════════════════════════════
#  INTEGRATED PIPELINE
# ══════════════════════════════════════════════════════════════════════

@dataclass
class KukulkanV2:
    """
    Full pipeline: NWP → calibration → edge → Kelly sizing.
    Orchestrates all library integrations.
    """
    bankroll: float = 1000.0
    max_kelly_frac: float = 0.25
    max_price_cents: int = 40  # buy ≤40c per trading rules

    def run_city(self, city_key: str, target_date: Optional[datetime] = None) -> dict:
        """Full pipeline for one city."""
        city = KALSHI_CITIES.get(city_key)
        if not city:
            return {"error": f"unknown city: {city_key}"}

        name, settle_icao, bufkit_icao, lat, lon, series_high, series_low, cli_id, wfo = city
        target = target_date or datetime.now(timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)

        result = {"city": city_key, "name": name, "date": target.strftime("%Y-%m-%d")}

        # 1. Fetch NWP — BUFKIT daily max first, Herbie TMAX fallback
        t2m = {}
        try:
            from nwp_pipeline import ingest_city
            ens = ingest_city(city_key, verbose=False, target_date=target)
            for model, info in ens.model_spread().items():
                t2m[model] = info["max"]
        except Exception:
            pass

        # Herbie fallback using proper TMAX (not instantaneous T2m)
        if len(t2m) < 2:
            for model in ["gfs", "nam", "hrrr"]:
                if model in t2m:
                    continue
                val = fetch_herbie_daily_max(lat, lon, model=model)
                if val is not None:
                    t2m[f"{model}_herbie"] = val

        # NYC settles at Central Park (KNYC), not LGA. Force Central Park-grid
        # model values instead of KLGA BUFKIT proxy temperatures.
        if city_key == "nyc":
            nyc_models = {}
            for model in ["hrrr", "rap", "nam", "gfs"]:
                val = fetch_herbie_daily_max(lat, lon, model=model)
                if val is not None:
                    nyc_models[model] = val
            if nyc_models:
                t2m = nyc_models
                result["nyc_station_mode"] = "central_park_only"

        # Override BUFKIT GFS with Herbie native TMAX for daily-high consistency.
        gfs_tmax = fetch_herbie_daily_max(lat, lon, model="gfs")
        if gfs_tmax is not None:
            result["gfs_tmax"] = gfs_tmax
            t2m["gfs"] = gfs_tmax

        # Add independent ensemble members: NBM, LAMP-like hourly, ECMWF, T850 proxy.
        extra_models = fetch_additional_model_highs(lat, lon, target)
        if extra_models:
            t2m.update(extra_models)
            result["extra_models"] = extra_models

        result["nwp"] = t2m

        # 2. Latest METAR + short history (used for intraday trajectory adjustment)
        import urllib.request
        metar_url = f"https://aviationweather.gov/api/data/metar?ids={settle_icao}&format=raw&hours=12"
        metar_history = []
        try:
            req = urllib.request.Request(metar_url, headers={"User-Agent": "kukulkan/2.0"})
            with urllib.request.urlopen(req, timeout=8) as r:
                raws = [x.strip() for x in r.read().decode().strip().split("\n") if x.strip()]
            for raw in raws:
                parsed = parse_metar(raw)
                if parsed and "error" not in parsed:
                    metar_history.append(parsed)
            result["metar"] = metar_history[0] if metar_history else None
            result["metar_count"] = len(metar_history)
        except Exception:
            result["metar"] = None

        # 3. IEM CLI for verification
        result["cli"] = fetch_iem_cli(cli_id)

        # 4. Calibrate and produce bucket probs
        if t2m:
            from calibration import calibrate_city
            model_highs = {}
            for k, v in t2m.items():
                model_name = k.split("_")[0]
                if model_name not in model_highs or v > model_highs[model_name]:
                    model_highs[model_name] = v

            calib = calibrate_city(city_key, model_highs, target)
            cal_mean = float(calib.get("calib_mean", 70.0))
            cal_std = float(calib.get("calib_std", 5.0))
            dist = calib.get("distribution")

            # Intraday trajectory correction (adaptive after sunrise).
            traj = apply_intraday_trajectory_adjustment(
                metar_history, datetime.now(timezone.utc), lon, cal_mean, cal_std
            )
            result["trajectory"] = traj
            if traj.get("applied"):
                cal_mean = float(traj.get("mean", cal_mean))
                cal_std = float(traj.get("std", cal_std))
                try:
                    from calibration import CalibratedDistribution
                    dist = CalibratedDistribution(
                        city=city_key,
                        target_date=target,
                        mean=cal_mean,
                        std=max(2.0, cal_std),
                        n_models=len(model_highs),
                        n_forecasts=len(model_highs),
                        model_highs=model_highs,
                    )
                except Exception:
                    pass

            result["calib_mean"] = round(cal_mean, 1)
            result["calib_std"] = round(cal_std, 1)
            if dist is not None:
                result["p10"] = dist.quantile(0.10)
                result["p90"] = dist.quantile(0.90)
            else:
                result["p10"] = calib.get("p10")
                result["p90"] = calib.get("p90")

            # 5. Fetch Kalshi markets and run multinomial Kelly
            markets = self._fetch_markets(series_high)
            if markets and dist is not None:

                # Compute calibrated probs per bucket
                probs = []
                for m in markets:
                    if m.get("is_tail_up"):
                        p = 1.0 - dist.cdf(m["lo"])
                    elif m.get("is_tail_down"):
                        p = dist.cdf(m["hi"])
                    else:
                        p = dist.bucket_prob(m["lo"], m["hi"])
                    probs.append(p)

                total_p = sum(probs) or 1.0
                probs = [p / total_p for p in probs]
                prices = [m["yes_price"] / 100.0 for m in markets]

                # MULTINOMIAL KELLY
                fracs = multinomial_kelly(probs, prices, self.max_kelly_frac)

                signals = []
                for m, p, q, f in zip(markets, probs, prices, fracs):
                    edge_vs_mid = p - q
                    ask_c = m.get("yes_ask", 100)
                    bid_c = m.get("yes_bid", 0)
                    edge_vs_ask = p - ask_c / 100.0  # edge after paying the ask
                    spread = ask_c - bid_c
                    bet = self.bankroll * f

                    # Label for display
                    if m.get("is_tail_up"):
                        label = f">{m['lo']:.0f}°F"
                    elif m.get("is_tail_down"):
                        label = f"<{m['hi']:.0f}°F"
                    else:
                        label = f"{m['lo']:.0f}-{m['hi']:.0f}°F"

                    # Signal: BUY only if edge survives the ask AND price ≤ max
                    is_buy = (f > 0.01 and ask_c <= self.max_price_cents
                              and edge_vs_ask > 0.03 and spread < 15)

                    signals.append({
                        "ticker": m.get("ticker", ""),
                        "bucket": label,
                        "calib_prob": round(p, 4),
                        "market_cents": round(q * 100, 1),
                        "yes_bid": bid_c,
                        "yes_ask": ask_c,
                        "spread": spread,
                        "edge_vs_mid": round(edge_vs_mid * 100, 1),
                        "edge_vs_ask": round(edge_vs_ask * 100, 1),
                        "edge_pct": round(edge_vs_mid * 100, 1),
                        "kelly_frac": round(f, 4),
                        "bet_size": round(bet, 2),
                        "signal": "BUY" if is_buy else "HOLD",
                    })
                result["signals"] = signals
                result["total_risk"] = round(sum(s["bet_size"] for s in signals if s["signal"] == "BUY"), 2)

        return result

    def deep_city(self, city_key: str, target_date: Optional[datetime] = None) -> dict:
        """
        Full deep analysis for one city. Chains every library:

        1. NWP fetch (Herbie + BUFKIT fallback) — raw model T2m
        2. METAR parse (python-metar) — current obs + 6/24hr extremes + SLP
        3. IEM CLI (JSON) — yesterday's settlement for verification
        4. Kalman bias correction (pykalman) — EM-learned Q/R on historical errors
        5. ADWIN drift detection (River) — has model behavior shifted?
        6. EMOS calibration (scoringrules) — CRPS-minimized [a,b,c,d]
        7. Parton-Logan DTC fit — intraday Tmax constraint from morning obs
        8. EVT tail (pyextremes) — GPD for extreme bucket probabilities
        9. Orderbook fetch — real bid/ask from Kalshi
        10. Multinomial Kelly — closed-form position sizing
        """
        import time as _time

        city = KALSHI_CITIES.get(city_key)
        if not city:
            return {"error": f"unknown city: {city_key}"}

        name, settle_icao, bufkit_icao, lat, lon, series_high, series_low, cli_id, wfo = city
        target = target_date or datetime.now(timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)
        now_utc = datetime.now(timezone.utc)

        result = {
            "city": city_key, "name": name, "date": target.strftime("%Y-%m-%d"),
            "analysis": "deep", "stages": {},
        }

        # ── Stage 1: NWP (BUFKIT daily max — NOT instantaneous T2m) ──
        t0 = _time.time()
        t2m = {}
        # BUFKIT pipeline extracts MAX temperature across all forecast hours
        # for the settlement period. This is what we need for daily high markets.
        # Herbie T2m is instantaneous — WRONG for daily high forecasts.
        try:
            from nwp_pipeline import ingest_city
            ens = ingest_city(city_key, verbose=False, target_date=target)
            for model, info in ens.model_spread().items():
                t2m[model] = info["max"]
        except Exception:
            pass

        # Herbie fallback ONLY for models BUFKIT missed, and ONLY if we
        # fetch the max across multiple forecast hours (f12-f21 for ~afternoon)
        if len(t2m) < 2:
            for model in ["hrrr", "rap", "nam", "gfs"]:
                if model in t2m:
                    continue
                val = fetch_herbie_daily_max(lat, lon, model=model)
                if val is not None:
                    t2m[f"{model}_herbie_max"] = val

        # NYC settles at Central Park (KNYC), not LGA. Force Central Park-grid
        # model values instead of KLGA BUFKIT proxy temperatures.
        if city_key == "nyc":
            nyc_models = {}
            for model in ["hrrr", "rap", "nam", "gfs"]:
                val = fetch_herbie_daily_max(lat, lon, model=model)
                if val is not None:
                    nyc_models[model] = val
            if nyc_models:
                t2m = nyc_models
                result["nyc_station_mode"] = "central_park_only"

        # Add independent supplemental model members.
        extra_models = fetch_additional_model_highs(lat, lon, target)
        if extra_models:
            t2m.update(extra_models)
            result["extra_models"] = extra_models

        result["nwp"] = t2m
        result["stages"]["nwp"] = round(_time.time() - t0, 2)

        # ── Stage 2: METAR (full remarks) ─────────────────────────────
        t0 = _time.time()
        import urllib.request
        metar_data = None
        metar_history = []
        metar_url = f"https://aviationweather.gov/api/data/metar?ids={settle_icao}&format=raw&hours=12"
        try:
            req = urllib.request.Request(metar_url, headers={"User-Agent": "kukulkan/2.0"})
            with urllib.request.urlopen(req, timeout=10) as r:
                lines = r.read().decode().strip().split("\n")
            for line in lines:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parsed = parse_metar(line)
                if parsed and "error" not in parsed:
                    metar_history.append(parsed)
            if metar_history:
                metar_data = metar_history[0]  # most recent
        except Exception:
            pass

        result["metar"] = metar_data
        result["metar_count"] = len(metar_history)
        result["stages"]["metar"] = round(_time.time() - t0, 2)

        # ── Stage 3: IEM CLI (yesterday's settlement) ─────────────────
        t0 = _time.time()
        cli_yesterday = fetch_iem_cli(cli_id, now_utc - timedelta(days=1))
        result["cli_yesterday"] = cli_yesterday
        result["stages"]["cli"] = round(_time.time() - t0, 2)

        # ── Stage 4: Kalman bias correction ───────────────────────────
        t0 = _time.time()
        bias_info = {}
        # Build error history from calibration DB if available
        try:
            import sqlite3
            from calibration import CALIB_DB
            conn = sqlite3.connect(str(CALIB_DB))
            for model_name in set(k.split("_")[0] for k in t2m.keys()):
                rows = conn.execute("""
                    SELECT bias_high FROM verification
                    WHERE city = ? AND model = ? AND bias_high IS NOT NULL
                    ORDER BY date DESC LIMIT 60
                """, (city_key, model_name)).fetchall()
                if rows and len(rows) >= 5:
                    errors = [r[0] for r in rows]
                    kb, ku = kalman_bias_tracker(errors)
                    drift = detect_drift(errors)
                    bias_info[model_name] = {
                        "kalman_bias": round(kb, 2),
                        "kalman_unc": round(ku, 2),
                        "drift_detected": drift["drifted"],
                        "drift_n": drift["n_detections"],
                        "n_verif": len(rows),
                    }
            conn.close()
        except Exception:
            pass

        result["bias"] = bias_info
        result["stages"]["kalman"] = round(_time.time() - t0, 2)

        # ── Stage 5: Smart model aggregation + EMOS ─────────────────
        t0 = _time.time()

        # Fetch 30-day climatological stats from CLI verification DB
        climo_mean = climo_std = None
        try:
            import sqlite3
            from calibration import CALIB_DB
            conn = sqlite3.connect(str(CALIB_DB))
            rows = conn.execute("""
                SELECT obs_high_f FROM verification
                WHERE city = ? AND model = 'cli' AND obs_high_f IS NOT NULL
                ORDER BY date DESC LIMIT 30
            """, (city_key,)).fetchall()
            conn.close()
            if rows and len(rows) >= 7:
                cli_highs = [r[0] for r in rows]
                climo_mean = float(np.mean(cli_highs))
                climo_std = float(np.std(cli_highs))
        except Exception:
            pass

        if t2m:
            # Deduplicate to best per model
            model_highs = {}
            for k, v in t2m.items():
                mname = k.split("_")[0]
                if mname not in model_highs or v > model_highs[mname]:
                    model_highs[mname] = v

            # Use Herbie GFS TMAX as a better daily-high estimate for GFS.
            herbie_tmax = fetch_herbie_daily_max(lat, lon, model="gfs")
            if herbie_tmax is not None:
                result["gfs_tmax"] = herbie_tmax
                model_highs["gfs"] = herbie_tmax

            # Apply Kalman bias correction where available
            corrected = {}
            for m, raw_t in model_highs.items():
                base = m.split("_")[0]
                if base in bias_info:
                    corrected[m] = round(raw_t - bias_info[base]["kalman_bias"], 1)
                else:
                    try:
                        from calibration import BiasCorrector
                        bc = BiasCorrector(city_key, base, "high")
                        corrected[m] = bc.correct(raw_t, target)
                    except Exception:
                        corrected[m] = raw_t

            # Robust aggregation: trim outliers, weight by model skill
            temps = list(corrected.values())
            n = len(temps)

            if n >= 4:
                # Trimmed mean: drop highest and lowest, less sensitive to one bad model
                sorted_t = sorted(temps)
                trimmed = sorted_t[1:-1]
                ens_mean = sum(trimmed) / len(trimmed)
                # Use IQR-based spread instead of range (more robust)
                q25 = np.percentile(temps, 25)
                q75 = np.percentile(temps, 75)
                ens_spread = max(float(q75 - q25), 2.0)
            elif n >= 2:
                ens_mean = sum(temps) / n
                ens_spread = max(max(temps) - min(temps), 3.0)
            else:
                ens_mean = temps[0] if temps else 70.0
                ens_spread = 5.0

            # Weighted mean by model skill.
            weights = {
                "hrrr": 1.5, "nam3": 1.3, "rap": 1.0, "nam": 0.9, "gfs": 0.7, "sref": 1.2,
                "nbm": 1.7, "lamp": 1.6, "ecmwf": 1.4, "sounding": 1.1,
            }
            total_w = sum(weights.get(m, 1.0) for m in corrected)
            weighted_mean = sum(corrected[m] * weights.get(m, 1.0) for m in corrected) / total_w

            # Blend with climatology if models disagree wildly
            if climo_mean is not None and ens_spread > 12:
                # Models are garbage — anchor toward climatology
                climo_weight = min(0.4, (ens_spread - 8) / 20.0)
                weighted_mean = weighted_mean * (1 - climo_weight) + climo_mean * climo_weight
                ens_spread = ens_spread * (1 - climo_weight) + (climo_std or 8.0) * climo_weight

            # EMOS calibration
            try:
                from calibration import EMOSCalibrator
                emos = EMOSCalibrator(city_key, "high")
                cal_mean, cal_std = emos.calibrate(weighted_mean, ens_spread, target)
            except Exception:
                cal_mean = round(weighted_mean, 1)
                cal_std = max(round(ens_spread * 0.5 + 1.5, 1), 2.5)

            # Floor σ based on model count and spread
            if n >= 4 and ens_spread < 6:
                cal_std = max(cal_std, 2.5)  # tight agreement
            elif n >= 3:
                cal_std = max(cal_std, 3.0)
            else:
                cal_std = max(cal_std, 4.0)

            result["corrected"] = corrected
            result["ens_mean"] = round(ens_mean, 1)
            result["weighted_mean"] = round(weighted_mean, 1)
            result["raw_calib_mean"] = cal_mean
            result["raw_calib_std"] = cal_std
            if climo_mean is not None and climo_std is not None:
                result["climo_30d"] = {"mean": round(climo_mean, 1), "std": round(climo_std, 1)}
        else:
            cal_mean = climo_mean or 70.0
            cal_std = climo_std or 10.0
            corrected = {}
            weighted_mean = cal_mean

        result["stages"]["emos"] = round(_time.time() - t0, 2)

        # ── Stage 6: Parton-Logan DTC tightening ──────────────────────
        # ONLY use today's post-midnight observations. Yesterday's afternoon
        # obs constrain YESTERDAY's Tmax, not today's. The DTC rising limb
        # (post-sunrise) is what constrains today's Tmax.
        t0 = _time.time()
        dtc_info = {}
        if metar_history:
            tz_offset = round(lon / 15.0)

            # Filter to TODAY's observations only
            # "Today" starts at local midnight = 00:00 local = -tz_offset UTC
            today_midnight_utc = now_utc.replace(hour=0, minute=0, second=0) - timedelta(hours=tz_offset)

            today_obs_hours = []
            today_obs_temps = []
            min_temp = None
            for m in reversed(metar_history):  # oldest first
                if m.get("temp_f") is None or m.get("time") is None:
                    continue
                t = m["time"]
                if not hasattr(t, "hour"):
                    continue
                # Reconstruct UTC datetime for this obs
                obs_utc = now_utc.replace(hour=t.hour, minute=t.minute, second=0)
                if obs_utc > now_utc:
                    obs_utc -= timedelta(days=1)
                # Skip observations from before today's local midnight
                if obs_utc < today_midnight_utc:
                    continue

                local_hour = (t.hour + tz_offset) % 24 + t.minute / 60.0
                today_obs_hours.append(local_hour)
                today_obs_temps.append(m["temp_f"])
                if min_temp is None or m["temp_f"] < min_temp:
                    min_temp = m["temp_f"]

            latest_local = today_obs_hours[-1] if today_obs_hours else 0

            if len(today_obs_hours) >= 3 and min_temp is not None and latest_local > 6:
                try:
                    tmax_est = fit_tmax_from_morning(today_obs_hours, today_obs_temps, t_min=min_temp)
                    # Weight DTC only after sunrise, strong after 10AM local
                    dtc_weight = min(0.6, max(0.0, (latest_local - 6) / 10.0))

                    if dtc_weight > 0:
                        blended_mean = cal_mean * (1 - dtc_weight) + tmax_est * dtc_weight
                        blended_std = cal_std * (1 - dtc_weight * 0.5)
                    else:
                        blended_mean = cal_mean
                        blended_std = cal_std

                    dtc_info = {
                        "tmax_est": round(tmax_est, 1),
                        "min_obs": round(min_temp, 1),
                        "n_today_obs": len(today_obs_hours),
                        "latest_local_hr": round(latest_local, 1),
                        "dtc_weight": round(dtc_weight, 2),
                        "blended_mean": round(blended_mean, 1),
                        "blended_std": round(blended_std, 1),
                    }
                    cal_mean = round(blended_mean, 1)
                    cal_std = round(blended_std, 1)
                except Exception:
                    pass
            else:
                dtc_info = {
                    "status": "pre-sunrise" if latest_local <= 6 else "insufficient_obs",
                    "n_today_obs": len(today_obs_hours),
                    "latest_local_hr": round(latest_local, 1),
                    "dtc_weight": 0.0,
                }

        result["dtc"] = dtc_info
        result["stages"]["dtc"] = round(_time.time() - t0, 2)

        # ── Stage 7: EVT for tails ────────────────────────────────────
        t0 = _time.time()
        evt_info = {}
        try:
            import sqlite3
            # Get historical highs for this station from climate DB
            climo_db = Path(__file__).parent / "climate_data" / "climatology.db"
            if climo_db.exists():
                conn = sqlite3.connect(str(climo_db))
                # Get daily highs for this station within ±30 days of year
                doy = target.timetuple().tm_yday
                rows = conn.execute("""
                    SELECT tmax FROM daily_obs
                    WHERE station = ? AND tmax IS NOT NULL
                    AND abs(cast(strftime('%j', date) as integer) - ?) <= 30
                """, (settle_icao, doy)).fetchall()
                conn.close()

                if rows and len(rows) >= 100:
                    highs = [r[0] for r in rows]
                    # High tail: P(T > extreme threshold)
                    p99 = np.percentile(highs, 99)
                    evt_high = fit_evt_tail(highs, threshold_pctile=0.95, target_temp=p99)
                    # Low tail: invert for P(T < cold threshold)
                    p01 = np.percentile(highs, 1)
                    evt_info = {
                        "n_historical": len(highs),
                        "climo_mean": round(float(np.mean(highs)), 1),
                        "climo_std": round(float(np.std(highs)), 1),
                        "p01": round(p01, 1),
                        "p99": round(p99, 1),
                        "evt_high": evt_high,
                    }
        except Exception:
            pass

        result["evt"] = evt_info
        result["stages"]["evt"] = round(_time.time() - t0, 2)

        # ── Stage 8: Build final distribution ─────────────────────────
        from calibration import CalibratedDistribution
        dist = CalibratedDistribution(
            city=city_key,
            target_date=target,
            mean=cal_mean,
            std=max(cal_std, 2.0),
            n_models=len(t2m),
            n_forecasts=len(t2m),
            model_highs=corrected,
        )
        result["calib_mean"] = cal_mean
        result["calib_std"] = max(cal_std, 2.0)
        result["p10"] = dist.quantile(0.10)
        result["p25"] = dist.quantile(0.25)
        result["p50"] = dist.quantile(0.50)
        result["p75"] = dist.quantile(0.75)
        result["p90"] = dist.quantile(0.90)

        # ── Stage 9+10: Markets + Multinomial Kelly ───────────────────
        t0 = _time.time()
        markets = self._fetch_markets(series_high)
        result["stages"]["markets"] = round(_time.time() - t0, 2)

        if markets:
            probs = []
            for m in markets:
                if m.get("is_tail_up"):
                    p_gauss = 1.0 - dist.cdf(m["lo"])
                    # Blend with EVT for upper tail if available
                    if evt_info.get("evt_high") and m["lo"] > evt_info.get("p99", 999):
                        p_evt = evt_info["evt_high"].get("p_exceed", p_gauss)
                        p = 0.5 * p_gauss + 0.5 * p_evt
                    else:
                        p = p_gauss
                elif m.get("is_tail_down"):
                    p = dist.cdf(m["hi"])
                else:
                    p = dist.bucket_prob(m["lo"], m["hi"])
                probs.append(p)

            total_p = sum(probs) or 1.0
            probs = [p / total_p for p in probs]
            prices = [m["yes_price"] / 100.0 for m in markets]

            fracs = multinomial_kelly(probs, prices, self.max_kelly_frac)

            signals = []
            for m, p, q, f in zip(markets, probs, prices, fracs):
                ask_c = m.get("yes_ask", 100)
                bid_c = m.get("yes_bid", 0)
                spread = ask_c - bid_c
                edge_mid = p - q
                edge_ask = p - ask_c / 100.0
                bet = self.bankroll * f

                if m.get("is_tail_up"):
                    label = f">{m['lo']:.0f}°F"
                elif m.get("is_tail_down"):
                    label = f"<{m['hi']:.0f}°F"
                else:
                    label = f"{m['lo']:.0f}-{m['hi']:.0f}°F"

                is_buy = (f > 0.01 and ask_c <= self.max_price_cents
                          and edge_ask > 0.03 and spread < 15)

                signals.append({
                    "ticker": m.get("ticker", ""),
                    "bucket": label,
                    "calib_prob": round(p, 4),
                    "market_cents": round(q * 100, 1),
                    "yes_bid": bid_c,
                    "yes_ask": ask_c,
                    "spread": spread,
                    "edge_vs_mid": round(edge_mid * 100, 1),
                    "edge_vs_ask": round(edge_ask * 100, 1),
                    "edge_pct": round(edge_mid * 100, 1),
                    "kelly_frac": round(f, 4),
                    "bet_size": round(bet, 2),
                    "signal": "BUY" if is_buy else "HOLD",
                })

            result["signals"] = signals
            result["total_risk"] = round(sum(s["bet_size"] for s in signals if s["signal"] == "BUY"), 2)

        return result

    def _fetch_markets(self, series_ticker: str) -> list[dict]:
        """
        Fetch live Kalshi bucket markets with real orderbook prices.
        Public API returns None for yes_bid/yes_ask, so we hit the
        orderbook endpoint: best YES ask = 1 - best NO bid.
        Filters to today-only markets (excludes settled).
        """
        import json
        import time as _time
        import urllib.request

        # Determine today's date tag for filtering (e.g. "26APR10")
        now = datetime.now(timezone.utc)
        date_tag = now.strftime("%y%b%d").upper()  # "26APR10"

        url = f"https://api.elections.kalshi.com/trade-api/v2/markets?series_ticker={series_ticker}&status=open&limit=40"
        try:
            req = urllib.request.Request(url, headers={"Accept": "application/json"})
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.loads(r.read())
        except Exception:
            return []

        raw_markets = data.get("markets", [])
        # Filter to today's markets only (ticker contains date tag)
        raw_markets = [m for m in raw_markets if date_tag in m.get("ticker", "")]
        if not raw_markets:
            return []

        def _get_book_price(ticker: str) -> Optional[dict]:
            """Fetch orderbook and derive mid-price for one ticker."""
            book_url = f"https://api.elections.kalshi.com/trade-api/v2/markets/{ticker}/orderbook"
            try:
                req = urllib.request.Request(book_url, headers={"Accept": "application/json"})
                with urllib.request.urlopen(req, timeout=8) as r:
                    book = json.loads(r.read())
                fp = book.get("orderbook_fp") or book.get("orderbook", {})
                yes_levels = fp.get("yes_dollars") or fp.get("yes", [])
                no_levels = fp.get("no_dollars") or fp.get("no", [])

                # Best YES bid = highest price in yes_dollars
                yes_bid = max((float(p) for p, _ in yes_levels), default=0) if yes_levels else 0
                # Best NO bid = highest price in no_dollars → YES ask = 1 - best_no_bid
                best_no = max((float(p) for p, _ in no_levels), default=0) if no_levels else 0
                yes_ask = (1.0 - best_no) if best_no > 0 else 1.0

                # Mid-price (in dollars, 0-1 scale)
                if yes_bid > 0 and yes_ask < 1.0:
                    mid = (yes_bid + yes_ask) / 2.0
                elif yes_bid > 0:
                    mid = yes_bid
                elif yes_ask < 1.0:
                    mid = yes_ask
                else:
                    return None  # no liquidity at all

                return {"yes_bid": round(yes_bid * 100, 1),
                        "yes_ask": round(yes_ask * 100, 1),
                        "mid": round(mid * 100, 1)}
            except Exception:
                return None

        # Sequential orderbook fetch with throttle to avoid 429
        book_prices = {}
        for m in raw_markets:
            t = m.get("ticker", "")
            book_prices[t] = _get_book_price(t)
            _time.sleep(0.12)  # ~8 req/s, under Kalshi's 10/s limit

        markets = []
        for m in raw_markets:
            ticker = m.get("ticker", "")
            floor = m.get("floor_strike")
            cap = m.get("cap_strike")
            bp = book_prices.get(ticker)

            # Determine bucket bounds
            # Tail-up: floor=X, cap=None → "will T > X?"  → bucket (X, +inf)
            # Tail-down: floor=None, cap=X → "will T < X?" → bucket (-inf, X)
            # Interior: floor=X, cap=Y → bucket (X, Y)
            if floor is not None and cap is not None:
                lo, hi = float(floor), float(cap)
            elif floor is not None:
                lo, hi = float(floor), float(floor) + 50  # tail-up
            elif cap is not None:
                lo, hi = float(cap) - 50, float(cap)  # tail-down
            else:
                continue

            if bp is None:
                continue  # skip illiquid

            markets.append({
                "lo": lo, "hi": hi,
                "yes_price": bp["mid"],
                "yes_bid": bp["yes_bid"],
                "yes_ask": bp["yes_ask"],
                "ticker": ticker,
                "is_tail_up": cap is None,
                "is_tail_down": floor is None,
            })

        return sorted(markets, key=lambda x: x["lo"])


# ══════════════════════════════════════════════════════════════════════
#  CLI
# ══════════════════════════════════════════════════════════════════════

def main():
    import argparse

    parser = argparse.ArgumentParser(description="Kukulkan v2 — weather market intelligence")
    parser.add_argument("city", nargs="*", help="City keys (e.g. nyc chi phx). Default: all 20")
    parser.add_argument("--bankroll", type=float, default=1000.0)
    parser.add_argument("--max-kelly", type=float, default=0.25)
    parser.add_argument("--json", action="store_true", help="JSON output")
    parser.add_argument("--deep", action="store_true", help="Deep analysis with all libraries")
    parser.add_argument("--kelly-demo", action="store_true", help="Demo multinomial Kelly on sample data")
    args = parser.parse_args()

    if args.kelly_demo:
        # Demo: 5-bucket market, our model says bucket 3 is underpriced
        probs  = [0.05, 0.15, 0.45, 0.25, 0.10]  # our calibrated probs
        prices = [0.08, 0.18, 0.30, 0.28, 0.16]  # market prices (sum = 1.00)
        fracs = multinomial_kelly(probs, prices, max_frac=0.25)
        print("\n  MULTINOMIAL KELLY DEMO")
        print(f"  {'Bucket':>8} {'P(ours)':>8} {'P(mkt)':>8} {'Edge':>8} {'Kelly%':>8} {'Bet($)':>8}")
        print(f"  {'─'*52}")
        for i, (p, q, f) in enumerate(zip(probs, prices, fracs)):
            bet = 1000 * f
            print(f"  {'ABCDE'[i]:>8} {p:>8.2%} {q:>8.2%} {p-q:>+8.2%} {f:>8.2%} {bet:>8.2f}")
        print(f"\n  Total risk: ${sum(1000*f for f in fracs):.2f} / $1000")
        return

    engine = KukulkanV2(bankroll=args.bankroll, max_kelly_frac=args.max_kelly)
    cities = args.city or sorted(KALSHI_CITIES.keys())
    run_fn = engine.deep_city if args.deep else engine.run_city

    if args.json:
        import json
        results = [run_fn(c) for c in cities]
        print(json.dumps(results, indent=2, default=str))
    else:
        for ck in cities:
            r = run_fn(ck)
            print(f"\n{'═'*60}")
            print(f"  {r.get('name', ck)} — {r.get('date', '?')}")
            print(f"{'═'*60}")

            if "calib_mean" in r and r["calib_mean"]:
                print(f"  Forecast:  μ={r['calib_mean']}°F  σ={r['calib_std']}°F  [{r.get('p10','?')}–{r.get('p90','?')}°F]")
            if r.get("nwp"):
                nwp_str = "  ".join(f"{k}={v}°" for k, v in r["nwp"].items())
                print(f"  NWP:       {nwp_str}")
            if r.get("corrected"):
                corr_str = "  ".join(f"{k}={v}°" for k, v in r["corrected"].items())
                print(f"  Corrected: {corr_str}")
            if r.get("metar") and not r["metar"].get("error"):
                m = r["metar"]
                slp = m.get('slp_mb')
                slp_s = f"  SLP={slp:.1f}mb" if slp else ""
                t6max = m.get('max_temp_6hr_f')
                t6_s = f"  6hr-max={t6max}°" if t6max else ""
                print(f"  METAR:     {m.get('temp_f', '?')}°F  dew={m.get('dewpoint_f', '?')}°F  "
                      f"wind={m.get('wind_speed_kt', '?')}kt{slp_s}{t6_s}")
            if r.get("cli_yesterday"):
                c = r["cli_yesterday"]
                print(f"  CLI(yday): high={c.get('high_f', '?')}°F  low={c.get('low_f', '?')}°F  "
                      f"normal={c.get('high_normal', '?')}°F")
            elif r.get("cli"):
                c = r["cli"]
                print(f"  CLI:       high={c.get('high_f', '?')}°F  low={c.get('low_f', '?')}°F")

            # Deep analysis extras
            if r.get("bias"):
                for m, b in r["bias"].items():
                    drift_s = " DRIFT!" if b.get("drift_detected") else ""
                    print(f"  Kalman[{m}]: bias={b['kalman_bias']:+.1f}°F  unc={b['kalman_unc']:.1f}°F  "
                          f"n={b['n_verif']}{drift_s}")
            if r.get("dtc") and r["dtc"]:
                d = r["dtc"]
                if "tmax_est" in d:
                    print(f"  DTC:       Tmax_est={d['tmax_est']}°F  min_obs={d['min_obs']}°F  "
                          f"weight={d['dtc_weight']:.0%}  blended={d['blended_mean']}°±{d['blended_std']}°")
                else:
                    print(f"  DTC:       {d.get('status', 'inactive')}  "
                          f"today_obs={d.get('n_today_obs', 0)}  local_hr={d.get('latest_local_hr', '?')}")
            if r.get("climo_30d"):
                c = r["climo_30d"]
                print(f"  Climo30d:  μ={c['mean']}°F  σ={c['std']}°F")
            if r.get("evt") and r["evt"]:
                e = r["evt"]
                print(f"  EVT:       climo={e['climo_mean']}°±{e['climo_std']}°  "
                      f"[P01={e['p01']}° P99={e['p99']}°]  n={e['n_historical']}")
            if r.get("stages"):
                st = r["stages"]
                parts = [f"{k}={v:.1f}s" for k, v in st.items()]
                print(f"  Timing:    {' '.join(parts)}")

            if r.get("signals"):
                print(f"\n  {'Bucket':<14} {'Model%':>7} {'Bid':>5} {'Ask':>5} {'Mid¢':>5} {'Edge':>7} {'Kelly':>7} {'Bet':>8} {'Signal'}")
                print(f"  {'─'*78}")
                for s in r["signals"]:
                    flag = " ◄" if s["signal"] == "BUY" else ""
                    bid = s.get('yes_bid', 0)
                    ask = s.get('yes_ask', 100)
                    print(f"  {s['bucket']:<14} {s['calib_prob']*100:>6.1f}% {bid:>4.0f}¢ {ask:>4.0f}¢ {s['market_cents']:>4.0f}¢ "
                          f"{s['edge_pct']:>+6.1f}% {s['kelly_frac']*100:>6.1f}% ${s['bet_size']:>7.2f} {s['signal']}{flag}")
                if r.get("total_risk"):
                    print(f"\n  Total risk: ${r['total_risk']:.2f} / ${args.bankroll:.0f}")


if __name__ == "__main__":
    main()
