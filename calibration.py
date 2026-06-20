#!/usr/bin/env python3
"""
Kukulkan Ensemble Calibration Engine
=====================================
Bias-aware multi-model ensemble calibration for Kalshi temperature bucket trading.

Implements:
  - EMOS (Ensemble Model Output Statistics) — Gneiting et al. 2005
  - Per-model per-city bias tracking with exponential decay
  - Station transfer functions (LGA→Central Park, etc.)
  - Dress/spread calibration for raw ensemble dispersion
  - Extended logistic regression for bucket probabilities (Wilks 2009)
  - CRPS-optimal distribution fitting
  - Multinomial Kelly criterion for categorical market position sizing
  - Historical verification database

The pipeline:
  raw NWP forecasts → bias correction → EMOS calibration → predictive CDF
  → bucket probabilities → edge vs Kalshi prices → Kelly sizing → signals
"""

import math
import json
import sqlite3
import time
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional


def _get_kalshi_cities():
    from nwp_pipeline import KALSHI_CITIES
    return KALSHI_CITIES


# ── Storage ──────────────────────────────────────────────────────────
CALIB_DIR = Path.home() / ".dsco" / "calibration"
CALIB_DB = CALIB_DIR / "calibration.db"
NWP_DB = Path(__file__).parent / "climate_data" / "nwp_forecasts.db"
GLOBAL_EMOS_CITY = "__global__"


def _ensure_db():
    CALIB_DIR.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(CALIB_DB))
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS verification (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            date TEXT NOT NULL,          -- YYYY-MM-DD (local settlement date)
            model TEXT NOT NULL,
            cycle INTEGER NOT NULL,
            fcst_high_f REAL,
            fcst_low_f REAL,
            obs_high_f REAL,             -- NWS CLI observed high
            obs_low_f REAL,              -- NWS CLI observed low
            bias_high REAL,              -- fcst - obs
            bias_low REAL,
            abs_err_high REAL,
            abs_err_low REAL,
            lead_hours REAL,             -- hours from init to settlement
            recorded_at TEXT DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(city, date, model, cycle)
        );

        CREATE TABLE IF NOT EXISTS emos_params (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            param TEXT NOT NULL,          -- 'high' or 'low'
            a REAL NOT NULL,              -- location intercept
            b REAL NOT NULL,              -- location slope on ensemble mean
            c REAL NOT NULL,              -- scale intercept
            d REAL NOT NULL,              -- scale slope on ensemble spread
            n_train INTEGER,
            crps_train REAL,
            valid_from TEXT,
            valid_to TEXT,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(city, param)
        );

        CREATE TABLE IF NOT EXISTS bias_tracker (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            model TEXT NOT NULL,
            param TEXT NOT NULL,           -- 'high' or 'low'
            running_bias REAL DEFAULT 0,   -- exponentially weighted bias
            running_mae REAL DEFAULT 0,
            running_rmse REAL DEFAULT 0,
            n_obs INTEGER DEFAULT 0,
            last_updated TEXT,
            UNIQUE(city, model, param)
        );

        CREATE TABLE IF NOT EXISTS station_offsets (
            city TEXT NOT NULL,
            bufkit_icao TEXT NOT NULL,
            settle_icao TEXT NOT NULL,
            param TEXT NOT NULL,
            offset_f REAL DEFAULT 0,       -- settle - bufkit (add to forecast)
            std_f REAL DEFAULT 2.0,
            n_pairs INTEGER DEFAULT 0,
            updated_at TEXT,
            UNIQUE(city, param)
        );

        CREATE TABLE IF NOT EXISTS trade_signals (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            date TEXT NOT NULL,
            bucket_lo REAL,
            bucket_hi REAL,
            ticker TEXT,
            calib_prob REAL,               -- calibrated probability
            market_price REAL,             -- Kalshi cents
            edge_pct REAL,
            kelly_frac REAL,
            signal TEXT,                   -- BUY / SELL / HOLD
            confidence TEXT,               -- HIGH / MED / LOW
            generated_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS kalman_state (
            city TEXT NOT NULL,
            model TEXT NOT NULL,
            variable TEXT NOT NULL,
            state_mean REAL NOT NULL,
            state_var REAL NOT NULL,
            process_var REAL NOT NULL,
            obs_var REAL NOT NULL,
            regime TEXT,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (city, model, variable)
        );

        CREATE TABLE IF NOT EXISTS calibration_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            target_date TEXT NOT NULL,
            mode TEXT NOT NULL,
            regime TEXT,
            status TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            fallback_reason_codes TEXT,
            is_last_good INTEGER DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS calibration_metrics (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            target_date TEXT NOT NULL,
            metric_name TEXT NOT NULL,
            metric_value REAL NOT NULL,
            baseline_value REAL,
            regime TEXT,
            model TEXT,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS emos_training_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            param TEXT NOT NULL,
            training_mode TEXT NOT NULL,
            target_date TEXT,
            window_days INTEGER,
            season INTEGER,
            n_train INTEGER NOT NULL,
            crps_train REAL,
            candidate_json TEXT NOT NULL,
            stability_json TEXT NOT NULL,
            deployed INTEGER NOT NULL DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS station_metadata_versions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            station_icao TEXT NOT NULL,
            role TEXT NOT NULL,
            effective_from TEXT NOT NULL,
            effective_to TEXT,
            metadata_json TEXT NOT NULL DEFAULT '{}',
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS ingest_counters (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source TEXT NOT NULL,
            model TEXT,
            city TEXT,
            status TEXT NOT NULL,
            counter INTEGER NOT NULL DEFAULT 0,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(source, model, city, status)
        );

        CREATE INDEX IF NOT EXISTS idx_verif_city_date ON verification(city, date);
        CREATE INDEX IF NOT EXISTS idx_verif_model ON verification(model, city);
        CREATE INDEX IF NOT EXISTS idx_bias_city_model ON bias_tracker(city, model, param);
        CREATE INDEX IF NOT EXISTS idx_signals_city_date ON trade_signals(city, date);
        CREATE INDEX IF NOT EXISTS idx_runs_city_date ON calibration_runs(city, target_date);
        CREATE INDEX IF NOT EXISTS idx_metrics_city_date ON calibration_metrics(city, target_date);
        CREATE INDEX IF NOT EXISTS idx_emos_training_city_param
            ON emos_training_runs(city, param, training_mode, created_at);
        CREATE INDEX IF NOT EXISTS idx_station_metadata_lookup
            ON station_metadata_versions(city, role, effective_from, effective_to);
    """)

    # Seed explicit station metadata overrides so the lookup path is stable
    # even before any user-driven metadata history is written.
    now = datetime.now(timezone.utc).isoformat()
    for city_key, override in STATION_SOURCE_OVERRIDES.items():
        for role, station_icao in (
            ("settlement", override["settlement_icao"]),
            ("observation", override["observation_icao"]),
        ):
            conn.execute("""
                INSERT OR IGNORE INTO station_metadata_versions
                (city, station_icao, role, effective_from, effective_to, metadata_json, updated_at)
                VALUES (?, ?, ?, ?, NULL, ?, ?)
            """, (
                city_key,
                station_icao,
                role,
                "1970-01-01T00:00:00+00:00",
                json.dumps({"source": override.get("observation_source", station_icao)}),
                now,
            ))
    conn.close()


def _parse_init_cycle(init_cycle: str) -> Optional[datetime]:
    """Parse init_cycle strings like YYYYMMDD_12z into UTC datetimes."""
    try:
        return datetime.strptime(init_cycle, "%Y%m%d_%Hz").replace(tzinfo=timezone.utc)
    except Exception:
        return None


def _nwp_cutoff_cycle(date_str: str, cutoff_hour_utc: int) -> str:
    """Build an init_cycle cutoff key for a settlement date."""
    return f"{date_str.replace('-', '')}_{cutoff_hour_utc:02d}z"


def _collect_model_forecasts_from_nwp(city_key: str, date_str: str,
                                      cutoff_hour_utc: int = 23) -> dict[str, dict]:
    """
    Build per-model daily high/low forecasts for one city/date from stored NWP rows.

    Uses the latest init_cycle <= cutoff_hour_utc on the settlement date.
    Returns model -> {high_f, low_f, cycle, lead_hours}.
    """
    if not NWP_DB.exists():
        return {}

    cutoff_cycle = _nwp_cutoff_cycle(date_str, cutoff_hour_utc)
    conn = sqlite3.connect(str(NWP_DB))
    rows = conn.execute(
        """
        SELECT model, init_cycle, MAX(t2m_f) AS high_f, MIN(t2m_f) AS low_f
        FROM nwp_forecasts
        WHERE city = ? AND date(valid_utc) = ? AND init_cycle <= ? AND t2m_f > -900
        GROUP BY model, init_cycle
        ORDER BY model, init_cycle
        """,
        (city_key, date_str, cutoff_cycle),
    ).fetchall()
    conn.close()

    grouped: dict[str, list[tuple[str, float, float]]] = defaultdict(list)
    for model, init_cycle, high_f, low_f in rows:
        if high_f is None:
            continue
        grouped[str(model)].append((str(init_cycle), float(high_f), float(low_f)))

    out: dict[str, dict] = {}
    target_end = datetime.strptime(f"{date_str} 23:59:00", "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    season = _season(target_end)
    station_hi = STATION_OFFSETS.get(city_key, {}).get("high", {}).get(season, 0.0)
    station_lo = STATION_OFFSETS.get(city_key, {}).get("low", {}).get(season, 0.0)
    for model, vals in grouped.items():
        latest_cycle, high_f, low_f = sorted(vals, key=lambda x: x[0])[-1]
        init_dt = _parse_init_cycle(latest_cycle)
        lead_h = 0.0
        cycle_h = 0
        if init_dt is not None:
            lead_h = max(0.0, (target_end - init_dt).total_seconds() / 3600.0)
            cycle_h = init_dt.hour

        out[model] = {
            "high_f": round(high_f + station_hi, 1),
            "low_f": round(low_f + station_lo, 1),
            "cycle": cycle_h,
            "lead_hours": round(lead_h, 1),
        }
    return out


# ══════════════════════════════════════════════════════════════════════
#  KNOWN BIASES (priors before verification data accumulates)
# ══════════════════════════════════════════════════════════════════════

# Documented model biases (°F) for daily high temperature
# Positive = model runs warm, negative = model runs cold
# Sources: various MOS verification studies, ASOS sensor literature
PRIOR_BIASES = {
    # (model, season) → bias °F
    # Season: 1=DJF, 2=MAM, 3=JJA, 4=SON
    ("hrrr", 1):  +0.5,   ("hrrr", 2):  +1.5,   ("hrrr", 3):  +3.6,   ("hrrr", 4):  +1.0,
    ("rap",  1):  +0.8,   ("rap",  2):  +1.2,   ("rap",  3):  +2.8,   ("rap",  4):  +0.9,
    ("nam",  1):  +0.3,   ("nam",  2):  +1.0,   ("nam",  3):  +2.5,   ("nam",  4):  +0.5,
    ("nam3", 1):  +0.2,   ("nam3", 2):  +0.8,   ("nam3", 3):  +2.0,   ("nam3", 4):  +0.4,
    ("gfs",  1):  -1.5,   ("gfs",  2):  -1.0,   ("gfs",  3):  -3.5,   ("gfs",  4):  -1.8,
    ("sref", 1):  +0.0,   ("sref", 2):  +0.5,   ("sref", 3):  +1.0,   ("sref", 4):  +0.3,
}

# ASOS measurement biases relative to NWS CLI settlement
# The MMTS-to-ASOS changeover shifted temps by -0.44°C (~-0.8°F)
# HO-1088 hygrothermometer 5-min averaging dampens extremes ~0.3°F
ASOS_DAMPENING_BIAS = -0.3  # °F — ASOS understates true max by this much

# Station transfer offsets (BUFKIT proxy → settlement station)
# NYC: LGA (KLGA) → Central Park (KNYC)
# Central Park runs ~2-4°F cooler than LGA in summer (urban heat island inversion)
# and ~1-2°F warmer in winter (park thermal lag)
STATION_OFFSETS = {
    "nyc": {
        "high": {1: +1.5, 2: -0.5, 3: -3.0, 4: -1.0},  # season → offset (add to LGA fcst)
        "low":  {1: +2.0, 2: +1.0, 3: -1.5, 4: +0.5},
    },
}

# Explicit settlement/observation station overrides.
# These are intentionally hardcoded so the ingest and calibration paths can
# validate the same city/station contract even before DB-backed metadata exists.
STATION_SOURCE_OVERRIDES = {
    "nyc": {
        "settlement_icao": "KNYC",
        "observation_icao": "KNYC",
        "observation_source": "Central Park",
        "fallback_order": ["station_obs", "station_metadata", "bufkit", "model"],
    },
    "chi": {
        "settlement_icao": "KMDW",
        "observation_icao": "KMDW",
        "observation_source": "Chicago Midway",
        "fallback_order": ["station_obs", "station_metadata", "bufkit", "model"],
    },
}

DEFAULT_FALLBACK_ORDER = ["station_obs", "station_metadata", "bufkit", "model"]
DEFAULT_PHYSICS_BOUNDS = {
    "high": (-80.0, 140.0),
    "low": (-90.0, 135.0),
}
DEFAULT_SIGMA_FLOOR = 1.0
DEFAULT_SIGMA_CEILING = 20.0


def _season(dt: datetime) -> int:
    """Month to season: 1=DJF, 2=MAM, 3=JJA, 4=SON."""
    m = dt.month
    if m in (12, 1, 2): return 1
    if m in (3, 4, 5): return 2
    if m in (6, 7, 8): return 3
    return 4


def get_station_profile(city_key: str, as_of: Optional[datetime] = None) -> dict:
    """
    Resolve the active station profile for a city.

    This is intentionally lightweight: hardcoded overrides provide a stable
    bootstrap path, while station_metadata_versions offers an effective-dated
    history table for later expansion.
    """
    cities = _get_kalshi_cities()
    if city_key not in cities:
        raise KeyError(f"unknown city key: {city_key}")

    city = cities[city_key]
    override = STATION_SOURCE_OVERRIDES.get(city_key, {})
    profile = {
        "city": city_key,
        "city_name": city[0],
        "settlement_icao": override.get("settlement_icao", city[1]),
        "observation_icao": override.get("observation_icao", city[1]),
        "observation_source": override.get("observation_source", city[0]),
        "bufkit_icao": city[2],
        "cli_id": city[7],
        "wfo": city[8],
        "fallback_order": list(override.get("fallback_order", DEFAULT_FALLBACK_ORDER)),
        "effective_at": (as_of or datetime.now(timezone.utc)).isoformat(),
    }
    return profile


def validate_station_mappings() -> list[dict]:
    """
    Validate that the city map aligns with the settlement-station contract.

    Returns a list of mismatch dictionaries. Empty means the mapping set is
    internally consistent.
    """
    mismatches = []
    for city_key, city in _get_kalshi_cities().items():
        profile = get_station_profile(city_key)
        expected = profile["settlement_icao"]
        if city[1] != expected:
            mismatches.append({
                "city": city_key,
                "expected_settlement_icao": expected,
                "configured_settlement_icao": city[1],
            })
        if city_key in STATION_SOURCE_OVERRIDES and profile["observation_icao"] != expected:
            mismatches.append({
                "city": city_key,
                "expected_observation_icao": expected,
                "configured_observation_icao": profile["observation_icao"],
            })
    return mismatches


def get_fallback_order(variable: str = "high", city_key: Optional[str] = None) -> list[str]:
    """Return the source fallback order for a variable/city pair."""
    if city_key and city_key in STATION_SOURCE_OVERRIDES:
        return list(STATION_SOURCE_OVERRIDES[city_key].get("fallback_order", DEFAULT_FALLBACK_ORDER))
    if variable in {"high", "low"}:
        return list(DEFAULT_FALLBACK_ORDER)
    return ["station_obs", "model"]


def validate_model_forecasts(model_forecasts: dict[str, object]) -> list[str]:
    """
    Validate forecast payloads before calibration or verification writes.

    Missing or non-finite model values are treated as hard failures.
    """
    errors = []
    if not model_forecasts:
        return ["missing model forecast payload"]

    for model, value in model_forecasts.items():
        if value is None:
            errors.append(f"{model}: missing mandatory forecast value")
            continue
        try:
            fv = float(value)
        except Exception:
            errors.append(f"{model}: forecast value is not numeric")
            continue
        if not math.isfinite(fv):
            errors.append(f"{model}: forecast value is not finite")
    return errors


def validate_observation_timestamp(obs_time: Optional[datetime],
                                   as_of: Optional[datetime] = None,
                                   stale_after_minutes: int = 180) -> dict:
    """Detect stale observation timestamps during ingest."""
    as_of = as_of or datetime.now(timezone.utc)
    if obs_time is None:
        return {"is_stale": True, "reason": "missing_observation_time", "age_minutes": None}
    if obs_time.tzinfo is None:
        obs_time = obs_time.replace(tzinfo=timezone.utc)
    age_minutes = (as_of - obs_time).total_seconds() / 60.0
    is_stale = age_minutes > stale_after_minutes
    return {
        "is_stale": is_stale,
        "reason": "stale_observation" if is_stale else None,
        "age_minutes": round(age_minutes, 1),
    }


def normalize_utc_timestamp(value: datetime) -> datetime:
    """Enforce UTC-only storage for time columns."""
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def enforce_physics_bounds(mean_f: float, std_f: float,
                           variable: str = "high") -> tuple[float, float, list[str]]:
    """Clamp forecasts to sane physical bounds and keep variance positive."""
    lo, hi = DEFAULT_PHYSICS_BOUNDS.get(variable, (-100.0, 150.0))
    reasons = []
    bounded_mean = min(max(float(mean_f), lo), hi)
    if bounded_mean != float(mean_f):
        reasons.append("physics_bounds_clamped")

    bounded_std = max(float(std_f), DEFAULT_SIGMA_FLOOR)
    if not math.isfinite(bounded_std):
        bounded_std = DEFAULT_SIGMA_FLOOR
        reasons.append("nonfinite_variance_reset")
    if bounded_std < DEFAULT_SIGMA_FLOOR:
        reasons.append("variance_floor_applied")
    bounded_std = min(bounded_std, DEFAULT_SIGMA_CEILING)
    return round(bounded_mean, 1), round(bounded_std, 2), reasons


def enforce_high_low_consistency(high_f: Optional[float], low_f: Optional[float]) -> tuple[Optional[float], Optional[float], list[str]]:
    """Reject or repair impossible high/low relationships."""
    reasons = []
    if high_f is None or low_f is None:
        return high_f, low_f, reasons
    if low_f > high_f:
        reasons.append("high_low_inverted")
        midpoint = (high_f + low_f) / 2.0
        spread = max(abs(high_f - low_f) / 2.0, 1.0)
        high_f = midpoint + spread
        low_f = midpoint - spread
    return round(high_f, 1), round(low_f, 1), reasons


def robust_assimilate_observation(prior_mean: float, obs_value: Optional[float],
                                  obs_sigma: float = 2.0,
                                  huber_k: float = 1.5) -> dict:
    """
    Robustly assimilate an observation into a prior estimate.

    Returns the updated mean, a downweighted observation weight, and a reason.
    """
    if obs_value is None:
        return {"mean": round(prior_mean, 1), "weight": 0.0, "reason": "missing_observation"}
    residual = float(obs_value) - float(prior_mean)
    scale = max(float(obs_sigma), DEFAULT_SIGMA_FLOOR)
    threshold = huber_k * scale
    if abs(residual) <= threshold:
        weight = 1.0
        mean = float(obs_value)
        reason = "direct_assimilation"
    else:
        weight = max(0.1, threshold / max(abs(residual), 1e-9))
        mean = prior_mean + weight * residual
        reason = "huber_downweighted"
    return {"mean": round(mean, 1), "weight": round(weight, 3), "reason": reason}


def _lookup_sigma_hierarchy(city_key: str, variable: str,
                            target_date: datetime,
                            hour_bucket: Optional[int] = None) -> tuple[float, list[str]]:
    """
    Station/hour/month fallback hierarchy for sigma estimation.

    The hierarchy is:
      1. station-hour direct estimate
      2. station-month estimate
      3. model-family estimate
      4. global floor
    """
    target_date = normalize_utc_timestamp(target_date)
    month = target_date.month
    season = _season(target_date)
    fallback_reason_codes = []
    conn = sqlite3.connect(str(CALIB_DB))
    try:
        if hour_bucket is not None:
            row = conn.execute("""
                SELECT metric_value
                FROM calibration_metrics
                WHERE city = ? AND target_date = ? AND metric_name = ? AND model = ?
                ORDER BY created_at DESC
                LIMIT 1
            """, (city_key, target_date.strftime("%Y-%m-%d"), f"sigma_hour_{hour_bucket}", variable)).fetchone()
            if row and row[0] is not None:
                return max(float(row[0]), DEFAULT_SIGMA_FLOOR), ["sigma_station_hour"]

        row = conn.execute("""
            SELECT AVG(metric_value)
            FROM calibration_metrics
            WHERE city = ? AND metric_name = ? AND model = ?
              AND substr(target_date, 6, 2) = ?
        """, (city_key, "sigma_month", variable, f"{month:02d}")).fetchone()
        if row and row[0] is not None:
            fallback_reason_codes.append("sigma_station_month")
            return max(float(row[0]), DEFAULT_SIGMA_FLOOR), fallback_reason_codes

        row = conn.execute("""
            SELECT AVG(metric_value)
            FROM calibration_metrics
            WHERE metric_name = ? AND model = ?
        """, ("sigma_family", variable)).fetchone()
        if row and row[0] is not None:
            fallback_reason_codes.append("sigma_model_family")
            return max(float(row[0]), DEFAULT_SIGMA_FLOOR), fallback_reason_codes
    finally:
        conn.close()

    fallback_reason_codes.append(f"sigma_global_default_season_{season}")
    return 3.0, fallback_reason_codes


def _station_metadata_version(city_key: str, role: str,
                              target_date: Optional[datetime] = None) -> Optional[dict]:
    """Resolve the active effective-dated station metadata version."""
    target_date = normalize_utc_timestamp(target_date or datetime.now(timezone.utc))
    conn = sqlite3.connect(str(CALIB_DB))
    row = conn.execute("""
        SELECT station_icao, role, effective_from, effective_to, metadata_json
        FROM station_metadata_versions
        WHERE city = ? AND role = ?
          AND effective_from <= ?
          AND (effective_to IS NULL OR effective_to > ?)
        ORDER BY effective_from DESC
        LIMIT 1
    """, (city_key, role, target_date.isoformat(), target_date.isoformat())).fetchone()
    conn.close()
    if not row:
        return None
    station_icao, role, effective_from, effective_to, metadata_json = row
    try:
        metadata = json.loads(metadata_json or "{}")
    except Exception:
        metadata = {}
    return {
        "city": city_key,
        "role": role,
        "station_icao": station_icao,
        "effective_from": effective_from,
        "effective_to": effective_to,
        "metadata": metadata,
    }


def upsert_kalman_state(city_key: str, model: str, variable: str,
                        state_mean: float, state_var: float,
                        process_var: float, obs_var: float,
                        regime: Optional[str] = None) -> None:
    """Persist Kalman state per station and variable."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    conn.execute("""
        INSERT OR REPLACE INTO kalman_state
        (city, model, variable, state_mean, state_var, process_var, obs_var, regime, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        city_key, model, variable,
        float(state_mean), max(float(state_var), DEFAULT_SIGMA_FLOOR),
        max(float(process_var), DEFAULT_SIGMA_FLOOR),
        max(float(obs_var), DEFAULT_SIGMA_FLOOR),
        regime, datetime.now(timezone.utc).isoformat(),
    ))
    conn.commit()
    conn.close()


def load_kalman_state(city_key: str, model: str, variable: str) -> Optional[dict]:
    """Load the latest persisted Kalman state for a city/model/variable."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    row = conn.execute("""
        SELECT state_mean, state_var, process_var, obs_var, regime, updated_at
        FROM kalman_state
        WHERE city = ? AND model = ? AND variable = ?
        LIMIT 1
    """, (city_key, model, variable)).fetchone()
    conn.close()
    if not row:
        return None
    return {
        "state_mean": row[0],
        "state_var": row[1],
        "process_var": row[2],
        "obs_var": row[3],
        "regime": row[4],
        "updated_at": row[5],
    }


def record_calibration_run(city_key: str, target_date: datetime, mode: str,
                           payload: dict, status: str = "ok",
                           regime: Optional[str] = None,
                           fallback_reason_codes: Optional[list[str]] = None,
                           last_good: bool = False) -> None:
    """Persist a calibration run snapshot for replay/debugging."""
    _ensure_db()
    snapshot = dict(payload)
    dist = snapshot.pop("distribution", None)
    if isinstance(dist, CalibratedDistribution):
        snapshot["distribution_summary"] = {
            "mean": dist.mean,
            "std": dist.std,
            "skew": dist.skew,
            "n_models": dist.n_models,
            "n_forecasts": dist.n_forecasts,
        }
    conn = sqlite3.connect(str(CALIB_DB))
    if last_good:
        conn.execute("UPDATE calibration_runs SET is_last_good = 0 WHERE city = ?", (city_key,))
    conn.execute("""
        INSERT INTO calibration_runs
        (city, target_date, mode, regime, status, payload_json, fallback_reason_codes, is_last_good)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        city_key,
        normalize_utc_timestamp(target_date).strftime("%Y-%m-%d"),
        mode,
        regime,
        status,
        json.dumps(snapshot, sort_keys=True, default=str),
        json.dumps(fallback_reason_codes or []),
        1 if last_good else 0,
    ))
    conn.commit()
    conn.close()


