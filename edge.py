#!/usr/bin/env python3
"""
edge.py — Gradient-boosted temperature bucket classifier.

Trains on 566K station-days of raw GHCN/ISD climate data + 7,419 settled
Kalshi KXHIGH events. No lookup tables. Computes features from raw
observations at train and inference time.

Features (42):
  - Climatological: DOY mean/std computed from all historical obs for that
    station within ±21 days, multi-decade regression slope (warming trend),
    recent 5yr/10yr anomaly vs long-term mean
  - Persistence: prior day high, 3-day/7-day/30-day rolling mean of highs,
    deviation of each from climatological DOY mean
  - Bucket geometry: where bucket sits relative to station's DOY distribution
    (z-score of bucket center, tail flags, bucket width as fraction of DOY std)
  - Seasonal: sin/cos of DOY, month indicators for shoulder seasons
  - Station identity: lat, lon, elevation proxy (mean annual temp),
    continental vs coastal (DOY std proxy), diurnal range

Walk-forward backtest with no lookahead: each event is predicted using only
data available before that event's date.
"""
import json
import math
import sqlite3
import sys
import time
from collections import defaultdict
from datetime import datetime
from pathlib import Path

import lightgbm as lgb
import numpy as np
from sklearn.calibration import CalibratedClassifierCV
from sklearn.metrics import brier_score_loss, log_loss

CLIMO_DB = Path(__file__).parent / "climate_data" / "climatology.db"
KALSHI_DB = Path(__file__).parent / "papers" / "kalshi_training.db"
SETTLED_JSON = Path(__file__).parent / "papers" / "all_settled_kxhigh.json"

TICKER_TO_ICAO = {
    "KXHIGHTATL": "KATL", "KXHIGHAUS": "KAUS", "KXHIGHTBOS": "KBOS",
    "KXHIGHTDAL": "KDAL", "KXHIGHTDC": "KDCA", "KXHIGHDEN": "KDEN",
    "KXHIGHTHOU": "KHOU", "KXHIGHTLV": "KLAS", "KXHIGHLAX": "KLAX",
    "KXHIGHCHI": "KMDW", "KXHIGHMIA": "KMIA", "KXHIGHTMIN": "KMSP",
    "KXHIGHTNOLA": "KMSY", "KXHIGHNY": "KNYC", "KXHIGHTOKC": "KOKC",
    "KXHIGHPHIL": "KPHL", "KXHIGHTPHX": "KPHX", "KXHIGHTSATX": "KSAT",
    "KXHIGHTSEA": "KSEA", "KXHIGHTSFO": "KSFO",
    "HIGHTATL": "KATL", "HIGHAUS": "KAUS", "HIGHTBOS": "KBOS",
    "HIGHTDAL": "KDAL", "HIGHTDC": "KDCA", "HIGHDEN": "KDEN",
    "HIGHTHOU": "KHOU", "HIGHTLV": "KLAS", "HIGHLAX": "KLAX",
    "HIGHCHI": "KMDW", "HIGHMIA": "KMIA", "HIGHTMIN": "KMSP",
    "HIGHTNOLA": "KMSY", "HIGHNY": "KNYC", "HIGHTOKC": "KOKC",
    "HIGHPHIL": "KPHL", "HIGHTPHX": "KPHX", "HIGHTSATX": "KSAT",
    "HIGHTSEA": "KSEA", "HIGHTSFO": "KSFO",
}

MONTHS = {"JAN":1,"FEB":2,"MAR":3,"APR":4,"MAY":5,"JUN":6,
          "JUL":7,"AUG":8,"SEP":9,"OCT":10,"NOV":11,"DEC":12}

