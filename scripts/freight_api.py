#!/usr/bin/env python3
"""
DSCO Freight Terminal — Data API Server
Port 8422 | Demo data with realistic random-walk simulation
"""

import json
import math
import random
import time
import hashlib
from datetime import datetime, timedelta
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import threading

# ── Configuration ───────────────────────────────────────────────────────────

PORT = 8422
CORS_ORIGIN = "http://127.0.0.1:8421"

# ── Cache Layer ─────────────────────────────────────────────────────────────

class Cache:
    def __init__(self):
        self._store = {}
        self._lock = threading.Lock()

    def get(self, key, ttl_seconds):
        with self._lock:
            if key in self._store:
                val, ts = self._store[key]
                if time.time() - ts < ttl_seconds:
                    return val
        return None

    def set(self, key, value):
        with self._lock:
            self._store[key] = (value, time.time())

cache = Cache()

# ── Rate Simulation Engine ──────────────────────────────────────────────────

class RateSimulator:
    """Generates realistic freight rate random walks per vessel class."""

    VESSEL_PARAMS = {
        # vessel_class: (base_rate, volatility, unit, mean_revert_speed)
        "VLCC":       (55.0,   8.0,   "WS",     0.02),
        "Suezmax":    (75.0,   10.0,  "WS",     0.02),
        "Aframax":    (90.0,   12.0,  "WS",     0.025),
        "Panamax_T":  (85.0,   10.0,  "WS",     0.02),
        "MR":         (110.0,  15.0,  "WS",     0.025),
        "LR1":        (95.0,   12.0,  "WS",     0.02),
        "LR2":        (80.0,   10.0,  "WS",     0.02),
        "Handy_T":    (120.0,  18.0,  "WS",     0.03),
        "Capesize":   (22000,  4000,  "$/day",  0.015),
        "Panamax":    (14000,  3000,  "$/day",  0.02),
        "Supramax":   (13000,  2500,  "$/day",  0.02),
        "Handysize":  (11000,  2000,  "$/day",  0.025),
        "VLGC":       (45.0,   8.0,   "$/mt",   0.02),
        "MGC":        (38.0,   6.0,   "$/mt",   0.02),
        "Container":  (2200,   400,   "$/TEU",  0.02),
        "Container_40": (3800, 600,   "$/FEU",  0.02),
    }

    def __init__(self, seed=42):
        self._rng = random.Random(seed)
        self._rates = {}
        self._history = {}
        self._initialized = False

    def _init_rates(self):
        if self._initialized:
            return
        self._initialized = True
        now = time.time()
        # Generate 365 days of history
        for route_id, params in ROUTES.items():
            vc = params.get("vessel_class", "Capesize")
            base, vol, unit, mr = self.VESSEL_PARAMS.get(vc, (15000, 3000, "$/day", 0.02))
            history = []
            rate = base * (0.8 + self._rng.random() * 0.4)
            for d in range(365, -1, -1):
                ts = now - d * 86400
                shock = self._rng.gauss(0, 1) * vol * 0.05
                mean_pull = mr * (base - rate)
                # Seasonal component
                day_of_year = datetime.fromtimestamp(ts).timetuple().tm_yday
                seasonal = math.sin(2 * math.pi * day_of_year / 365) * vol * 0.3
                rate += shock + mean_pull + seasonal * 0.01
                rate = max(rate * 0.3, rate)  # floor
                history.append({"ts": ts, "rate": round(rate, 2)})
            self._history[route_id] = history
            self._rates[route_id] = history[-1]["rate"]

    def get_current_rate(self, route_id):
        self._init_rates()
        return self._rates.get(route_id, 0)

    def get_history(self, route_id, days=365):
        self._init_rates()
        hist = self._history.get(route_id, [])
        if days < 365:
            return hist[-days:]
        return hist

    def tick(self):
        """Advance all rates by one step (called periodically)."""
        self._init_rates()
        now = time.time()
        for route_id, params in ROUTES.items():
            vc = params.get("vessel_class", "Capesize")
            base, vol, unit, mr = self.VESSEL_PARAMS.get(vc, (15000, 3000, "$/day", 0.02))
            rate = self._rates[route_id]
            shock = self._rng.gauss(0, 1) * vol * 0.02
            mean_pull = mr * (base - rate) * 0.1
            rate += shock + mean_pull
            rate = max(base * 0.2, rate)
            self._rates[route_id] = round(rate, 2)
            self._history[route_id].append({"ts": now, "rate": rate})
            # Keep max 400 points
            if len(self._history[route_id]) > 400:
                self._history[route_id] = self._history[route_id][-380:]

simulator = RateSimulator()

# ── Route Database ──────────────────────────────────────────────────────────