def load_last_good_calibration(city_key: str) -> Optional[dict]:
    """Return the most recent last-good calibrated run for a city."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    row = conn.execute("""
        SELECT target_date, mode, regime, payload_json, fallback_reason_codes
        FROM calibration_runs
        WHERE city = ? AND status = 'ok'
        ORDER BY is_last_good DESC, target_date DESC, id DESC
        LIMIT 1
    """, (city_key,)).fetchone()
    conn.close()
    if not row:
        return None
    target_date, mode, regime, payload_json, fallback_reason_codes = row
    try:
        payload = json.loads(payload_json or "{}")
    except Exception:
        payload = {}
    try:
        reasons = json.loads(fallback_reason_codes or "[]")
    except Exception:
        reasons = []
    return {
        "city": city_key,
        "target_date": target_date,
        "mode": mode,
        "regime": regime,
        "payload": payload,
        "fallback_reason_codes": reasons,
    }


def record_calibration_metric(city_key: str, target_date: datetime, metric_name: str,
                              metric_value: float, baseline_value: Optional[float] = None,
                              regime: Optional[str] = None, model: Optional[str] = None) -> None:
    """Persist a calibration metric snapshot."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    conn.execute("""
        INSERT INTO calibration_metrics
        (city, target_date, metric_name, metric_value, baseline_value, regime, model)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    """, (
        city_key,
        normalize_utc_timestamp(target_date).strftime("%Y-%m-%d"),
        metric_name,
        float(metric_value),
        baseline_value,
        regime,
        model,
    ))
    conn.commit()
    conn.close()


def build_calibration_report_stub(city_key: str, target_date: datetime) -> dict:
    """Build a lightweight report object for CRPS/Brier/PIT/reliability tracking."""
    return {
        "city": city_key,
        "target_date": normalize_utc_timestamp(target_date).strftime("%Y-%m-%d"),
        "metrics": {
            "crps": None,
            "brier": None,
            "reliability": None,
            "pit": None,
            "sharpness": None,
        },
    }


def calibration_drift_alert(metric_name: str, current_value: float,
                            baseline_value: float, threshold_pct: float = 15.0) -> dict:
    """Simple drift alert helper for metric regressions."""
    if baseline_value == 0:
        return {"alert": False, "reason": "baseline_zero"}
    delta_pct = ((current_value - baseline_value) / baseline_value) * 100.0
    return {
        "alert": abs(delta_pct) >= threshold_pct,
        "metric_name": metric_name,
        "current_value": current_value,
        "baseline_value": baseline_value,
        "delta_pct": round(delta_pct, 2),
        "threshold_pct": threshold_pct,
        "reason": "metric_drift" if abs(delta_pct) >= threshold_pct else "within_bounds",
    }


def compute_payload_checksum(payload: object) -> str:
    """Checksum helper for downloaded artifacts or raw API payloads."""
    import hashlib
    blob = json.dumps(payload, sort_keys=True, default=str).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def validate_external_schema(payload: dict, required_fields: list[str],
                             optional_fields: Optional[list[str]] = None) -> list[str]:
    """Schema validation for external weather API responses."""
    optional_fields = optional_fields or []
    errors = []
    if not isinstance(payload, dict):
        return ["payload_is_not_object"]
    for field in required_fields:
        if field not in payload:
            errors.append(f"missing_required_field:{field}")
    for field in payload.keys():
        if field not in required_fields and field not in optional_fields:
            continue
    return errors


def persist_raw_payload_snapshot(source: str, payload: object, city_key: Optional[str] = None,
                                 payload_type: str = "api") -> dict:
    """Persist a replay/debug snapshot as a calibration run record."""
    _ensure_db()
    checksum = compute_payload_checksum(payload)
    snapshot = {
        "source": source,
        "city": city_key,
        "payload_type": payload_type,
        "checksum": checksum,
        "payload": payload,
    }
    record_calibration_run(
        city_key or "__global__",
        datetime.now(timezone.utc),
        mode="snapshot",
        payload=snapshot,
        status="ok",
        regime=None,
        fallback_reason_codes=["raw_payload_snapshot"],
        last_good=False,
    )
    return snapshot


def detect_duplicate_verification_rows(city_key: str, date_str: str) -> list[dict]:
    """Duplicate-row detection helper for verification tables."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    rows = conn.execute("""
        SELECT model, cycle, COUNT(*) AS n
        FROM verification
        WHERE city = ? AND date = ?
        GROUP BY model, cycle
        HAVING COUNT(*) > 1
    """, (city_key, date_str)).fetchall()
    conn.close()
    return [{"model": model, "cycle": cycle, "count": n} for model, cycle, n in rows]


def dedupe_verification_rows(city_key: Optional[str] = None) -> int:
    """Dedupe job that removes duplicate verification rows in-place."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    params = []
    city_clause = ""
    if city_key:
        city_clause = "WHERE city = ?"
        params.append(city_key)
    duplicate_ids = conn.execute(f"""
        SELECT v1.id
        FROM verification v1
        JOIN verification v2
          ON v1.city = v2.city AND v1.date = v2.date AND v1.model = v2.model AND v1.cycle = v2.cycle
         AND v1.id < v2.id
        {city_clause.replace("city", "v1.city")}
    """, params).fetchall()
    deleted = 0
    if duplicate_ids:
        ids = [str(row[0]) for row in duplicate_ids]
        conn.execute(f"DELETE FROM verification WHERE id IN ({','.join('?' for _ in ids)})", ids)
        deleted = len(ids)
    conn.commit()
    conn.close()
    return deleted