FEATURE_NAMES = [
    # Climatological (7)
    "climo_mean", "climo_std", "climo_n",
    "climo_skew", "climo_p10", "climo_p50", "climo_p90",
    # Trend (4)
    "trend_5yr_anomaly", "trend_10yr_anomaly",
    "trend_decade_slope", "trend_last30_anomaly",
    # Persistence (6)
    "persist_prev1", "persist_prev3_mean", "persist_prev7_mean",
    "persist_prev30_mean", "persist_prev1_vs_climo", "persist_prev7_vs_climo",
    # Bucket geometry (8)
    "bucket_center", "bucket_width", "bucket_center_zscore",
    "bucket_is_lower_tail", "bucket_is_upper_tail",
    "bucket_climo_mass",  # how much climo mass falls in this bucket
    "bucket_rank",        # 0=lowest bucket, 1=highest
    "bucket_from_climo_peak",  # how far bucket center is from climo mode
    # Seasonal (5)
    "doy_sin", "doy_cos", "is_shoulder_spring", "is_shoulder_fall", "month",
    # Station (6)
    "lat", "lon", "mean_annual_temp", "doy_std_annual",
    "coastal_flag", "diurnal_range_proxy",
    # Context (6)
    "n_buckets", "event_bucket_spread",  # total range covered by all buckets
    "bucket_idx_normalized",  # position within event (0=lowest, 1=highest)
    "climo_mean_vs_bucket_center_event",  # climo mean - event center
    "recent_volatility",  # std of last 14 days
    "warming_since_1980",  # long-term warming signal
]


# ═══════════════════════════════════════════════════════════════════
# RAW DATA LOADER
# ═══════════════════════════════════════════════════════════════════