ROUTES = {
    # ── Dry Bulk: Capesize ──
    "C2":   {"name": "Tubarao-Rotterdam", "vessel_class": "Capesize", "size": 180000, "region": "Atlantic", "from": [-20.3, -40.2], "to": [51.9, 4.5]},
    "C3":   {"name": "Tubarao-Qingdao", "vessel_class": "Capesize", "size": 180000, "region": "Atlantic-Pacific", "from": [-20.3, -40.2], "to": [36.1, 120.3]},
    "C4":   {"name": "Richards Bay-Rotterdam", "vessel_class": "Capesize", "size": 180000, "region": "Atlantic", "from": [-28.8, 32.1], "to": [51.9, 4.5]},
    "C5":   {"name": "W Australia-Qingdao", "vessel_class": "Capesize", "size": 180000, "region": "Pacific", "from": [-20.7, 116.8], "to": [36.1, 120.3]},
    "C7":   {"name": "Bolivar-Rotterdam", "vessel_class": "Capesize", "size": 180000, "region": "Atlantic", "from": [10.0, -67.0], "to": [51.9, 4.5]},
    "C8_14": {"name": "Gibraltar-Hamburg T/C", "vessel_class": "Capesize", "size": 180000, "region": "Atlantic", "from": [36.1, -5.3], "to": [53.5, 10.0]},
    "C9_14": {"name": "Continent-Med trip", "vessel_class": "Capesize", "size": 180000, "region": "Atlantic", "from": [51.9, 4.5], "to": [37.5, 15.1]},
    "C10_14": {"name": "China-Japan Pacific RV", "vessel_class": "Capesize", "size": 180000, "region": "Pacific", "from": [31.2, 121.5], "to": [35.4, 139.6]},
    "C14":  {"name": "China-Brazil RV", "vessel_class": "Capesize", "size": 180000, "region": "Pacific-Atlantic", "from": [31.2, 121.5], "to": [-23.0, -43.2]},
    "C16":  {"name": "Revised C16 Pacific RV", "vessel_class": "Capesize", "size": 180000, "region": "Pacific", "from": [36.1, 120.3], "to": [-33.9, 151.2]},

    # ── Dry Bulk: Panamax ──
    "P1A_82": {"name": "Skaw-Gibraltar T/C", "vessel_class": "Panamax", "size": 82500, "region": "Atlantic", "from": [57.7, 10.6], "to": [36.1, -5.3]},
    "P2A_82": {"name": "Skaw-Gibraltar trip", "vessel_class": "Panamax", "size": 82500, "region": "Atlantic", "from": [57.7, 10.6], "to": [36.1, -5.3]},
    "P3A_82": {"name": "Japan-SK Pacific RV", "vessel_class": "Panamax", "size": 82500, "region": "Pacific", "from": [35.4, 139.6], "to": [35.1, 129.0]},
    "P4_82": {"name": "Japan-SK Indo RV", "vessel_class": "Panamax", "size": 82500, "region": "Pacific", "from": [35.4, 139.6], "to": [1.3, 103.8]},
    "P5_82": {"name": "EC South America RV", "vessel_class": "Panamax", "size": 82500, "region": "Atlantic", "from": [-23.0, -43.2], "to": [-34.6, -58.4]},
    "P6_82": {"name": "Continent-ECSA trip", "vessel_class": "Panamax", "size": 82500, "region": "Atlantic", "from": [51.9, 4.5], "to": [-23.0, -43.2]},

    # ── Dry Bulk: Supramax ──
    "S1B_58": {"name": "Canakkale-Far East", "vessel_class": "Supramax", "size": 58000, "region": "Med-Pacific", "from": [40.1, 26.4], "to": [31.2, 121.5]},
    "S1C_58": {"name": "US Gulf-Skaw-Passero", "vessel_class": "Supramax", "size": 58000, "region": "Atlantic", "from": [29.0, -89.0], "to": [57.7, 10.6]},
    "S2_58": {"name": "Japan-SK Pacific RV", "vessel_class": "Supramax", "size": 58000, "region": "Pacific", "from": [35.4, 139.6], "to": [35.1, 129.0]},
    "S3_58": {"name": "Japan-SK Indo RV", "vessel_class": "Supramax", "size": 58000, "region": "Pacific-Indian", "from": [35.4, 139.6], "to": [1.3, 103.8]},
    "S4A_58": {"name": "USG-Skaw-Passero", "vessel_class": "Supramax", "size": 58000, "region": "Atlantic", "from": [29.0, -89.0], "to": [57.7, 10.6]},
    "S4B_58": {"name": "Skaw-Passero T/C", "vessel_class": "Supramax", "size": 58000, "region": "Atlantic", "from": [57.7, 10.6], "to": [36.7, 15.1]},
    "S5_58": {"name": "W Africa-Far East", "vessel_class": "Supramax", "size": 58000, "region": "Atlantic-Pacific", "from": [6.4, 3.4], "to": [31.2, 121.5]},
    "S8_58": {"name": "SE Asia-China", "vessel_class": "Supramax", "size": 58000, "region": "Pacific", "from": [1.3, 103.8], "to": [31.2, 121.5]},
    "S9_58": {"name": "W Africa-Continent", "vessel_class": "Supramax", "size": 58000, "region": "Atlantic", "from": [6.4, 3.4], "to": [51.9, 4.5]},

    # ── Dry Bulk: Handysize ──
    "HS1_38": {"name": "Skaw-Passero T/C", "vessel_class": "Handysize", "size": 38000, "region": "Atlantic", "from": [57.7, 10.6], "to": [36.7, 15.1]},
    "HS2_38": {"name": "Skaw-Passero trip", "vessel_class": "Handysize", "size": 38000, "region": "Atlantic", "from": [57.7, 10.6], "to": [36.7, 15.1]},
    "HS3_38": {"name": "ECSAm-Skaw-Passero", "vessel_class": "Handysize", "size": 38000, "region": "Atlantic", "from": [-23.0, -43.2], "to": [57.7, 10.6]},
    "HS4_38": {"name": "USG-Skaw-Passero", "vessel_class": "Handysize", "size": 38000, "region": "Atlantic", "from": [29.0, -89.0], "to": [57.7, 10.6]},
    "HS5_38": {"name": "SE Asia-Japan", "vessel_class": "Handysize", "size": 38000, "region": "Pacific", "from": [1.3, 103.8], "to": [35.4, 139.6]},
    "HS6_38": {"name": "Pacific RV", "vessel_class": "Handysize", "size": 38000, "region": "Pacific", "from": [35.4, 139.6], "to": [-33.9, 151.2]},

    # ── Tanker: Dirty ──
    "TD1":  {"name": "MEG-USG VLCC", "vessel_class": "VLCC", "size": 280000, "region": "MEG-Atlantic", "from": [26.2, 50.2], "to": [29.0, -89.0]},
    "TD2":  {"name": "MEG-Singapore VLCC", "vessel_class": "VLCC", "size": 270000, "region": "MEG-Pacific", "from": [26.2, 50.2], "to": [1.3, 103.8]},
    "TD3C": {"name": "MEG-China VLCC", "vessel_class": "VLCC", "size": 270000, "region": "MEG-Pacific", "from": [26.2, 50.2], "to": [31.2, 121.5]},
    "TD6":  {"name": "Black Sea-Med Suezmax", "vessel_class": "Suezmax", "size": 135000, "region": "Med", "from": [41.0, 29.0], "to": [43.3, 5.4]},
    "TD7":  {"name": "N Sea-Continent 80kt", "vessel_class": "Aframax", "size": 80000, "region": "Atlantic", "from": [60.4, 1.8], "to": [51.9, 4.5]},
    "TD8":  {"name": "Kuwait-USG VLCC", "vessel_class": "VLCC", "size": 280000, "region": "MEG-Atlantic", "from": [29.4, 47.9], "to": [29.0, -89.0]},
    "TD9":  {"name": "Caribs-USG Aframax", "vessel_class": "Aframax", "size": 70000, "region": "Atlantic", "from": [10.6, -61.5], "to": [29.0, -89.0]},
    "TD14": {"name": "SE Asia-EC Australia", "vessel_class": "Aframax", "size": 70000, "region": "Pacific", "from": [1.3, 103.8], "to": [-33.9, 151.2]},
    "TD15": {"name": "W Africa-China VLCC", "vessel_class": "VLCC", "size": 260000, "region": "Atlantic-Pacific", "from": [6.4, 3.4], "to": [31.2, 121.5]},
    "TD17": {"name": "Baltic-UKC Aframax", "vessel_class": "Aframax", "size": 100000, "region": "Atlantic", "from": [59.3, 18.1], "to": [51.0, 1.8]},
    "TD19": {"name": "Cross-Med Aframax", "vessel_class": "Aframax", "size": 80000, "region": "Med", "from": [36.8, 10.2], "to": [40.6, 0.3]},
    "TD20": {"name": "WAf-Continent Suezmax", "vessel_class": "Suezmax", "size": 130000, "region": "Atlantic", "from": [6.4, 3.4], "to": [51.9, 4.5]},
    "TD22": {"name": "EC Mexico-USG Panamax", "vessel_class": "Panamax_T", "size": 55000, "region": "Atlantic", "from": [19.2, -96.1], "to": [29.0, -89.0]},
    "TD23": {"name": "MEG-MEG VLCC", "vessel_class": "VLCC", "size": 280000, "region": "MEG", "from": [26.2, 50.2], "to": [24.5, 54.7]},
    "TD24": {"name": "Primorsk-Sidi Kerir", "vessel_class": "Suezmax", "size": 135000, "region": "Baltic-Med", "from": [60.3, 28.7], "to": [31.2, 29.8]},
    "TD25": {"name": "USG-UK-Cont VLCC", "vessel_class": "VLCC", "size": 270000, "region": "Atlantic", "from": [29.0, -89.0], "to": [51.9, 4.5]},
    "TD26": {"name": "MEG-EAf VLCC", "vessel_class": "VLCC", "size": 270000, "region": "MEG-Indian", "from": [26.2, 50.2], "to": [-4.0, 39.7]},

    # ── Tanker: Clean ──
    "TC1":  {"name": "MEG-Japan LR2", "vessel_class": "LR2", "size": 75000, "region": "MEG-Pacific", "from": [26.2, 50.2], "to": [35.4, 139.6]},
    "TC2_37": {"name": "Continent-USAC MR", "vessel_class": "MR", "size": 37000, "region": "Atlantic", "from": [51.9, 4.5], "to": [40.7, -74.0]},
    "TC5":  {"name": "MEG-Japan LR1", "vessel_class": "LR1", "size": 55000, "region": "MEG-Pacific", "from": [26.2, 50.2], "to": [35.4, 139.6]},
    "TC6":  {"name": "Algeria-Euromed Handy", "vessel_class": "Handy_T", "size": 30000, "region": "Med", "from": [36.8, 3.1], "to": [43.3, 5.4]},
    "TC7":  {"name": "Singapore-ECA LR2", "vessel_class": "LR2", "size": 75000, "region": "Pacific", "from": [1.3, 103.8], "to": [31.2, 121.5]},
    "TC8":  {"name": "MEG-UK-Cont LR1", "vessel_class": "LR1", "size": 65000, "region": "MEG-Atlantic", "from": [26.2, 50.2], "to": [51.9, 4.5]},
    "TC9":  {"name": "Baltic-UK-Cont MR", "vessel_class": "MR", "size": 35000, "region": "Atlantic", "from": [59.3, 18.1], "to": [51.9, 4.5]},
    "TC10": {"name": "Korea-Japan MR", "vessel_class": "MR", "size": 40000, "region": "Pacific", "from": [35.1, 129.0], "to": [35.4, 139.6]},
    "TC11": {"name": "Korea-Aus-NZ MR", "vessel_class": "MR", "size": 40000, "region": "Pacific", "from": [35.1, 129.0], "to": [-33.9, 151.2]},
    "TC12": {"name": "Continent-W Africa MR", "vessel_class": "MR", "size": 37000, "region": "Atlantic", "from": [51.9, 4.5], "to": [6.4, 3.4]},
    "TC14": {"name": "SE Asia-EC Australia MR", "vessel_class": "MR", "size": 38000, "region": "Pacific", "from": [1.3, 103.8], "to": [-33.9, 151.2]},
    "TC15": {"name": "MEG-Japan MR", "vessel_class": "MR", "size": 40000, "region": "MEG-Pacific", "from": [26.2, 50.2], "to": [35.4, 139.6]},
    "TC16": {"name": "Amsterdam-Caribs MR", "vessel_class": "MR", "size": 37000, "region": "Atlantic", "from": [52.4, 4.9], "to": [10.6, -61.5]},
    "TC17": {"name": "Jubail-Yosu LR1", "vessel_class": "LR1", "size": 55000, "region": "MEG-Pacific", "from": [27.0, 49.7], "to": [34.7, 127.8]},
    "TC18": {"name": "USG-Brazil MR", "vessel_class": "MR", "size": 38000, "region": "Atlantic", "from": [29.0, -89.0], "to": [-23.0, -43.2]},
    "TC19": {"name": "Amsterdam-W Africa MR", "vessel_class": "MR", "size": 37000, "region": "Atlantic", "from": [52.4, 4.9], "to": [6.4, 3.4]},

    # ── Container: FBX ──
    "FBX01": {"name": "China/EA-N.America WC", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [31.2, 121.5], "to": [33.7, -118.3]},
    "FBX02": {"name": "China/EA-N.America EC", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [31.2, 121.5], "to": [40.7, -74.0]},
    "FBX03": {"name": "China/EA-N.Europe", "vessel_class": "Container", "size": 0, "region": "Asia-Europe", "from": [31.2, 121.5], "to": [51.9, 4.5]},
    "FBX04": {"name": "China/EA-Mediterranean", "vessel_class": "Container", "size": 0, "region": "Asia-Med", "from": [31.2, 121.5], "to": [41.0, 29.0]},
    "FBX05": {"name": "China/EA-S.America EC", "vessel_class": "Container", "size": 0, "region": "Asia-ECSA", "from": [31.2, 121.5], "to": [-23.0, -43.2]},
    "FBX06": {"name": "N.Europe-N.America EC", "vessel_class": "Container", "size": 0, "region": "Transatlantic", "from": [51.9, 4.5], "to": [40.7, -74.0]},
    "FBX07": {"name": "N.Europe-S.America EC", "vessel_class": "Container", "size": 0, "region": "Atlantic", "from": [51.9, 4.5], "to": [-23.0, -43.2]},
    "FBX08": {"name": "N.America EC-N.Europe", "vessel_class": "Container", "size": 0, "region": "Transatlantic", "from": [40.7, -74.0], "to": [51.9, 4.5]},
    "FBX09": {"name": "N.Europe-Mediterranean", "vessel_class": "Container", "size": 0, "region": "Intra-Europe", "from": [51.9, 4.5], "to": [41.0, 29.0]},
    "FBX10": {"name": "China/EA-SE Asia", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [1.3, 103.8]},
    "FBX11": {"name": "China/EA-Middle East", "vessel_class": "Container", "size": 0, "region": "Asia-MEG", "from": [31.2, 121.5], "to": [25.3, 55.3]},
    "FBX12": {"name": "N.America WC-China/EA", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [33.7, -118.3], "to": [31.2, 121.5]},

    # ── Container: SCFI ──
    "SCFI_EU":  {"name": "Shanghai-N.Europe", "vessel_class": "Container", "size": 0, "region": "Asia-Europe", "from": [31.2, 121.5], "to": [51.9, 4.5]},
    "SCFI_MED": {"name": "Shanghai-Mediterranean", "vessel_class": "Container", "size": 0, "region": "Asia-Med", "from": [31.2, 121.5], "to": [37.5, 15.1]},
    "SCFI_USWC": {"name": "Shanghai-US West Coast", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [31.2, 121.5], "to": [33.7, -118.3]},
    "SCFI_USEC": {"name": "Shanghai-US East Coast", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [31.2, 121.5], "to": [40.7, -74.0]},
    "SCFI_PG":  {"name": "Shanghai-Persian Gulf", "vessel_class": "Container", "size": 0, "region": "Asia-MEG", "from": [31.2, 121.5], "to": [26.2, 50.2]},
    "SCFI_SEA": {"name": "Shanghai-SE Asia", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [1.3, 103.8]},
    "SCFI_JP":  {"name": "Shanghai-Japan", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [35.4, 139.6]},
    "SCFI_KR":  {"name": "Shanghai-Korea", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [35.1, 129.0]},
    "SCFI_ECSA": {"name": "Shanghai-E.Coast S.America", "vessel_class": "Container", "size": 0, "region": "Asia-ECSA", "from": [31.2, 121.5], "to": [-23.0, -43.2]},
    "SCFI_WAF": {"name": "Shanghai-W.Africa", "vessel_class": "Container", "size": 0, "region": "Asia-Africa", "from": [31.2, 121.5], "to": [6.4, 3.4]},
    "SCFI_ANZ": {"name": "Shanghai-Australia/NZ", "vessel_class": "Container", "size": 0, "region": "Asia-Oceania", "from": [31.2, 121.5], "to": [-33.9, 151.2]},

    # ── Container: CCFI ──
    "CCFI_EU":  {"name": "China-Europe", "vessel_class": "Container", "size": 0, "region": "Asia-Europe", "from": [31.2, 121.5], "to": [51.9, 4.5]},
    "CCFI_MED": {"name": "China-Med", "vessel_class": "Container", "size": 0, "region": "Asia-Med", "from": [31.2, 121.5], "to": [37.5, 15.1]},
    "CCFI_USWC": {"name": "China-USWC", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [31.2, 121.5], "to": [33.7, -118.3]},
    "CCFI_USEC": {"name": "China-USEC", "vessel_class": "Container", "size": 0, "region": "Transpacific", "from": [31.2, 121.5], "to": [40.7, -74.0]},
    "CCFI_SEA": {"name": "China-SE Asia", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [1.3, 103.8]},
    "CCFI_JP":  {"name": "China-Japan", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [35.4, 139.6]},
    "CCFI_KR":  {"name": "China-Korea", "vessel_class": "Container", "size": 0, "region": "Intra-Asia", "from": [31.2, 121.5], "to": [35.1, 129.0]},
    "CCFI_SAF": {"name": "China-S.Africa", "vessel_class": "Container", "size": 0, "region": "Asia-Africa", "from": [31.2, 121.5], "to": [-33.9, 18.4]},
    "CCFI_ANZ": {"name": "China-Australia/NZ", "vessel_class": "Container", "size": 0, "region": "Asia-Oceania", "from": [31.2, 121.5], "to": [-33.9, 151.2]},
    "CCFI_PG":  {"name": "China-Persian Gulf", "vessel_class": "Container", "size": 0, "region": "Asia-MEG", "from": [31.2, 121.5], "to": [26.2, 50.2]},
    "CCFI_ECSA": {"name": "China-ECSA", "vessel_class": "Container", "size": 0, "region": "Asia-ECSA", "from": [31.2, 121.5], "to": [-23.0, -43.2]},
    "CCFI_WAF": {"name": "China-W.Africa", "vessel_class": "Container", "size": 0, "region": "Asia-Africa", "from": [31.2, 121.5], "to": [6.4, 3.4]},

    # ── Gas Carriers ──
    "LPG_AG_JP": {"name": "AG-Japan VLGC", "vessel_class": "VLGC", "size": 84000, "region": "MEG-Pacific", "from": [26.2, 50.2], "to": [35.4, 139.6]},
    "LPG_USG_ARA": {"name": "Houston-ARA VLGC", "vessel_class": "VLGC", "size": 84000, "region": "Atlantic", "from": [29.0, -89.0], "to": [51.9, 4.5]},
    "LPG_AG_IN": {"name": "AG-India MGC", "vessel_class": "MGC", "size": 38000, "region": "MEG-Indian", "from": [26.2, 50.2], "to": [19.1, 72.9]},
    "LPG_USG_JP": {"name": "Houston-Japan VLGC", "vessel_class": "VLGC", "size": 84000, "region": "Atlantic-Pacific", "from": [29.0, -89.0], "to": [35.4, 139.6]},
}

