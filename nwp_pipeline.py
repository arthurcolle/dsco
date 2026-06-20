#!/usr/bin/env python3
"""
NWP Model Data Pipeline for Kalshi Temperature Trading
=======================================================
Fetches BUFKIT soundings, HRRR/GFS/NAM/RAP/SREF model data
for the 20 Kalshi cities and produces calibrated temperature
probability distributions for bucket trading.

Sources:
  - Penn State BUFKIT:  https://www.meteo.psu.edu/bufkit/
  - Iowa State Archive:  https://mtarchive.geol.iastate.edu/
  - NOAA NOMADS:         https://nomads.ncep.noaa.gov/
  - AWS Open Data HRRR:  s3://noaa-hrrr-bdp-pds/
  - AWS Open Data GFS:   s3://noaa-gfs-bdp-pds/

Models integrated:
  HRRR  - 3km, hourly init, 0-18h (best short-range)
  RAP   - 13km, hourly init, 0-21h
  NAM   - 12km, 4x daily, 0-84h
  NAM3  - 3km nest, 4x daily, 0-60h
  GFS   - 25km, 4x daily, 0-384h
  SREF  - 16km ensemble, 4x daily (21 members)
"""

import argparse
import csv
import hashlib
import io
import json
import math
import os
import random
import re
import sqlite3
import struct
import sys
import time
import urllib.request
import urllib.error
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Optional

import requests

# ── 20 Kalshi cities ─────────────────────────────────────────────────
# Kalshi settles on the NWS CLI (Daily Climate Report) for each station.
# settlement_icao = the EXACT station the CLI is issued for
# bufkit_icao     = the station to pull BUFKIT soundings from (Penn State uses lowercase ICAO)
#
# Maps: city_key → (display_name, settlement_icao, bufkit_icao, lat, lon, series_high, series_low, cli_id, nws_wfo)
KALSHI_CITIES = {
    "nyc":  ("New York City",  "KNYC", "KLGA", 40.7829, -73.9654, "KXHIGHNY",    "KXLOWNY",   "NYC", "OKX"),  # Central Park — no BUFKIT, use LGA
    "chi":  ("Chicago",        "KMDW", "KMDW", 41.786,  -87.752,  "KXHIGHCHI",   "KXLOWCHI",  "MDW", "LOT"),  # Midway, NOT O'Hare
    "hou":  ("Houston",        "KHOU", "KHOU", 29.645,  -95.279,  "KXHIGHTHOU",  "KXLOWTHOU", "HOU", "HGX"),  # Hobby, NOT Bush IAH
    "mia":  ("Miami",          "KMIA", "KMIA", 25.793,  -80.290,  "KXHIGHMIA",   "KXLOWMIA",  "MIA", "MFL"),
    "lax":  ("Los Angeles",    "KLAX", "KLAX", 33.943,  -118.408, "KXHIGHLAX",   "KXLOWLAX",  "LAX", "LOX"),
    "aus":  ("Austin",         "KAUS", "KAUS", 30.195,  -97.670,  "KXHIGHAUS",   "KXLOWAUS",  "AUS", "EWX"),
    "den":  ("Denver",         "KDEN", "KDEN", 39.856,  -104.673, "KXHIGHDEN",   "KXLOWDEN",  "DEN", "BOU"),
    "phl":  ("Philadelphia",   "KPHL", "KPHL", 39.872,  -75.241,  "KXHIGHPHIL",  "KXLOWPHIL", "PHL", "PHI"),
    "sea":  ("Seattle",        "KSEA", "KSEA", 47.449,  -122.309, "KXHIGHTSEA",  "KXLOWTSEA", "SEA", "SEW"),
    "phx":  ("Phoenix",        "KPHX", "KPHX", 33.428,  -112.004, "KXHIGHTPHX",  "KXLOWTPHX", "PHX", "PSR"),
    "las":  ("Las Vegas",      "KLAS", "KLAS", 36.080,  -115.152, "KXHIGHTLV",   "KXLOWTLV",  "LAS", "VEF"),
    "bos":  ("Boston",         "KBOS", "KBOS", 42.361,  -71.010,  "KXHIGHTBOS",  "KXLOWTBOS", "BOS", "BOX"),
    "dal":  ("Dallas",         "KDFW", "KDFW", 32.897,  -97.038,  "KXHIGHTDAL",  "KXLOWTDAL", "DFW", "FWD"),  # DFW Airport, NOT Love Field
    "msy":  ("New Orleans",    "KMSY", "KMSY", 29.993,  -90.258,  "KXHIGHTNOU",  "KXLOWTNOU", "MSY", "LIX"),
    "atl":  ("Atlanta",        "KATL", "KATL", 33.640,  -84.427,  "KXHIGHTATL",  "KXLOWTATL", "ATL", "FFC"),
    "msp":  ("Minneapolis",    "KMSP", "KMSP", 44.883,  -93.229,  "KXHIGHTMIN",  "KXLOWTMIN", "MSP", "MPX"),
    "det":  ("Detroit",        "KDTW", "KDTW", 42.212,  -83.349,  "KXHIGHTDET",  "KXLOWTDET", "DTW", "DTX"),
    "dca":  ("Washington DC",  "KDCA", "KDCA", 38.852,  -77.037,  "KXHIGHTDC",   "KXLOWTDC",  "DCA", "LWX"),  # Reagan National, NOT Dulles
    "sfo":  ("San Francisco",  "KSFO", "KSFO", 37.619,  -122.375, "KXHIGHTSFO",  "KXLOWTSFO", "SFO", "MTR"),
    "okc":  ("Oklahoma City",  "KOKC", "KOKC", 35.393,  -97.601,  "KXHIGHTOKC",  "KXLOWTOKC", "OKC", "OUN"),
}

# ── Model configurations ─────────────────────────────────────────────
MODELS = {
    "hrrr": {
        "name": "HRRR",
        "cycles": list(range(24)),  # hourly
        "max_fhr": 18,
        "resolution_km": 3,
        "ensemble": False,
        "bufkit_prefix": "hrrr",
    },
    "rap": {
        "name": "RAP",
        "cycles": list(range(24)),
        "max_fhr": 21,
        "resolution_km": 13,
        "ensemble": False,
        "bufkit_prefix": "rap",
    },
    "nam": {
        "name": "NAM",
        "cycles": [0, 6, 12, 18],
        "max_fhr": 84,
        "resolution_km": 12,
        "ensemble": False,
        "bufkit_prefix": "nam",
    },
    "nam3": {
        "name": "NAM Nest",
        "cycles": [0, 6, 12, 18],
        "max_fhr": 60,
        "resolution_km": 3,
        "ensemble": False,
        "bufkit_prefix": "namnest",
    },
    "gfs": {
        "name": "GFS",
        "cycles": [0, 6, 12, 18],
        "max_fhr": 384,
        "resolution_km": 25,
        "ensemble": False,
        "bufkit_prefix": "gfs3",
    },
    "sref": {
        "name": "SREF",
        "cycles": [3, 9, 15, 21],
        "max_fhr": 87,
        "resolution_km": 16,
        "ensemble": True,
        "members": 21,
        "bufkit_prefix": "sref",
    },
}

# ── URL templates ─────────────────────────────────────────────────────
# Penn State BUFKIT (latest runs) — verified working pattern:
#   http://www.meteo.psu.edu/bufkit/data/{MODEL_UPPER}/{cycle:02d}/{prefix}_{station_lower}.buf
# e.g. http://www.meteo.psu.edu/bufkit/data/HRRR/02/hrrr_kmdw.buf
PSU_BUFKIT = "http://www.meteo.psu.edu/bufkit/data/{model_upper}/{cycle:02d}/{prefix}_{station}.buf"

# Iowa State BUFKIT warehouse
ISU_BUFKIT = "https://mtarchive.geol.iastate.edu/{year}/{month:02d}/{day:02d}/bufkit/{cycle:02d}/{prefix}/{prefix}_{station}.buf"

# NOMADS GFS (grib filter for point extraction)
NOMADS_GFS = (
    "https://nomads.ncep.noaa.gov/cgi-bin/filter_gfs_0p25.pl"
    "?dir=%2Fgfs.{date}%2F{cycle:02d}%2Fatmos"
    "&file=gfs.t{cycle:02d}z.pgrb2.0p25.f{fhr:03d}"
    "&var_TMP=on&lev_2_m_above_ground=on"
    "&subregion=&toplat={lat_n}&leftlon={lon_w}&rightlon={lon_e}&bottomlat={lat_s}"
)

# NOMADS HRRR
NOMADS_HRRR = (
    "https://nomads.ncep.noaa.gov/cgi-bin/filter_hrrr_2d.pl"
    "?dir=%2Fhrrr.{date}%2Fconus"
    "&file=hrrr.t{cycle:02d}z.wrfsfcf{fhr:02d}.grib2"
    "&var_TMP=on&lev_2_m_above_ground=on"
    "&subregion=&toplat={lat_n}&leftlon={lon_w}&rightlon={lon_e}&bottomlat={lat_s}"
)

# NOMADS NAM
NOMADS_NAM = (
    "https://nomads.ncep.noaa.gov/cgi-bin/filter_nam.pl"
    "?dir=%2Fnam.{date}"
    "&file=nam.t{cycle:02d}z.awphys{fhr:02d}.tm00.grib2"
    "&var_TMP=on&lev_2_m_above_ground=on"
    "&subregion=&toplat={lat_n}&leftlon={lon_w}&rightlon={lon_e}&bottomlat={lat_s}"
)

# AWS S3 HRRR (via HTTPS)
AWS_HRRR = "https://noaa-hrrr-bdp-pds.s3.amazonaws.com/hrrr.{date}/conus/hrrr.t{cycle:02d}z.wrfsfcf{fhr:02d}.grib2"

CACHE_DIR = Path.home() / ".dsco" / "nwp_cache"
INGEST_DIR = Path.home() / ".dsco" / "nwp_ingest"
SNAPSHOT_DIR = INGEST_DIR / "payload_snapshots"
DB_PATH = Path(__file__).parent / "climate_data" / "nwp_forecasts.db"
SCHEMA_FIXTURE_PATH = Path(__file__).parent / "schemas" / "weather_api_schemas.json"
CHECKSUM_MANIFEST_PATH = INGEST_DIR / "checksum_manifest.json"
MAX_OBS_STALENESS_HOURS = 3
MAX_FETCH_RETRIES = 4
MAX_BACKOFF_SECONDS = 30
SOURCE_CIRCUIT_BREAKER_THRESHOLD = 3
SOURCE_CIRCUIT_BREAKER_TTL_SECONDS = 600

OBSERVATION_SOURCE_OVERRIDES = {
    "nyc": "KNYC",  # Central Park
    "chi": "KMDW",  # Midway settlement source
}

SETTLEMENT_STATION_OVERRIDES = {
    "nyc": "KNYC",
    "chi": "KMDW",
}

VARIABLE_FALLBACK_ORDER = {
    "high": ["hrrr", "nam3", "rap", "nam", "gfs", "sref"],
    "low": ["hrrr", "nam3", "rap", "nam", "gfs", "sref"],
}

MANDATORY_FORECAST_FIELDS = ("valid_utc", "fhr", "t2m_f")
NWS_POINT_SCHEMA = ("properties.gridId", "properties.gridX", "properties.gridY")
NWS_FORECAST_SCHEMA = ("properties.periods",)
NWS_OBSERVATION_SCHEMA = ("features",)
KALSHI_MARKET_SCHEMA = ("ticker",)
KALSHI_EVENT_SCHEMA = ("events",)

EXPECTED_SOURCE_FIELDS = {
    "bufkit.parsed_forecast": {
        "valid_utc", "fhr", "t2m_f", "td2m_f", "wspd_kt", "wdir",
    },
    "nws.observations.feature": {
        "properties.timestamp", "properties.temperature.value",
    },
    "hourly_observation": {
        "station", "observed_at_utc", "temp_f",
    },
}

OPTIONAL_SOURCE_FIELDS = {
    "bufkit.parsed_forecast": {"sfc_pres_mb", "pmsl_mb", "cape", "cin", "pwat_mm"},
    "nws.observations.feature": {
        "properties.dewpoint.value",
        "properties.windDirection.value",
        "properties.windSpeed.value",
    },
    "hourly_observation": {
        "dewpoint_f", "wind_dir", "wind_spd", "raw_payload", "raw_payload_sha256",
        "idempotency_key",
    },
}

VARIABLE_ALIAS_HINTS = {
    "t2m_f": {"tmp2m_f", "temperature_2m", "temperature2m", "TMP", "TMPC", "t2m"},
    "td2m_f": {"dewpoint_2m", "dewpoint2m", "DWPC", "d2m"},
    "wspd_kt": {"wind_speed", "windSpeed", "SKNT", "wspd"},
    "wdir": {"wind_direction", "windDirection", "DRCT"},
}

CHECKSUM_PROVIDER_POLICIES = {
    "bufkit": {"required": False, "algorithm": "sha256"},
    "nomads": {"required": False, "algorithm": "sha256"},
    "aws_hrrr": {"required": False, "algorithm": "sha256"},
    "nws": {"required": False, "algorithm": "sha256"},
    "generic": {"required": False, "algorithm": "sha256"},
}

PLAUSIBLE_TEMP_F_RANGE = (-100.0, 140.0)
PLAUSIBLE_DEWPOINT_F_RANGE = (-120.0, 95.0)
NWS_CELSIUS_UNIT_CODES = {"wmoUnit:degC", "unit:degC", "unit:degreesCelsius", "degC"}
NWS_DEGREE_UNIT_CODES = {"wmoUnit:degree_(angle)", "unit:degrees", "degree"}
NWS_WIND_SPEED_UNIT_CODES = {"wmoUnit:km_h-1", "unit:km_h-1", "unit:m_s-1", "m/s", "km/h"}

_CITY_MAPPING_ERRORS: list[str] = []


class ExternalSchemaValidationError(ValueError):
    """Raised when strict external payload schema validation fails."""


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _ensure_utc(dt: datetime) -> datetime:
    if dt.tzinfo is None:
        return dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def _coerce_datetime(value: Any) -> Optional[datetime]:
    if value is None:
        return None
    if isinstance(value, datetime):
        return _ensure_utc(value)
    if isinstance(value, str):
        try:
            return _ensure_utc(datetime.fromisoformat(value.replace("Z", "+00:00")))
        except Exception:
            return None
    return None


def _nested_value(data: Any, dotted_path: str) -> Any:
    cur = data
    for part in dotted_path.split("."):
        if isinstance(cur, dict):
            if part not in cur:
                return None
            cur = cur[part]
        else:
            return None
    return cur


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(result):
        return None
    return result


def _c_to_f(value: Any) -> Optional[float]:
    numeric = _safe_float(value)
    if numeric is None:
        return None
    return round((numeric * 9 / 5) + 32, 1)


def _range_error(name: str, value: Any, low: float, high: float) -> Optional[str]:
    numeric = _safe_float(value)
    if numeric is None:
        return f"{name}:not_numeric"
    if numeric < low or numeric > high:
        return f"{name}:out_of_range:{numeric:g}"
    return None


def _validate_required_fields(source: str, payload: Any, required_paths: tuple[str, ...]) -> list[str]:
    missing = [path for path in required_paths if _nested_value(payload, path) is None]
    if missing:
        print(f"  [!] {source} missing required fields: {', '.join(missing)}")
    return missing