class RawClimate:
    """Direct access to 566K raw daily highs. No precomputation."""

    def __init__(self):
        t0 = time.time()
        conn = sqlite3.connect(str(CLIMO_DB))
        rows = conn.execute(
            "SELECT icao, date, max_f FROM daily_highs ORDER BY icao, date"
        ).fetchall()
        conn.close()

        # Index: station → sorted list of (date_str, doy, year, temp)
        self.data = defaultdict(list)
        for icao, date_str, temp in rows:
            try:
                y, m, d = int(date_str[:4]), int(date_str[5:7]), int(date_str[8:10])
                dt = datetime(y, m, d)
                doy = dt.timetuple().tm_yday
            except (ValueError, IndexError):
                continue
            self.data[icao].append((date_str, doy, y, temp))

        self.n_rows = len(rows)
        self.load_time = time.time() - t0

    def get_before(self, icao, cutoff_date):
        """All observations for station strictly before cutoff_date."""
        return [(ds, doy, yr, t) for ds, doy, yr, t in self.data[icao] if ds < cutoff_date]

    def climo_features(self, icao, target_doy, obs_before_date):
        """
        Compute climatological features from raw data.
        Only uses observations before the cutoff date (no lookahead).
        """
        window = 21
        nan = float("nan")

        # Filter to ±window DOY (circular)
        nearby = []
        all_temps = []
        for _, doy, yr, t in obs_before_date:
            all_temps.append((yr, t))
            d = min(abs(doy - target_doy), 365 - abs(doy - target_doy))
            if d <= window:
                w = math.exp(-0.5 * (d / (window / 2.5)) ** 2)
                nearby.append((yr, t, w))

        if len(nearby) < 20:
            return {
                "climo_mean": nan, "climo_std": nan, "climo_n": 0,
                "climo_skew": nan, "climo_p10": nan, "climo_p50": nan, "climo_p90": nan,
                "trend_5yr_anomaly": nan, "trend_10yr_anomaly": nan,
                "trend_decade_slope": nan, "trend_last30_anomaly": nan,
                "mean_annual_temp": nan, "doy_std_annual": nan,
                "warming_since_1980": nan,
            }

        # Weighted stats
        total_w = sum(w for _, _, w in nearby)
        wmean = sum(t * w for _, t, w in nearby) / total_w
        wvar = sum(w * (t - wmean) ** 2 for _, t, w in nearby) / total_w
        wstd = math.sqrt(wvar) if wvar > 0 else 2.0

        temps_sorted = sorted(t for _, t, _ in nearby)
        n = len(temps_sorted)

        # Skewness
        if wstd > 0.1 and n > 10:
            skew = sum(w * ((t - wmean) / wstd) ** 3 for _, t, w in nearby) / total_w
        else:
            skew = 0.0

        # Recent anomalies
        now_year = max(yr for yr, _, _ in nearby)
        recent_5 = [t for yr, t, _ in nearby if yr >= now_year - 5]
        recent_10 = [t for yr, t, _ in nearby if yr >= now_year - 10]
        anom_5 = (sum(recent_5) / len(recent_5) - wmean) if recent_5 else nan
        anom_10 = (sum(recent_10) / len(recent_10) - wmean) if recent_10 else nan

        # Decadal warming slope (simple linear regression of yearly DOY means)
        year_means = defaultdict(list)
        for yr, t, _ in nearby:
            year_means[yr].append(t)
        if len(year_means) >= 10:
            years = sorted(year_means.keys())
            x = np.array([y - years[0] for y in years], dtype=np.float64)
            y = np.array([sum(year_means[yr]) / len(year_means[yr]) for yr in years])
            if len(x) > 1:
                slope = float(np.polyfit(x, y, 1)[0])  # °F per year
            else:
                slope = 0.0
        else:
            slope = 0.0

        # Last 30 days
        cutoff_30 = sorted(obs_before_date, key=lambda x: x[0])[-30:] if len(obs_before_date) >= 30 else obs_before_date
        last30_mean = sum(t for _, _, _, t in cutoff_30) / len(cutoff_30) if cutoff_30 else wmean
        last30_anom = last30_mean - wmean

        # Station-level features
        if all_temps:
            all_t = [t for _, t in all_temps]
            mean_annual = sum(all_t) / len(all_t)
            # Compute DOY std as proxy for continentality
            by_doy = defaultdict(list)
            for _, doy, _, t in obs_before_date[-3650:]:  # last ~10 years
                by_doy[doy].append(t)
            doy_means = [sum(v)/len(v) for v in by_doy.values() if len(v) >= 3]
            doy_std = float(np.std(doy_means)) if len(doy_means) > 30 else 15.0
        else:
            mean_annual = wmean
            doy_std = 15.0

        # Warming since 1980
        pre_1980 = [t for yr, t, _ in nearby if yr < 1980]
        post_2010 = [t for yr, t, _ in nearby if yr >= 2010]
        if pre_1980 and post_2010:
            warming = sum(post_2010) / len(post_2010) - sum(pre_1980) / len(pre_1980)
        else:
            warming = slope * 30  # fallback

        return {
            "climo_mean": wmean, "climo_std": wstd, "climo_n": n,
            "climo_skew": skew,
            "climo_p10": temps_sorted[int(n * 0.10)],
            "climo_p50": temps_sorted[int(n * 0.50)],
            "climo_p90": temps_sorted[int(n * 0.90)],
            "trend_5yr_anomaly": anom_5,
            "trend_10yr_anomaly": anom_10,
            "trend_decade_slope": slope,
            "trend_last30_anomaly": last30_anom,
            "mean_annual_temp": mean_annual,
            "doy_std_annual": doy_std,
            "warming_since_1980": warming,
        }

    def persistence_features(self, obs_before_date):
        """Recent persistence: yesterday, 3d/7d/30d rolling means."""
        nan = float("nan")
        if not obs_before_date:
            return {"persist_prev1": nan, "persist_prev3_mean": nan,
                    "persist_prev7_mean": nan, "persist_prev30_mean": nan,
                    "persist_prev1_vs_climo": nan, "persist_prev7_vs_climo": nan,
                    "recent_volatility": nan}

        recent = sorted(obs_before_date, key=lambda x: x[0])
        prev1 = recent[-1][3] if recent else nan
        prev3 = sum(r[3] for r in recent[-3:]) / min(3, len(recent))
        prev7 = sum(r[3] for r in recent[-7:]) / min(7, len(recent))
        prev30 = sum(r[3] for r in recent[-30:]) / min(30, len(recent))

        # Volatility: std of last 14 days
        last14 = [r[3] for r in recent[-14:]]
        vol = float(np.std(last14)) if len(last14) >= 3 else nan

        return {"persist_prev1": prev1, "persist_prev3_mean": prev3,
                "persist_prev7_mean": prev7, "persist_prev30_mean": prev30,
                "persist_prev1_vs_climo": nan,  # filled later
                "persist_prev7_vs_climo": nan,
                "recent_volatility": vol}

    def bucket_mass(self, obs_before_date, target_doy, lo, hi, window=21):
        """What fraction of historical observations for this DOY fall in [lo, hi]?"""
        nearby = []
        for _, doy, _, t in obs_before_date:
            d = min(abs(doy - target_doy), 365 - abs(doy - target_doy))
            if d <= window:
                nearby.append(t)
        if not nearby:
            return 0.0
        hits = 0
        for t in nearby:
            r = round(t)
            if lo is None and hi is not None and r <= hi:
                hits += 1
            elif hi is None and lo is not None and r >= lo:
                hits += 1
            elif lo is not None and hi is not None and lo <= r <= hi:
                hits += 1
        return hits / len(nearby)