# ── Chokepoints ─────────────────────────────────────────────────────────────

CHOKEPOINTS = [
    {"id": "suez", "name": "Suez Canal", "lat": 30.46, "lng": 32.35, "status": "open", "delay_days": 0.5, "daily_transits": 52},
    {"id": "panama", "name": "Panama Canal", "lat": 9.08, "lng": -79.68, "status": "restricted", "delay_days": 2.0, "daily_transits": 36},
    {"id": "hormuz", "name": "Strait of Hormuz", "lat": 26.56, "lng": 56.25, "status": "open", "delay_days": 0.0, "daily_transits": 21},
    {"id": "malacca", "name": "Strait of Malacca", "lat": 2.50, "lng": 101.50, "status": "open", "delay_days": 0.0, "daily_transits": 84},
    {"id": "bab_el_mandeb", "name": "Bab el-Mandeb", "lat": 12.58, "lng": 43.33, "status": "elevated_risk", "delay_days": 5.0, "daily_transits": 12},
    {"id": "gibraltar", "name": "Strait of Gibraltar", "lat": 35.96, "lng": -5.35, "status": "open", "delay_days": 0.0, "daily_transits": 65},
    {"id": "dover", "name": "Strait of Dover", "lat": 51.04, "lng": 1.42, "status": "open", "delay_days": 0.0, "daily_transits": 400},
    {"id": "cape_good_hope", "name": "Cape of Good Hope", "lat": -34.36, "lng": 18.47, "status": "open", "delay_days": 4.0, "daily_transits": 30},
]