def _strict_schema_enabled(strict: Optional[bool] = None) -> bool:
    if strict is not None:
        return strict
    return os.getenv("DSCO_NWP_STRICT_SCHEMA", "").lower() in {"1", "true", "yes", "on"}


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _make_idempotency_key(*parts: Any) -> str:
    canonical = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _station_for_city(city_key: str, purpose: str = "settlement") -> str:
    city = KALSHI_CITIES.get(city_key)
    if not city:
        raise KeyError(f"unknown city: {city_key}")
    if purpose == "observation":
        return OBSERVATION_SOURCE_OVERRIDES.get(city_key, city[1])
    if purpose == "settlement":
        return SETTLEMENT_STATION_OVERRIDES.get(city_key, city[1])
    if purpose == "bufkit":
        return city[2]
    raise ValueError(f"unknown station purpose: {purpose}")


def settlement_source_url(city_key: str) -> str:
    city = KALSHI_CITIES[city_key]
    return f"https://forecast.weather.gov/product.php?site={city[8]}&issuedby={city[7]}&product=CLI"


def validate_city_station_mappings() -> list[str]:
    errors = []
    for city_key, city in KALSHI_CITIES.items():
        expected_settle = SETTLEMENT_STATION_OVERRIDES.get(city_key, city[1])
        expected_obs = OBSERVATION_SOURCE_OVERRIDES.get(city_key, expected_settle)
        if city[1] != expected_settle:
            errors.append(f"{city_key}: settlement station {city[1]} != expected {expected_settle}")
        if _station_for_city(city_key, "observation") != expected_obs:
            errors.append(f"{city_key}: observation station {_station_for_city(city_key, 'observation')} != expected {expected_obs}")
    return errors


def fallback_models_for_variable(variable: str) -> list[str]:
    return list(VARIABLE_FALLBACK_ORDER.get(variable, VARIABLE_FALLBACK_ORDER["high"]))


def is_stale_observation(observed_at: Any, max_age_hours: int = MAX_OBS_STALENESS_HOURS) -> bool:
    ts = _coerce_datetime(observed_at)
    if ts is None:
        return True
    age = _utcnow() - ts
    return age > timedelta(hours=max_age_hours) or age < timedelta(minutes=-15)


def record_ingest_metric(model: str, city_key: str, source: str,
                         ok: Optional[bool] = None,
                         reason: str = "", stale: bool = False,
                         success_count: int = 0, failure_count: int = 0,
                         stale_count: int = 0) -> None:
    init_db()
    INGEST_DIR.mkdir(parents=True, exist_ok=True)
    day = _utcnow().strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("PRAGMA busy_timeout = 5000")
    if ok is not None:
        if ok:
            success_count += 1
        else:
            failure_count += 1
    if stale:
        stale_count += 1
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS ingest_metrics (
            model TEXT NOT NULL,
            city TEXT NOT NULL,
            source TEXT NOT NULL,
            day TEXT NOT NULL,
            success_count INTEGER NOT NULL DEFAULT 0,
            failure_count INTEGER NOT NULL DEFAULT 0,
            stale_count INTEGER NOT NULL DEFAULT 0,
            last_error TEXT,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (model, city, source, day)
        )
        """
    )
    conn.execute(
        """
        INSERT INTO ingest_metrics(model, city, source, day, success_count, failure_count, stale_count, last_error)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(model, city, source, day) DO UPDATE SET
            success_count = success_count + excluded.success_count,
            failure_count = failure_count + excluded.failure_count,
            stale_count = stale_count + excluded.stale_count,
            last_error = CASE
                WHEN excluded.last_error IS NULL OR excluded.last_error = '' THEN ingest_metrics.last_error
                ELSE excluded.last_error
            END,
            updated_at = CURRENT_TIMESTAMP
        """,
        (
            model,
            city_key,
            source,
            day,
            success_count,
            failure_count,
            stale_count,
            reason or None,
        ),
    )
    conn.commit()
    conn.close()


def record_source_latency(provider: str, source: str, latency_ms: float,
                          city_key: str = "") -> None:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS source_latency_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            provider TEXT NOT NULL,
            source TEXT NOT NULL,
            city TEXT,
            latency_ms REAL NOT NULL,
            captured_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
        """
    )
    conn.execute(
        "INSERT INTO source_latency_samples(provider, source, city, latency_ms) VALUES (?, ?, ?, ?)",
        (provider, source, city_key or None, float(latency_ms)),
    )
    conn.commit()
    conn.close()


def source_latency_distributions(provider: Optional[str] = None) -> list[dict[str, Any]]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        f"""
        SELECT provider, source,
               COUNT(*) AS n,
               AVG(latency_ms) AS avg_ms,
               MIN(latency_ms) AS min_ms,
               MAX(latency_ms) AS max_ms
        FROM source_latency_samples
        {"WHERE provider = ?" if provider else ""}
        GROUP BY provider, source
        ORDER BY provider, source
        """,
        (provider,) if provider else (),
    ).fetchall()
    conn.close()
    return [
        {
            "provider": row[0],
            "source": row[1],
            "count": row[2],
            "avg_ms": row[3],
            "min_ms": row[4],
            "max_ms": row[5],
        }
        for row in rows
    ]


def provider_health_score(provider: str, *, source: Optional[str] = None,
                          city_key: str = "", window_days: int = 1) -> dict[str, Any]:
    init_db()
    cutoff = (_utcnow() - timedelta(days=max(1, int(window_days)))).isoformat()
    conn = sqlite3.connect(str(DB_PATH))

    metric_params: list[Any] = [cutoff]
    metric_where = ["updated_at >= ?"]
    metric_where.append("(source = ? OR model = ?)")
    metric_params.extend([source or provider, provider])
    if city_key:
        metric_where.append("city = ?")
        metric_params.append(city_key)
    metric_row = conn.execute(
        f"""
        SELECT COALESCE(SUM(success_count), 0),
               COALESCE(SUM(failure_count), 0),
               COALESCE(SUM(stale_count), 0)
        FROM ingest_metrics
        WHERE {' AND '.join(metric_where)}
        """,
        tuple(metric_params),
    ).fetchone()

    fresh_params: list[Any] = [cutoff, f"%{source or provider}%"]
    fresh_where = ["created_at >= ?", "feed_name LIKE ?"]
    if city_key:
        fresh_where.append("city = ?")
        fresh_params.append(city_key)
    fresh_row = conn.execute(
        f"""
        SELECT COUNT(*), COALESCE(SUM(is_stale), 0)
        FROM feed_freshness_checks
        WHERE {' AND '.join(fresh_where)}
        """,
        tuple(fresh_params),
    ).fetchone()

    latency_params: list[Any] = [cutoff, provider]
    latency_where = ["captured_at >= ?", "provider = ?"]
    if source:
        latency_where.append("source = ?")
        latency_params.append(source)
    if city_key:
        latency_where.append("city = ?")
        latency_params.append(city_key)
    latency_row = conn.execute(
        f"""
        SELECT COUNT(*), AVG(latency_ms), MAX(latency_ms)
        FROM source_latency_samples
        WHERE {' AND '.join(latency_where)}
        """,
        tuple(latency_params),
    ).fetchone()
    conn.close()

    successes, failures, stale_metric = [int(x or 0) for x in metric_row]
    total_attempts = successes + failures
    success_rate = successes / total_attempts if total_attempts else 1.0

    freshness_n = int(fresh_row[0] or 0)
    freshness_stale = int(fresh_row[1] or 0)
    if freshness_n:
        stale_rate = freshness_stale / freshness_n
    else:
        stale_rate = stale_metric / max(1, successes + failures + stale_metric)

    latency_n = int(latency_row[0] or 0)
    avg_latency_ms = float(latency_row[1] or 0.0)
    max_latency_ms = float(latency_row[2] or 0.0)
    latency_score = 1.0 if latency_n == 0 else max(0.0, min(1.0, 1.0 - avg_latency_ms / 5000.0))

    score = round(100.0 * (0.55 * success_rate + 0.25 * (1.0 - stale_rate) + 0.20 * latency_score), 1)
    if failures >= 3 and successes == 0:
        status = "down"
    elif score >= 85:
        status = "healthy"
    elif score >= 60:
        status = "degraded"
    else:
        status = "down"

    reasons = []
    if success_rate < 0.95:
        reasons.append("provider_failures")
    if stale_rate > 0.0:
        reasons.append("stale_feed")
    if latency_n and avg_latency_ms > 2500:
        reasons.append("high_latency")
    if not reasons:
        reasons.append("within_sla")

    return {
        "provider": provider,
        "source": source,
        "city": city_key or None,
        "window_days": window_days,
        "score": score,
        "status": status,
        "success_rate": round(success_rate, 4),
        "stale_rate": round(stale_rate, 4),
        "avg_latency_ms": round(avg_latency_ms, 2),
        "max_latency_ms": round(max_latency_ms, 2),
        "counts": {
            "successes": successes,
            "failures": failures,
            "stale_metric": stale_metric,
            "freshness_checks": freshness_n,
            "freshness_stale": freshness_stale,
            "latency_samples": latency_n,
        },
        "reasons": reasons,
    }


def record_feed_freshness(feed_name: str, observed_at: Any, *,
                          city_key: str = "", sla_hours: int = 3) -> bool:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS feed_freshness_checks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            feed_name TEXT NOT NULL,
            city TEXT,
            observed_at_utc TEXT NOT NULL,
            checked_at_utc TEXT NOT NULL,
            sla_hours INTEGER NOT NULL,
            is_stale INTEGER NOT NULL,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
        """
    )
    obs = _coerce_datetime(observed_at)
    stale = obs is None or (_utcnow() - obs) > timedelta(hours=sla_hours)
    conn.execute(
        """
        INSERT INTO feed_freshness_checks(feed_name, city, observed_at_utc, checked_at_utc, sla_hours, is_stale)
        VALUES (?, ?, ?, ?, ?, ?)
        """,
        (
            feed_name,
            city_key or None,
            obs.isoformat() if obs else None,
            _utcnow().isoformat(),
            int(sla_hours),
            1 if stale else 0,
        ),
    )
    conn.commit()
    conn.close()
    return stale


def _seed_station_metadata_versions_into(conn: sqlite3.Connection) -> None:
    cols = {row[1] for row in conn.execute("PRAGMA table_info(station_metadata_versions)").fetchall()}
    if "source_url" not in cols:
        conn.execute("ALTER TABLE station_metadata_versions ADD COLUMN source_url TEXT")
    for row in station_metadata_config_rows():
        conn.execute(
            """
            UPDATE station_metadata_versions
            SET source_url = COALESCE(source_url, ?)
            WHERE city = ? AND station_role = ? AND station_icao = ?
            """,
            (row.get("source_url"), row["city"], row["station_role"], row["station_icao"]),
        )
    existing = conn.execute("SELECT COUNT(*) FROM station_metadata_versions").fetchone()[0]
    if existing:
        return
    for city_key, city in KALSHI_CITIES.items():
        entries = [
            ("settlement", _station_for_city(city_key, "settlement"), "settlement source", settlement_source_url(city_key)),
            ("observation", _station_for_city(city_key, "observation"), "observation source", f"https://api.weather.gov/stations/{_station_for_city(city_key, 'observation')}"),
            ("bufkit", _station_for_city(city_key, "bufkit"), "bufkit source", None),
        ]
        for role, station, notes, source_url in entries:
            conn.execute(
                """
                INSERT OR IGNORE INTO station_metadata_versions(city, station_role, station_icao, effective_from, effective_to, notes, source_url)
                VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (city_key, role, station, "1970-01-01T00:00:00Z", None, notes, source_url),
            )


def seed_station_metadata_versions() -> None:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    _seed_station_metadata_versions_into(conn)
    conn.commit()
    conn.close()


def current_station_metadata(city_key: str, role: str, as_of: Optional[datetime] = None) -> Optional[dict[str, Any]]:
    init_db()
    seed_station_metadata_versions()
    conn = sqlite3.connect(str(DB_PATH))
    ts = _ensure_utc(as_of or _utcnow()).isoformat()
    row = conn.execute(
        """
        SELECT city, station_role, station_icao, effective_from, effective_to, notes, source_url
        FROM station_metadata_versions
        WHERE city = ? AND station_role = ?
          AND effective_from <= ?
          AND (effective_to IS NULL OR effective_to > ?)
        ORDER BY effective_from DESC
        LIMIT 1
        """,
        (city_key, role, ts, ts),
    ).fetchone()
    conn.close()
    if not row:
        return None
    return {
        "city": row[0],
        "station_role": row[1],
        "station_icao": row[2],
        "effective_from": row[3],
        "effective_to": row[4],
        "notes": row[5],
        "source_url": row[6],
    }


def station_metadata_config_rows() -> list[dict[str, Any]]:
    rows = []
    for city_key, city in sorted(KALSHI_CITIES.items()):
        rows.extend([
            {
                "city": city_key,
                "station_role": "settlement",
                "station_icao": _station_for_city(city_key, "settlement"),
                "effective_from": "1970-01-01T00:00:00Z",
                "effective_to": None,
                "notes": "config settlement source",
                "source_url": settlement_source_url(city_key),
            },
            {
                "city": city_key,
                "station_role": "observation",
                "station_icao": _station_for_city(city_key, "observation"),
                "effective_from": "1970-01-01T00:00:00Z",
                "effective_to": None,
                "notes": "config observation source",
                "source_url": f"https://api.weather.gov/stations/{_station_for_city(city_key, 'observation')}",
            },
            {
                "city": city_key,
                "station_role": "bufkit",
                "station_icao": _station_for_city(city_key, "bufkit"),
                "effective_from": "1970-01-01T00:00:00Z",
                "effective_to": None,
                "notes": "config bufkit source",
                "source_url": None,
            },
        ])
    return rows


def fetch_live_nws_station_metadata(station_icao: str) -> dict[str, Any]:
    url = f"https://api.weather.gov/stations/{station_icao}"

    def _call():
        r = requests.get(url, headers=HEADERS_NWS, timeout=15)
        r.raise_for_status()
        payload = r.json()
        persist_payload_snapshot(
            "nws.station_metadata",
            r.content,
            city_key=station_icao,
            payload_type="json",
            metadata={"url": url, "status": r.status_code},
        )
        return payload

    payload = _retryable_fetch(_call, source=url)
    if not isinstance(payload, dict):
        raise RuntimeError(f"unable to fetch NWS station metadata for {station_icao}")
    return payload


def _station_metadata_lat_lon(payload: dict[str, Any]) -> tuple[Optional[float], Optional[float]]:
    coords = _nested_value(payload, "geometry.coordinates")
    if isinstance(coords, list) and len(coords) >= 2:
        try:
            return float(coords[1]), float(coords[0])
        except (TypeError, ValueError):
            return None, None
    return None, None