# ═══════════════════════════════════════════════════════════════════
# FEATURE EXTRACTION
# ═══════════════════════════════════════════════════════════════════

STATION_COORDS = {
    "KATL": (33.64, -84.43), "KAUS": (30.19, -97.67), "KBOS": (42.37, -71.01),
    "KDAL": (32.85, -96.85), "KDCA": (38.85, -77.04), "KDEN": (39.86, -104.67),
    "KHOU": (29.65, -95.28), "KLAS": (36.08, -115.15), "KLAX": (33.94, -118.41),
    "KMDW": (41.79, -87.75), "KMIA": (25.80, -80.29), "KMSP": (44.88, -93.22),
    "KMSY": (29.99, -90.26), "KNYC": (40.78, -73.97), "KOKC": (35.39, -97.60),
    "KPHL": (39.87, -75.24), "KPHX": (33.44, -112.01), "KSAT": (29.53, -98.47),
    "KSEA": (47.45, -122.31), "KSFO": (37.62, -122.38),
}

COASTAL = {"KBOS", "KLAX", "KMIA", "KNYC", "KPHL", "KSEA", "KSFO", "KHOU", "KMSY"}


def extract_features(climate, icao, target_doy, target_date, bucket_lo, bucket_hi,
                     all_buckets, bucket_idx):
    """Build feature vector for one (station, date, bucket) prediction."""
    obs = climate.get_before(icao, target_date)

    climo = climate.climo_features(icao, target_doy, obs)
    persist = climate.persistence_features(obs)

    cmean = climo["climo_mean"]
    cstd = climo["climo_std"] if climo["climo_std"] and climo["climo_std"] > 0 else 3.0

    # Bucket geometry
    if bucket_lo is None and bucket_hi is not None:
        bcenter = bucket_hi - 0.5
        bwidth = 99.0
        is_lower = 1.0
        is_upper = 0.0
    elif bucket_hi is None and bucket_lo is not None:
        bcenter = bucket_lo + 0.5
        bwidth = 99.0
        is_lower = 0.0
        is_upper = 1.0
    else:
        bcenter = (bucket_lo + bucket_hi) / 2.0
        bwidth = bucket_hi - bucket_lo + 1.0
        is_lower = 0.0
        is_upper = 0.0

    bzscore = (bcenter - cmean) / cstd if cstd > 0.1 and not math.isnan(cmean) else 0.0
    bmass = climate.bucket_mass(obs, target_doy, bucket_lo, bucket_hi)

    # Event-level geometry
    n_buckets = len(all_buckets)
    all_centers = []
    for lo, hi in all_buckets:
        if lo is None and hi is not None:
            all_centers.append(hi - 0.5)
        elif hi is None and lo is not None:
            all_centers.append(lo + 0.5)
        elif lo is not None and hi is not None:
            all_centers.append((lo + hi) / 2.0)
    event_spread = max(all_centers) - min(all_centers) if all_centers else 10
    event_center = sum(all_centers) / len(all_centers) if all_centers else cmean
    bucket_idx_norm = bucket_idx / (n_buckets - 1) if n_buckets > 1 else 0.5

    # Persistence vs climo
    p1_vs = persist["persist_prev1"] - cmean if not math.isnan(persist["persist_prev1"]) and not math.isnan(cmean) else float("nan")
    p7_vs = persist["persist_prev7_mean"] - cmean if not math.isnan(persist["persist_prev7_mean"]) and not math.isnan(cmean) else float("nan")

    # Seasonal
    doy_sin = math.sin(2 * math.pi * target_doy / 365)
    doy_cos = math.cos(2 * math.pi * target_doy / 365)
    month = (target_doy - 1) // 30 + 1
    is_spring = 1.0 if month in (3, 4, 5) else 0.0
    is_fall = 1.0 if month in (9, 10, 11) else 0.0

    # Station
    lat, lon = STATION_COORDS.get(icao, (35.0, -90.0))
    coastal = 1.0 if icao in COASTAL else 0.0
    # Diurnal range proxy: continental stations have wider daily ranges
    dr = climo["doy_std_annual"] if not math.isnan(climo.get("doy_std_annual", float("nan"))) else 15.0

    return np.array([
        climo["climo_mean"], climo["climo_std"], climo["climo_n"],
        climo["climo_skew"], climo["climo_p10"], climo["climo_p50"], climo["climo_p90"],
        climo["trend_5yr_anomaly"], climo["trend_10yr_anomaly"],
        climo["trend_decade_slope"], climo["trend_last30_anomaly"],
        persist["persist_prev1"], persist["persist_prev3_mean"],
        persist["persist_prev7_mean"], persist["persist_prev30_mean"],
        p1_vs, p7_vs,
        bcenter, bwidth, bzscore, is_lower, is_upper,
        bmass, bucket_idx_norm, bcenter - cmean if not math.isnan(cmean) else 0,
        doy_sin, doy_cos, is_spring, is_fall, float(month),
        lat, lon, climo["mean_annual_temp"], climo["doy_std_annual"],
        coastal, dr,
        float(n_buckets), event_spread, bucket_idx_norm,
        cmean - event_center if not math.isnan(cmean) else 0,
        persist["recent_volatility"],
        climo["warming_since_1980"],
    ], dtype=np.float64)