# ── Composite Indices ───────────────────────────────────────────────────────

INDICES = {
    "BDI": {
        "name": "Baltic Dry Index",
        "components": ["C5", "P1A_82", "S1C_58", "HS1_38"],
        "weights": [0.4, 0.3, 0.2, 0.1],
        "base": 1000,
    },
    "BDTI": {
        "name": "Baltic Dirty Tanker Index",
        "components": ["TD1", "TD3C", "TD6", "TD7", "TD9"],
        "weights": [0.25, 0.25, 0.2, 0.15, 0.15],
        "base": 1000,
    },
    "BCTI": {
        "name": "Baltic Clean Tanker Index",
        "components": ["TC1", "TC2_37", "TC5", "TC6"],
        "weights": [0.3, 0.3, 0.2, 0.2],
        "base": 700,
    },
    "BCI": {
        "name": "Baltic Capesize Index",
        "components": ["C2", "C3", "C5", "C7"],
        "weights": [0.25, 0.25, 0.25, 0.25],
        "base": 2500,
    },
    "BPI": {
        "name": "Baltic Panamax Index",
        "components": ["P1A_82", "P2A_82", "P3A_82", "P5_82"],
        "weights": [0.25, 0.25, 0.25, 0.25],
        "base": 1400,
    },
    "BSI": {
        "name": "Baltic Supramax Index",
        "components": ["S1C_58", "S2_58", "S4A_58", "S5_58"],
        "weights": [0.25, 0.25, 0.25, 0.25],
        "base": 1100,
    },
    "BHSI": {
        "name": "Baltic Handysize Index",
        "components": ["HS1_38", "HS2_38", "HS3_38", "HS5_38"],
        "weights": [0.25, 0.25, 0.25, 0.25],
        "base": 600,
    },
    "FBX": {
        "name": "Freightos Baltic Index (Global)",
        "components": ["FBX01", "FBX02", "FBX03", "FBX06"],
        "weights": [0.3, 0.3, 0.25, 0.15],
        "base": 2500,
    },
    "SCFI": {
        "name": "Shanghai Containerized Freight Index",
        "components": ["SCFI_EU", "SCFI_USWC", "SCFI_USEC", "SCFI_SEA"],
        "weights": [0.3, 0.25, 0.25, 0.2],
        "base": 1800,
    },
}