def build_missing_data_daily_report(date_str: str, city_key: Optional[str] = None) -> dict:
    """Per-station missing-data daily report."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    params = [date_str]
    city_clause = ""
    if city_key:
        city_clause = "AND city = ?"
        params.append(city_key)
    rows = conn.execute(f"""
        SELECT city,
               SUM(CASE WHEN obs_high_f IS NULL THEN 1 ELSE 0 END) AS missing_obs,
               SUM(CASE WHEN fcst_high_f IS NULL THEN 1 ELSE 0 END) AS missing_fcst,
               COUNT(*) AS total_rows
        FROM verification
        WHERE date = ? {city_clause}
        GROUP BY city
        ORDER BY city
    """, params).fetchall()
    conn.close()
    return {
        "date": date_str,
        "cities": [
            {
                "city": city,
                "missing_obs": int(missing_obs or 0),
                "missing_fcst": int(missing_fcst or 0),
                "total_rows": int(total_rows or 0),
            }
            for city, missing_obs, missing_fcst, total_rows in rows
        ],
    }


def dry_run_calibration_preview(current: dict, proposed: dict) -> dict:
    """In-memory diff preview for dry-run calibration changes."""
    keys = sorted(set(current.keys()) | set(proposed.keys()))
    diff = {}
    for key in keys:
        if current.get(key) != proposed.get(key):
            diff[key] = {"current": current.get(key), "proposed": proposed.get(key)}
    return {
        "changed_fields": sorted(diff.keys()),
        "diff": diff,
        "n_changes": len(diff),
    }


def make_idempotency_key(city_key: str, target_date: datetime, model: str, variable: str) -> str:
    """Deterministic idempotency key for ingest write operations."""
    return compute_payload_checksum({
        "city": city_key,
        "target_date": normalize_utc_timestamp(target_date).isoformat(),
        "model": model,
        "variable": variable,
    })


class IngestionCircuitBreaker:
    """Tiny circuit breaker for repeated provider failures."""

    def __init__(self, failure_threshold: int = 3, reset_after_seconds: int = 300):
        self.failure_threshold = failure_threshold
        self.reset_after_seconds = reset_after_seconds
        self.failure_count = 0
        self.opened_at: Optional[datetime] = None

    def allow(self) -> bool:
        if self.opened_at is None:
            return True
        elapsed = (datetime.now(timezone.utc) - self.opened_at).total_seconds()
        if elapsed >= self.reset_after_seconds:
            self.failure_count = 0
            self.opened_at = None
            return True
        return False

    def record_success(self) -> None:
        self.failure_count = 0
        self.opened_at = None

    def record_failure(self) -> bool:
        self.failure_count += 1
        if self.failure_count >= self.failure_threshold:
            self.opened_at = datetime.now(timezone.utc)
            return False
        return True


def retry_with_backoff(func, *args, retries: int = 3, base_delay: float = 0.5,
                       max_delay: float = 5.0, jitter: float = 0.2, **kwargs):
    """Retry helper with capped exponential backoff and jitter."""
    last_exc = None
    for attempt in range(retries + 1):
        try:
            return func(*args, **kwargs)
        except Exception as exc:
            last_exc = exc
            if attempt >= retries:
                raise
            delay = min(max_delay, base_delay * (2 ** attempt))
            delay += min(jitter, delay * 0.5)
            time.sleep(delay)
    if last_exc:
        raise last_exc


def decompose_confidence(city_key: str, n_models: int, spread: float,
                         regime: Optional[str] = None,
                         stale_obs: bool = False,
                         conservative: bool = False,
                         aggressive: bool = False) -> dict:
    """Deterministic confidence decomposition used in output explainability."""
    model_component = min(1.0, n_models / 4.0)
    spread_component = max(0.0, 1.0 - min(spread / 12.0, 1.0))
    regime_component = 0.9 if regime in {"stable", "marine"} else (0.7 if regime else 0.8)
    data_component = 0.6 if stale_obs else 1.0
    score = model_component * 0.35 + spread_component * 0.30 + regime_component * 0.20 + data_component * 0.15
    if conservative:
        score *= 0.92
    if aggressive:
        score *= 1.05
    score = max(0.0, min(score, 1.0))
    return {
        "city": city_key,
        "data": round(data_component, 3),
        "model": round(model_component, 3),
        "regime": round(regime_component, 3),
        "spread": round(spread_component, 3),
        "score": round(score, 3),
    }


def build_why_object(city_key: str, target_date: datetime, mode: str,
                     regime: Optional[str], fallback_reason_codes: list[str],
                     confidence: dict, physics_reasons: list[str],
                     model_weights: dict[str, float],
                     feature_importance: Optional[dict[str, float]] = None) -> dict:
    """Deterministic explanation object for a forecast/calibration result."""
    return {
        "city": city_key,
        "target_date": normalize_utc_timestamp(target_date).strftime("%Y-%m-%d"),
        "mode": mode,
        "regime": regime,
        "fallback_reason_codes": list(fallback_reason_codes),
        "confidence": confidence,
        "physics_reasons": list(physics_reasons),
        "model_weights": dict(sorted(model_weights.items())),
        "feature_importance": feature_importance or {},
    }


def validate_emos_coefficients(a: float, b: float, c: float, d: float) -> tuple[bool, list[str]]:
    """Guard against unstable EMOS coefficient sets before deployment."""
    reasons = []
    if not all(math.isfinite(v) for v in (a, b, c, d)):
        reasons.append("nonfinite_coefficients")
    if b < 0.0 or b > 2.0:
        reasons.append("slope_out_of_bounds")
    if c < 0.0 or c > 50.0:
        reasons.append("variance_intercept_out_of_bounds")
    if d < 0.0 or d > 10.0:
        reasons.append("variance_slope_out_of_bounds")
    return (len(reasons) == 0), reasons


def validate_calibration_inputs(model_highs: dict[str, object]) -> list[str]:
    """Reject runs with missing mandatory model fields or impossible values."""
    errors = validate_model_forecasts(model_highs)
    for model, value in model_highs.items():
        if value is None:
            continue
        try:
            fv = float(value)
        except Exception:
            continue
        if fv < -120 or fv > 160:
            errors.append(f"{model}: physically impossible trajectory state")
    return errors


def _verification_date(value: str) -> datetime:
    return datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)


def gaussian_crps(obs: float, mu: float, sigma: float) -> float:
    """Closed-form CRPS for a Gaussian predictive distribution."""
    sigma = max(float(sigma), 1e-6)
    z = (float(obs) - float(mu)) / sigma
    phi_z = 0.5 * (1 + math.erf(z / math.sqrt(2)))
    pdf_z = math.exp(-0.5 * z * z) / math.sqrt(2 * math.pi)
    return sigma * (z * (2 * phi_z - 1) + 2 * pdf_z - 1 / math.sqrt(math.pi))


def _normal_cdf(x: float, mu: float, sigma: float) -> float:
    sigma = max(float(sigma), 1e-6)
    z = (float(x) - float(mu)) / sigma
    return max(0.0, min(1.0, 0.5 * (1 + math.erf(z / math.sqrt(2)))))


def _emos_mu_sigma(params: dict[str, float], ens_mean: float, ens_spread: float) -> tuple[float, float]:
    mu = float(params["a"]) + float(params["b"]) * float(ens_mean)
    var = max(float(params["c"]) + float(params["d"]) * max(float(ens_spread), 0.0), 0.25)
    return mu, math.sqrt(var)


def _params_dict(row_or_params: Optional[object]) -> Optional[dict[str, float]]:
    if row_or_params is None:
        return None
    if isinstance(row_or_params, dict):
        return {
            "a": float(row_or_params["a"]),
            "b": float(row_or_params["b"]),
            "c": float(row_or_params["c"]),
            "d": float(row_or_params["d"]),
            "n_train": int(row_or_params.get("n_train", 0)),
            "crps_train": float(row_or_params.get("crps_train", 0.0)),
        }
    row = list(row_or_params)
    return {
        "a": float(row[0]),
        "b": float(row[1]),
        "c": float(row[2]),
        "d": float(row[3]),
        "n_train": int(row[4]) if len(row) > 4 and row[4] is not None else 0,
        "crps_train": float(row[5]) if len(row) > 5 and row[5] is not None else 0.0,
    }


def _load_emos_param_dict(city_key: str, param: str = "high") -> Optional[dict[str, float]]:
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    row = conn.execute("""
        SELECT a, b, c, d, n_train, crps_train
        FROM emos_params
        WHERE city = ? AND param = ?
    """, (city_key, param)).fetchone()
    conn.close()
    return _params_dict(row)


def _store_emos_params(city_key: str, param: str, candidate: dict[str, float],
                       valid_from: Optional[datetime] = None) -> None:
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    conn.execute("""
        INSERT OR REPLACE INTO emos_params
        (city, param, a, b, c, d, n_train, crps_train, valid_from, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        city_key,
        param,
        float(candidate["a"]),
        float(candidate["b"]),
        max(float(candidate["c"]), 0.25),
        max(float(candidate["d"]), 0.0),
        int(candidate.get("n_train", 0)),
        float(candidate.get("crps_train", 0.0)),
        normalize_utc_timestamp(valid_from).isoformat() if valid_from else None,
        datetime.now(timezone.utc).isoformat(),
    ))
    conn.commit()
    conn.close()