# ═══════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════

def parse_bucket_sub(sub):
    sub = sub.replace("\u00b0", "").replace("°", "").strip()
    if "or below" in sub:
        v = int("".join(c for c in sub.split("or")[0].strip() if c.isdigit() or c == "-"))
        return (None, v)
    if "or above" in sub:
        v = int("".join(c for c in sub.split("or")[0].strip() if c.isdigit() or c == "-"))
        return (v, None)
    if " to " in sub:
        parts = sub.split(" to ")
        lo = int("".join(c for c in parts[0].strip() if c.isdigit() or c == "-"))
        hi = int("".join(c for c in parts[1].strip() if c.isdigit() or c == "-"))
        return (lo, hi)
    return (None, None)


def event_date(event_ticker):
    """Parse KXHIGHTATL-26MAR21 → '2026-03-21' or None."""
    parts = event_ticker.split("-")
    if len(parts) < 2:
        return None
    dp = parts[1].upper()
    for mname, mnum in MONTHS.items():
        if mname in dp:
            try:
                yy = int(dp[:2])
                dd = int(dp[dp.index(mname) + 3:])
                yr = 2000 + yy if yy < 50 else 1900 + yy
                return f"{yr:04d}-{mnum:02d}-{dd:02d}"
            except (ValueError, IndexError):
                return None
    return None