# ── News Generator ──────────────────────────────────────────────────────────

NEWS_TEMPLATES = [
    "BDI {dir} {pct}% to {val} on {reason}",
    "Capesize rates {dir2} as {reason2}",
    "VLCC MEG-China fixtures at WS {val2}, {dir} {pct}% w/w",
    "Panama Canal draft restrictions {action} to {ft}ft",
    "Container spot rates {dir} on {lane} lane",
    "Suez Canal transit {news_type} amid {situation}",
    "Iron ore shipments from Brazil {dir2} by {pct}% m/m",
    "Dry bulk FFA activity picks up ahead of {season}",
    "Clean tanker rates surge on {factor} demand",
    "LPG VLGC rates {dir2} as US exports {dir3}",
    "Baltic Exchange launches new {asset} contract",
    "IMO 2025 compliance costs impact {segment} margins",
    "Chinese steel production {dir2}, supporting Capesize demand",
    "Red Sea diversions add {days} days to {lane2} transit times",
    "Grain season lifts Panamax rates in {basin}",
]

def generate_news(count=15):
    rng = random.Random(int(time.time() / 120))  # changes every 2 min
    news = []
    dirs = ["up", "down"]
    reasons = ["strong iron ore demand", "weather delays in Brazil", "Chinese restocking",
               "port congestion in Singapore", "seasonal coal trade", "refinery maintenance",
               "geopolitical tensions", "tonnage oversupply", "bunker cost increases",
               "fleet growth concerns", "inventory draws", "pre-holiday shipping rush"]
    lanes = ["Transpacific", "Asia-Europe", "Transatlantic", "Asia-Med", "Intra-Asia"]

    for i in range(count):
        template = rng.choice(NEWS_TEMPLATES)
        d = rng.choice(dirs)
        news.append({
            "id": i,
            "ts": datetime.utcnow().isoformat() + "Z",
            "headline": template.format(
                dir=d, dir2="rise" if d == "up" else "fall",
                dir3="increase" if d == "up" else "decrease",
                pct=round(rng.uniform(0.5, 8.0), 1),
                val=rng.randint(800, 3500),
                val2=rng.randint(30, 120),
                reason=rng.choice(reasons), reason2=rng.choice(reasons),
                action=rng.choice(["eased", "tightened"]),
                ft=rng.choice([44, 45, 46, 47, 50]),
                lane=rng.choice(lanes), lane2=rng.choice(lanes),
                news_type=rng.choice(["delays reported", "queue lengthens", "traffic normalized"]),
                situation=rng.choice(["regional tensions", "maintenance schedule", "weather conditions"]),
                season=rng.choice(["Q1 restocking", "grain season", "winter demand", "golden week"]),
                factor=rng.choice(["naphtha", "jet fuel", "gasoline", "diesel"]),
                asset=rng.choice(["Capesize", "Supramax", "VLCC", "MR"]),
                segment=rng.choice(["tanker", "dry bulk", "container", "gas carrier"]),
                days=rng.randint(5, 15),
                basin=rng.choice(["Atlantic", "Pacific", "Indian Ocean"]),
            ),
            "source": rng.choice(["Baltic Exchange", "Platts", "Clarksons", "Braemar", "SSY", "Reuters"]),
            "sentiment": d,
        })
    return news