def _record_emos_training_run(city_key: str, param: str, training_mode: str,
                              candidate: Optional[dict[str, float]],
                              stability: dict, *, target_date: Optional[datetime],
                              window_days: Optional[int], season: Optional[int],
                              deployed: bool) -> None:
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    conn.execute("""
        INSERT INTO emos_training_runs
        (city, param, training_mode, target_date, window_days, season, n_train,
         crps_train, candidate_json, stability_json, deployed)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        city_key,
        param,
        training_mode,
        normalize_utc_timestamp(target_date).strftime("%Y-%m-%d") if target_date else None,
        window_days,
        season,
        int((candidate or {}).get("n_train", 0)),
        (candidate or {}).get("crps_train"),
        json.dumps(candidate or {}, sort_keys=True),
        json.dumps(stability, sort_keys=True),
        1 if deployed else 0,
    ))
    conn.commit()
    conn.close()


def build_emos_training_data(city_key: Optional[str] = None, *,
                             param: str = "high",
                             start_date: Optional[datetime] = None,
                             end_date: Optional[datetime] = None,
                             season: Optional[int] = None) -> list[tuple[float, float, float]]:
    """
    Build EMOS training rows from verification data.

    Each row is (ensemble_mean, ensemble_spread, observed_value), grouped by
    city/date from all non-CLI model verification rows.
    """
    _ensure_db()
    obs_col = "obs_high_f" if param == "high" else "obs_low_f"
    fcst_col = "fcst_high_f" if param == "high" else "fcst_low_f"
    where = [
        "model != 'cli'",
        f"{obs_col} IS NOT NULL",
        f"{fcst_col} IS NOT NULL",
    ]
    params: list[object] = []
    if city_key and city_key != GLOBAL_EMOS_CITY:
        where.append("city = ?")
        params.append(city_key)
    if start_date is not None:
        where.append("date >= ?")
        params.append(normalize_utc_timestamp(start_date).strftime("%Y-%m-%d"))
    if end_date is not None:
        where.append("date < ?")
        params.append(normalize_utc_timestamp(end_date).strftime("%Y-%m-%d"))

    conn = sqlite3.connect(str(CALIB_DB))
    rows = conn.execute(f"""
        SELECT city, date, {obs_col}, {fcst_col}
        FROM verification
        WHERE {' AND '.join(where)}
        ORDER BY city, date
    """, params).fetchall()
    conn.close()

    grouped: dict[tuple[str, str], dict[str, object]] = {}
    for city, date_str, obs, fcst in rows:
        dt = _verification_date(str(date_str))
        if season is not None and _season(dt) != int(season):
            continue
        key = (str(city), str(date_str))
        if key not in grouped:
            grouped[key] = {"obs": float(obs), "fcst": []}
        grouped[key]["fcst"].append(float(fcst))

    data: list[tuple[float, float, float]] = []
    for item in grouped.values():
        temps = list(item["fcst"])
        if len(temps) < 2:
            continue
        ens_mean = sum(temps) / len(temps)
        var = sum((temp - ens_mean) ** 2 for temp in temps) / len(temps)
        ens_spread = max(math.sqrt(var), 1.0)
        data.append((ens_mean, ens_spread, float(item["obs"])))
    return data


def evaluate_emos_params(params: dict[str, float],
                         training_data: list[tuple[float, float, float]]) -> float:
    """Mean CRPS for EMOS coefficients over training or validation rows."""
    if not training_data:
        return math.inf
    total = 0.0
    for ens_mean, ens_spread, obs in training_data:
        mu, sigma = _emos_mu_sigma(params, ens_mean, ens_spread)
        total += gaussian_crps(obs, mu, sigma)
    return total / len(training_data)


def fit_emos_params(training_data: list[tuple[float, float, float]],
                    *, min_samples: int = 10) -> Optional[dict[str, float]]:
    """Fit EMOS coefficients by minimizing Gaussian CRPS."""
    if len(training_data) < min_samples:
        return None

    def objective(values):
        a, b, c, d = values
        candidate = {"a": a, "b": b, "c": max(c, 0.25), "d": max(d, 0.0)}
        return evaluate_emos_params(candidate, training_data)

    best = _nelder_mead(objective, [0.0, 1.0, 4.0, 0.5],
                        step=[5.0, 0.3, 3.0, 0.3], max_iter=500, tol=1e-6)
    if best is None:
        return None
    a, b, c, d = best
    candidate = {
        "a": float(a),
        "b": float(b),
        "c": max(float(c), 0.25),
        "d": max(float(d), 0.0),
        "n_train": len(training_data),
    }
    candidate["crps_train"] = evaluate_emos_params(candidate, training_data)
    return candidate


def coefficient_stability_gate(candidate: Optional[dict[str, float]],
                               previous: Optional[object] = None,
                               *, min_samples: int = 10,
                               max_intercept_delta: float = 25.0,
                               max_slope_delta: float = 0.75,
                               max_variance_delta: float = 50.0,
                               max_crps_regression: float = 0.25) -> dict:
    """Deployment gate for EMOS coefficient stability."""
    if candidate is None:
        return {"ok": False, "reasons": ["insufficient_training_data"], "deltas": {}}

    reasons = []
    stable, coefficient_reasons = validate_emos_coefficients(
        candidate["a"], candidate["b"], candidate["c"], candidate["d"]
    )
    if not stable:
        reasons.extend(coefficient_reasons)
    if int(candidate.get("n_train", 0)) < min_samples:
        reasons.append("n_train_below_minimum")
    if not math.isfinite(float(candidate.get("crps_train", math.inf))):
        reasons.append("crps_not_finite")

    previous_params = _params_dict(previous)
    deltas = {}
    if previous_params:
        deltas = {
            "a": round(abs(candidate["a"] - previous_params["a"]), 6),
            "b": round(abs(candidate["b"] - previous_params["b"]), 6),
            "c": round(abs(candidate["c"] - previous_params["c"]), 6),
            "d": round(abs(candidate["d"] - previous_params["d"]), 6),
        }
        if deltas["a"] > max_intercept_delta:
            reasons.append("intercept_delta_too_large")
        if deltas["b"] > max_slope_delta:
            reasons.append("slope_delta_too_large")
        if max(deltas["c"], deltas["d"]) > max_variance_delta:
            reasons.append("variance_delta_too_large")
        prev_crps = previous_params.get("crps_train")
        if prev_crps and candidate.get("crps_train", 0.0) > prev_crps * (1.0 + max_crps_regression):
            reasons.append("crps_regression")

    return {
        "ok": not reasons,
        "reasons": reasons or ["stable"],
        "deltas": deltas,
        "min_samples": min_samples,
    }


def train_emos_rolling(city_key: str, *, param: str = "high",
                       target_date: Optional[datetime] = None,
                       window_days: int = 90,
                       deploy: bool = True,
                       global_fit: bool = False,
                       min_samples: int = 10) -> dict:
    """Train EMOS coefficients from a rolling verification window."""
    target = normalize_utc_timestamp(target_date or datetime.now(timezone.utc))
    start = target - timedelta(days=int(window_days))
    fit_city = GLOBAL_EMOS_CITY if global_fit else city_key
    data = build_emos_training_data(
        None if global_fit else city_key,
        param=param,
        start_date=start,
        end_date=target + timedelta(days=1),
    )
    candidate = fit_emos_params(data, min_samples=min_samples)
    previous = _load_emos_param_dict(fit_city, param)
    gate = coefficient_stability_gate(candidate, previous, min_samples=min_samples)
    deployed = bool(deploy and gate["ok"] and candidate)
    if deployed:
        _store_emos_params(fit_city, param, candidate, valid_from=target)
    _record_emos_training_run(
        fit_city, param, "rolling", candidate, gate,
        target_date=target, window_days=window_days, season=None, deployed=deployed,
    )
    return {
        "city": fit_city,
        "param": param,
        "training_mode": "rolling",
        "target_date": target.strftime("%Y-%m-%d"),
        "window_days": int(window_days),
        "n_train": len(data),
        "candidate": candidate,
        "stability": gate,
        "deployed": deployed,
    }


def train_emos_season_matched(city_key: str, *, param: str = "high",
                              target_date: Optional[datetime] = None,
                              lookback_years: int = 5,
                              deploy: bool = True,
                              global_fit: bool = False,
                              min_samples: int = 10) -> dict:
    """Train EMOS coefficients from season-matched historical verification rows."""
    target = normalize_utc_timestamp(target_date or datetime.now(timezone.utc))
    season = _season(target)
    start = target - timedelta(days=365 * max(1, int(lookback_years)))
    fit_city = GLOBAL_EMOS_CITY if global_fit else city_key
    data = build_emos_training_data(
        None if global_fit else city_key,
        param=param,
        start_date=start,
        end_date=target + timedelta(days=1),
        season=season,
    )
    candidate = fit_emos_params(data, min_samples=min_samples)
    previous = _load_emos_param_dict(fit_city, param)
    gate = coefficient_stability_gate(candidate, previous, min_samples=min_samples)
    deployed = bool(deploy and gate["ok"] and candidate)
    if deployed:
        _store_emos_params(fit_city, param, candidate, valid_from=target)
    _record_emos_training_run(
        fit_city, param, "season_matched", candidate, gate,
        target_date=target, window_days=365 * max(1, int(lookback_years)),
        season=season, deployed=deployed,
    )
    return {
        "city": fit_city,
        "param": param,
        "training_mode": "season_matched",
        "target_date": target.strftime("%Y-%m-%d"),
        "lookback_years": int(lookback_years),
        "season": season,
        "n_train": len(data),
        "candidate": candidate,
        "stability": gate,
        "deployed": deployed,
    }


def tune_pooled_station_shrinkage(city_key: str, *, param: str = "high",
                                  target_date: Optional[datetime] = None,
                                  window_days: int = 180,
                                  alphas: Optional[list[float]] = None,
                                  deploy: bool = False,
                                  min_samples: int = 10) -> dict:
    """
    Tune pooled-vs-station EMOS coefficient shrinkage.

    alpha=1.0 means station-only coefficients; alpha=0.0 means pooled/global.
    """
    target = normalize_utc_timestamp(target_date or datetime.now(timezone.utc))
    start = target - timedelta(days=int(window_days))
    station_data = build_emos_training_data(
        city_key, param=param, start_date=start, end_date=target + timedelta(days=1)
    )
    pooled_data = build_emos_training_data(
        None, param=param, start_date=start, end_date=target + timedelta(days=1)
    )
    station_fit = fit_emos_params(station_data, min_samples=min_samples)
    pooled_fit = fit_emos_params(pooled_data, min_samples=min_samples)
    if station_fit is None or pooled_fit is None:
        return {
            "city": city_key,
            "param": param,
            "ok": False,
            "reason": "insufficient_training_data",
            "station_n": len(station_data),
            "pooled_n": len(pooled_data),
        }

    grid = alphas if alphas is not None else [i / 10.0 for i in range(11)]
    scores = []
    best = None
    for alpha in grid:
        alpha = max(0.0, min(1.0, float(alpha)))
        params = {
            key: alpha * station_fit[key] + (1.0 - alpha) * pooled_fit[key]
            for key in ("a", "b", "c", "d")
        }
        params["n_train"] = len(station_data)
        params["crps_train"] = evaluate_emos_params(params, station_data)
        row = {"alpha": round(alpha, 4), "crps": params["crps_train"], "params": params}
        scores.append(row)
        if best is None or row["crps"] < best["crps"]:
            best = row

    recommended = dict(best["params"])
    previous = _load_emos_param_dict(city_key, param)
    gate = coefficient_stability_gate(recommended, previous, min_samples=min_samples)
    deployed = bool(deploy and gate["ok"])
    if deployed:
        _store_emos_params(city_key, param, recommended, valid_from=target)
    _record_emos_training_run(
        city_key, param, "shrinkage", recommended, gate,
        target_date=target, window_days=window_days, season=None, deployed=deployed,
    )
    return {
        "city": city_key,
        "param": param,
        "ok": True,
        "target_date": target.strftime("%Y-%m-%d"),
        "window_days": int(window_days),
        "station_n": len(station_data),
        "pooled_n": len(pooled_data),
        "best_alpha": best["alpha"],
        "best_crps": round(best["crps"], 6),
        "recommended_params": recommended,
        "grid": [
            {"alpha": row["alpha"], "crps": round(row["crps"], 6)}
            for row in scores
        ],
        "stability": gate,
        "deployed": deployed,
    }


def build_pit_histogram(city_key: Optional[str] = None, *, param: str = "high",
                        start_date: Optional[datetime] = None,
                        end_date: Optional[datetime] = None,
                        bins: int = 10,
                        params: Optional[dict[str, float]] = None) -> dict:
    """Generate a PIT histogram from verification rows and active EMOS parameters."""
    bins = max(2, min(int(bins), 50))
    fit_city = city_key or GLOBAL_EMOS_CITY
    active_params = params or _load_emos_param_dict(fit_city, param)
    if active_params is None and city_key:
        active_params = _load_emos_param_dict(GLOBAL_EMOS_CITY, param)
    data = build_emos_training_data(
        city_key, param=param, start_date=start_date, end_date=end_date
    )
    counts = [0 for _ in range(bins)]
    pit_values = []
    if active_params:
        for ens_mean, ens_spread, obs in data:
            mu, sigma = _emos_mu_sigma(active_params, ens_mean, ens_spread)
            pit = _normal_cdf(obs, mu, sigma)
            pit_values.append(pit)
            idx = min(bins - 1, int(pit * bins))
            counts[idx] += 1

    n = len(pit_values)
    expected = n / bins if n else 0.0
    chi_square = (
        sum(((count - expected) ** 2) / expected for count in counts)
        if expected > 0 else None
    )
    edge_mass = counts[0] + counts[-1] if counts else 0
    middle_mass = sum(counts[1:-1]) if len(counts) > 2 else 0
    if n == 0:
        shape = "empty"
    elif edge_mass > middle_mass:
        shape = "u_shaped"
    elif expected and max(counts) > expected * 2.0:
        shape = "peaked"
    else:
        shape = "roughly_uniform"

    histogram = {
        "city": city_key,
        "param": param,
        "bins": [
            {
                "lo": round(i / bins, 4),
                "hi": round((i + 1) / bins, 4),
                "count": count,
                "expected": round(expected, 3),
            }
            for i, count in enumerate(counts)
        ],
        "n": n,
        "mean_pit": round(sum(pit_values) / n, 4) if n else None,
        "chi_square": round(chi_square, 4) if chi_square is not None else None,
        "shape": shape,
        "has_params": active_params is not None,
    }
    if city_key and n:
        record_calibration_metric(
            city_key,
            end_date or datetime.now(timezone.utc),
            "pit_chi_square",
            histogram["chi_square"] or 0.0,
            model=param,
        )
    return histogram


def _increment_ingest_counter(source: str, status: str,
                              model: Optional[str] = None,
                              city: Optional[str] = None) -> None:
    """Increment per-model/per-source ingest counters."""
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    conn.execute("""
        INSERT INTO ingest_counters (source, model, city, status, counter, updated_at)
        VALUES (?, ?, ?, ?, 1, ?)
        ON CONFLICT(source, model, city, status)
        DO UPDATE SET counter = counter + 1, updated_at = excluded.updated_at
    """, (source, model, city, status, datetime.now(timezone.utc).isoformat()))
    conn.commit()
    conn.close()



# ══════════════════════════════════════════════════════════════════════
#  BIAS CORRECTION
# ══════════════════════════════════════════════════════════════════════

@dataclass
class BiasCorrector:
    """
    Per-model per-city bias correction with exponential decay.

    Combines:
      1. Prior bias from literature (PRIOR_BIASES)
      2. Running verification-based bias (exponential moving average)
      3. Station transfer offset (BUFKIT proxy → settlement station)
      4. ASOS sensor dampening correction
    """
    city: str
    model: str
    param: str = "high"  # 'high' or 'low'
    alpha: float = 0.15  # EMA decay rate (higher = more weight on recent obs)

    def __post_init__(self):
        _ensure_db()

    def get_prior_bias(self, target_date: datetime) -> float:
        """Get literature-based prior bias for this model/season."""
        season = _season(target_date)
        return PRIOR_BIASES.get((self.model, season), 0.0)

    def get_db_prior_bias(self, target_date: datetime) -> tuple[float, str]:
        """
        Learned / DB-backed prior bias fallback hierarchy.

        Hierarchy:
          1. recent verification-based model bias
          2. station transfer offset
          3. literature prior
        """
        running, n_obs = self.get_running_bias()
        station = self.get_station_offset(target_date)
        prior = self.get_prior_bias(target_date)
        if n_obs >= 5:
            return running, "db_running_bias"
        if station != 0.0:
            return station, "station_offset_prior"
        return prior, "literature_prior"

    def get_station_offset(self, target_date: datetime) -> float:
        """Get station transfer offset (BUFKIT proxy → settlement)."""
        if self.city not in STATION_OFFSETS:
            return 0.0
        offsets = STATION_OFFSETS[self.city].get(self.param, {})
        season = _season(target_date)
        return offsets.get(season, 0.0)

    def get_running_bias(self) -> tuple[float, int]:
        """Get exponentially-weighted running bias from verification DB."""
        conn = sqlite3.connect(str(CALIB_DB))
        col = "bias_high" if self.param == "high" else "bias_low"
        rows = conn.execute(f"""
            SELECT {col}, date FROM verification
            WHERE city = ? AND model = ? AND {col} IS NOT NULL
            ORDER BY date DESC LIMIT 60
        """, (self.city, self.model)).fetchall()
        conn.close()

        if not rows:
            return 0.0, 0

        # Exponential decay weighted average
        total_w = 0.0
        total_b = 0.0
        for i, (bias, _) in enumerate(rows):
            w = (1 - self.alpha) ** i
            total_w += w
            total_b += bias * w

        return total_b / total_w if total_w > 0 else 0.0, len(rows)

    def correct(self, raw_forecast_f: float, target_date: datetime) -> float:
        """
        Apply full bias correction chain to a raw model forecast.

        Returns corrected temperature in °F.
        """
        prior, prior_source = self.get_db_prior_bias(target_date)
        station = self.get_station_offset(target_date)
        running, n_obs = self.get_running_bias()

        # Blend prior and running bias based on sample size
        # With 0 obs, use 100% prior. With 30+ obs, use ~90% running.
        blend = min(n_obs / 30.0, 0.9)
        effective_bias = (1 - blend) * prior + blend * running

        # Correct: subtract model bias, add station offset, add ASOS correction
        corrected = raw_forecast_f - effective_bias + station
        if self.param == "high":
            corrected -= ASOS_DAMPENING_BIAS  # ASOS understates max

        return round(corrected, 1)

    def ingest_counter_key(self) -> tuple[str, str, str, str]:
        return ("bias_corrector", self.model, self.city, self.param)

    def update(self, fcst_f: float, obs_f: float, date: str):
        """Record a verification pair and update running bias."""
        bias = fcst_f - obs_f
        conn = sqlite3.connect(str(CALIB_DB))

        # Update bias_tracker with EMA
        row = conn.execute("""
            SELECT running_bias, running_mae, running_rmse, n_obs
            FROM bias_tracker WHERE city=? AND model=? AND param=?
        """, (self.city, self.model, self.param)).fetchone()

        if row:
            old_bias, old_mae, old_rmse, n = row
            new_bias = self.alpha * bias + (1 - self.alpha) * old_bias
            new_mae = self.alpha * abs(bias) + (1 - self.alpha) * old_mae
            new_rmse = math.sqrt(self.alpha * bias**2 + (1 - self.alpha) * old_rmse**2)
            conn.execute("""
                UPDATE bias_tracker SET running_bias=?, running_mae=?, running_rmse=?,
                n_obs=?, last_updated=? WHERE city=? AND model=? AND param=?
            """, (new_bias, new_mae, new_rmse, n+1, datetime.now(timezone.utc).isoformat(),
                  self.city, self.model, self.param))
        else:
            conn.execute("""
                INSERT INTO bias_tracker (city, model, param, running_bias, running_mae,
                running_rmse, n_obs, last_updated)
                VALUES (?, ?, ?, ?, ?, ?, 1, ?)
            """, (self.city, self.model, self.param, bias, abs(bias),
                  abs(bias), datetime.now(timezone.utc).isoformat()))

        conn.commit()
        conn.close()
        _increment_ingest_counter("verification", "success", model=self.model, city=self.city)


# ══════════════════════════════════════════════════════════════════════
#  EMOS CALIBRATION (Gneiting et al. 2005)
# ══════════════════════════════════════════════════════════════════════

@dataclass
class EMOSCalibrator:
    """
    Ensemble Model Output Statistics — nonhomogeneous Gaussian regression.

    Given ensemble mean μ_ens and spread σ_ens, produces a calibrated
    predictive distribution N(a + b·μ_ens, c + d·σ_ens) where parameters
    (a, b, c, d) are fit by minimizing CRPS over a training window.

    When training data is insufficient, falls back to bias-corrected
    Gaussian with inflated spread.
    """
    city: str
    param: str = "high"
    train_window_days: int = 45  # rolling training window

    def __post_init__(self):
        _ensure_db()

    def _load_params(self, city_key: str) -> Optional[tuple]:
        """Load stored EMOS parameters."""
        conn = sqlite3.connect(str(CALIB_DB))
        row = conn.execute("""
            SELECT a, b, c, d, n_train, crps_train
            FROM emos_params WHERE city=? AND param=?
        """, (city_key, self.param)).fetchone()
        conn.close()
        return row

    def get_params(self) -> Optional[tuple]:
        return self._load_params(self.city)

    def get_sigma(self, target_date: datetime, hour_bucket: Optional[int] = None) -> tuple[float, list[str]]:
        """Station-hour sigma lookup hierarchy with deterministic fallbacks."""
        return _lookup_sigma_hierarchy(self.city, self.param, target_date, hour_bucket=hour_bucket)

    def _build_training_data(self, city_key: Optional[str] = None) -> list[tuple[float, float, float]]:
        """
        Build (ensemble_mean, ensemble_spread, observation) rows from verification DB.
        If city_key is None, pools across all cities for global fallback fitting.
        """
        cutoff = (datetime.now(timezone.utc) - timedelta(days=self.train_window_days)).strftime("%Y-%m-%d")
        obs_col = "obs_high_f" if self.param == "high" else "obs_low_f"
        fcst_col = "fcst_high_f" if self.param == "high" else "fcst_low_f"

        conn = sqlite3.connect(str(CALIB_DB))
        if city_key is None:
            rows = conn.execute(f"""
                SELECT city, date, {obs_col}, {fcst_col}
                FROM verification
                WHERE date >= ? AND model != 'cli'
                  AND {obs_col} IS NOT NULL AND {fcst_col} IS NOT NULL
                ORDER BY city, date
            """, (cutoff,)).fetchall()
        else:
            rows = conn.execute(f"""
                SELECT city, date, {obs_col}, {fcst_col}
                FROM verification
                WHERE city = ? AND date >= ? AND model != 'cli'
                  AND {obs_col} IS NOT NULL AND {fcst_col} IS NOT NULL
                ORDER BY date
            """, (city_key, cutoff)).fetchall()
        conn.close()

        grouped: dict[tuple[str, str], dict[str, list[float] | float]] = {}
        for city, date_str, obs, fcst in rows:
            key = (str(city), str(date_str))
            if key not in grouped:
                grouped[key] = {"obs": float(obs), "fcst": []}
            grouped[key]["fcst"].append(float(fcst))

        train_data: list[tuple[float, float, float]] = []
        for item in grouped.values():
            temps = item["fcst"]
            if len(temps) < 2:
                continue
            ens_mean = sum(temps) / len(temps)
            var = sum((t - ens_mean) ** 2 for t in temps) / max(len(temps), 1)
            ens_spread = max(math.sqrt(var), 1.0)
            train_data.append((ens_mean, ens_spread, float(item["obs"])))
        return train_data

    def calibrate(self, ensemble_mean: float, ensemble_spread: float,
                  target_date: datetime, *, mode: str = "normal",
                  regime: Optional[str] = None,
                  conservative: bool = False,
                  aggressive: bool = False) -> tuple[float, float]:
        """
        Produce calibrated (mean, std) from raw ensemble statistics.

        Returns (calibrated_mean_f, calibrated_std_f).
        """
        params = self.get_params()
        if not (params and params[4] >= 10):
            # Fall back to globally pooled parameters when per-city history is sparse.
            params = self._load_params(GLOBAL_EMOS_CITY)

        if params and params[4] >= 10:
            a, b, c, d, n_train, crps = params
            # Guardrails: tiny-sample fits can produce unstable coefficients.
            b = min(max(float(b), 0.6), 1.4)
            c = min(max(float(c), 0.25), 36.0)
            d = min(max(float(d), 0.0), 4.0)
            cal_mean = a + b * ensemble_mean
            cal_var = max(c + d * max(ensemble_spread, 1.0), 1.0)
            cal_std = min(math.sqrt(cal_var), 12.0)
            if mode == "conservative" or conservative:
                cal_std *= 1.12
            if mode == "aggressive" or aggressive:
                cal_std *= 0.92
            cal_mean, cal_std, reasons = enforce_physics_bounds(cal_mean, cal_std, self.param)
            if "variance_floor_applied" in reasons:
                cal_std = max(cal_std, DEFAULT_SIGMA_FLOOR)
            return round(cal_mean, 1), round(cal_std, 1)

        # Fallback: bias-correct the mean, inflate spread
        # Use 2.5°F as minimum uncertainty (can't be more precise than this)
        cal_std = min(max(ensemble_spread * 1.15 + 1.0, 2.5), 12.0)
        if mode == "conservative" or conservative:
            cal_std *= 1.12
        if mode == "aggressive" or aggressive:
            cal_std *= 0.92
        cal_mean, cal_std, _ = enforce_physics_bounds(ensemble_mean, cal_std, self.param)
        return round(cal_mean, 1), round(cal_std, 1)

    def train(self, global_fit: bool = False):
        """
        Fit EMOS parameters from verification data.

        Minimizes CRPS = E|X - y| - 0.5·E|X - X'|
        for X ~ N(a + b·μ, c + d·σ²) using Nelder-Mead.
        """
        return train_emos_rolling(
            self.city,
            param=self.param,
            window_days=self.train_window_days,
            global_fit=global_fit,
            min_samples=15 if global_fit else 10,
            deploy=True,
        )


def _nelder_mead(func, x0, step=None, max_iter=500, tol=1e-6):
    """Nelder-Mead simplex optimization (no scipy needed)."""
    n = len(x0)
    if step is None:
        step = [1.0] * n

    # Initialize simplex
    simplex = [list(x0)]
    for i in range(n):
        point = list(x0)
        point[i] += step[i]
        simplex.append(point)

    alpha, gamma, rho, sigma = 1.0, 2.0, 0.5, 0.5

    for _ in range(max_iter):
        # Sort by function value
        simplex.sort(key=func)
        best = simplex[0]
        worst = simplex[-1]
        second_worst = simplex[-2]

        f_best = func(best)
        f_worst = func(worst)
        f_second = func(second_worst)

        # Check convergence
        spread = max(abs(func(s) - f_best) for s in simplex)
        if spread < tol:
            return best

        # Centroid (excluding worst)
        centroid = [sum(simplex[i][j] for i in range(n)) / n for j in range(n)]

        # Reflection
        reflected = [centroid[j] + alpha * (centroid[j] - worst[j]) for j in range(n)]
        f_reflected = func(reflected)

        if f_best <= f_reflected < f_second:
            simplex[-1] = reflected
        elif f_reflected < f_best:
            # Expansion
            expanded = [centroid[j] + gamma * (reflected[j] - centroid[j]) for j in range(n)]
            simplex[-1] = expanded if func(expanded) < f_reflected else reflected
        else:
            # Contraction
            contracted = [centroid[j] + rho * (worst[j] - centroid[j]) for j in range(n)]
            if func(contracted) < f_worst:
                simplex[-1] = contracted
            else:
                # Shrink
                for i in range(1, n + 1):
                    simplex[i] = [best[j] + sigma * (simplex[i][j] - best[j]) for j in range(n)]

    simplex.sort(key=func)
    return simplex[0]


# ══════════════════════════════════════════════════════════════════════
#  CALIBRATED BUCKET PROBABILITIES
# ══════════════════════════════════════════════════════════════════════

@dataclass
class CalibratedDistribution:
    """
    Full calibrated predictive distribution for a city/date.

    Combines:
      - Bias-corrected multi-model forecasts
      - EMOS-calibrated mean and spread
      - Dress distribution (accounts for ensemble underdispersion)
      - Optional skew correction for bounded temperatures
    """
    city: str
    target_date: datetime
    mean: float = 70.0
    std: float = 5.0
    skew: float = 0.0  # positive = warm tail, negative = cold tail
    n_models: int = 0
    n_forecasts: int = 0
    model_highs: dict = field(default_factory=dict)  # model → corrected high

    def __post_init__(self):
        self.mean, self.std, _ = enforce_physics_bounds(self.mean, self.std, "high")

    def cdf(self, x: float) -> float:
        """Cumulative distribution function at temperature x."""
        if self.std <= 0:
            return 1.0 if x >= self.mean else 0.0

        # Use skew-normal approximation if skew != 0
        z = (x - self.mean) / self.std
        base_cdf = 0.5 * (1 + math.erf(z / math.sqrt(2)))

        if abs(self.skew) > 0.01:
            # Cornish-Fisher expansion for skew correction
            # Adjusts quantiles to account for asymmetry
            pdf_z = math.exp(-0.5 * z * z) / math.sqrt(2 * math.pi)
            correction = self.skew / 6.0 * (z * z - 1) * pdf_z
            return max(0.0, min(1.0, base_cdf + correction))

        return base_cdf

    def pdf(self, x: float) -> float:
        """Probability density function at temperature x."""
        if self.std <= 0:
            return 1.0 if abs(x - self.mean) < 0.5 else 0.0
        z = (x - self.mean) / self.std
        return math.exp(-0.5 * z * z) / (self.std * math.sqrt(2 * math.pi))

    def bucket_prob(self, lo: float, hi: float) -> float:
        """P(lo <= T <= hi) from calibrated CDF."""
        return max(0.0, self.cdf(hi) - self.cdf(lo))

    def bucket_distribution(self, buckets: list[tuple[float, float]]) -> list[dict]:
        """
        Compute calibrated probabilities for Kalshi temperature buckets.

        Handles:
          - Interior buckets: P(lo <= T < hi)
          - Lower tail: P(T < lo) lumped into lowest bucket
          - Upper tail: P(T >= hi) lumped into highest bucket
          - Normalization to ensure sum = 1.0
        """
        raw_probs = []
        for lo, hi in buckets:
            raw_probs.append(self.bucket_prob(lo, hi))

        # Normalize
        total = sum(raw_probs) if sum(raw_probs) > 0 else 1.0
        probs = [p / total for p in raw_probs]
        probs = _apply_rounding_shift(probs, buckets, self.mean)

        result = []
        for (lo, hi), prob in zip(buckets, probs):
            result.append({
                "lo": lo, "hi": hi,
                "label": f"{lo:.0f}-{hi:.0f}°F",
                "prob": round(prob, 4),
                "calib_mean": self.mean,
                "calib_std": self.std,
                "n_models": self.n_models,
            })

        return result

    def quantile(self, p: float) -> float:
        """Inverse CDF — find temperature at cumulative probability p."""
        if p <= 0: return self.mean - 5 * self.std
        if p >= 1: return self.mean + 5 * self.std

        # Newton's method on CDF
        x = self.mean + self.std * _probit(p)
        for _ in range(20):
            fx = self.cdf(x) - p
            dfx = self.pdf(x)
            if abs(dfx) < 1e-15:
                break
            x -= fx / dfx
            if abs(fx) < 1e-8:
                break
        return round(x, 1)

    def crps(self, obs: float) -> float:
        """
        CRPS (Continuous Ranked Probability Score) against an observation.
        Closed form for Gaussian: σ[z(2Φ(z)-1) + 2φ(z) - 1/√π]
        """
        if self.std <= 0:
            return abs(obs - self.mean)
        z = (obs - self.mean) / self.std
        phi_z = 0.5 * (1 + math.erf(z / math.sqrt(2)))
        pdf_z = math.exp(-0.5 * z * z) / math.sqrt(2 * math.pi)
        return self.std * (z * (2 * phi_z - 1) + 2 * pdf_z - 1 / math.sqrt(math.pi))


def _probit(p: float) -> float:
    """Inverse of standard normal CDF (Beasley-Springer-Moro algorithm)."""
    if p <= 0: return -8.0
    if p >= 1: return 8.0
    if p == 0.5: return 0.0

    # Rational approximation
    t = math.sqrt(-2 * math.log(min(p, 1 - p)))
    c0, c1, c2 = 2.515517, 0.802853, 0.010328
    d1, d2, d3 = 1.432788, 0.189269, 0.001308
    result = t - (c0 + c1*t + c2*t*t) / (1 + d1*t + d2*t*t + d3*t*t*t)
    return result if p > 0.5 else -result


# ══════════════════════════════════════════════════════════════════════
#  EDGE CALCULATOR WITH KELLY SIZING
# ══════════════════════════════════════════════════════════════════════

@dataclass
class EdgeCalculator:
    """
    Compares calibrated probability distribution against Kalshi market prices.

    Implements:
      - Simple edge = model_prob - market_prob
      - Kelly fraction for individual bucket (binary contract)
      - Multinomial Kelly for simultaneous positions across buckets
      - Confidence scoring based on model agreement and calibration quality
    """

    def compute_edges(self, calib_dist: CalibratedDistribution,
                      market_buckets: list[dict]) -> list[dict]:
        """
        Compute trading signals for all buckets.

        Args:
            calib_dist: calibrated temperature distribution
            market_buckets: list of dicts with keys: lo, hi, yes_price (cents), ticker

        Returns:
            list of dicts with edge, kelly, signal, confidence per bucket
        """
        # Get calibrated probabilities
        bucket_ranges = [(b["lo"], b["hi"]) for b in market_buckets]
        calib_probs = calib_dist.bucket_distribution(bucket_ranges)

        results = []
        for market, calib in zip(market_buckets, calib_probs):
            model_p = calib["prob"]
            market_p = (market.get("yes_price") or 50) / 100.0

            # Edge
            edge = model_p - market_p

            # Kelly fraction for binary contract
            # f* = (p·b - q) / b  where b = payout odds, p = true prob, q = 1-p
            kelly_buy = kelly_sell = 0.0
            if 0 < market_p < 1:
                # Buy YES: risk market_p to win (1 - market_p)
                b_buy = (1.0 - market_p) / market_p
                kelly_buy = (b_buy * model_p - (1 - model_p)) / b_buy
                kelly_buy = max(0, min(kelly_buy, 0.20))  # cap at 20%

                # Buy NO (sell YES): risk (1 - market_p) to win market_p
                b_sell = market_p / (1.0 - market_p)
                kelly_sell = (b_sell * (1 - model_p) - model_p) / b_sell
                kelly_sell = max(0, min(kelly_sell, 0.20))

            # Signal
            if edge > 0.08 and kelly_buy > 0.02:
                signal = "BUY"
                kelly = kelly_buy
            elif edge < -0.08 and kelly_sell > 0.02:
                signal = "SELL"
                kelly = kelly_sell
            elif edge > 0.04:
                signal = "LEAN_BUY"
                kelly = kelly_buy * 0.5
            elif edge < -0.04:
                signal = "LEAN_SELL"
                kelly = kelly_sell * 0.5
            else:
                signal = "HOLD"
                kelly = 0.0

            # Confidence: based on model count and edge magnitude
            conf = "LOW"
            if calib["n_models"] >= 3 and abs(edge) > 0.10:
                conf = "HIGH"
            elif calib["n_models"] >= 2 and abs(edge) > 0.06:
                conf = "MED"

            results.append({
                "lo": market["lo"],
                "hi": market["hi"],
                "label": f"{market['lo']:.0f}-{market['hi']:.0f}°F",
                "ticker": market.get("ticker", ""),
                "calib_prob": round(model_p, 4),
                "calib_prob_pct": round(model_p * 100, 1),
                "market_price": market.get("yes_price", 50),
                "market_prob": round(market_p, 4),
                "edge": round(edge, 4),
                "edge_pct": round(edge * 100, 1),
                "kelly": round(kelly, 4),
                "kelly_pct": round(kelly * 100, 1),
                "signal": signal,
                "confidence": conf,
                "calib_mean": calib_dist.mean,
                "calib_std": calib_dist.std,
            })

        return results

    def expected_value(self, edges: list[dict], bankroll: float = 1000.0) -> dict:
        """
        Portfolio-level expected value and risk metrics.

        Returns summary with total EV, max drawdown estimate, and Sharpe proxy.
        """
        total_ev = 0
        total_risk = 0
        positions = []

        for e in edges:
            if e["signal"] in ("BUY", "LEAN_BUY"):
                # Buying YES at market price
                bet_size = bankroll * e["kelly"]
                ev = bet_size * e["edge"]
                total_ev += ev
                total_risk += bet_size
                positions.append({"bucket": e["label"], "side": "YES",
                                  "size": round(bet_size, 2), "ev": round(ev, 2)})
            elif e["signal"] in ("SELL", "LEAN_SELL"):
                bet_size = bankroll * e["kelly"]
                ev = bet_size * abs(e["edge"])
                total_ev += ev
                total_risk += bet_size
                positions.append({"bucket": e["label"], "side": "NO",
                                  "size": round(bet_size, 2), "ev": round(ev, 2)})

        sharpe_proxy = total_ev / max(total_risk, 1) if total_risk > 0 else 0

        return {
            "total_ev": round(total_ev, 2),
            "total_risk": round(total_risk, 2),
            "n_positions": len(positions),
            "sharpe_proxy": round(sharpe_proxy, 3),
            "positions": positions,
        }


# ══════════════════════════════════════════════════════════════════════
#  MAIN CALIBRATION PIPELINE
# ══════════════════════════════════════════════════════════════════════

def _station_model_weights(city_key: str, target_date: datetime,
                           window_days: int = 14) -> dict[str, float]:
    """
    Per-station inverse-RMSE weights from recent verification.
    weight_i ∝ 1 / rmse_i^2
    """
    cutoff = (target_date - timedelta(days=window_days)).strftime("%Y-%m-%d")
    target_s = target_date.strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(CALIB_DB))
    rows = conn.execute("""
        SELECT model, COUNT(*) AS n, AVG(bias_high * bias_high) AS mse
        FROM verification
        WHERE city = ? AND model != 'cli'
          AND date >= ? AND date < ?
          AND bias_high IS NOT NULL
        GROUP BY model
    """, (city_key, cutoff, target_s)).fetchall()
    conn.close()

    if not rows:
        return {}

    raw = {}
    for model, n, mse in rows:
        if n is None or n < 3 or mse is None:
            continue
        rmse = math.sqrt(max(float(mse), 0.25))
        raw[str(model)] = 1.0 / (rmse * rmse)

    if not raw:
        return {}
    total = sum(raw.values())
    return {m: w / total for m, w in raw.items()}


def _station_sigma_rmse(city_key: str, target_date: datetime,
                        window_days: int = 14) -> Optional[float]:
    """
    Station-specific sigma from recent day-level ensemble RMSE.
    """
    cutoff = (target_date - timedelta(days=window_days)).strftime("%Y-%m-%d")
    target_s = target_date.strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(CALIB_DB))
    rows = conn.execute("""
        SELECT date, AVG(fcst_high_f) AS ens_mean, MAX(obs_high_f) AS obs
        FROM verification
        WHERE city = ? AND model != 'cli'
          AND date >= ? AND date < ?
          AND fcst_high_f IS NOT NULL
          AND obs_high_f IS NOT NULL
        GROUP BY date
        ORDER BY date
    """, (city_key, cutoff, target_s)).fetchall()
    conn.close()

    if len(rows) < 4:
        return None
    errs = [(float(ens_mean) - float(obs)) for _, ens_mean, obs in rows]
    rmse = math.sqrt(sum(e * e for e in errs) / len(errs))
    return max(1.5, rmse)


def _kalman_model_bias(city_key: str, model: str, target_date: datetime,
                       window_days: int = 30) -> tuple[float, float]:
    """
    Kalman-estimated per-model residual bias from recent verification errors.
    """
    cutoff = (target_date - timedelta(days=window_days)).strftime("%Y-%m-%d")
    target_s = target_date.strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(CALIB_DB))
    rows = conn.execute("""
        SELECT bias_high
        FROM verification
        WHERE city = ? AND model = ?
          AND date >= ? AND date < ?
          AND bias_high IS NOT NULL
        ORDER BY date
    """, (city_key, model, cutoff, target_s)).fetchall()
    conn.close()
    if len(rows) < 3:
        return 0.0, 2.0
    from kalman_bias import estimate_bias
    return estimate_bias([r[0] for r in rows])


def _apply_rounding_shift(bucket_probs: list[float], buckets: list[tuple[float, float]],
                          mean_f: float) -> list[float]:
    """
    Shift probability mass near Celsius integer rounding boundaries.
    """
    if not bucket_probs or len(bucket_probs) != len(buckets):
        return bucket_probs

    c = (mean_f - 32.0) * 5.0 / 9.0
    c_round = round(c)
    dist_c = abs(c - c_round)
    edge_window_c = 0.18
    if dist_c > edge_window_c:
        return bucket_probs

    f_round = c_round * 9.0 / 5.0 + 32.0
    naive_idx = None
    target_idx = None
    for i, (lo, hi) in enumerate(buckets):
        if lo <= mean_f <= hi:
            naive_idx = i
        if lo <= f_round <= hi:
            target_idx = i
    if naive_idx is None or target_idx is None or naive_idx == target_idx:
        return bucket_probs

    strength = (1.0 - dist_c / edge_window_c) * 0.15
    strength = max(0.0, min(strength, 0.20))
    delta = min(strength, bucket_probs[naive_idx] * 0.8)
    if delta <= 0:
        return bucket_probs

    shifted = list(bucket_probs)
    shifted[naive_idx] -= delta
    shifted[target_idx] += delta
    total = sum(shifted) or 1.0
    return [max(0.0, p / total) for p in shifted]


def _lead_time_bias_adjustment(model: str, lead_hours: float) -> float:
    """Lead-time dependent bias correction layer."""
    if lead_hours <= 6:
        return 0.0
    if lead_hours <= 24:
        return 0.2 if model in {"hrrr", "rap"} else 0.4
    if lead_hours <= 72:
        return 0.5 if model in {"nam", "nam3"} else 0.8
    return 1.0 if model == "gfs" else 0.6


def _model_family_bias_adjustment(model: str, target_date: datetime) -> float:
    """Model-family-specific bias correction layer."""
    family = model.lower()
    season = _season(target_date)
    if family in {"hrrr", "rap"}:
        return 0.1 if season in (1, 4) else 0.3
    if family in {"nam", "nam3"}:
        return 0.2 if season in (1, 2) else 0.4
    if family == "gfs":
        return -0.2 if season == 3 else 0.1
    if family in {"sref", "nbm"}:
        return 0.0
    return 0.0


def _infer_regime(city_key: str, target_date: datetime, spread: float,
                  obs_stale: bool = False) -> str:
    """Lightweight regime hook used for confidence and uncertainty inflation."""
    season = _season(target_date)
    if obs_stale:
        return "stale_obs"
    if spread >= 8.0:
        return "convective" if season in (2, 3) else "frontal"
    if spread >= 5.0:
        return "marine" if city_key in {"sea", "sfo", "lax"} else "inversion"
    return "stable"


def select_day_night_parameter_set(target_date: datetime) -> str:
    """Hook for separate daytime and nighttime parameter sets."""
    hour = normalize_utc_timestamp(target_date).hour
    return "day" if 6 <= hour < 18 else "night"


def _fuse_model_values(city_key: str, target_date: datetime,
                       corrected_values: dict[str, float],
                       conservative: bool = False,
                       aggressive: bool = False) -> tuple[float, dict[str, float], list[str]]:
    """
    Weighted multi-model fusion using rolling skill and guardrail modifiers.
    """
    dyn_weights = _station_model_weights(city_key, target_date, window_days=14)
    static_weights = {
        "hrrr": 1.5, "nam3": 1.3, "rap": 1.0,
        "nam": 0.9, "gfs": 0.7, "sref": 1.2,
        "nbm": 1.7, "lamp": 1.6, "ecmwf": 1.4,
        "sounding": 1.1,
    }
    weights = {}
    reasons = []
    for model in corrected_values:
        base = static_weights.get(model, 1.0)
        if model in dyn_weights:
            base = 0.7 * dyn_weights[model] + 0.3 * base
            reasons.append("rolling_skill_weighting")
        lead_adjust = _lead_time_bias_adjustment(model, 24.0)
        family_adjust = _model_family_bias_adjustment(model, target_date)
        weight = max(base - lead_adjust * 0.05 + family_adjust * 0.01, 0.05)
        if conservative:
            weight *= 0.92
        if aggressive:
            weight *= 1.05
        weights[model] = weight
    total_w = sum(weights.values()) or 1.0
    fused = sum(corrected_values[m] * weights[m] for m in corrected_values) / total_w
    return fused, weights, sorted(set(reasons))


def calibrate_city(city_key: str, model_highs: dict[str, float],
                   target_date: datetime,
                   market_buckets: Optional[list[dict]] = None,
                   *,
                   mode: str = "normal",
                   conservative: bool = False,
                   aggressive: bool = False,
                   regime_hint: Optional[str] = None) -> dict:
    """
    Full calibration pipeline for one city.

    Args:
        city_key: e.g. "nyc"
        model_highs: {model: raw_high_f} from NWP fetchers
        target_date: settlement date
        market_buckets: Kalshi bucket markets (optional)

    Returns:
        dict with calibrated distribution, edges, and signals
    """
    _ensure_db()
    station_profile = get_station_profile(city_key, target_date)
    fallback_reason_codes = []
    if city_key in STATION_SOURCE_OVERRIDES:
        fallback_reason_codes.append(f"station_override_{city_key}")

    if not model_highs:
        last_good = load_last_good_calibration(city_key)
        if last_good:
            payload = dict(last_good.get("payload", {}))
            payload["fallback_reason_codes"] = payload.get("fallback_reason_codes", []) + ["last_good_calibration"]
            payload["why"] = build_why_object(
                city_key,
                target_date,
                mode,
                regime_hint or last_good.get("regime"),
                payload["fallback_reason_codes"],
                payload.get("confidence", {"score": 0.0}),
                ["last_good_run_reused"],
                payload.get("dynamic_weights", {}),
                payload.get("feature_importance", {}),
            )
            payload["source"] = "last_good_calibration"
            return payload
        return {"error": "no model data", "city": city_key, "fallback_reason_codes": ["missing_model_data"]}

    input_errors = validate_calibration_inputs(model_highs)
    if input_errors:
        return {
            "error": "invalid forecast inputs",
            "city": city_key,
            "input_errors": input_errors,
            "fallback_reason_codes": ["invalid_model_inputs"],
        }

    # Step 1: Bias-correct each model
    corrected = {}
    kalman_bias = {}
    for model, raw_high in model_highs.items():
        bc = BiasCorrector(city_key, model, "high")
        base = bc.correct(raw_high, target_date)
        kb, ku = _kalman_model_bias(city_key, model, target_date, window_days=30)
        lead_adjust = _lead_time_bias_adjustment(model, (target_date - datetime.now(timezone.utc)).total_seconds() / 3600.0)
        family_adjust = _model_family_bias_adjustment(model, target_date)
        corrected[model] = round(base - kb - lead_adjust - family_adjust, 1)
        kalman_bias[model] = {"bias": round(kb, 2), "unc": round(ku, 2)}

    # Step 2: Compute ensemble statistics from corrected forecasts
    temps = list(corrected.values())
    n_t = len(temps)
    sorted_t = sorted(temps)
    core = sorted_t[1:-1] if n_t >= 4 else sorted_t
    ens_mean = sum(core) / len(core)
    core_var = sum((t - ens_mean) ** 2 for t in core) / max(len(core), 1)
    core_std = math.sqrt(core_var)
    ens_spread = max(core_std, 1.0)
    if n_t >= 4:
        q1 = sorted_t[n_t // 4]
        q3 = sorted_t[(3 * n_t) // 4]
        ens_spread = max(ens_spread, (q3 - q1) / 1.35, 1.5)
    elif n_t == 2:
        ens_spread = max(ens_spread, abs(sorted_t[1] - sorted_t[0]) / 2.0, 1.5)
    elif n_t == 1:
        ens_spread = max(ens_spread, 4.0)

    clipped = dict(corrected)
    if n_t >= 4:
        lo_clip = sorted_t[1]
        hi_clip = sorted_t[-2]
        for m, v in corrected.items():
            clipped[m] = min(max(v, lo_clip), hi_clip)

    # Weight by rolling skill with small policy adjustments.
    weighted_mean, weights, fusion_reasons = _fuse_model_values(
        city_key,
        target_date,
        clipped,
        conservative=conservative or mode == "conservative",
        aggressive=aggressive or mode == "aggressive",
    )

    # Step 3: EMOS calibration
    emos = EMOSCalibrator(city_key, "high")
    cal_mean, cal_std = emos.calibrate(
        weighted_mean,
        ens_spread,
        target_date,
        mode=mode,
        conservative=conservative,
        aggressive=aggressive,
        regime=regime_hint,
    )
    station_sigma = _station_sigma_rmse(city_key, target_date, window_days=14)
    if station_sigma is not None:
        cal_std = round(max(2.0, 0.6 * cal_std + 0.4 * station_sigma), 1)

    sigma_lookup, sigma_reasons = emos.get_sigma(target_date, hour_bucket=target_date.hour)
    cal_std = round(max(cal_std, sigma_lookup), 1)
    fallback_reason_codes.extend(sigma_reasons)

    # Step 4: Skew estimation
    # If models disagree about direction, add skew toward the outlier side
    skew = 0.0
    if len(temps) >= 3:
        median_t = sorted(temps)[len(temps) // 2]
        # Positive skew if mean > median (warm outlier)
        skew = (ens_mean - median_t) / max(cal_std, 1.0) * 0.3
        skew = max(-0.5, min(0.5, skew))

    if len(temps) >= 2:
        disagreement = max(temps) - min(temps)
        if disagreement >= 6.0:
            cal_std *= 1.12
            fallback_reason_codes.append("model_disagreement_inflation")
    regime = regime_hint or _infer_regime(city_key, target_date, ens_spread, obs_stale=False)
    day_night_set = select_day_night_parameter_set(target_date)
    if regime == "stale_obs":
        cal_std *= 1.10
        fallback_reason_codes.append("stale_observation_inflation")

    cal_mean, cal_std, physics_reasons = enforce_physics_bounds(cal_mean, cal_std, "high")
    fallback_reason_codes.extend(physics_reasons)
    if temps:
        cal_mean = min(max(cal_mean, min(temps) - 10.0), max(temps) + 10.0)
    cal_std = max(cal_std, DEFAULT_SIGMA_FLOOR)
    if cal_std <= 0:
        cal_std = DEFAULT_SIGMA_FLOOR
        fallback_reason_codes.append("variance_guard_floor")

    # Step 5: Build calibrated distribution
    dist = CalibratedDistribution(
        city=city_key,
        target_date=target_date,
        mean=cal_mean,
        std=cal_std,
        skew=skew,
        n_models=len(corrected),
        n_forecasts=len(corrected),
        model_highs=corrected,
    )

    result = {
        "city": city_key,
        "city_name": _get_kalshi_cities()[city_key][0],
        "station_profile": station_profile,
        "target_date": target_date.strftime("%Y-%m-%d"),
        "raw_models": {m: round(v, 1) for m, v in model_highs.items()},
        "corrected_models": corrected,
        "ensemble_mean": round(ens_mean, 1),
        "ensemble_spread": round(ens_spread, 2),
        "weighted_mean": round(weighted_mean, 1),
        "calib_mean": cal_mean,
        "calib_std": cal_std,
        "station_sigma": round(station_sigma, 2) if station_sigma is not None else None,
        "dynamic_weights": {m: round(w, 4) for m, w in weights.items()},
        "kalman_bias": kalman_bias,
        "skew": round(skew, 2),
        "n_models": len(corrected),
        "p10": dist.quantile(0.10),
        "p25": dist.quantile(0.25),
        "p50": dist.quantile(0.50),
        "p75": dist.quantile(0.75),
        "p90": dist.quantile(0.90),
        "distribution": dist,
        "fallback_reason_codes": sorted(set(fallback_reason_codes + fusion_reasons)),
        "parameter_set": day_night_set,
    }

    confidence = decompose_confidence(
        city_key,
        len(corrected),
        ens_spread,
        regime=regime,
        stale_obs=False,
        conservative=conservative or mode == "conservative",
        aggressive=aggressive or mode == "aggressive",
    )
    result["confidence"] = confidence
    result["why"] = build_why_object(
        city_key,
        target_date,
        mode,
        regime,
        result["fallback_reason_codes"],
        confidence,
        physics_reasons,
        weights,
        feature_importance={
            "ensemble_mean": round(ens_mean, 3),
            "ensemble_spread": round(ens_spread, 3),
            "station_sigma": round(station_sigma, 3) if station_sigma is not None else None,
            "regime": regime,
            "parameter_set": day_night_set,
        },
    )

    record_calibration_metric(city_key, target_date, "calib_std", cal_std,
                              baseline_value=ens_spread, regime=regime, model="high")
    record_calibration_metric(city_key, target_date, "ensemble_spread", ens_spread,
                              baseline_value=station_sigma, regime=regime, model="high")

    # Step 6: Edge calculation if market data available
    if market_buckets:
        calc = EdgeCalculator()
        edges = calc.compute_edges(dist, market_buckets)
        portfolio = calc.expected_value(edges)
        result["edges"] = edges
        result["portfolio"] = portfolio

    record_calibration_run(
        city_key,
        target_date,
        mode,
        result,
        status="ok",
        regime=regime,
        fallback_reason_codes=result["fallback_reason_codes"],
        last_good=True,
    )

    return result


def calibrate_all_cities(model_data: dict[str, dict[str, float]],
                         target_date: datetime,
                         market_data: Optional[dict[str, list[dict]]] = None) -> list[dict]:
    """
    Calibrate all cities at once.

    Args:
        model_data: {city_key: {model: raw_high_f}}
        target_date: settlement date
        market_data: {city_key: [bucket markets]} (optional)

    Returns:
        list of calibration results per city
    """
    results = []
    for city_key in sorted(model_data.keys()):
        markets = (market_data or {}).get(city_key, None)
        r = calibrate_city(city_key, model_data[city_key], target_date, markets)
        results.append(r)
    return results


# ══════════════════════════════════════════════════════════════════════
#  VERIFICATION: INGEST NWS CLI OBSERVATIONS
# ══════════════════════════════════════════════════════════════════════

def upsert_cli_settlement(city_key: str, date: datetime,
                          obs_high: float, obs_low: Optional[float] = None):
    """Persist official settlement observations in verification table."""
    _ensure_db()
    date_str = date.strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(CALIB_DB))
    conn.execute("""
        INSERT OR REPLACE INTO verification
        (city, date, model, cycle, fcst_high_f, fcst_low_f,
         obs_high_f, obs_low_f, bias_high, bias_low,
         abs_err_high, abs_err_low, lead_hours)
        VALUES (?, ?, 'cli', 0, NULL, NULL, ?, ?, NULL, NULL, NULL, NULL, 0)
    """, (city_key, date_str, obs_high, obs_low))
    conn.commit()
    conn.close()


def get_cli_settlement(city_key: str, date: datetime) -> Optional[dict]:
    """Read stored CLI settlement from verification table."""
    _ensure_db()
    date_str = date.strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(CALIB_DB))
    row = conn.execute("""
        SELECT obs_high_f, obs_low_f
        FROM verification
        WHERE city = ? AND date = ? AND model = 'cli' AND obs_high_f IS NOT NULL
        LIMIT 1
    """, (city_key, date_str)).fetchone()
    conn.close()
    if not row:
        return None
    return {"date": date_str, "high_f": row[0], "low_f": row[1], "source": "calibration_db"}


def backfill_model_verification(start_date: datetime, end_date: datetime,
                                cutoff_hour_utc: int = 23,
                                cities: Optional[list[str]] = None,
                                verbose: bool = True) -> dict:
    """
    Backfill model-vs-settlement verification rows from stored NWP forecasts.
    Requires CLI settlements already present in verification table (model='cli').
    """
    _ensure_db()
    city_keys = cities or sorted(_get_kalshi_cities().keys())
    start_s = start_date.strftime("%Y-%m-%d")
    end_s = end_date.strftime("%Y-%m-%d")

    conn = sqlite3.connect(str(CALIB_DB))
    cli_rows = conn.execute("""
        SELECT city, date, obs_high_f, obs_low_f
        FROM verification
        WHERE model='cli' AND date >= ? AND date <= ? AND obs_high_f IS NOT NULL
        ORDER BY date, city
    """, (start_s, end_s)).fetchall()
    conn.close()

    cli_map = {(c, d): (h, l) for c, d, h, l in cli_rows if c in city_keys}
    n_days = 0
    n_model_rows = 0
    n_missing_nwp = 0

    cur = start_date
    while cur <= end_date:
        date_str = cur.strftime("%Y-%m-%d")
        for city_key in city_keys:
            obs = cli_map.get((city_key, date_str))
            if obs is None:
                continue
            obs_high, obs_low = obs
            model_fcst = _collect_model_forecasts_from_nwp(city_key, date_str, cutoff_hour_utc)
            if not model_fcst:
                n_missing_nwp += 1
                continue
            record_verification(city_key, cur, model_fcst, float(obs_high),
                                float(obs_low) if obs_low is not None else None)
            n_days += 1
            n_model_rows += len(model_fcst)
            if verbose:
                print(f"  {city_key} {date_str}: +{len(model_fcst)} model rows")
        cur += timedelta(days=1)

    return {
        "cities": len(city_keys),
        "start": start_s,
        "end": end_s,
        "city_days_updated": n_days,
        "model_rows_written": n_model_rows,
        "city_days_missing_nwp": n_missing_nwp,
    }

def fetch_cli_observation(city_key: str, date: datetime) -> Optional[dict]:
    """
    Fetch NWS CLI (Daily Climate Report) to get official high/low.
    This is what Kalshi settles on.

    The CLI is issued daily around 5-6 PM local time.
    Product: https://forecast.weather.gov/product.php?site=XXX&issuedby=XXX&product=CLI
    """
    import urllib.request

    city = _get_kalshi_cities().get(city_key)
    if not city:
        return None

    cli_id = city[7]  # e.g. "NYC", "MDW", "HOU"
    wfo = city[8]

    # Try IEM archive for historical CLI
    date_str = date.strftime("%Y-%m-%d")
    url = f"https://mesonet.agron.iastate.edu/cgi-bin/afos/retrieve.py?pil=CLI{cli_id}&limit=5"

    try:
        req = urllib.request.Request(url, headers={"User-Agent": "dsco-calib/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except:
        return None

    # Parse CLI text for high/low temps
    # Format varies but typically:
    #   TEMPERATURE (F)
    #     MAXIMUM        85
    #     MINIMUM        67
    high = low = None
    lines = text.split("\n")
    in_temp = False
    for line in lines:
        s = line.strip()
        if "TEMPERATURE" in s and "(F)" in s:
            in_temp = True
            continue
        if in_temp:
            m = re.search(r'MAXIMUM\s+(\d+)', s)
            if m:
                high = float(m.group(1))
            m = re.search(r'MINIMUM\s+(\d+)', s)
            if m:
                low = float(m.group(1))
            if high and low:
                break
            # Stop if we hit a non-temp section
            if s and not s.startswith(("MAX", "MIN", "AVERAGE", "DEPARTURE")):
                if not any(c.isdigit() for c in s[:20]):
                    in_temp = False

    if high is not None:
        return {"date": date_str, "high_f": high, "low_f": low, "cli_id": cli_id}
    return None


def record_verification(city_key: str, date: datetime,
                        model_forecasts: dict[str, dict],
                        obs_high: float, obs_low: Optional[float] = None):
    """
    Record forecast-observation verification pair.
    Updates both verification table and bias tracker.
    """
    _ensure_db()
    conn = sqlite3.connect(str(CALIB_DB))
    date_str = date.strftime("%Y-%m-%d")
    success_models = []
    failure_models = []

    for model, fcst in model_forecasts.items():
        fcst_high = fcst.get("high_f") or fcst.get("t2m_f")
        if fcst_high is None:
            failure_models.append(model)
            continue

        bias_high = fcst_high - obs_high
        fcst_low = fcst.get("low_f")
        bias_low = (fcst_low - obs_low) if (fcst_low and obs_low) else None
        cycle = fcst.get("cycle", 0)
        lead = fcst.get("lead_hours", 0)

        try:
            conn.execute("""
                INSERT OR REPLACE INTO verification
                (city, date, model, cycle, fcst_high_f, fcst_low_f,
                 obs_high_f, obs_low_f, bias_high, bias_low,
                 abs_err_high, abs_err_low, lead_hours)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (city_key, date_str, model, cycle, fcst_high, fcst_low,
                  obs_high, obs_low, bias_high, bias_low,
                  abs(bias_high), abs(bias_low) if bias_low is not None else None,
                  lead))
            success_models.append(model)
        except:
            failure_models.append(model)
            pass

        # Note: bias correction reads directly from verification rows.
        # Avoid nested SQLite writes here (can trigger "database is locked").

    conn.commit()
    conn.close()
    for model in success_models:
        _increment_ingest_counter("verification", "success", model=model, city=city_key)
    for model in failure_models:
        _increment_ingest_counter("verification", "failure", model=model, city=city_key)