def main():
    t_start = time.time()
    print("=" * 72)
    print("EDGE MODEL — LightGBM on 566K station-days")
    print("=" * 72)

    # Load settled events
    print("\n[1/4] Loading settled events...")
    with open(SETTLED_JSON) as f:
        settled = json.load(f)

    # Parse into training examples
    examples = []  # (date_str, icao, target_doy, all_buckets, winner_idx)
    for ev in settled:
        et = ev["event_ticker"]
        series = et.split("-")[0]
        icao = TICKER_TO_ICAO.get(series)
        if not icao:
            continue
        date_str = event_date(et)
        if not date_str:
            continue
        try:
            dt = datetime.strptime(date_str, "%Y-%m-%d")
            target_doy = dt.timetuple().tm_yday
        except ValueError:
            continue

        # Parse buckets
        raw_buckets = ev.get("buckets", [])
        buckets = []
        winner_idx = -1
        for b in raw_buckets:
            lo, hi = parse_bucket_sub(b.get("yes_sub_title", ""))
            if lo is None and hi is None:
                continue
            is_win = (b.get("result") == "yes" or b.get("ticker") == ev.get("winner", ""))
            if is_win:
                winner_idx = len(buckets)
            buckets.append((lo, hi))

        if winner_idx >= 0 and len(buckets) >= 4:
            examples.append((date_str, icao, target_doy, buckets, winner_idx))

    examples.sort(key=lambda x: x[0])
    print(f"  {len(examples)} events parsed, date range {examples[0][0]} → {examples[-1][0]}")

    # Load raw climate
    print("\n[2/4] Loading 566K station-days of raw climate data...")
    climate = RawClimate()
    print(f"  {climate.n_rows:,} observations loaded in {climate.load_time:.1f}s")
    print(f"  Stations: {sorted(climate.data.keys())}")

    # Build feature matrix
    print("\n[3/4] Extracting features (no lookahead)...")
    t_feat = time.time()
    X_list = []
    y_list = []
    meta = []  # (date, icao, bucket_idx, winner_idx, n_buckets)

    for i, (date_str, icao, target_doy, buckets, winner_idx) in enumerate(examples):
        if i > 0 and i % 500 == 0:
            elapsed = time.time() - t_feat
            rate = i / elapsed
            eta = (len(examples) - i) / rate
            print(f"  {i}/{len(examples)} events ({rate:.0f}/s, ETA {eta:.0f}s)")

        for bidx, (lo, hi) in enumerate(buckets):
            features = extract_features(
                climate, icao, target_doy, date_str,
                lo, hi, buckets, bidx
            )
            X_list.append(features)
            y_list.append(1 if bidx == winner_idx else 0)
            meta.append((date_str, icao, bidx, winner_idx, len(buckets)))

    X = np.array(X_list)
    y = np.array(y_list)
    print(f"  {X.shape[0]:,} rows × {X.shape[1]} features in {time.time()-t_feat:.1f}s")
    print(f"  Positive rate: {y.mean():.1%} ({y.sum():.0f}/{len(y)})")

    # Walk-forward train/test
    print("\n[4/4] Walk-forward backtest (3 folds)...")
    dates = [m[0] for m in meta]
    unique_dates = sorted(set(dates))
    n_dates = len(unique_dates)

    # 3-fold time series split
    fold_results = []
    for fold in range(3):
        train_end_idx = int(n_dates * (0.4 + fold * 0.2))
        test_end_idx = int(n_dates * (0.6 + fold * 0.2))
        train_cutoff = unique_dates[train_end_idx]
        test_cutoff = unique_dates[min(test_end_idx, n_dates - 1)]

        train_mask = np.array([d < train_cutoff for d in dates])
        test_mask = np.array([(d >= train_cutoff) & (d < test_cutoff) for d in dates])

        if train_mask.sum() < 100 or test_mask.sum() < 50:
            continue

        X_train, y_train = X[train_mask], y[train_mask]
        X_test, y_test = X[test_mask], y[test_mask]

        model = lgb.LGBMClassifier(
            n_estimators=300, max_depth=7, learning_rate=0.04,
            num_leaves=40, min_child_samples=30,
            subsample=0.8, colsample_bytree=0.7,
            reg_alpha=0.1, reg_lambda=0.5,
            verbose=-1, n_jobs=-1,
        )
        model.fit(X_train, y_train)
        probs = model.predict_proba(X_test)[:, 1]

        bs = brier_score_loss(y_test, probs)
        ll = log_loss(y_test, probs)
        n_b = int(np.median([m[4] for m, mask in zip(meta, test_mask) if mask]))
        uniform_bs = brier_score_loss(y_test, np.full_like(probs, 1.0 / n_b))

        fold_results.append({
            "fold": fold + 1, "train": train_mask.sum(), "test": test_mask.sum(),
            "brier": bs, "uniform_brier": uniform_bs, "logloss": ll,
            "test_mask": test_mask, "probs": probs, "y_test": y_test,
        })
        imp = (uniform_bs - bs) / uniform_bs * 100
        print(f"  Fold {fold+1}: train={train_mask.sum():,} test={test_mask.sum():,} "
              f"Brier={bs:.4f} vs uniform={uniform_bs:.4f} ({imp:+.1f}%)")

    # Train final model on all data
    print("\n  Training final model on all data...")
    final_model = lgb.LGBMClassifier(
        n_estimators=300, max_depth=7, learning_rate=0.04,
        num_leaves=40, min_child_samples=30,
        subsample=0.8, colsample_bytree=0.7,
        reg_alpha=0.1, reg_lambda=0.5,
        verbose=-1, n_jobs=-1,
    )
    final_model.fit(X, y)

    # Feature importance
    importances = sorted(zip(FEATURE_NAMES, final_model.feature_importances_),
                         key=lambda x: -x[1])

    # ── AGGREGATE RESULTS ──
    print("\n" + "=" * 72)
    print("RESULTS")
    print("=" * 72)

    if fold_results:
        avg_brier = np.mean([f["brier"] for f in fold_results])
        avg_uniform = np.mean([f["uniform_brier"] for f in fold_results])
        avg_imp = (avg_uniform - avg_brier) / avg_uniform * 100

        print(f"\n  Mean Brier (model):   {avg_brier:.4f}")
        print(f"  Mean Brier (uniform): {avg_uniform:.4f}")
        print(f"  Improvement:          {avg_imp:+.1f}%")

    # Simulate trading on all test folds combined
    print(f"\n  ── Trading Simulation ──")
    all_trades = []
    for fr in fold_results:
        test_indices = np.where(fr["test_mask"])[0]
        probs = fr["probs"]

        # Group by event (consecutive rows with same date+icao)
        event_groups = defaultdict(list)
        for i, idx in enumerate(test_indices):
            date_str, icao, bidx, winner_idx, n_b = meta[idx]
            event_groups[(date_str, icao)].append({
                "bidx": bidx, "prob": probs[i], "winner": bidx == winner_idx,
                "n_b": n_b,
            })

        for (date_str, icao), buckets in event_groups.items():
            # Normalize probs within event
            total_p = sum(b["prob"] for b in buckets)
            if total_p <= 0:
                continue
            for b in buckets:
                b["norm_prob"] = b["prob"] / total_p

            n_b = buckets[0]["n_b"]
            uniform = 1.0 / n_b

            for b in buckets:
                edge = b["norm_prob"] - uniform
                if edge > 0.05:
                    pnl = (1.0 - uniform) if b["winner"] else -uniform
                    all_trades.append({
                        "date": date_str, "icao": icao,
                        "edge": edge, "prob": b["norm_prob"],
                        "won": b["winner"], "pnl": pnl,
                    })

    if all_trades:
        total_pnl = sum(t["pnl"] for t in all_trades)
        wins = sum(1 for t in all_trades if t["won"])
        wr = wins / len(all_trades)
        avg_edge = np.mean([t["edge"] for t in all_trades])

        cum = 0; peak = 0; dd = 0
        for t in all_trades:
            cum += t["pnl"]; peak = max(peak, cum); dd = max(dd, peak - cum)

        print(f"  Signals:        {len(all_trades)}")
        print(f"  Win rate:       {wr:.1%}")
        print(f"  Avg edge:       {avg_edge:+.1%}")
        print(f"  Total P&L:      ${total_pnl:+.2f}")
        print(f"  Max drawdown:   ${dd:.2f}")
        print(f"  Profit factor:  {sum(t['pnl'] for t in all_trades if t['pnl']>0) / abs(sum(t['pnl'] for t in all_trades if t['pnl']<0) or 1):.2f}")

        # By edge bucket
        print(f"\n  ── By Edge Size ──")
        for lo, hi, label in [(0.05, 0.10, " 5-10%"), (0.10, 0.20, "10-20%"),
                               (0.20, 0.35, "20-35%"), (0.35, 1.0, "  35%+")]:
            eb = [t for t in all_trades if lo <= t["edge"] < hi]
            if eb:
                ep = sum(t["pnl"] for t in eb)
                ew = sum(1 for t in eb if t["won"]) / len(eb)
                print(f"    {label}: {len(eb):4d} trades  WR={ew:.0%}  P&L=${ep:+.2f}")

        # By station
        print(f"\n  ── By Station ──")
        by_stn = defaultdict(list)
        for t in all_trades:
            by_stn[t["icao"]].append(t)
        for stn in sorted(by_stn, key=lambda s: sum(t["pnl"] for t in by_stn[s]), reverse=True)[:15]:
            ts = by_stn[stn]
            sp = sum(t["pnl"] for t in ts)
            sw = sum(1 for t in ts if t["won"]) / len(ts)
            print(f"    {stn}: {len(ts):4d} trades  WR={sw:.0%}  P&L=${sp:+.2f}")

        # Calibration
        print(f"\n  ── Calibration ──")
        for plo, phi in [(0.0, 0.15), (0.15, 0.25), (0.25, 0.40), (0.40, 0.60), (0.60, 1.0)]:
            cal = [t for t in all_trades if plo <= t["prob"] < phi]
            if len(cal) >= 10:
                pred = np.mean([t["prob"] for t in cal])
                actual = sum(1 for t in cal if t["won"]) / len(cal)
                bar = "█" * int(actual * 40)
                gap = abs(actual - pred)
                ok = "✓" if gap < 0.08 else "✗"
                print(f"    pred {plo:.0%}-{phi:.0%}: actual={actual:.0%} "
                      f"(n={len(cal):4d}) {ok} {bar}")

    # Feature importance
    print(f"\n  ── Top 15 Features ──")
    for name, imp in importances[:15]:
        bar = "█" * (imp // 5)
        print(f"    {name:<30} {imp:>5}  {bar}")

    print(f"\n  Total time: {time.time() - t_start:.1f}s")


if __name__ == "__main__":
    main()