# ── HTTP Handler ────────────────────────────────────────────────────────────

class FreightAPIHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # suppress default logging

    def _cors_headers(self):
        return {
            "Access-Control-Allow-Origin": "*",
            "Access-Control-Allow-Methods": "GET, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
        }

    def _send_json(self, data, status=200):
        body = json.dumps(data, separators=(",", ":"))
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        for k, v in self._cors_headers().items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())

    def do_OPTIONS(self):
        self.send_response(204)
        for k, v in self._cors_headers().items():
            self.send_header(k, v)
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")
        params = parse_qs(parsed.query)

        try:
            if path == "/api/health":
                self._send_json({"status": "ok", "ts": time.time(), "routes": len(ROUTES)})

            elif path == "/api/rates":
                cached = cache.get("all_rates", 30)
                if cached:
                    self._send_json(cached)
                    return
                result = {}
                for rid, rinfo in ROUTES.items():
                    rate = simulator.get_current_rate(rid)
                    hist = simulator.get_history(rid, 30)
                    prev = hist[-2]["rate"] if len(hist) >= 2 else rate
                    chg = rate - prev
                    chg_pct = (chg / prev * 100) if prev != 0 else 0
                    # Sparkline: last 14 points
                    spark = [h["rate"] for h in hist[-14:]]
                    vc = rinfo.get("vessel_class", "Capesize")
                    _, _, unit, _ = RateSimulator.VESSEL_PARAMS.get(vc, (0, 0, "$/day", 0))
                    result[rid] = {
                        "id": rid,
                        "name": rinfo["name"],
                        "vessel_class": vc,
                        "size": rinfo.get("size", 0),
                        "region": rinfo.get("region", ""),
                        "rate": round(rate, 2),
                        "change": round(chg, 2),
                        "change_pct": round(chg_pct, 2),
                        "unit": unit,
                        "sparkline": [round(s, 2) for s in spark],
                        "from": rinfo.get("from", [0, 0]),
                        "to": rinfo.get("to", [0, 0]),
                    }
                cache.set("all_rates", result)
                self._send_json(result)

            elif path.startswith("/api/rates/"):
                route_id = path.split("/")[-1]
                if route_id not in ROUTES:
                    self._send_json({"error": "route not found"}, 404)
                    return
                days = int(params.get("days", ["365"])[0])
                hist = simulator.get_history(route_id, days)
                rinfo = ROUTES[route_id]
                vc = rinfo.get("vessel_class", "Capesize")
                _, _, unit, _ = RateSimulator.VESSEL_PARAMS.get(vc, (0, 0, "$/day", 0))
                self._send_json({
                    "id": route_id,
                    "name": rinfo["name"],
                    "vessel_class": vc,
                    "unit": unit,
                    "history": [{"ts": h["ts"], "rate": round(h["rate"], 2)} for h in hist],
                })

            elif path == "/api/indices":
                cached = cache.get("indices", 30)
                if cached:
                    self._send_json(cached)
                    return
                result = {}
                for idx_id, idx_info in INDICES.items():
                    val = 0
                    for comp, w in zip(idx_info["components"], idx_info["weights"]):
                        rate = simulator.get_current_rate(comp)
                        vc = ROUTES.get(comp, {}).get("vessel_class", "Capesize")
                        base, _, _, _ = RateSimulator.VESSEL_PARAMS.get(vc, (15000, 0, "", 0))
                        val += (rate / base) * w * idx_info["base"]
                    # Get previous
                    prev_val = val * (1 + random.uniform(-0.02, 0.02))
                    chg = val - prev_val
                    chg_pct = (chg / prev_val * 100) if prev_val else 0
                    result[idx_id] = {
                        "id": idx_id,
                        "name": idx_info["name"],
                        "value": round(val, 1),
                        "change": round(chg, 1),
                        "change_pct": round(chg_pct, 2),
                        "components": idx_info["components"],
                    }
                cache.set("indices", result)
                self._send_json(result)

            elif path.startswith("/api/container/"):
                idx = path.split("/")[-1].upper()
                prefix_map = {"FBX": "FBX", "SCFI": "SCFI_", "CCFI": "CCFI_"}
                prefix = prefix_map.get(idx)
                if not prefix:
                    self._send_json({"error": "unknown container index"}, 404)
                    return
                result = {}
                for rid, rinfo in ROUTES.items():
                    if rid.startswith(prefix) or (idx == "FBX" and rid.startswith("FBX")):
                        rate = simulator.get_current_rate(rid)
                        hist = simulator.get_history(rid, 30)
                        prev = hist[-2]["rate"] if len(hist) >= 2 else rate
                        spark = [h["rate"] for h in hist[-14:]]
                        result[rid] = {
                            "id": rid,
                            "name": rinfo["name"],
                            "rate": round(rate, 2),
                            "change": round(rate - prev, 2),
                            "change_pct": round((rate - prev) / prev * 100, 2) if prev else 0,
                            "unit": "$/TEU",
                            "sparkline": [round(s, 2) for s in spark],
                        }
                self._send_json(result)

            elif path == "/api/chokepoints":
                cached = cache.get("chokepoints", 60)
                if cached:
                    self._send_json(cached)
                    return
                # Add some randomness to status
                rng = random.Random(int(time.time() / 300))
                cps = []
                for cp in CHOKEPOINTS:
                    c = dict(cp)
                    if rng.random() < 0.1:
                        c["status"] = rng.choice(["restricted", "elevated_risk"])
                        c["delay_days"] = round(rng.uniform(1, 10), 1)
                    cps.append(c)
                cache.set("chokepoints", cps)
                self._send_json(cps)

            elif path == "/api/news":
                cached = cache.get("news", 120)
                if cached:
                    self._send_json(cached)
                    return
                news = generate_news(15)
                cache.set("news", news)
                self._send_json(news)

            elif path == "/api/derivatives":
                cached = cache.get("derivatives", 60)
                if cached:
                    self._send_json(cached)
                    return
                result = self._build_derivatives()
                cache.set("derivatives", result)
                self._send_json(result)

            else:
                self._send_json({"error": "not found"}, 404)

        except Exception as e:
            self._send_json({"error": str(e)}, 500)

    def _build_derivatives(self):
        """Generate derivative contract data for exchanges."""
        rng = random.Random(int(time.time() / 60))
        exchanges = {}

        # Forward curve months
        months = []
        now = datetime.utcnow()
        for i in range(12):
            d = now + timedelta(days=30 * (i + 1))
            months.append(d.strftime("%b%y").upper())

        for exchange, contracts in [
            ("EEX", [("Cape5TC", "Capesize 5TC", "$/day"), ("P5TC", "Panamax 5TC", "$/day"), ("S10TC", "Supramax 10TC", "$/day")]),
            ("ICE", [("BDI", "Baltic Dry Index", "pts"), ("BDTI", "Baltic Dirty Tanker", "pts")]),
            ("CME", [("LNG_JKM", "JKM LNG", "$/MMBtu"), ("VLCC_TD3C", "VLCC MEG-China", "WS")]),
            ("SGX", [("FFA_Cape", "Capesize FFA", "$/day"), ("FFA_Pana", "Panamax FFA", "$/day"), ("IO62", "Iron Ore 62% Fe", "$/mt")]),
        ]:
            contracts_data = []
            for code, name, unit in contracts:
                base = simulator.get_current_rate(
                    {"Cape5TC": "C5", "P5TC": "P1A_82", "S10TC": "S1C_58",
                     "BDI": "C5", "BDTI": "TD3C", "LNG_JKM": "LPG_AG_JP",
                     "VLCC_TD3C": "TD3C", "FFA_Cape": "C5", "FFA_Pana": "P1A_82",
                     "IO62": "C5"}.get(code, "C5")
                )
                curve = []
                val = base
                for m in months:
                    val *= (1 + rng.gauss(0, 0.02))
                    vol = rng.randint(50, 2000)
                    oi = rng.randint(500, 15000)
                    curve.append({
                        "month": m,
                        "settle": round(val, 2),
                        "bid": round(val * 0.998, 2),
                        "ask": round(val * 1.002, 2),
                        "volume": vol,
                        "open_interest": oi,
                    })
                contracts_data.append({
                    "code": code,
                    "name": name,
                    "unit": unit,
                    "curve": curve,
                })
            exchanges[exchange] = contracts_data

        return exchanges


# ── Background Tick ─────────────────────────────────────────────────────────

def tick_loop():
    while True:
        time.sleep(10)
        simulator.tick()

# ── Main ────────────────────────────────────────────────────────────────────

def main():
    # Start background ticker
    t = threading.Thread(target=tick_loop, daemon=True)
    t.start()

    # Initialize simulator
    simulator._init_rates()

    server = HTTPServer(("127.0.0.1", PORT), FreightAPIHandler)
    print(f"\n  DSCO Freight API running on http://127.0.0.1:{PORT}")
    print(f"  Routes: {len(ROUTES)} | Chokepoints: {len(CHOKEPOINTS)} | Indices: {len(INDICES)}")
    print(f"  Endpoints: /api/rates /api/indices /api/chokepoints /api/news /api/derivatives")
    print(f"  Press Ctrl+C to stop.\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n  Shutting down freight API...")
        server.shutdown()

if __name__ == "__main__":
    main()