def validate_live_station_metadata(city_keys: Optional[list[str]] = None,
                                   roles: Optional[list[str]] = None,
                                   fetcher=None,
                                   max_distance_deg: float = 1.0) -> list[dict[str, Any]]:
    init_db()
    seed_station_metadata_versions()
    city_keys = city_keys or sorted(KALSHI_CITIES.keys())
    roles = roles or ["settlement", "observation"]
    fetcher = fetcher or fetch_live_nws_station_metadata
    results = []

    conn = sqlite3.connect(str(DB_PATH))
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS station_live_validation (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            station_role TEXT NOT NULL,
            station_icao TEXT NOT NULL,
            ok INTEGER NOT NULL,
            name TEXT,
            timezone TEXT,
            latitude REAL,
            longitude REAL,
            errors_json TEXT NOT NULL,
            checked_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
        """
    )
    for city_key in city_keys:
        city = KALSHI_CITIES[city_key]
        for role in roles:
            station = _station_for_city(city_key, role)
            errors = []
            name = tz = None
            lat = lon = None
            try:
                payload = fetcher(station)
                station_id = str(_nested_value(payload, "properties.stationIdentifier") or "")
                if station_id and station_id.upper() != station.upper():
                    errors.append(f"station_identifier_mismatch:{station_id}")
                name = _nested_value(payload, "properties.name")
                tz = _nested_value(payload, "properties.timeZone")
                lat, lon = _station_metadata_lat_lon(payload)
                if lat is None or lon is None:
                    errors.append("missing_geometry")
                elif abs(lat - float(city[3])) > max_distance_deg or abs(lon - float(city[4])) > max_distance_deg:
                    errors.append("location_distance_exceeds_threshold")
            except Exception as exc:
                errors.append(f"fetch_error:{exc}")
            ok = not errors
            result = {
                "city": city_key,
                "station_role": role,
                "station_icao": station,
                "ok": ok,
                "errors": errors,
                "name": name,
                "timezone": tz,
                "latitude": lat,
                "longitude": lon,
            }
            results.append(result)
            conn.execute(
                """
                INSERT INTO station_live_validation(
                    city, station_role, station_icao, ok, name, timezone,
                    latitude, longitude, errors_json
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    city_key,
                    role,
                    station,
                    1 if ok else 0,
                    name,
                    tz,
                    lat,
                    lon,
                    json.dumps(errors, sort_keys=True),
                ),
            )
    conn.commit()
    conn.close()
    return results


def station_metadata_diff(as_of: Optional[datetime] = None) -> dict[str, Any]:
    init_db()
    seed_station_metadata_versions()
    ts = _ensure_utc(as_of or _utcnow())
    expected = {(row["city"], row["station_role"]): row for row in station_metadata_config_rows()}
    mismatches = []

    for key, expected_row in expected.items():
        actual = current_station_metadata(expected_row["city"], expected_row["station_role"], ts)
        if actual is None:
            mismatches.append({
                "type": "missing_db_row",
                "city": expected_row["city"],
                "station_role": expected_row["station_role"],
                "expected_station": expected_row["station_icao"],
                "actual_station": None,
            })
            continue
        if actual["station_icao"] != expected_row["station_icao"]:
            mismatches.append({
                "type": "station_mismatch",
                "city": expected_row["city"],
                "station_role": expected_row["station_role"],
                "expected_station": expected_row["station_icao"],
                "actual_station": actual["station_icao"],
                "effective_from": actual["effective_from"],
                "effective_to": actual["effective_to"],
            })
        if actual.get("source_url") != expected_row.get("source_url"):
            mismatches.append({
                "type": "source_url_mismatch",
                "city": expected_row["city"],
                "station_role": expected_row["station_role"],
                "expected_source_url": expected_row.get("source_url"),
                "actual_source_url": actual.get("source_url"),
                "effective_from": actual["effective_from"],
                "effective_to": actual["effective_to"],
            })

    conn = sqlite3.connect(str(DB_PATH))
    active_rows = conn.execute(
        """
        SELECT city, station_role, station_icao, effective_from, effective_to
        FROM station_metadata_versions
        WHERE effective_from <= ?
          AND (effective_to IS NULL OR effective_to > ?)
        """,
        (ts.isoformat(), ts.isoformat()),
    ).fetchall()
    conn.close()
    for city, role, station, effective_from, effective_to in active_rows:
        if (city, role) not in expected:
            mismatches.append({
                "type": "extra_db_row",
                "city": city,
                "station_role": role,
                "expected_station": None,
                "actual_station": station,
                "effective_from": effective_from,
                "effective_to": effective_to,
            })

    return {
        "checked_at": _utcnow().isoformat(),
        "as_of": ts.isoformat(),
        "ok": not mismatches,
        "mismatch_count": len(mismatches),
        "mismatches": mismatches,
    }


def export_station_metadata(path: Optional[Path] = None) -> dict[str, Any]:
    init_db()
    seed_station_metadata_versions()
    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        """
        SELECT city, station_role, station_icao, effective_from, effective_to, notes, source_url
        FROM station_metadata_versions
        ORDER BY city, station_role, effective_from
        """
    ).fetchall()
    conn.close()
    data = {
        "version": 1,
        "exported_at": _utcnow().isoformat(),
        "rows": [
            {
                "city": row[0],
                "station_role": row[1],
                "station_icao": row[2],
                "effective_from": row[3],
                "effective_to": row[4],
                "notes": row[5],
                "source_url": row[6],
            }
            for row in rows
        ],
    }
    if path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    return data


def import_station_metadata(data_or_path: Any, *, dry_run: bool = True) -> dict[str, Any]:
    if isinstance(data_or_path, (str, Path)):
        data = json.loads(Path(data_or_path).read_text())
    else:
        data = data_or_path
    rows = data.get("rows", []) if isinstance(data, dict) else []
    required = {"city", "station_role", "station_icao", "effective_from"}
    preview = []
    errors = []

    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    for idx, row in enumerate(rows):
        missing = sorted(required - set(row))
        if missing:
            errors.append({"row": idx, "missing": missing})
            continue
        existing = conn.execute(
            """
            SELECT station_icao, effective_to, notes
            FROM station_metadata_versions
            WHERE city = ? AND station_role = ? AND effective_from = ?
            """,
            (row["city"], row["station_role"], row["effective_from"]),
        ).fetchone()
        action = "insert" if existing is None else "update"
        preview.append({
            "action": action,
            "city": row["city"],
            "station_role": row["station_role"],
            "station_icao": row["station_icao"],
            "effective_from": row["effective_from"],
        })
        if not dry_run:
            conn.execute(
                """
                INSERT INTO station_metadata_versions(city, station_role, station_icao, effective_from, effective_to, notes, source_url)
                VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(city, station_role, effective_from) DO UPDATE SET
                    station_icao = excluded.station_icao,
                    effective_to = excluded.effective_to,
                    notes = excluded.notes,
                    source_url = excluded.source_url,
                    updated_at = CURRENT_TIMESTAMP
                """,
                (
                    row["city"],
                    row["station_role"],
                    row["station_icao"],
                    row["effective_from"],
                    row.get("effective_to"),
                    row.get("notes"),
                    row.get("source_url"),
                ),
            )
    if not dry_run:
        conn.commit()
    conn.close()
    return {
        "dry_run": dry_run,
        "row_count": len(rows),
        "preview": preview,
        "errors": errors,
    }


def _payload_snapshot_dir(source: str) -> Path:
    path = SNAPSHOT_DIR / source
    path.mkdir(parents=True, exist_ok=True)
    return path


def persist_payload_snapshot(source: str, payload: bytes, *,
                             city_key: str = "", model: str = "",
                             payload_type: str = "raw",
                             metadata: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    init_db()
    SNAPSHOT_DIR.mkdir(parents=True, exist_ok=True)
    sha = _sha256_bytes(payload)
    day = _utcnow().strftime("%Y%m%dT%H%M%SZ")
    filename = f"{day}_{sha[:16]}.bin"
    path = _payload_snapshot_dir(source) / filename
    if not path.exists():
        path.write_bytes(payload)

    conn = sqlite3.connect(str(DB_PATH))
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS payload_snapshots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source TEXT NOT NULL,
            city TEXT,
            model TEXT,
            payload_type TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            byte_len INTEGER NOT NULL,
            path TEXT NOT NULL,
            metadata_json TEXT,
            captured_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
        """
    )
    conn.execute(
        """
        INSERT INTO payload_snapshots(source, city, model, payload_type, sha256, byte_len, path, metadata_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            source,
            city_key or None,
            model or None,
            payload_type,
            sha,
            len(payload),
            str(path),
            json.dumps(metadata or {}, sort_keys=True),
        ),
    )
    conn.commit()
    conn.close()
    return {"sha256": sha, "path": str(path), "byte_len": len(payload)}