# ══════════════════════════════════════════════════════════════════════
#  CLI
# ══════════════════════════════════════════════════════════════════════

import re

def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Kukulkan Ensemble Calibration Engine",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s status                      # Show calibration status for all cities
  %(prog)s calibrate nyc               # Full calibration pipeline for NYC
  %(prog)s calibrate --all             # All 20 cities
  %(prog)s verify nyc                  # Fetch CLI and record verification
  %(prog)s train nyc                   # Train EMOS parameters
  %(prog)s bias nyc                    # Show bias tracker
  %(prog)s signals                     # Show all active trading signals
        """)

    sub = parser.add_subparsers(dest="command")

    p_status = sub.add_parser("status", help="Calibration system status")

    p_calib = sub.add_parser("calibrate", help="Run calibration pipeline")
    p_calib.add_argument("city", nargs="?", help="City key")
    p_calib.add_argument("--all", action="store_true")

    p_verify = sub.add_parser("verify", help="Fetch CLI obs and verify")
    p_verify.add_argument("city", nargs="?")
    p_verify.add_argument("--date", help="YYYY-MM-DD")
    p_verify.add_argument("--cutoff-hour", type=int, default=23,
                          help="Use model runs with init_cycle <= this UTC hour (default: 23)")

    p_backfill = sub.add_parser("backfill", help="Backfill model verification from stored NWP forecasts")
    p_backfill.add_argument("--start", required=True, help="Start date YYYY-MM-DD")
    p_backfill.add_argument("--end", required=True, help="End date YYYY-MM-DD")
    p_backfill.add_argument("--cities", nargs="+", help="Optional city keys")
    p_backfill.add_argument("--cutoff-hour", type=int, default=23,
                            help="Use model runs with init_cycle <= this UTC hour (default: 23)")
    p_backfill.add_argument("--train", action="store_true",
                            help="Train EMOS after backfill (city + global)")

    p_train = sub.add_parser("train", help="Train EMOS parameters")
    p_train.add_argument("city", nargs="?")
    p_train.add_argument("--all", action="store_true")
    p_train.add_argument("--global", action="store_true", dest="global_fit",
                         help="Train pooled global fallback EMOS")
    p_train.add_argument("--param", choices=["high", "low"], default="high")
    p_train.add_argument("--rolling-days", type=int, default=90,
                         help="Rolling training window in days (default: 90)")
    p_train.add_argument("--season-matched", action="store_true",
                         help="Train from same-season historical windows")
    p_train.add_argument("--lookback-years", type=int, default=5,
                         help="Historical lookback for --season-matched")
    p_train.add_argument("--target-date", help="YYYY-MM-DD deployment target date")
    p_train.add_argument("--no-deploy", action="store_true",
                         help="Run training and stability checks without writing emos_params")
    p_train.add_argument("--json", action="store_true", help="Emit JSON training summary")

    p_shrink = sub.add_parser("shrinkage", help="Tune pooled-vs-station EMOS shrinkage")
    p_shrink.add_argument("city", help="City key")
    p_shrink.add_argument("--param", choices=["high", "low"], default="high")
    p_shrink.add_argument("--window-days", type=int, default=180)
    p_shrink.add_argument("--target-date", help="YYYY-MM-DD")
    p_shrink.add_argument("--apply", action="store_true", help="Deploy recommended shrunken coefficients")
    p_shrink.add_argument("--json", action="store_true", help="Emit JSON summary")

    p_pit = sub.add_parser("pit-histogram", help="Generate PIT histogram from verification rows")
    p_pit.add_argument("--city", help="Optional city key")
    p_pit.add_argument("--param", choices=["high", "low"], default="high")
    p_pit.add_argument("--start", help="Start date YYYY-MM-DD")
    p_pit.add_argument("--end", help="End date YYYY-MM-DD")
    p_pit.add_argument("--bins", type=int, default=10)
    p_pit.add_argument("--json", action="store_true", help="Emit JSON summary")

    p_bias = sub.add_parser("bias", help="Show bias tracker")
    p_bias.add_argument("city", nargs="?")

    p_signals = sub.add_parser("signals", help="Show trading signals")

    args = parser.parse_args()

    def _parse_date_arg(value: Optional[str], default: Optional[datetime] = None) -> datetime:
        if not value:
            return default or datetime.now(timezone.utc)
        return datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)

    def _print_training_result(result: dict) -> None:
        candidate = result.get("candidate") or result.get("recommended_params")
        status = "DEPLOYED" if result.get("deployed") else "DRY-RUN/HELD"
        print(
            f"  {result.get('training_mode', 'shrinkage')} {result.get('city')} {result.get('param')}: "
            f"{status} n={result.get('n_train', result.get('station_n', 0))}"
        )
        if candidate:
            print(
                f"    a={candidate['a']:.2f} b={candidate['b']:.3f} "
                f"c={candidate['c']:.2f} d={candidate['d']:.3f} "
                f"CRPS={candidate.get('crps_train', result.get('best_crps', 0.0)):.3f}"
            )
        stability = result.get("stability", {})
        if stability:
            print(f"    gate={stability.get('ok')} reasons={','.join(stability.get('reasons', []))}")

    if args.command == "status":
        _ensure_db()
        conn = sqlite3.connect(str(CALIB_DB))
        n_verif = conn.execute("SELECT COUNT(*) FROM verification").fetchone()[0]
        n_emos = conn.execute("SELECT COUNT(*) FROM emos_params").fetchone()[0]
        n_bias = conn.execute("SELECT COUNT(*) FROM bias_tracker").fetchone()[0]
        n_signals = conn.execute("SELECT COUNT(*) FROM trade_signals").fetchone()[0]
        conn.close()

        print(f"\n  KUKULKAN CALIBRATION ENGINE STATUS")
        print(f"  {'─'*40}")
        print(f"  Verification pairs:  {n_verif}")
        print(f"  EMOS parameters:     {n_emos} city-param combos")
        print(f"  Bias trackers:       {n_bias} model-city combos")
        print(f"  Trade signals:       {n_signals}")
        print(f"  Database: {CALIB_DB}")
        conn = sqlite3.connect(str(CALIB_DB))
        g = conn.execute("""
            SELECT a, b, c, d, n_train, crps_train
            FROM emos_params WHERE city = ? AND param = 'high'
        """, (GLOBAL_EMOS_CITY,)).fetchone()
        conn.close()
        if g:
            print(f"  Global EMOS(high):   a={g[0]:.2f} b={g[1]:.3f} c={g[2]:.2f} d={g[3]:.3f} (n={g[4]}, CRPS={g[5]:.2f})")
        print()

        # Show per-city status
        conn = sqlite3.connect(str(CALIB_DB))
        print(f"  {'City':<16} {'Verif':>6} {'EMOS':>6} {'Bias':>6} {'Latest Verif'}")
        print(f"  {'─'*56}")
        for ck in sorted(_get_kalshi_cities().keys()):
            nv = conn.execute("SELECT COUNT(*) FROM verification WHERE city=?", (ck,)).fetchone()[0]
            ne = conn.execute("SELECT COUNT(*) FROM emos_params WHERE city=?", (ck,)).fetchone()[0]
            nb = conn.execute("SELECT COUNT(*) FROM bias_tracker WHERE city=?", (ck,)).fetchone()[0]
            latest = conn.execute("SELECT MAX(date) FROM verification WHERE city=?", (ck,)).fetchone()[0] or "—"
            print(f"  {_get_kalshi_cities()[ck][0]:<16} {nv:>6} {ne:>6} {nb:>6} {latest}")
        conn.close()

    elif args.command == "calibrate":
        from nwp_pipeline import ingest_city
        target = datetime.now(timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)

        cities = sorted(_get_kalshi_cities().keys()) if args.all else [args.city] if args.city else []
        if not cities:
            print("Specify a city or --all")
            return

        for ck in cities:
            print(f"\n{'='*60}")
            print(f"  CALIBRATING: {_get_kalshi_cities()[ck][0]} ({ck.upper()})")
            print(f"{'='*60}")

            # Fetch NWP data
            ens = ingest_city(ck, verbose=True, target_date=target)
            if not ens.forecasts:
                print(f"  No forecast data available")
                continue

            # Get model highs
            model_highs = {}
            for model, info in ens.model_spread().items():
                model_highs[model] = info["max"]

            # Run calibration
            result = calibrate_city(ck, model_highs, target)

            # Display
            print(f"\n  Raw models:       {result['raw_models']}")
            print(f"  Bias-corrected:   {result['corrected_models']}")
            print(f"  Ensemble mean:    {result['ensemble_mean']}°F")
            print(f"  Weighted mean:    {result['weighted_mean']}°F")
            print(f"  CALIBRATED:       μ={result['calib_mean']}°F  σ={result['calib_std']}°F  skew={result['skew']}")
            print(f"  Percentiles:      P10={result['p10']}  P25={result['p25']}  P50={result['p50']}  P75={result['p75']}  P90={result['p90']}")

    elif args.command == "bias":
        _ensure_db()
        conn = sqlite3.connect(str(CALIB_DB))
        city = args.city
        if city:
            rows = conn.execute("""
                SELECT model, param, running_bias, running_mae, running_rmse, n_obs, last_updated
                FROM bias_tracker WHERE city=? ORDER BY model, param
            """, (city,)).fetchall()
            if rows:
                print(f"\n  BIAS TRACKER: {_get_kalshi_cities().get(city, ['?'])[0]} ({city.upper()})")
                print(f"  {'Model':<8} {'Param':<6} {'Bias':>7} {'MAE':>7} {'RMSE':>7} {'N':>5} {'Updated'}")
                print(f"  {'─'*54}")
                for model, param, bias, mae, rmse, n, updated in rows:
                    print(f"  {model:<8} {param:<6} {bias:>+7.1f} {mae:>7.1f} {rmse:>7.1f} {n:>5} {updated[:10] if updated else '—'}")
            else:
                print(f"  No bias data for {city}. Run verification first.")

            # Also show prior biases
            season = _season(datetime.now(timezone.utc))
            print(f"\n  Prior biases (season {season}):")
            for model in ["hrrr", "rap", "nam", "nam3", "gfs", "sref"]:
                prior = PRIOR_BIASES.get((model, season), 0)
                print(f"    {model:<8} {prior:>+5.1f}°F")
        else:
            print("Specify a city key")
        conn.close()

    elif args.command == "train":
        target = _parse_date_arg(args.target_date)
        deploy = not args.no_deploy
        if args.global_fit:
            if args.season_matched:
                result = train_emos_season_matched(
                    GLOBAL_EMOS_CITY,
                    param=args.param,
                    target_date=target,
                    lookback_years=args.lookback_years,
                    deploy=deploy,
                    global_fit=True,
                    min_samples=15,
                )
            else:
                result = train_emos_rolling(
                    GLOBAL_EMOS_CITY,
                    param=args.param,
                    target_date=target,
                    window_days=args.rolling_days,
                    deploy=deploy,
                    global_fit=True,
                    min_samples=15,
                )
            if args.json:
                print(json.dumps(result, indent=2, sort_keys=True))
            else:
                _print_training_result(result)
            return

        cities = sorted(_get_kalshi_cities().keys()) if args.all else [args.city] if args.city else []
        if not cities:
            print("Specify a city or --all")
            return
        results = []
        for ck in cities:
            if args.season_matched:
                result = train_emos_season_matched(
                    ck,
                    param=args.param,
                    target_date=target,
                    lookback_years=args.lookback_years,
                    deploy=deploy,
                )
            else:
                result = train_emos_rolling(
                    ck,
                    param=args.param,
                    target_date=target,
                    window_days=args.rolling_days,
                    deploy=deploy,
                )
            results.append(result)
            if not args.json:
                _print_training_result(result)
        if args.json:
            print(json.dumps(results, indent=2, sort_keys=True))

    elif args.command == "shrinkage":
        result = tune_pooled_station_shrinkage(
            args.city,
            param=args.param,
            target_date=_parse_date_arg(args.target_date),
            window_days=args.window_days,
            deploy=args.apply,
        )
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            _print_training_result({
                **result,
                "training_mode": "shrinkage",
                "n_train": result.get("station_n", 0),
            })
            if result.get("ok"):
                print(f"    best_alpha={result['best_alpha']:.2f} best_crps={result['best_crps']:.3f}")
            else:
                print(f"    reason={result.get('reason')}")

    elif args.command == "pit-histogram":
        result = build_pit_histogram(
            args.city,
            param=args.param,
            start_date=_parse_date_arg(args.start) if args.start else None,
            end_date=_parse_date_arg(args.end) if args.end else None,
            bins=args.bins,
        )
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(
                f"  PIT {args.city or 'pooled'} {args.param}: "
                f"n={result['n']} mean={result['mean_pit']} chi2={result['chi_square']} shape={result['shape']}"
            )
            for item in result["bins"]:
                bar = "█" * item["count"]
                print(f"    {item['lo']:.2f}-{item['hi']:.2f} {item['count']:>4} {bar}")

    elif args.command == "verify":
        if not args.city:
            print("Specify a city key")
            return
        date = datetime.now(timezone.utc) - timedelta(days=1)
        if args.date:
            date = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        print(f"  Fetching CLI for {args.city.upper()} on {date.strftime('%Y-%m-%d')}...")
        obs = fetch_cli_observation(args.city, date)
        if not obs:
            obs = get_cli_settlement(args.city, date)
        if obs:
            print(f"  CLI: High={obs['high_f']}°F  Low={obs.get('low_f', '—')}°F")
            upsert_cli_settlement(args.city, date, obs["high_f"], obs.get("low_f"))

            model_fcst = _collect_model_forecasts_from_nwp(
                args.city, date.strftime("%Y-%m-%d"), args.cutoff_hour
            )
            if model_fcst:
                record_verification(
                    args.city, date, model_fcst, obs["high_f"], obs.get("low_f")
                )
                print(f"  Recorded {len(model_fcst)} model verification rows "
                      f"(cutoff <= {args.cutoff_hour:02d}Z).")
            else:
                print("  No NWP forecasts found for that date/cutoff; CLI row stored.")
        else:
            print("  Could not fetch CLI observation (and no stored settlement found)")

    elif args.command == "backfill":
        start = datetime.strptime(args.start, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        end = datetime.strptime(args.end, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        if end < start:
            print("--end must be on or after --start")
            return
        summary = backfill_model_verification(
            start, end, cutoff_hour_utc=args.cutoff_hour, cities=args.cities, verbose=True
        )
        print(f"\n  Backfill summary: {summary}")
        if args.train:
            print("\n  Training city EMOS models...")
            for ck in sorted((args.cities or _get_kalshi_cities().keys())):
                emos = EMOSCalibrator(ck, "high")
                emos.train()
            print("  Training global pooled EMOS...")
            EMOSCalibrator(GLOBAL_EMOS_CITY, "high").train(global_fit=True)
            print("  Training complete.")

    elif args.command == "signals":
        _ensure_db()
        conn = sqlite3.connect(str(CALIB_DB))
        rows = conn.execute("""
            SELECT city, date, bucket_lo, bucket_hi, ticker, calib_prob, market_price,
                   edge_pct, kelly_frac, signal, confidence
            FROM trade_signals WHERE date >= date('now', '-1 day')
            ORDER BY abs(edge_pct) DESC
        """).fetchall()
        conn.close()

        if rows:
            print(f"\n  ACTIVE TRADE SIGNALS")
            print(f"  {'City':<6} {'Date':<11} {'Bucket':<12} {'Calib%':>7} {'Mkt¢':>5} {'Edge':>7} {'Kelly':>6} {'Signal':<10} {'Conf'}")
            print(f"  {'─'*80}")
            for city, date, lo, hi, ticker, cp, mp, edge, kelly, sig, conf in rows:
                sig_color = sig
                print(f"  {city:<6} {date:<11} {lo:.0f}-{hi:.0f}°F   {cp*100:>6.1f}% {mp:>4.0f}¢ {edge:>+6.1f}% {kelly*100:>5.1f}% {sig:<10} {conf}")
        else:
            print("  No active signals. Run calibration first.")

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