def payload_snapshot_rows(source: Optional[str] = None, *, limit: int = 100) -> list[dict[str, Any]]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    limit = max(1, min(int(limit), 1000))
    if source:
        rows = conn.execute(
            """
            SELECT id, source, city, model, payload_type, sha256, byte_len, path,
                   metadata_json, captured_at
            FROM payload_snapshots
            WHERE source = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (source, limit),
        ).fetchall()
    else:
        rows = conn.execute(
            """
            SELECT id, source, city, model, payload_type, sha256, byte_len, path,
                   metadata_json, captured_at
            FROM payload_snapshots
            ORDER BY id DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    conn.close()
    return [
        {
            "id": row[0],
            "source": row[1],
            "city": row[2],
            "model": row[3],
            "payload_type": row[4],
            "sha256": row[5],
            "byte_len": row[6],
            "path": row[7],
            "metadata": json.loads(row[8] or "{}"),
            "captured_at": row[9],
        }
        for row in rows
    ]


def replay_payload_snapshot(snapshot_id: int, *, dry_run: bool = True) -> dict[str, Any]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    row = conn.execute(
        """
        SELECT id, source, city, model, payload_type, sha256, byte_len, path, metadata_json, captured_at
        FROM payload_snapshots
        WHERE id = ?
        """,
        (snapshot_id,),
    ).fetchone()
    conn.close()
    if not row:
        raise KeyError(f"snapshot id not found: {snapshot_id}")

    snapshot = {
        "id": row[0],
        "source": row[1],
        "city": row[2],
        "model": row[3],
        "payload_type": row[4],
        "sha256": row[5],
        "byte_len": row[6],
        "path": row[7],
        "metadata": json.loads(row[8] or "{}"),
        "captured_at": row[9],
    }
    raw = Path(snapshot["path"]).read_bytes()
    checksum = validate_artifact_checksum(snapshot["source"], raw)
    result: dict[str, Any] = {
        "snapshot": snapshot,
        "checksum": checksum,
        "dry_run": dry_run,
        "actions": [],
    }

    if checksum["actual"] != snapshot["sha256"]:
        result["actions"].append({"type": "checksum_mismatch", "stored": snapshot["sha256"], "actual": checksum["actual"]})
        return result

    source = snapshot["source"]
    metadata_url = str(snapshot["metadata"].get("url", ""))
    source_hint = f"{source} {metadata_url}".lower()
    if "bufkit" in source_hint or source_hint.endswith(".buf"):
        forecasts = parse_bufkit(raw, snapshot.get("model") or "unknown")
        drift = record_source_drift(
            "bufkit.parsed_forecast",
            forecasts[:20],
            provider="bufkit",
            model=snapshot.get("model") or "",
            city_key=snapshot.get("city") or "",
        ) if forecasts else {"drift": True, "missing_fields": ["parsed_forecasts"], "severity": "critical"}
        result["actions"].append({
            "type": "parse_bufkit",
            "forecast_rows": len(forecasts),
            "drift": drift,
        })
    elif snapshot["payload_type"] == "json" or source.startswith("nws."):
        payload = json.loads(raw.decode("utf-8"))
        fixture_errors = validate_payload_against_fixture(source, payload)
        result["actions"].append({
            "type": "validate_json",
            "source": source,
            "fixture_errors": fixture_errors,
        })
    else:
        result["actions"].append({"type": "read_only", "reason": "no_replay_adapter"})
    return result


def prune_payload_snapshots(older_than_days: int, *,
                            source: Optional[str] = None,
                            dry_run: bool = True) -> dict[str, Any]:
    init_db()
    cutoff = _utcnow() - timedelta(days=max(0, int(older_than_days)))
    conn = sqlite3.connect(str(DB_PATH))
    if source:
        rows = conn.execute(
            """
            SELECT id, path, byte_len
            FROM payload_snapshots
            WHERE source = ? AND captured_at < ?
            """,
            (source, cutoff.isoformat()),
        ).fetchall()
    else:
        rows = conn.execute(
            """
            SELECT id, path, byte_len
            FROM payload_snapshots
            WHERE captured_at < ?
            """,
            (cutoff.isoformat(),),
        ).fetchall()

    deleted_files = 0
    reclaimed_bytes = 0
    ids = [row[0] for row in rows]
    if not dry_run:
        for _, path_s, byte_len in rows:
            path = Path(path_s)
            if path.exists():
                path.unlink()
                deleted_files += 1
            reclaimed_bytes += int(byte_len or 0)
        if ids:
            placeholders = ",".join("?" for _ in ids)
            conn.execute(f"DELETE FROM payload_snapshots WHERE id IN ({placeholders})", ids)
            conn.commit()
    else:
        reclaimed_bytes = sum(int(row[2] or 0) for row in rows)
    conn.close()
    return {
        "dry_run": dry_run,
        "cutoff_utc": cutoff.isoformat(),
        "candidate_rows": len(rows),
        "deleted_files": deleted_files,
        "reclaimed_bytes": reclaimed_bytes,
        "ids": ids,
    }


def _circuit_breaker_table(conn: sqlite3.Connection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS ingest_circuit_breaker (
            source TEXT PRIMARY KEY,
            failure_count INTEGER NOT NULL DEFAULT 0,
            opened_until TEXT,
            last_failure TEXT,
            last_success TEXT
        )
        """
    )


def _circuit_breaker_allows(source: str) -> bool:
    conn = sqlite3.connect(str(DB_PATH))
    _circuit_breaker_table(conn)
    row = conn.execute(
        "SELECT failure_count, opened_until FROM ingest_circuit_breaker WHERE source = ?",
        (source,),
    ).fetchone()
    conn.close()
    if not row:
        return True
    opened_until = _coerce_datetime(row[1])
    return opened_until is None or opened_until <= _utcnow()


def _circuit_breaker_record(source: str, ok: bool) -> None:
    conn = sqlite3.connect(str(DB_PATH))
    _circuit_breaker_table(conn)
    row = conn.execute(
        "SELECT failure_count FROM ingest_circuit_breaker WHERE source = ?",
        (source,),
    ).fetchone()
    failures = int(row[0]) if row else 0
    if ok:
        conn.execute(
            """
            INSERT INTO ingest_circuit_breaker(source, failure_count, opened_until, last_success)
            VALUES (?, 0, NULL, ?)
            ON CONFLICT(source) DO UPDATE SET
                failure_count = 0,
                opened_until = NULL,
                last_success = excluded.last_success
            """,
            (source, _utcnow().isoformat()),
        )
    else:
        failures += 1
        opened_until = _utcnow().isoformat() if failures >= SOURCE_CIRCUIT_BREAKER_THRESHOLD else None
        if opened_until is not None:
            opened_until = (_utcnow() + timedelta(seconds=SOURCE_CIRCUIT_BREAKER_TTL_SECONDS)).isoformat()
        conn.execute(
            """
            INSERT INTO ingest_circuit_breaker(source, failure_count, opened_until, last_failure)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(source) DO UPDATE SET
                failure_count = excluded.failure_count,
                opened_until = excluded.opened_until,
                last_failure = excluded.last_failure
            """,
            (source, failures, opened_until, _utcnow().isoformat()),
        )
    conn.commit()
    conn.close()


def _sleep_with_jitter(base_seconds: float, attempt: int) -> None:
    delay = min(MAX_BACKOFF_SECONDS, base_seconds * (2 ** attempt))
    delay += random.uniform(0.0, max(0.1, delay * 0.2))
    time.sleep(delay)


def _retryable_fetch(callable_fn, *, source: str, base_seconds: float = 0.5):
    if not _circuit_breaker_allows(source):
        return None
    for attempt in range(MAX_FETCH_RETRIES):
        try:
            result = callable_fn()
            if result is None:
                raise RuntimeError("empty response")
            _circuit_breaker_record(source, True)
            return result
        except Exception:
            _circuit_breaker_record(source, False)
            if attempt >= MAX_FETCH_RETRIES - 1:
                return None
            _sleep_with_jitter(base_seconds, attempt)
    return None


def dry_run_forecast_diff(existing: Optional[dict[str, Any]], incoming: dict[str, Any]) -> dict[str, Any]:
    diff = {"insert": existing is None, "changes": {}}
    if existing:
        for key in sorted(set(existing) | set(incoming)):
            if existing.get(key) != incoming.get(key):
                diff["changes"][key] = {"before": existing.get(key), "after": incoming.get(key)}
    else:
        diff["changes"] = {key: {"before": None, "after": incoming.get(key)} for key in sorted(incoming)}
    return diff


def _validate_forecast_record(model: str, forecast: dict[str, Any]) -> list[str]:
    missing = [field for field in MANDATORY_FORECAST_FIELDS if forecast.get(field) is None]
    if missing:
        return missing
    ts = _coerce_datetime(forecast.get("valid_utc"))
    if ts is None:
        return ["valid_utc"]
    if forecast.get("fhr") is None:
        return ["fhr"]
    return []


def validate_nws_observation_feature(feature: Any) -> list[str]:
    """Validate NWS station observation feature units and physical ranges."""
    if not isinstance(feature, dict):
        return ["feature:not_object"]
    props = feature.get("properties", {})
    if not isinstance(props, dict):
        return ["properties:not_object"]

    errors: list[str] = []
    if _coerce_datetime(props.get("timestamp")) is None:
        errors.append("timestamp:missing_or_invalid")

    checks = [
        ("temperature", "temp_f", PLAUSIBLE_TEMP_F_RANGE),
        ("dewpoint", "dewpoint_f", PLAUSIBLE_DEWPOINT_F_RANGE),
    ]
    for nws_field, out_field, bounds in checks:
        value = _nested_value(props, f"{nws_field}.value")
        if value is None:
            continue
        unit = _nested_value(props, f"{nws_field}.unitCode")
        if unit not in NWS_CELSIUS_UNIT_CODES:
            errors.append(f"{nws_field}:unexpected_unit:{unit or 'missing'}")
            continue
        temp_f = _c_to_f(value)
        range_err = _range_error(out_field, temp_f, bounds[0], bounds[1])
        if range_err:
            errors.append(range_err)

    wind_dir = _nested_value(props, "windDirection.value")
    if wind_dir is not None:
        unit = _nested_value(props, "windDirection.unitCode")
        if unit and unit not in NWS_DEGREE_UNIT_CODES:
            errors.append(f"windDirection:unexpected_unit:{unit}")
        numeric = _safe_float(wind_dir)
        if numeric is None:
            errors.append("wind_dir:not_numeric")
        elif numeric < 0.0 or numeric > 360.0:
            errors.append(f"wind_dir:out_of_range:{numeric:g}")

    wind_spd = _nested_value(props, "windSpeed.value")
    if wind_spd is not None:
        unit = _nested_value(props, "windSpeed.unitCode")
        if unit and unit not in NWS_WIND_SPEED_UNIT_CODES:
            errors.append(f"windSpeed:unexpected_unit:{unit}")
        numeric = _safe_float(wind_spd)
        if numeric is None:
            errors.append("wind_spd:not_numeric")
        elif numeric < 0.0:
            errors.append(f"wind_spd:negative:{numeric:g}")

    return errors


def validate_hourly_observation_record(city_key: str, station_icao: str,
                                       observation: dict[str, Any]) -> list[str]:
    """Validate normalized observation rows before ingest writes."""
    errors: list[str] = []
    if city_key not in KALSHI_CITIES:
        errors.append("city:unknown")
    expected_station = station_icao.upper()
    observed_station = str(observation.get("station") or station_icao).upper()
    if observed_station != expected_station:
        errors.append(f"station:mismatch:{observed_station}")
    if _coerce_datetime(observation.get("observed_at_utc")) is None:
        errors.append("observed_at_utc:missing_or_invalid")

    temp_f = observation.get("temp_f")
    if temp_f is not None:
        range_err = _range_error("temp_f", temp_f, *PLAUSIBLE_TEMP_F_RANGE)
        if range_err:
            errors.append(range_err)
    dewpoint_f = observation.get("dewpoint_f")
    if dewpoint_f is not None:
        range_err = _range_error("dewpoint_f", dewpoint_f, *PLAUSIBLE_DEWPOINT_F_RANGE)
        if range_err:
            errors.append(range_err)
    if temp_f is not None and dewpoint_f is not None:
        temp_numeric = _safe_float(temp_f)
        dew_numeric = _safe_float(dewpoint_f)
        if temp_numeric is not None and dew_numeric is not None and dew_numeric > temp_numeric + 2.0:
            errors.append("dewpoint_f:above_temperature")

    wind_dir = observation.get("wind_dir")
    if wind_dir is not None:
        numeric = _safe_float(wind_dir)
        if numeric is None:
            errors.append("wind_dir:not_numeric")
        elif numeric < 0.0 or numeric > 360.0:
            errors.append(f"wind_dir:out_of_range:{numeric:g}")

    wind_spd = observation.get("wind_spd")
    if wind_spd is not None:
        numeric = _safe_float(wind_spd)
        if numeric is None:
            errors.append("wind_spd:not_numeric")
        elif numeric < 0.0:
            errors.append(f"wind_spd:negative:{numeric:g}")

    raw_payload = observation.get("raw_payload")
    if raw_payload is not None:
        errors.extend(validate_nws_observation_feature(raw_payload))
    return errors


def _forecast_idempotency_key(city_key: str, model: str, init_cycle: str, fhr: int) -> str:
    return _make_idempotency_key("forecast", city_key, model, init_cycle, fhr)


def _observation_idempotency_key(station: str, observed_at: datetime) -> str:
    return _make_idempotency_key("observation", station, _ensure_utc(observed_at).isoformat())


def _validate_external_json(source: str, payload: Any, schema: tuple[str, ...],
                            strict: Optional[bool] = None) -> bool:
    missing = _validate_required_fields(source, payload, schema)
    if missing and _strict_schema_enabled(strict):
        raise ExternalSchemaValidationError(
            f"{source} missing required fields: {', '.join(missing)}"
        )
    return not missing


def load_schema_fixtures(path: Optional[Path] = None) -> dict[str, Any]:
    fixture_path = path or SCHEMA_FIXTURE_PATH
    if not fixture_path.exists():
        return {}
    return json.loads(fixture_path.read_text())


def validate_payload_against_fixture(source: str, payload: Any,
                                     fixtures: Optional[dict[str, Any]] = None,
                                     strict: Optional[bool] = None) -> list[str]:
    data = fixtures if fixtures is not None else load_schema_fixtures()
    spec = data.get(source, {})
    required = tuple(spec.get("required_paths", ()))
    missing = _validate_required_fields(source, payload, required) if required else []
    if missing and _strict_schema_enabled(strict):
        raise ExternalSchemaValidationError(
            f"{source} missing required fields: {', '.join(missing)}"
        )
    return missing


def _checksum_provider_for_source(source: str) -> str:
    low = source.lower()
    if "bufkit" in low or low.endswith(".buf"):
        return "bufkit"
    if "nomads" in low or ".grib2" in low:
        return "nomads"
    if "noaa-hrrr" in low or "s3.amazonaws.com/hrrr" in low:
        return "aws_hrrr"
    if "api.weather.gov" in low or low.startswith("nws."):
        return "nws"
    return "generic"


def load_checksum_manifest(path: Optional[Path] = None) -> dict[str, Any]:
    manifest_path = path or CHECKSUM_MANIFEST_PATH
    if not manifest_path.exists():
        return {"providers": {}, "artifacts": {}}
    with manifest_path.open() as f:
        data = json.load(f)
    if "artifacts" not in data and "providers" not in data:
        data = {"providers": {}, "artifacts": data}
    data.setdefault("providers", {})
    data.setdefault("artifacts", {})
    return data


def checksum_policy_for_source(source: str, manifest: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    provider = _checksum_provider_for_source(source)
    policy = dict(CHECKSUM_PROVIDER_POLICIES.get(provider, CHECKSUM_PROVIDER_POLICIES["generic"]))
    if manifest:
        policy.update(manifest.get("providers", {}).get(provider, {}))
    policy["provider"] = provider
    return policy


def _artifact_manifest_entry(source: str, manifest: dict[str, Any]) -> Optional[dict[str, Any]]:
    artifacts = manifest.get("artifacts", {})
    entry = artifacts.get(source)
    if isinstance(entry, str):
        return {"sha256": entry}
    if isinstance(entry, dict):
        return entry
    source_name = Path(source).name
    entry = artifacts.get(source_name)
    if isinstance(entry, str):
        return {"sha256": entry}
    if isinstance(entry, dict):
        return entry
    return None


def validate_artifact_checksum(source: str, payload: bytes,
                               manifest: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    manifest = manifest if manifest is not None else load_checksum_manifest()
    policy = checksum_policy_for_source(source, manifest)
    actual = _sha256_bytes(payload)
    entry = _artifact_manifest_entry(source, manifest)
    expected = entry.get("sha256") if entry else None

    if expected:
        ok = actual.lower() == str(expected).lower()
        return {
            "source": source,
            "provider": policy["provider"],
            "algorithm": "sha256",
            "status": "ok" if ok else "mismatch",
            "ok": ok,
            "required": bool(policy.get("required")),
            "expected": expected,
            "actual": actual,
        }

    required = bool(policy.get("required"))
    return {
        "source": source,
        "provider": policy["provider"],
        "algorithm": "sha256",
        "status": "missing_manifest" if required else "unchecked",
        "ok": not required,
        "required": required,
        "expected": None,
        "actual": actual,
    }


def _flatten_payload_fields(payload: Any, prefix: str = "", *,
                            max_list_items: int = 20,
                            max_fields: int = 2000) -> set[str]:
    fields: set[str] = set()

    def walk(value: Any, path: str) -> None:
        if len(fields) >= max_fields:
            return
        if isinstance(value, dict):
            for key, child in value.items():
                key_s = str(key)
                next_path = f"{path}.{key_s}" if path else key_s
                fields.add(next_path)
                walk(child, next_path)
        elif isinstance(value, list):
            for item in value[:max_list_items]:
                walk(item, path)

    walk(payload, prefix)
    return fields


def detect_source_drift(source: str, payload: Any, *,
                        expected_fields: Optional[set[str]] = None,
                        optional_fields: Optional[set[str]] = None) -> dict[str, Any]:
    expected = set(expected_fields if expected_fields is not None else EXPECTED_SOURCE_FIELDS.get(source, set()))
    optional = set(optional_fields if optional_fields is not None else OPTIONAL_SOURCE_FIELDS.get(source, set()))
    observed = _flatten_payload_fields(payload)
    missing = sorted(field for field in expected if field not in observed)
    allowed = expected | optional
    added = sorted(field for field in observed if field not in allowed)

    alias_hits = {}
    leaf_names = {field.split(".")[-1] for field in observed}
    for canonical, aliases in VARIABLE_ALIAS_HINTS.items():
        hits = sorted((observed | leaf_names) & aliases)
        if canonical in expected and canonical in missing and hits:
            alias_hits[canonical] = hits

    drift = bool(missing or alias_hits)
    severity = "critical" if missing else ("warning" if alias_hits else "ok")
    sample_bytes = json.dumps(payload, sort_keys=True, default=str).encode("utf-8")
    return {
        "source": source,
        "drift": drift,
        "severity": severity,
        "missing_fields": missing,
        "added_fields": added[:100],
        "alias_hits": alias_hits,
        "observed_field_count": len(observed),
        "observed_fields_hash": _sha256_bytes("\n".join(sorted(observed)).encode("utf-8")),
        "sample_sha256": _sha256_bytes(sample_bytes),
    }


def _source_drift_table(conn: sqlite3.Connection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS source_drift_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source TEXT NOT NULL,
            provider TEXT,
            model TEXT,
            city TEXT,
            severity TEXT NOT NULL,
            missing_fields_json TEXT NOT NULL,
            added_fields_json TEXT NOT NULL,
            alias_hits_json TEXT NOT NULL,
            observed_fields_hash TEXT NOT NULL,
            sample_sha256 TEXT NOT NULL,
            sample_json TEXT,
            detected_at TEXT DEFAULT CURRENT_TIMESTAMP,
            resolved_at TEXT
        )
        """
    )


def record_source_drift(source: str, payload: Any, *,
                        provider: str = "", model: str = "", city_key: str = "",
                        expected_fields: Optional[set[str]] = None,
                        optional_fields: Optional[set[str]] = None,
                        record_ok: bool = False) -> dict[str, Any]:
    report = detect_source_drift(
        source,
        payload,
        expected_fields=expected_fields,
        optional_fields=optional_fields,
    )
    if not report["drift"] and not record_ok:
        return report

    init_db()
    sample_json = json.dumps(payload, sort_keys=True, default=str)
    if len(sample_json) > 8000:
        sample_json = sample_json[:8000] + "...[truncated]"
    conn = sqlite3.connect(str(DB_PATH))
    _source_drift_table(conn)
    conn.execute(
        """
        INSERT INTO source_drift_events(
            source, provider, model, city, severity, missing_fields_json,
            added_fields_json, alias_hits_json, observed_fields_hash,
            sample_sha256, sample_json
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            source,
            provider or None,
            model or None,
            city_key or None,
            report["severity"],
            json.dumps(report["missing_fields"], sort_keys=True),
            json.dumps(report["added_fields"], sort_keys=True),
            json.dumps(report["alias_hits"], sort_keys=True),
            report["observed_fields_hash"],
            report["sample_sha256"],
            sample_json,
        ),
    )
    conn.commit()
    conn.close()
    return report


def source_drift_report(source: Optional[str] = None, *, limit: int = 50) -> list[dict[str, Any]]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    _source_drift_table(conn)
    limit = max(1, min(int(limit), 500))
    if source:
        rows = conn.execute(
            """
            SELECT id, source, provider, model, city, severity, missing_fields_json,
                   added_fields_json, alias_hits_json, observed_fields_hash,
                   sample_sha256, detected_at, resolved_at
            FROM source_drift_events
            WHERE source = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (source, limit),
        ).fetchall()
    else:
        rows = conn.execute(
            """
            SELECT id, source, provider, model, city, severity, missing_fields_json,
                   added_fields_json, alias_hits_json, observed_fields_hash,
                   sample_sha256, detected_at, resolved_at
            FROM source_drift_events
            ORDER BY id DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    conn.close()
    return [
        {
            "id": row[0],
            "source": row[1],
            "provider": row[2],
            "model": row[3],
            "city": row[4],
            "severity": row[5],
            "missing_fields": json.loads(row[6] or "[]"),
            "added_fields": json.loads(row[7] or "[]"),
            "alias_hits": json.loads(row[8] or "{}"),
            "observed_fields_hash": row[9],
            "sample_sha256": row[10],
            "detected_at": row[11],
            "resolved_at": row[12],
        }
        for row in rows
    ]


def _ensure_dirs():
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    INGEST_DIR.mkdir(parents=True, exist_ok=True)
    SNAPSHOT_DIR.mkdir(parents=True, exist_ok=True)
    for model in MODELS:
        (CACHE_DIR / model).mkdir(exist_ok=True)


def _fetch(url: str, timeout: int = 30) -> Optional[bytes]:
    """Fetch URL with retries and User-Agent."""
    headers = {"User-Agent": "dsco-nwp/1.0 (weather-trading-research)"}
    req = urllib.request.Request(url, headers=headers)

    def _call():
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            checksum = validate_artifact_checksum(url, raw)
            if not checksum["ok"]:
                raise RuntimeError(f"checksum validation failed: {checksum['status']} for {url}")
            persist_payload_snapshot(
                url,
                raw,
                payload_type="binary",
                metadata={
                    "url": url,
                    "status": getattr(resp, "status", None),
                    "checksum": checksum,
                },
            )
            return raw

    return _retryable_fetch(_call, source=url)


# ══════════════════════════════════════════════════════════════════════
#  BUFKIT PARSER
# ══════════════════════════════════════════════════════════════════════

def parse_bufkit(raw: bytes, model: str = "hrrr") -> list[dict]:
    """
    Parse a BUFKIT sounding file and extract surface temperature forecasts.

    BUFKIT format (per forecast hour block):
      STID = KMDW STNM = 725340 TIME = 260410/0200
      SLAT = 41.78 SLON = -87.75 SELV = 187
      STIM = 0
                                              ← blank line
      SHOW = 1.64 LIFT = 4.89 ...             ← KEY = value surface params
      LCLP = 907.46 PWAT = 24.37 ...
      ...
                                              ← blank line
      PRES TMPC TMWC DWPC THTE DRCT SKNT OMEG ← sounding header
      CFRL HGHT                                ← header cont'd
      996.70 15.64 12.10 9.40 ...             ← first level = surface
      0.00 195.52                             ← first level cont'd
      ...more levels...
    """
    try:
        text = raw.decode("utf-8", errors="replace")
    except:
        text = raw.decode("latin-1", errors="replace")

    forecasts = []
    lines = text.split('\n')

    # Parse SNPARM field names (semicolon-separated)
    snparm_fields = []
    for line in lines:
        if line.strip().startswith("SNPARM"):
            raw_f = line.split("=", 1)[1] if "=" in line else ""
            snparm_fields = [f.strip() for f in raw_f.replace(";", " ").split() if f.strip()]
            break

    if not snparm_fields:
        return forecasts

    n_snparm = len(snparm_fields)

    # Find all STID blocks
    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if not (line.startswith("STID") and "TIME" in line):
            i += 1
            continue

        # Parse valid time
        m = re.search(r'TIME\s*=\s*(\d{6})/(\d{4})', line)
        if not m:
            i += 1
            continue
        ymd, hm = m.groups()
        yr = int(ymd[:2]) + (2000 if int(ymd[:2]) < 50 else 1900)
        try:
            valid_utc = datetime(yr, int(ymd[2:4]), int(ymd[4:6]),
                                 int(hm[:2]), int(hm[2:4]), tzinfo=timezone.utc)
        except:
            i += 1
            continue

        # Parse STIM (forecast hour) and SLAT/SLON from next lines
        i += 1
        fhr = 0
        while i < len(lines):
            sl = lines[i].strip()
            if not sl:  # blank line = end of header section
                i += 1
                break
            m2 = re.search(r'STIM\s*=\s*(\d+)', sl)
            if m2:
                fhr = int(m2.group(1))
            i += 1

        # Parse surface params (KEY = value format)
        sfc_params = {}
        while i < len(lines):
            sl = lines[i].strip()
            if not sl:  # blank line = end of surface params
                i += 1
                break
            # Extract all KEY = VALUE pairs from the line
            for km in re.finditer(r'(\w+)\s*=\s*(-?[\d.]+)', sl):
                try:
                    sfc_params[km.group(1)] = float(km.group(2))
                except:
                    pass
            i += 1

        # Skip sounding header lines (e.g. "PRES TMPC TMWC DWPC ...")
        while i < len(lines):
            sl = lines[i].strip()
            if not sl:
                i += 1
                continue
            # Check if this is a header line (contains field names, not numbers)
            try:
                float(sl.split()[0])
                break  # It's numeric data, stop skipping
            except:
                i += 1  # Still a header line, skip
                continue

        # Read first sounding level (= surface) — spans ceil(n_snparm/~8) lines
        snd_vals = []
        while len(snd_vals) < n_snparm and i < len(lines):
            sl = lines[i].strip()
            if not sl or sl.startswith("STID"):
                break
            try:
                float(sl.split()[0])
                snd_vals.extend(sl.split())
                i += 1
            except:
                break

        if len(snd_vals) >= n_snparm:
            snd = {}
            for fi in range(n_snparm):
                try:
                    snd[snparm_fields[fi]] = float(snd_vals[fi])
                except:
                    pass

            tmpc = snd.get("TMPC", -9999.0)
            dwpc = snd.get("DWPC", -9999.0)

            if tmpc > -9990:
                forecasts.append({
                    "valid_utc": valid_utc,
                    "fhr": fhr,
                    "t2m_f": round(tmpc * 9.0 / 5.0 + 32.0, 1),
                    "td2m_f": round(dwpc * 9.0 / 5.0 + 32.0, 1) if dwpc > -9990 else -9999.0,
                    "sfc_pres_mb": round(snd.get("PRES", -9999.0), 1),
                    "wspd_kt": round(snd.get("SKNT", -9999.0), 1),
                    "wdir": round(snd.get("DRCT", -9999.0), 1),
                    "cape": sfc_params.get("CAPE", -9999.0),
                    "cin": sfc_params.get("CINS", -9999.0),
                    "pwat_mm": sfc_params.get("PWAT", -9999.0),
                })

        # Don't increment i here — we'll catch the next STID in the main loop

    return forecasts


# ══════════════════════════════════════════════════════════════════════
#  NOMADS GRIB2 MINI-PARSER (2m temp extraction without cfgrib)
# ══════════════════════════════════════════════════════════════════════

def parse_grib2_t2m(data: bytes) -> Optional[float]:
    """
    Extract 2m temperature from a small NOMADS-filtered GRIB2 file.
    Returns temperature in Fahrenheit, or None.

    For full GRIB2 parsing, install cfgrib/pygrib. This handles the
    simple case of a single-point NOMADS subregion extract.
    """
    try:
        import cfgrib
        import xarray as xr
        tmp = CACHE_DIR / "_tmp_grib2.grb"
        tmp.write_bytes(data)
        ds = xr.open_dataset(str(tmp), engine="cfgrib")
        t2m_k = float(ds["t2m"].values.flat[0])
        tmp.unlink(missing_ok=True)
        return round((t2m_k - 273.15) * 9/5 + 32, 1)
    except ImportError:
        pass

    try:
        import pygrib
        tmp = CACHE_DIR / "_tmp_grib2.grb"
        tmp.write_bytes(data)
        grbs = pygrib.open(str(tmp))
        for grb in grbs:
            if grb.shortName == "2t" or grb.name == "2 metre temperature":
                t2m_k = float(grb.values.flat[0])
                tmp.unlink(missing_ok=True)
                return round((t2m_k - 273.15) * 9/5 + 32, 1)
        tmp.unlink(missing_ok=True)
    except ImportError:
        pass

    # Fallback: scan raw bytes for a float near reasonable temp range
    # This is a last resort for when no GRIB libraries are available
    return None


# ══════════════════════════════════════════════════════════════════════
#  FETCHERS
# ══════════════════════════════════════════════════════════════════════

def fetch_bufkit(model: str, city_key: str, cycle: int,
                 date: Optional[datetime] = None) -> Optional[list[dict]]:
    """
    Fetch and parse BUFKIT sounding for a model/city/cycle.
    Uses Penn State for latest, Iowa State for historical.
    """
    if city_key not in KALSHI_CITIES:
        print(f"  [!] Unknown city: {city_key}")
        return None

    city = KALSHI_CITIES[city_key]
    model_cfg = MODELS.get(model)
    if not model_cfg:
        print(f"  [!] Unknown model: {model}")
        return None

    prefix = model_cfg["bufkit_prefix"]
    bufkit_stn = _station_for_city(city_key, "bufkit").lower()  # BUFKIT station ID

    if date is None or date.date() >= (datetime.now(timezone.utc) - timedelta(days=1)).date():
        # Latest from Penn State
        # Map model key to Penn State directory name
        psu_dir = {
            "hrrr": "HRRR", "rap": "RAP", "nam": "NAM",
            "nam3": "NAMNEST", "gfs": "GFS", "sref": "SREF",
        }.get(model, model.upper())
        url = PSU_BUFKIT.format(
            model_upper=psu_dir, cycle=cycle, prefix=prefix, station=bufkit_stn
        )
    else:
        # Historical from Iowa State
        url = ISU_BUFKIT.format(
            year=date.year, month=date.month, day=date.day,
            cycle=cycle, prefix=prefix, station=bufkit_stn
        )

    # Cache key
    date_str = (date or datetime.now(timezone.utc)).strftime("%Y%m%d")
    cache_file = CACHE_DIR / model / f"{bufkit_stn}_{date_str}_{cycle:02d}z.buf"

    if cache_file.exists() and (time.time() - cache_file.stat().st_mtime) < 3600:
        raw = cache_file.read_bytes()
    else:
        raw = _fetch(url, timeout=20)
        if raw:
            cache_file.write_bytes(raw)
        else:
            return None

    forecasts = parse_bufkit(raw, model)
    if not forecasts:
        record_ingest_metric(model, city_key, "bufkit", False, "empty_bufkit")
    return forecasts


def fetch_nomads_point(model: str, city_key: str, cycle: int,
                       fhr: int, date: Optional[datetime] = None) -> Optional[float]:
    """
    Fetch a single 2m temperature forecast point from NOMADS grib filter.
    Returns temperature in Fahrenheit.
    """
    city = KALSHI_CITIES[city_key]
    lat, lon = city[3], city[4]
    date_str = (date or datetime.now(timezone.utc)).strftime("%Y%m%d")

    # Small subregion around the point (0.5° box)
    params = {
        "date": date_str, "cycle": cycle, "fhr": fhr,
        "lat_n": lat + 0.25, "lat_s": lat - 0.25,
        "lon_w": lon - 0.25, "lon_e": lon + 0.25,
    }

    if model == "hrrr":
        url = NOMADS_HRRR.format(**params)
    elif model == "gfs":
        url = NOMADS_GFS.format(**params)
    elif model == "nam":
        url = NOMADS_NAM.format(**params)
    else:
        return None

    data = _fetch(url, timeout=30)
    if not data:
        return None

    return parse_grib2_t2m(data)


def fetch_nws_observations(station_icao: str, *, start: datetime, end: datetime,
                           limit: int = 500, city_key: str = "") -> list[dict[str, Any]]:
    """Fetch hourly NWS observations for a station within a time window."""
    url = f"https://api.weather.gov/stations/{station_icao}/observations"
    params = {
        "start": _ensure_utc(start).isoformat().replace("+00:00", "Z"),
        "end": _ensure_utc(end).isoformat().replace("+00:00", "Z"),
        "limit": limit,
    }

    def _call():
        r = requests.get(url, params=params, headers=HEADERS_NWS, timeout=20)
        r.raise_for_status()
        raw = r.content
        payload = r.json()
        persist_payload_snapshot(
            "nws.observations",
            raw,
            city_key=station_icao,
            payload_type="json",
            metadata={"url": url, "params": params, "status": r.status_code},
        )
        _validate_external_json("nws.observations", payload, NWS_OBSERVATION_SCHEMA)
        return payload

    payload = _retryable_fetch(_call, source=url)
    if payload is None or not isinstance(payload, dict):
        return []
    features = payload.get("features", [])
    observations = []
    rejected = 0
    last_error = ""
    for item in features:
        errors = validate_nws_observation_feature(item)
        if errors:
            rejected += 1
            last_error = ";".join(errors[:5])
            continue
        props = item.get("properties", {}) if isinstance(item, dict) else {}
        obs_time = _coerce_datetime(props.get("timestamp"))
        if obs_time is None:
            continue
        temp = _nested_value(props, "temperature.value")
        dewpoint = _nested_value(props, "dewpoint.value")
        if temp is None and dewpoint is None:
            continue
        observations.append({
            "station": station_icao,
            "observed_at_utc": obs_time,
            "temp_f": None if temp is None else _c_to_f(temp),
            "dewpoint_f": None if dewpoint is None else _c_to_f(dewpoint),
            "wind_dir": _safe_float(_nested_value(props, "windDirection.value")),
            "wind_spd": _safe_float(_nested_value(props, "windSpeed.value")),
            "raw_payload": item,
        })
    if rejected:
        record_ingest_metric(
            "obs",
            city_key or station_icao,
            "nws",
            None,
            reason=last_error or "invalid_observation_feature",
            failure_count=rejected,
        )
    return observations


def store_hourly_observations(city_key: str, station_icao: str,
                              observations: list[dict[str, Any]],
                              *, dry_run: bool = False) -> list[dict[str, Any]]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("PRAGMA busy_timeout = 5000")
    preview = []
    success_count = 0
    failure_count = 0
    stale_count = 0
    last_error = ""
    for obs in observations:
        validation_errors = validate_hourly_observation_record(city_key, station_icao, obs)
        if validation_errors:
            preview.append({
                "insert": False,
                "changes": {},
                "rejected": True,
                "errors": validation_errors,
            })
            if not dry_run:
                failure_count += 1
                last_error = ";".join(validation_errors[:5])
            continue
        observed_at = _ensure_utc(_coerce_datetime(obs["observed_at_utc"]))
        if is_stale_observation(observed_at):
            stale_count += 1
        idempotency_key = _observation_idempotency_key(station_icao, observed_at)
        row = {
            "city": city_key,
            "station": station_icao,
            "observed_at_utc": observed_at.isoformat(),
            "temp_f": obs.get("temp_f"),
            "dewpoint_f": obs.get("dewpoint_f"),
            "wind_dir": obs.get("wind_dir"),
            "wind_spd": obs.get("wind_spd"),
            "raw_payload_sha256": _sha256_bytes(json.dumps(obs.get("raw_payload", {}), sort_keys=True).encode("utf-8")),
            "idempotency_key": idempotency_key,
        }
        existing = conn.execute(
            "SELECT city, station, observed_at_utc, temp_f, dewpoint_f, wind_dir, wind_spd, raw_payload_sha256, idempotency_key FROM hourly_observations WHERE idempotency_key = ?",
            (idempotency_key,),
        ).fetchone()
        preview.append(dry_run_forecast_diff(
            dict(zip(["city", "station", "observed_at_utc", "temp_f", "dewpoint_f", "wind_dir", "wind_spd", "raw_payload_sha256", "idempotency_key"], existing)) if existing else None,
            row,
        ))
        if dry_run:
            continue
        conn.execute(
            """
            INSERT INTO hourly_observations(city, station, observed_at_utc, temp_f, dewpoint_f,
                                            wind_dir, wind_spd, raw_payload_sha256, idempotency_key)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(idempotency_key) DO UPDATE SET
                temp_f = excluded.temp_f,
                dewpoint_f = excluded.dewpoint_f,
                wind_dir = excluded.wind_dir,
                wind_spd = excluded.wind_spd,
                raw_payload_sha256 = excluded.raw_payload_sha256
            """,
            (
                city_key, station_icao, observed_at.isoformat(), obs.get("temp_f"),
                obs.get("dewpoint_f"), obs.get("wind_dir"), obs.get("wind_spd"),
                row["raw_payload_sha256"], idempotency_key,
            ),
        )
        success_count += 1
    conn.commit()
    conn.close()
    if not observations:
        failure_count += 1
        last_error = "no_observations"
    if success_count or failure_count or stale_count:
        record_ingest_metric(
            "obs",
            city_key,
            "nws",
            None,
            reason=last_error,
            success_count=success_count,
            failure_count=failure_count,
            stale_count=stale_count,
        )
    return preview


def backfill_hourly_observations(city_key: str, start: datetime, end: datetime,
                                 *, dry_run: bool = False) -> list[dict[str, Any]]:
    station = _station_for_city(city_key, "observation")
    observations = fetch_nws_observations(station, start=start, end=end, city_key=city_key)
    return store_hourly_observations(city_key, station, observations, dry_run=dry_run)


def daily_missing_data_report(target_date: datetime) -> list[dict[str, Any]]:
    init_db()
    day = _ensure_utc(target_date).strftime("%Y-%m-%d")
    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        """
        SELECT city, model, COUNT(*) AS n_rows,
               MIN(valid_utc) AS first_valid,
               MAX(valid_utc) AS last_valid
        FROM nwp_forecasts
        WHERE substr(valid_utc, 1, 10) = ?
        GROUP BY city, model
        ORDER BY city, model
        """,
        (day,),
    ).fetchall()
    conn.close()
    report = []
    seen = {(city, model): n for city, model, n, _, _ in rows}
    for city_key in KALSHI_CITIES:
        for model in fallback_models_for_variable("high"):
            n_rows = seen.get((city_key, model), 0)
            report.append({
                "city": city_key,
                "model": model,
                "date": day,
                "expected_rows": 24,
                "observed_rows": n_rows,
                "missing_rows": max(0, 24 - n_rows),
            })
    return report


def _hour_floor(value: datetime) -> datetime:
    return _ensure_utc(value).replace(minute=0, second=0, microsecond=0)


def _hour_ceil(value: datetime) -> datetime:
    floored = _hour_floor(value)
    if _ensure_utc(value) == floored:
        return floored
    return floored + timedelta(hours=1)


def _hour_sequence(start: datetime, end: datetime) -> list[datetime]:
    cursor = _hour_floor(start)
    stop = _hour_ceil(end)
    hours = []
    while cursor < stop:
        hours.append(cursor)
        cursor += timedelta(hours=1)
    return hours


def observation_gap_clusters(city_key: str, start: datetime, end: datetime,
                             *, station: Optional[str] = None) -> dict[str, Any]:
    """Group missing hourly observation rows into consecutive UTC ranges."""
    init_db()
    station = station or _station_for_city(city_key, "observation")
    start_utc = _hour_floor(start)
    end_utc = _hour_ceil(end)
    expected_hours = _hour_sequence(start_utc, end_utc)

    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        """
        SELECT observed_at_utc
        FROM hourly_observations
        WHERE city = ? AND station = ?
          AND observed_at_utc >= ? AND observed_at_utc < ?
        """,
        (city_key, station, start_utc.isoformat(), end_utc.isoformat()),
    ).fetchall()
    conn.close()
    observed_hours = {
        _hour_floor(parsed)
        for (raw_ts,) in rows
        if (parsed := _coerce_datetime(raw_ts)) is not None
    }

    clusters: list[dict[str, Any]] = []
    active: list[datetime] = []
    for hour in expected_hours:
        if hour in observed_hours:
            if active:
                clusters.append(_observation_gap_cluster(active))
                active = []
            continue
        active.append(hour)
    if active:
        clusters.append(_observation_gap_cluster(active))

    return {
        "city": city_key,
        "station": station,
        "start_utc": start_utc.isoformat(),
        "end_utc": end_utc.isoformat(),
        "expected_hours": len(expected_hours),
        "observed_hours": len(observed_hours),
        "missing_hours": sum(cluster["hours"] for cluster in clusters),
        "gap_count": len(clusters),
        "gaps": clusters,
    }


def _observation_gap_cluster(hours: list[datetime]) -> dict[str, Any]:
    start = hours[0]
    end = hours[-1] + timedelta(hours=1)
    return {
        "start_utc": start.isoformat(),
        "end_utc": end.isoformat(),
        "hours": len(hours),
        "missing_hour_starts": [hour.isoformat() for hour in hours],
    }


def daily_observation_gap_report(target_date: datetime,
                                 *, city_keys: Optional[list[str]] = None) -> list[dict[str, Any]]:
    day_start = _ensure_utc(target_date).replace(hour=0, minute=0, second=0, microsecond=0)
    day_end = day_start + timedelta(days=1)
    return [
        observation_gap_clusters(city_key, day_start, day_end)
        for city_key in (city_keys or sorted(KALSHI_CITIES.keys()))
    ]


def backfill_observation_dry_run_summary(city_key: str, start: datetime, end: datetime,
                                         *, station: Optional[str] = None) -> dict[str, Any]:
    gaps = observation_gap_clusters(city_key, start, end, station=station)
    return {
        "city": gaps["city"],
        "station": gaps["station"],
        "start_utc": gaps["start_utc"],
        "end_utc": gaps["end_utc"],
        "expected_hours": gaps["expected_hours"],
        "observed_hours": gaps["observed_hours"],
        "missing_hours": gaps["missing_hours"],
        "gap_count": gaps["gap_count"],
        "backfill_ranges": [
            {
                "start": gap["start_utc"],
                "end": gap["end_utc"],
                "hours": gap["hours"],
            }
            for gap in gaps["gaps"]
        ],
    }


def _expected_hour_count(start: datetime, end: datetime) -> int:
    start_utc = _ensure_utc(start)
    end_utc = _ensure_utc(end)
    seconds = max(0.0, (end_utc - start_utc).total_seconds())
    return max(1, int(math.ceil(seconds / 3600.0)))


def station_coverage_heatmap(start: datetime, end: datetime, *,
                             city_keys: Optional[list[str]] = None,
                             models: Optional[list[str]] = None) -> dict[str, Any]:
    init_db()
    start_utc = _ensure_utc(start)
    end_utc = _ensure_utc(end)
    city_keys = city_keys or sorted(KALSHI_CITIES.keys())
    models = models or fallback_models_for_variable("high")
    expected = _expected_hour_count(start_utc, end_utc)

    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        """
        SELECT city, model, COUNT(*) AS n_rows
        FROM nwp_forecasts
        WHERE valid_utc >= ? AND valid_utc < ?
        GROUP BY city, model
        """,
        (start_utc.isoformat(), end_utc.isoformat()),
    ).fetchall()
    conn.close()
    counts = {(city, model): int(n_rows) for city, model, n_rows in rows}

    heatmap_rows = []
    summary = {"good": 0, "partial": 0, "poor": 0, "missing": 0}
    for city_key in city_keys:
        city = KALSHI_CITIES[city_key]
        cells = {}
        ratios = []
        for model in models:
            observed = counts.get((city_key, model), 0)
            ratio = min(1.0, observed / expected) if expected else 0.0
            if ratio >= 0.90:
                status = "good"
                symbol = "█"
            elif ratio >= 0.50:
                status = "partial"
                symbol = "▓"
            elif ratio > 0:
                status = "poor"
                symbol = "▒"
            else:
                status = "missing"
                symbol = "·"
            summary[status] += 1
            ratios.append(ratio)
            cells[model] = {
                "observed_rows": observed,
                "expected_rows": expected,
                "coverage_ratio": round(ratio, 4),
                "status": status,
                "symbol": symbol,
            }
        heatmap_rows.append({
            "city": city_key,
            "city_name": city[0],
            "settlement_station": _station_for_city(city_key, "settlement"),
            "observation_station": _station_for_city(city_key, "observation"),
            "avg_coverage_ratio": round(sum(ratios) / len(ratios), 4) if ratios else 0.0,
            "cells": cells,
        })

    return {
        "generated_at": _utcnow().isoformat(),
        "start_utc": start_utc.isoformat(),
        "end_utc": end_utc.isoformat(),
        "expected_rows_per_cell": expected,
        "models": models,
        "summary": summary,
        "rows": heatmap_rows,
    }


def render_station_coverage_heatmap(heatmap: dict[str, Any]) -> str:
    models = heatmap.get("models", [])
    lines = [
        f"Station coverage {heatmap.get('start_utc')} -> {heatmap.get('end_utc')}",
        "Legend: █ >=90%, ▓ >=50%, ▒ >0%, · missing",
        f"{'City':<6} {'Station':<8} " + " ".join(f"{m[:4].upper():>4}" for m in models) + "  Avg",
        "-" * (18 + 5 * len(models) + 6),
    ]
    for row in heatmap.get("rows", []):
        cells = row.get("cells", {})
        symbols = " ".join(f"{cells.get(m, {}).get('symbol', '?'):>4}" for m in models)
        lines.append(
            f"{row.get('city', ''):<6} {row.get('settlement_station', ''):<8} "
            f"{symbols}  {row.get('avg_coverage_ratio', 0.0) * 100:5.1f}%"
        )
    return "\n".join(lines)


def detect_duplicate_forecasts() -> list[dict[str, Any]]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        """
        SELECT city, model, init_cycle, fhr, COUNT(*) AS c
        FROM nwp_forecasts
        GROUP BY city, model, init_cycle, fhr
        HAVING COUNT(*) > 1
        ORDER BY c DESC, city, model, init_cycle, fhr
        """
    ).fetchall()
    conn.close()
    return [
        {"city": city, "model": model, "init_cycle": init_cycle, "fhr": fhr, "count": count}
        for city, model, init_cycle, fhr, count in rows
    ]


def dedupe_forecasts() -> int:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    before = conn.total_changes
    conn.execute(
        """
        DELETE FROM nwp_forecasts
        WHERE rowid NOT IN (
            SELECT MAX(rowid)
            FROM nwp_forecasts
            GROUP BY city, model, init_cycle, fhr
        )
        """
    )
    conn.commit()
    deleted = conn.total_changes - before
    conn.close()
    return deleted


def detect_duplicate_observations() -> list[dict[str, Any]]:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    rows = conn.execute(
        """
        SELECT city, station, observed_at_utc, COUNT(*) AS c
        FROM hourly_observations
        GROUP BY city, station, observed_at_utc
        HAVING COUNT(*) > 1
        ORDER BY c DESC, city, station, observed_at_utc
        """
    ).fetchall()
    conn.close()
    return [
        {
            "city": city,
            "station": station,
            "observed_at_utc": observed_at,
            "count": count,
        }
        for city, station, observed_at, count in rows
    ]


def dedupe_observations() -> int:
    init_db()
    conn = sqlite3.connect(str(DB_PATH))
    before = conn.total_changes
    conn.execute(
        """
        DELETE FROM hourly_observations
        WHERE rowid NOT IN (
            SELECT MAX(rowid)
            FROM hourly_observations
            GROUP BY city, station, observed_at_utc
        )
        """
    )
    conn.commit()
    deleted = conn.total_changes - before
    conn.close()
    return deleted


# ══════════════════════════════════════════════════════════════════════
#  ENSEMBLE TEMPERATURE DISTRIBUTION
# ══════════════════════════════════════════════════════════════════════

def gaussian_pdf(x: float, mu: float, sigma: float) -> float:
    """Standard Gaussian PDF."""
    if sigma <= 0:
        return 1.0 if abs(x - mu) < 0.5 else 0.0
    return math.exp(-0.5 * ((x - mu) / sigma) ** 2) / (sigma * math.sqrt(2 * math.pi))


def bucket_probability(lo: float, hi: float, mu: float, sigma: float) -> float:
    """Probability that temp falls in [lo, hi] given N(mu, sigma)."""
    if sigma <= 0:
        return 1.0 if lo <= mu <= hi else 0.0

    def phi(x):
        return 0.5 * (1 + math.erf((x - mu) / (sigma * math.sqrt(2))))

    return phi(hi) - phi(lo)


class NWPEnsemble:
    """
    Multi-model temperature ensemble for a single city/date.

    Collects point forecasts from multiple models and produces
    a calibrated probability distribution over temperature buckets.
    """

    def __init__(self, city_key: str, target_date: datetime):
        self.city_key = city_key
        self.city = KALSHI_CITIES[city_key]
        self.target_date = target_date
        self.forecasts = []  # list of (model, cycle, fhr, t2m_f, weight)

    # Model weights based on typical skill for 0-48h temperature
    MODEL_WEIGHTS = {
        "hrrr": 1.5,    # best short-range
        "nam3": 1.3,    # high-res nest
        "rap": 1.0,     # good hourly
        "nam": 0.9,
        "gfs": 0.7,     # lower resolution
        "sref": 1.2,    # ensemble spread info
    }

    def add_forecast(self, model: str, cycle: int, fhr: int,
                     t2m_f: float, member: int = 0):
        """Add a single model forecast point."""
        w = self.MODEL_WEIGHTS.get(model, 1.0)
        # Weight decays with forecast hour (newer inits more reliable)
        w *= max(0.5, 1.0 - fhr / 200.0)
        self.forecasts.append({
            "model": model, "cycle": cycle, "fhr": fhr,
            "t2m_f": t2m_f, "member": member, "weight": w,
        })

    def weighted_mean_std(self) -> tuple[float, float]:
        """Compute weighted mean and std of all forecasts."""
        if not self.forecasts:
            return 70.0, 10.0  # fallback

        weights = [f["weight"] for f in self.forecasts]
        temps = [f["t2m_f"] for f in self.forecasts]
        total_w = sum(weights)

        if total_w == 0:
            return sum(temps) / len(temps), 5.0

        mean = sum(t * w for t, w in zip(temps, weights)) / total_w

        if len(self.forecasts) < 2:
            return mean, 5.0

        var = sum(w * (t - mean) ** 2 for t, w in zip(temps, weights)) / total_w
        std = math.sqrt(var) if var > 0 else 2.0

        # Floor std at 2°F (models can't be that precise)
        std = max(std, 2.0)

        return mean, std

    def bucket_distribution(self, buckets: list[tuple[float, float]]) -> list[dict]:
        """
        Compute probability distribution over Kalshi temperature buckets.

        Args:
            buckets: list of (lo, hi) temperature ranges in °F

        Returns:
            list of dicts with bucket info and probabilities
        """
        mean, std = self.weighted_mean_std()
        n_models = len(set(f["model"] for f in self.forecasts))
        n_forecasts = len(self.forecasts)

        result = []
        total_p = 0.0

        for lo, hi in buckets:
            p = bucket_probability(lo, hi, mean, std)
            total_p += p
            result.append({
                "lo": lo, "hi": hi,
                "label": f"{lo:.0f}-{hi:.0f}°F",
                "raw_prob": p,
            })

        # Normalize
        for r in result:
            r["prob"] = r["raw_prob"] / total_p if total_p > 0 else 1.0 / len(buckets)

        # Add ensemble metadata
        for r in result:
            r["ensemble_mean"] = round(mean, 1)
            r["ensemble_std"] = round(std, 1)
            r["n_models"] = n_models
            r["n_forecasts"] = n_forecasts

        return sorted(result, key=lambda x: x["lo"])

    def model_spread(self) -> dict:
        """Return per-model mean forecasts for spread analysis."""
        by_model = defaultdict(list)
        for f in self.forecasts:
            by_model[f["model"]].append(f["t2m_f"])

        return {
            model: {
                "mean": round(sum(temps) / len(temps), 1),
                "min": round(min(temps), 1),
                "max": round(max(temps), 1),
                "n": len(temps),
            }
            for model, temps in by_model.items()
        }

    def summary(self) -> str:
        """Human-readable ensemble summary."""
        mean, std = self.weighted_mean_std()
        spread = self.model_spread()
        lines = [
            f"  City: {self.city[0]} ({self.city_key.upper()})",
            f"  Date: {self.target_date.strftime('%Y-%m-%d')}",
            f"  Ensemble: μ={mean:.1f}°F  σ={std:.1f}°F  ({len(self.forecasts)} forecasts from {len(spread)} models)",
            f"  Model spread:",
        ]
        for model, s in sorted(spread.items()):
            lines.append(f"    {model:6s}: {s['mean']:5.1f}°F  (range {s['min']:.0f}-{s['max']:.0f}, n={s['n']})")
        return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════════
#  KALSHI BUCKET EDGE CALCULATOR
# ══════════════════════════════════════════════════════════════════════

def parse_kalshi_buckets(markets: list[dict]) -> list[tuple[float, float, float]]:
    """
    Parse Kalshi market data into (lo, hi, market_price) tuples.
    market_price is in [0,1] probability.
    """
    buckets = []
    for m in markets:
        # Parse bucket from subtitle like "72 to 79°F" or "above 80°F"
        subtitle = m.get("subtitle", m.get("title", ""))
        lo, hi = None, None

        match = re.search(r'(\d+)\s*(?:to|–|-)\s*(\d+)', subtitle)
        if match:
            lo, hi = float(match.group(1)), float(match.group(2))
        elif "above" in subtitle.lower() or "over" in subtitle.lower():
            match = re.search(r'(\d+)', subtitle)
            if match:
                lo, hi = float(match.group(1)), float(match.group(1)) + 20
        elif "below" in subtitle.lower() or "under" in subtitle.lower():
            match = re.search(r'(\d+)', subtitle)
            if match:
                lo, hi = float(match.group(1)) - 20, float(match.group(1))

        if lo is not None and hi is not None:
            # Price: yes_ask or last_price in cents → probability
            price = m.get("yes_ask", m.get("last_price", 50)) / 100.0
            buckets.append((lo, hi, price))

    return sorted(buckets, key=lambda x: x[0])


def compute_edge(ensemble: NWPEnsemble, kalshi_buckets: list[tuple[float, float, float]]) -> list[dict]:
    """
    Compare NWP ensemble probabilities against Kalshi market prices.

    Returns list of dicts with edge info for each bucket.
    """
    bucket_ranges = [(lo, hi) for lo, hi, _ in kalshi_buckets]
    nwp_dist = ensemble.bucket_distribution(bucket_ranges)

    results = []
    for (lo, hi, market_p), nwp in zip(kalshi_buckets, nwp_dist):
        model_p = nwp["prob"]
        edge = model_p - market_p

        # Kelly fraction (simplified)
        if market_p > 0 and market_p < 1:
            # f* = (bp - q) / b where b = (1/market_p - 1), p = model_p, q = 1-model_p
            b = (1.0 / market_p) - 1.0
            kelly = (b * model_p - (1 - model_p)) / b if b > 0 else 0
            kelly = max(0, min(kelly, 0.25))  # cap at 25%
        else:
            kelly = 0

        results.append({
            "lo": lo, "hi": hi,
            "label": f"{lo:.0f}-{hi:.0f}°F",
            "market_price": round(market_p * 100, 1),
            "model_prob": round(model_p * 100, 1),
            "edge_pct": round(edge * 100, 1),
            "kelly_frac": round(kelly, 3),
            "signal": "BUY" if edge > 0.05 else ("SELL" if edge < -0.05 else "HOLD"),
        })

    return results


# ══════════════════════════════════════════════════════════════════════
#  DATABASE STORAGE
# ══════════════════════════════════════════════════════════════════════

def init_db():
    """Create NWP forecast storage database."""
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(DB_PATH))
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS nwp_forecasts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            model TEXT NOT NULL,
            init_cycle TEXT NOT NULL,     -- YYYYMMDD_HHz
            fhr INTEGER NOT NULL,
            valid_utc TEXT NOT NULL,
            t2m_f REAL,
            td2m_f REAL,
            wspd_kt REAL,
            wdir REAL,
            pmsl_mb REAL,
            cape REAL,
            cin REAL,
            pwat_mm REAL,
            idempotency_key TEXT,
            fetched_at TEXT DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(city, model, init_cycle, fhr),
            UNIQUE(idempotency_key)
        );

        CREATE TABLE IF NOT EXISTS ensemble_distributions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            target_date TEXT NOT NULL,
            bucket_lo REAL NOT NULL,
            bucket_hi REAL NOT NULL,
            model_prob REAL,
            market_price REAL,
            edge_pct REAL,
            n_models INTEGER,
            n_forecasts INTEGER,
            ensemble_mean REAL,
            ensemble_std REAL,
            computed_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE INDEX IF NOT EXISTS idx_nwp_city_valid
            ON nwp_forecasts(city, valid_utc);
        CREATE INDEX IF NOT EXISTS idx_nwp_model_init
            ON nwp_forecasts(model, init_cycle);
        CREATE INDEX IF NOT EXISTS idx_ens_city_date
            ON ensemble_distributions(city, target_date);

        CREATE TABLE IF NOT EXISTS ingest_metrics (
            model TEXT NOT NULL,
            city TEXT NOT NULL,
            source TEXT NOT NULL,
            day TEXT NOT NULL,
            success_count INTEGER NOT NULL DEFAULT 0,
            failure_count INTEGER NOT NULL DEFAULT 0,
            stale_count INTEGER NOT NULL DEFAULT 0,
            last_error TEXT,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (model, city, source, day)
        );

        CREATE TABLE IF NOT EXISTS payload_snapshots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source TEXT NOT NULL,
            city TEXT,
            model TEXT,
            payload_type TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            byte_len INTEGER NOT NULL,
            path TEXT NOT NULL,
            metadata_json TEXT,
            captured_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS ingest_circuit_breaker (
            source TEXT PRIMARY KEY,
            failure_count INTEGER NOT NULL DEFAULT 0,
            opened_until TEXT,
            last_failure TEXT,
            last_success TEXT
        );

        CREATE TABLE IF NOT EXISTS hourly_observations (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            station TEXT NOT NULL,
            observed_at_utc TEXT NOT NULL,
            temp_f REAL,
            dewpoint_f REAL,
            wind_dir REAL,
            wind_spd REAL,
            raw_payload_sha256 TEXT,
            idempotency_key TEXT NOT NULL,
            fetched_at TEXT DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(idempotency_key)
        );

        CREATE INDEX IF NOT EXISTS idx_hourly_obs_city_time
            ON hourly_observations(city, observed_at_utc);

        CREATE TABLE IF NOT EXISTS station_metadata_versions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            station_role TEXT NOT NULL,
            station_icao TEXT NOT NULL,
            effective_from TEXT NOT NULL,
            effective_to TEXT,
            notes TEXT,
            source_url TEXT,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(city, station_role, effective_from)
        );

        CREATE TABLE IF NOT EXISTS station_live_validation (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            city TEXT NOT NULL,
            station_role TEXT NOT NULL,
            station_icao TEXT NOT NULL,
            ok INTEGER NOT NULL,
            name TEXT,
            timezone TEXT,
            latitude REAL,
            longitude REAL,
            errors_json TEXT NOT NULL,
            checked_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS feed_freshness_checks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            feed_name TEXT NOT NULL,
            city TEXT,
            observed_at_utc TEXT NOT NULL,
            checked_at_utc TEXT NOT NULL,
            sla_hours INTEGER NOT NULL,
            is_stale INTEGER NOT NULL,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS source_latency_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            provider TEXT NOT NULL,
            source TEXT NOT NULL,
            city TEXT,
            latency_ms REAL NOT NULL,
            captured_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS source_drift_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source TEXT NOT NULL,
            provider TEXT,
            model TEXT,
            city TEXT,
            severity TEXT NOT NULL,
            missing_fields_json TEXT NOT NULL,
            added_fields_json TEXT NOT NULL,
            alias_hits_json TEXT NOT NULL,
            observed_fields_hash TEXT NOT NULL,
            sample_sha256 TEXT NOT NULL,
            sample_json TEXT,
            detected_at TEXT DEFAULT CURRENT_TIMESTAMP,
            resolved_at TEXT
        );
    """)

    cols = {row[1] for row in conn.execute("PRAGMA table_info(nwp_forecasts)").fetchall()}
    if "idempotency_key" not in cols:
        conn.execute("ALTER TABLE nwp_forecasts ADD COLUMN idempotency_key TEXT")
    _seed_station_metadata_versions_into(conn)
    conn.commit()
    conn.close()


def store_forecasts(city_key: str, model: str, cycle: int,
                    date: datetime, forecasts: list[dict], *,
                    dry_run: bool = False) -> list[dict]:
    """Store parsed BUFKIT forecasts in the database."""
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("PRAGMA busy_timeout = 5000")
    init_cycle = f"{date.strftime('%Y%m%d')}_{cycle:02d}z"
    changes: list[dict] = []
    success_count = 0
    failure_count = 0
    last_error = ""
    for fc in forecasts:
        try:
            validation_errors = _validate_forecast_record(model, fc)
            if validation_errors:
                failure_count += 1
                last_error = ",".join(validation_errors)
                continue

            valid_utc = _ensure_utc(fc["valid_utc"])
            idempotency_key = _forecast_idempotency_key(city_key, model, init_cycle, int(fc["fhr"]))
            row = {
                "city": city_key,
                "model": model,
                "init_cycle": init_cycle,
                "fhr": int(fc["fhr"]),
                "valid_utc": valid_utc.isoformat(),
                "t2m_f": fc.get("t2m_f"),
                "td2m_f": fc.get("td2m_f"),
                "wspd_kt": fc.get("wspd_kt"),
                "wdir": fc.get("wdir"),
                "pmsl_mb": fc.get("pmsl_mb"),
                "cape": fc.get("cape"),
                "cin": fc.get("cin"),
                "pwat_mm": fc.get("pwat_mm"),
                "idempotency_key": idempotency_key,
            }
            existing = conn.execute(
                """
                SELECT city, model, init_cycle, fhr, valid_utc, t2m_f, td2m_f,
                       wspd_kt, wdir, pmsl_mb, cape, cin, pwat_mm, idempotency_key
                FROM nwp_forecasts
                WHERE idempotency_key = ?
                """,
                (idempotency_key,),
            ).fetchone()
            preview = dry_run_forecast_diff(
                dict(zip(
                    ["city", "model", "init_cycle", "fhr", "valid_utc", "t2m_f", "td2m_f",
                     "wspd_kt", "wdir", "pmsl_mb", "cape", "cin", "pwat_mm", "idempotency_key"],
                    existing,
                )) if existing else None,
                row,
            )
            if dry_run:
                changes.append(preview)
                continue
            conn.execute("""
                INSERT INTO nwp_forecasts
                (city, model, init_cycle, fhr, valid_utc, t2m_f, td2m_f,
                 wspd_kt, wdir, pmsl_mb, cape, cin, pwat_mm, idempotency_key)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(idempotency_key) DO UPDATE SET
                    valid_utc = excluded.valid_utc,
                    t2m_f = excluded.t2m_f,
                    td2m_f = excluded.td2m_f,
                    wspd_kt = excluded.wspd_kt,
                    wdir = excluded.wdir,
                    pmsl_mb = excluded.pmsl_mb,
                    cape = excluded.cape,
                    cin = excluded.cin,
                    pwat_mm = excluded.pwat_mm
            """, (
                city_key, model, init_cycle, int(fc["fhr"]),
                valid_utc.isoformat(),
                fc.get("t2m_f"), fc.get("td2m_f"),
                fc.get("wspd_kt"), fc.get("wdir"),
                fc.get("pmsl_mb"), fc.get("cape"),
                fc.get("cin"), fc.get("pwat_mm"),
                idempotency_key,
            ))
            success_count += 1
        except Exception as exc:
            failure_count += 1
            last_error = str(exc)
    conn.commit()
    conn.close()
    if success_count or failure_count:
        record_ingest_metric(
            model,
            city_key,
            "bufkit",
            None,
            reason=last_error,
            success_count=success_count,
            failure_count=failure_count,
        )
    return changes


# ══════════════════════════════════════════════════════════════════════
#  HIGH-LEVEL PIPELINE
# ══════════════════════════════════════════════════════════════════════

def ingest_city(city_key: str, models: list[str] = None,
                target_date: datetime = None, verbose: bool = True,
                dry_run: bool = False) -> NWPEnsemble:
    """
    Full NWP ingest pipeline for a single city.

    1. Fetch BUFKIT data from all requested models
    2. Parse surface temperature forecasts
    3. Build multi-model ensemble
    4. Return NWPEnsemble object

    Args:
        city_key: e.g. "nyc", "chi", "phx"
        models: list of model keys, default all
        target_date: target forecast date, default tomorrow
        verbose: print progress
    """
    _ensure_dirs()
    init_db()

    if models is None:
        models = fallback_models_for_variable("high")

    if target_date is None:
        target_date = datetime.now(timezone.utc) + timedelta(days=1)
        target_date = target_date.replace(hour=0, minute=0, second=0, microsecond=0)

    ensemble = NWPEnsemble(city_key, target_date)
    now = datetime.now(timezone.utc)

    for model in models:
        cfg = MODELS.get(model)
        if not cfg:
            continue

        # Find the most recent cycle: walk backwards from current hour
        candidates = []
        for offset in range(24):
            c = (now.hour - offset) % 24
            if c in cfg["cycles"]:
                candidates.append(c)
            if len(candidates) >= 4:
                break

        for cycle in candidates:
            init_time = now.replace(hour=cycle, minute=0, second=0, microsecond=0)
            if init_time > now:
                init_time -= timedelta(days=1)

            hours_to_end = (target_date.replace(hour=23, minute=59) - init_time).total_seconds() / 3600
            hours_to_start = (target_date - init_time).total_seconds() / 3600
            if hours_to_start > cfg["max_fhr"] or hours_to_end < 0:
                continue

            if verbose:
                print(f"  Fetching {cfg['name']} {cycle:02d}Z for {city_key.upper()}...", end=" ", flush=True)

            forecasts = fetch_bufkit(model, city_key, cycle, init_time)

            if forecasts:
                if verbose:
                    print(f"✓ {len(forecasts)} timesteps")

                record_source_drift(
                    "bufkit.parsed_forecast",
                    forecasts[:20],
                    provider="bufkit",
                    model=model,
                    city_key=city_key,
                )

                # Find forecasts valid during the target date
                for fc in forecasts:
                    if fc["valid_utc"].date() == target_date.date() and fc["t2m_f"] > -900:
                        ensemble.add_forecast(model, cycle, fc["fhr"], fc["t2m_f"])

                store_forecasts(city_key, model, cycle, init_time, forecasts, dry_run=dry_run)
                break  # got data for this model, move to next
            else:
                if verbose:
                    print("✗ no data")
                record_ingest_metric(model, city_key, "bufkit", False, "fetch_failed")

    return ensemble


def scan_all_cities(models: list[str] = None, target_date: datetime = None,
                    verbose: bool = True, dry_run: bool = False) -> dict[str, NWPEnsemble]:
    """Run full NWP ingest for all 20 Kalshi cities."""
    ensembles = {}
    for city_key in KALSHI_CITIES:
        if verbose:
            city = KALSHI_CITIES[city_key]
            print(f"\n{'='*60}")
            print(f"  {city[0]} ({city_key.upper()}) — {city[1]}")
            print(f"{'='*60}")

        ensembles[city_key] = ingest_city(city_key, models, target_date, verbose, dry_run=dry_run)

        if verbose and ensembles[city_key].forecasts:
            print(ensembles[city_key].summary())

    return ensembles


# ══════════════════════════════════════════════════════════════════════
#  CLI
# ══════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="NWP Model Pipeline for Kalshi Temperature Trading",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s scan                          # All 20 cities, all models
  %(prog)s scan --cities nyc chi phx     # Specific cities
  %(prog)s scan --models hrrr nam gfs    # Specific models
  %(prog)s city nyc                      # Single city detail
  %(prog)s city nyc --buckets 60,65,70,75,80,85  # With bucket probs
  %(prog)s models                        # List available models
  %(prog)s cities                        # List 20 Kalshi cities
        """)

    sub = parser.add_subparsers(dest="command")

    # scan
    p_scan = sub.add_parser("scan", help="Scan all/selected cities")
    p_scan.add_argument("--cities", nargs="+", help="City keys (e.g. nyc chi)")
    p_scan.add_argument("--models", nargs="+", help="Model keys (e.g. hrrr gfs)")
    p_scan.add_argument("--date", help="Target date YYYY-MM-DD")
    p_scan.add_argument("--dry-run", action="store_true", help="Preview writes without storing them")

    # city
    p_city = sub.add_parser("city", help="Single city detailed analysis")
    p_city.add_argument("city_key", help="City key (e.g. nyc)")
    p_city.add_argument("--models", nargs="+")
    p_city.add_argument("--date", help="Target date YYYY-MM-DD")
    p_city.add_argument("--buckets", help="Comma-sep bucket boundaries (e.g. 60,65,70,75,80,85)")
    p_city.add_argument("--dry-run", action="store_true", help="Preview writes without storing them")

    # report
    p_report = sub.add_parser("report", help="Daily ingest and missing-data report")
    p_report.add_argument("--date", help="Target date YYYY-MM-DD")

    # source drift
    p_drift = sub.add_parser("drift-report", help="Show source-drift events")
    p_drift.add_argument("--source", help="Limit to one source name")
    p_drift.add_argument("--limit", type=int, default=25, help="Max events to show")

    # checksum
    p_checksum = sub.add_parser("checksum", help="Validate an artifact against checksum manifest")
    p_checksum.add_argument("source", help="Source key or artifact URL/name")
    p_checksum.add_argument("path", help="Local artifact path")
    p_checksum.add_argument("--manifest", help="Checksum manifest JSON path")

    # replay and retention
    p_replay = sub.add_parser("replay-snapshot", help="Replay a stored payload snapshot")
    p_replay.add_argument("snapshot_id", type=int)
    p_replay.add_argument("--apply", action="store_true", help="Allow replay adapters to write when supported")

    p_prune = sub.add_parser("prune-snapshots", help="Prune old payload snapshots")
    p_prune.add_argument("--older-than-days", type=int, required=True)
    p_prune.add_argument("--source", help="Limit pruning to one source")
    p_prune.add_argument("--apply", action="store_true", help="Actually delete files and DB rows")

    # provider health
    p_health = sub.add_parser("provider-health", help="Score provider health from ingest metrics")
    p_health.add_argument("provider", help="Provider/source name, e.g. nws or bufkit")
    p_health.add_argument("--source", help="Optional source filter")
    p_health.add_argument("--city", help="Optional city key")
    p_health.add_argument("--window-days", type=int, default=1)

    # station metadata
    p_meta = sub.add_parser("station-metadata", help="Diff/import/export station metadata versions")
    meta_sub = p_meta.add_subparsers(dest="metadata_action", required=True)
    meta_sub.add_parser("diff", help="Compare config station mappings with DB versions")
    p_meta_export = meta_sub.add_parser("export", help="Export station metadata JSON")
    p_meta_export.add_argument("--path", help="Optional output path")
    p_meta_import = meta_sub.add_parser("import", help="Import station metadata JSON")
    p_meta_import.add_argument("path", help="Input JSON path")
    p_meta_import.add_argument("--apply", action="store_true", help="Apply import instead of dry-run")

    # coverage heatmap
    p_coverage = sub.add_parser("coverage", help="Station/model coverage heatmap")
    p_coverage.add_argument("--start", required=True, help="Start UTC timestamp or YYYY-MM-DD")
    p_coverage.add_argument("--end", required=True, help="End UTC timestamp or YYYY-MM-DD")
    p_coverage.add_argument("--cities", nargs="+", help="City keys")
    p_coverage.add_argument("--models", nargs="+", help="Model keys")
    p_coverage.add_argument("--json", action="store_true", help="Emit JSON instead of text")

    # backfill
    p_backfill = sub.add_parser("backfill", help="Backfill hourly observations for a city")
    p_backfill.add_argument("city_key", help="City key (e.g. nyc)")
    p_backfill.add_argument("--start", required=True, help="Start UTC timestamp or YYYY-MM-DD")
    p_backfill.add_argument("--end", required=True, help="End UTC timestamp or YYYY-MM-DD")
    p_backfill.add_argument("--dry-run", action="store_true", help="Preview writes without storing them")

    p_backfill_summary = sub.add_parser("backfill-summary", help="Summarize missing observation ranges before backfill")
    p_backfill_summary.add_argument("city_key", help="City key (e.g. nyc)")
    p_backfill_summary.add_argument("--start", required=True, help="Start UTC timestamp or YYYY-MM-DD")
    p_backfill_summary.add_argument("--end", required=True, help="End UTC timestamp or YYYY-MM-DD")
    p_backfill_summary.add_argument("--json", action="store_true", help="Emit JSON instead of text")

    p_obs_gaps = sub.add_parser("obs-gaps", help="Daily observation gap clusters")
    p_obs_gaps.add_argument("--date", help="Date YYYY-MM-DD, defaults to today UTC")
    p_obs_gaps.add_argument("--cities", nargs="+", help="City keys")
    p_obs_gaps.add_argument("--json", action="store_true", help="Emit JSON instead of text")

    p_live = sub.add_parser("validate-live-stations", help="Validate configured stations against live NWS metadata")
    p_live.add_argument("--cities", nargs="+", help="City keys")
    p_live.add_argument("--roles", nargs="+", choices=["settlement", "observation", "bufkit"], help="Station roles")
    p_live.add_argument("--max-distance-deg", type=float, default=1.0)
    p_live.add_argument("--json", action="store_true", help="Emit JSON instead of text")

    # duplicate management
    sub.add_parser("dedupe", help="Remove duplicate forecast rows")
    sub.add_parser("duplicate-observations", help="List duplicate hourly observation rows")
    sub.add_parser("dedupe-observations", help="Remove duplicate hourly observation rows")
    sub.add_parser("validate-mappings", help="Validate city-to-station mappings")

    # models
    sub.add_parser("models", help="List available NWP models")

    # cities
    sub.add_parser("cities", help="List 20 Kalshi cities")

    args = parser.parse_args()

    def _parse_dt(value: str) -> datetime:
        if len(value) == 10:
            return datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        return _ensure_utc(datetime.fromisoformat(value.replace("Z", "+00:00")))

    if args.command == "models":
        print(f"\n{'Model':<8} {'Name':<12} {'Res(km)':<8} {'Cycles':<24} {'Max Fhr':<8} {'Ensemble'}")
        print("─" * 80)
        for key, cfg in MODELS.items():
            cycles_str = ",".join(f"{c:02d}z" for c in cfg["cycles"][:6])
            if len(cfg["cycles"]) > 6:
                cycles_str += "..."
            print(f"{key:<8} {cfg['name']:<12} {cfg['resolution_km']:<8} {cycles_str:<24} {cfg['max_fhr']:<8} {'Yes ('+str(cfg.get('members',0))+' mbrs)' if cfg['ensemble'] else 'No'}")

    elif args.command == "cities":
        print(f"\n{'Key':<6} {'City':<18} {'Settle':<6} {'BUFKIT':<6} {'CLI':<4} {'WFO':<4} {'Lat':>7} {'Lon':>8}  {'High Series':<16} {'Low Series'}")
        print("─" * 110)
        for key, c in sorted(KALSHI_CITIES.items()):
            print(f"{key:<6} {c[0]:<18} {c[1]:<6} {c[2]:<6} {c[7]:<4} {c[8]:<4} {c[3]:>7.2f} {c[4]:>8.2f}  {c[5]:<16} {c[6]}")

    elif args.command == "city":
        target = None
        if args.date:
            target = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc)

        ens = ingest_city(args.city_key, args.models, target, dry_run=args.dry_run)
        print()
        print(ens.summary())

        if args.buckets:
            bounds = [float(x) for x in args.buckets.split(",")]
            buckets = [(bounds[i], bounds[i+1]) for i in range(len(bounds)-1)]
            # Add tails
            buckets.insert(0, (bounds[0] - 30, bounds[0]))
            buckets.append((bounds[-1], bounds[-1] + 30))

            dist = ens.bucket_distribution(buckets)
            print(f"\n  Bucket Distribution (μ={dist[0]['ensemble_mean']}°F, σ={dist[0]['ensemble_std']}°F):")
            print(f"  {'Bucket':<16} {'NWP Prob':>10}")
            print(f"  {'─'*28}")
            for d in dist:
                bar = "█" * int(d["prob"] * 50)
                print(f"  {d['label']:<16} {d['prob']*100:>8.1f}%  {bar}")

        # Model spread
        spread = ens.model_spread()
        if spread:
            print(f"\n  Model Spread:")
            all_temps = [f["t2m_f"] for f in ens.forecasts]
            if all_temps:
                print(f"    Total range: {min(all_temps):.0f}°F – {max(all_temps):.0f}°F")

    elif args.command == "scan":
        target = None
        if args.date:
            target = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc)

        cities = args.cities if args.cities else None
        if cities:
            ensembles = {}
            for ck in cities:
                ensembles[ck] = ingest_city(ck, args.models, target, dry_run=args.dry_run)
        else:
            ensembles = scan_all_cities(args.models, target, dry_run=args.dry_run)

        # Summary table
        print(f"\n{'='*70}")
        print(f"  NWP ENSEMBLE SUMMARY — {(target or datetime.now(timezone.utc) + timedelta(days=1)).strftime('%Y-%m-%d')}")
        print(f"{'='*70}")
        print(f"  {'City':<16} {'μ(°F)':>7} {'σ(°F)':>7} {'Models':>7} {'Fcsts':>7} {'Spread':>8}")
        print(f"  {'─'*56}")
        for ck, ens in sorted(ensembles.items()):
            if not ens.forecasts:
                print(f"  {KALSHI_CITIES[ck][0]:<16} {'—':>7} {'—':>7} {'0':>7} {'0':>7} {'—':>8}")
                continue
            mean, std = ens.weighted_mean_std()
            spread = ens.model_spread()
            all_t = [f["t2m_f"] for f in ens.forecasts]
            spread_str = f"{max(all_t)-min(all_t):.0f}°F" if all_t else "—"
            print(f"  {KALSHI_CITIES[ck][0]:<16} {mean:>7.1f} {std:>7.1f} {len(spread):>7} {len(ens.forecasts):>7} {spread_str:>8}")

    elif args.command == "report":
        target = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc) if args.date else _utcnow()
        errors = validate_city_station_mappings()
        if errors:
            print("City/station mapping issues:")
            for err in errors:
                print(f"  - {err}")
        else:
            print("City/station mappings OK")
        print("")
        report = daily_missing_data_report(target)
        print(f"Missing-data report for {target.strftime('%Y-%m-%d')}")
        print(f"{'City':<16} {'Model':<8} {'Obs':>4} {'Miss':>5}")
        print("-" * 38)
        for row in report:
            if row["missing_rows"] > 0:
                print(f"{KALSHI_CITIES[row['city']][0]:<16} {row['model']:<8} {row['observed_rows']:>4} {row['missing_rows']:>5}")
        dupes = detect_duplicate_forecasts()
        print("")
        print(f"Duplicate forecast groups: {len(dupes)}")

    elif args.command == "drift-report":
        events = source_drift_report(args.source, limit=args.limit)
        if not events:
            print("No source-drift events")
        else:
            print(f"{'ID':>4} {'Severity':<8} {'Source':<24} {'City':<6} {'Model':<8} Missing")
            print("-" * 82)
            for event in events:
                missing = ",".join(event["missing_fields"]) or "-"
                print(
                    f"{event['id']:>4} {event['severity']:<8} {event['source']:<24} "
                    f"{event.get('city') or '-':<6} {event.get('model') or '-':<8} {missing}"
                )

    elif args.command == "checksum":
        manifest = load_checksum_manifest(Path(args.manifest)) if args.manifest else load_checksum_manifest()
        raw = Path(args.path).read_bytes()
        print(json.dumps(validate_artifact_checksum(args.source, raw, manifest), indent=2, sort_keys=True))

    elif args.command == "replay-snapshot":
        result = replay_payload_snapshot(args.snapshot_id, dry_run=not args.apply)
        print(json.dumps(result, indent=2, sort_keys=True, default=str))

    elif args.command == "prune-snapshots":
        result = prune_payload_snapshots(
            args.older_than_days,
            source=args.source,
            dry_run=not args.apply,
        )
        print(json.dumps(result, indent=2, sort_keys=True))

    elif args.command == "provider-health":
        result = provider_health_score(
            args.provider,
            source=args.source,
            city_key=args.city or "",
            window_days=args.window_days,
        )
        print(json.dumps(result, indent=2, sort_keys=True))

    elif args.command == "station-metadata":
        if args.metadata_action == "diff":
            print(json.dumps(station_metadata_diff(), indent=2, sort_keys=True))
        elif args.metadata_action == "export":
            data = export_station_metadata(Path(args.path)) if args.path else export_station_metadata()
            if not args.path:
                print(json.dumps(data, indent=2, sort_keys=True))
            else:
                print(f"Exported {len(data['rows'])} station metadata rows to {args.path}")
        elif args.metadata_action == "import":
            result = import_station_metadata(Path(args.path), dry_run=not args.apply)
            print(json.dumps(result, indent=2, sort_keys=True))

    elif args.command == "coverage":
        heatmap = station_coverage_heatmap(
            _parse_dt(args.start),
            _parse_dt(args.end),
            city_keys=args.cities,
            models=args.models,
        )
        if args.json:
            print(json.dumps(heatmap, indent=2, sort_keys=True))
        else:
            print(render_station_coverage_heatmap(heatmap))

    elif args.command == "backfill":
        start = _parse_dt(args.start)
        end = _parse_dt(args.end)
        preview = backfill_hourly_observations(args.city_key, start, end, dry_run=args.dry_run)
        print(f"Backfill preview rows: {len(preview)}")

    elif args.command == "backfill-summary":
        result = backfill_observation_dry_run_summary(
            args.city_key,
            _parse_dt(args.start),
            _parse_dt(args.end),
        )
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(
                f"{result['city']} {result['station']}: "
                f"{result['missing_hours']}/{result['expected_hours']} missing hours "
                f"across {result['gap_count']} ranges"
            )
            for item in result["backfill_ranges"]:
                print(f"  {item['start']} -> {item['end']} ({item['hours']}h)")

    elif args.command == "obs-gaps":
        target = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc) if args.date else _utcnow()
        result = daily_observation_gap_report(target, city_keys=args.cities)
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(f"Observation gaps for {target.strftime('%Y-%m-%d')}")
            print(f"{'City':<6} {'Station':<8} {'Missing':>7} {'Gaps':>5} First gap")
            print("-" * 58)
            for row in result:
                first_gap = row["gaps"][0]["start_utc"] if row["gaps"] else "-"
                print(
                    f"{row['city']:<6} {row['station']:<8} "
                    f"{row['missing_hours']:>7} {row['gap_count']:>5} {first_gap}"
                )

    elif args.command == "validate-live-stations":
        result = validate_live_station_metadata(
            city_keys=args.cities,
            roles=args.roles,
            max_distance_deg=args.max_distance_deg,
        )
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            failures = [row for row in result if not row["ok"]]
            print(f"Live station metadata checks: {len(result) - len(failures)}/{len(result)} OK")
            for row in failures:
                print(
                    f"  {row['city']} {row['station_role']} {row['station_icao']}: "
                    f"{', '.join(row['errors'])}"
                )

    elif args.command == "dedupe":
        removed = dedupe_forecasts()
        print(f"Removed {removed} duplicate rows")

    elif args.command == "duplicate-observations":
        rows = detect_duplicate_observations()
        print(json.dumps(rows, indent=2, sort_keys=True))

    elif args.command == "dedupe-observations":
        removed = dedupe_observations()
        print(f"Removed {removed} duplicate observation rows")

    elif args.command == "validate-mappings":
        errors = validate_city_station_mappings()
        if errors:
            print("Mapping errors:")
            for err in errors:
                print(f"  - {err}")
        else:
            print("All city-to-station mappings validated")

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
