#!/usr/bin/env python3
"""
Full BUFKIT Parser — extracts every field the BUFKIT format provides.
=====================================================================

Sounding params (SNPARM) per vertical level:
  PRES  - Pressure (mb)
  TMPC  - Temperature (°C)
  TMWC  - Wet-bulb temperature (°C)
  DWPC  - Dewpoint (°C)
  THTE  - Equivalent potential temperature (K)
  DRCT  - Wind direction (°)
  SKNT  - Wind speed (kt)
  OMEG  - Omega / vertical velocity (μb/s)
  CFRL  - Cloud fraction (%, 0-100)  [not in GFS]
  HGHT  - Geopotential height (m)

Station/surface params (STNPRM) per forecast hour:
  SHOW  - Showalter Index
  LIFT  - Lifted Index
  SWET  - Severe Weather Threat Index
  KINX  - K-Index
  LCLP  - LCL pressure (mb)
  PWAT  - Precipitable water (mm)
  TOTL  - Total Totals Index
  CAPE  - CAPE (J/kg)
  LCLT  - LCL temperature (K)
  CINS  - CIN (J/kg)
  EQLV  - Equilibrium Level (mb)
  LFCT  - Level of Free Convection (mb)
  BRCH  - Bulk Richardson Number

Models supported:
  HRRR     - 19 fhr, 60 levels, hourly cycles
  RAP      - 22 fhr, 60 levels, hourly cycles
  NAM      - 85 fhr, 60 levels, 4x daily
  NAMNEST  - 61 fhr, 60 levels, 4x daily
  GFS      - 141 fhr, 60 levels (no CFRL), 4x daily
  SREF     - 85 fhr × 27 members, 60 levels, 4x daily
"""

import re
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Optional


@dataclass
class SoundingLevel:
    """One vertical level from a BUFKIT sounding."""
    pres: float    # Pressure (mb)
    tmpc: float    # Temperature (°C)
    tmwc: float    # Wet-bulb temp (°C)
    dwpc: float    # Dewpoint (°C)
    thte: float    # Equiv. potential temp (K)
    drct: float    # Wind direction (°)
    sknt: float    # Wind speed (kt)
    omeg: float    # Omega (μb/s), negative = rising
    hght: float    # Geopotential height (m)
    cfrl: float = 0.0  # Cloud fraction (%), not in GFS

    @property
    def tmpf(self) -> float:
        return self.tmpc * 9/5 + 32

    @property
    def dwpf(self) -> float:
        return self.dwpc * 9/5 + 32

    @property
    def rh(self) -> float:
        """Approximate relative humidity (%)."""
        if self.tmpc < -900 or self.dwpc < -900:
            return -9999.0
        return 100 - 5 * (self.tmpc - self.dwpc)

    @property
    def wspd_mph(self) -> float:
        return self.sknt * 1.15078

    @property
    def hght_ft(self) -> float:
        return self.hght * 3.28084


@dataclass
class SurfaceParams:
    """Derived surface/stability parameters for one forecast hour."""
    show: float = -9999.0   # Showalter Index
    lift: float = -9999.0   # Lifted Index
    swet: float = -9999.0   # Severe Weather Threat Index
    kinx: float = -9999.0   # K-Index
    lclp: float = -9999.0   # LCL pressure (mb)
    pwat: float = -9999.0   # Precipitable water (mm)
    totl: float = -9999.0   # Total Totals Index
    cape: float = -9999.0   # CAPE (J/kg)
    lclt: float = -9999.0   # LCL temperature (K)
    cins: float = -9999.0   # CIN (J/kg)
    eqlv: float = -9999.0   # Equilibrium level (mb)
    lfct: float = -9999.0   # Level of Free Convection (mb)
    brch: float = -9999.0   # Bulk Richardson Number

    @property
    def convective_risk(self) -> str:
        """Quick convective risk assessment."""
        if self.cape > 2500 and self.cins > -50:
            return "HIGH"
        if self.cape > 1000:
            return "MODERATE"
        if self.cape > 500:
            return "SLIGHT"
        return "LOW"


@dataclass
class ForecastSounding:
    """Complete sounding for one forecast hour at one station."""
    station: str
    station_num: int
    valid_utc: datetime
    fhr: int
    lat: float
    lon: float
    elev: float             # station elevation (m)
    surface: SurfaceParams
    levels: list[SoundingLevel] = field(default_factory=list)
    member: int = 0         # ensemble member (0 = deterministic)

    @property
    def n_levels(self) -> int:
        return len(self.levels)

    @property
    def sfc_temp_f(self) -> float:
        """Surface (lowest level) temperature in °F."""
        return self.levels[0].tmpf if self.levels else -9999.0

    @property
    def sfc_temp_c(self) -> float:
        return self.levels[0].tmpc if self.levels else -9999.0

    @property
    def sfc_dewpoint_f(self) -> float:
        return self.levels[0].dwpf if self.levels else -9999.0

    @property
    def sfc_wind(self) -> tuple[float, float]:
        """(direction°, speed_kt) at surface."""
        if self.levels:
            return (self.levels[0].drct, self.levels[0].sknt)
        return (0, 0)

    @property
    def sfc_pres(self) -> float:
        return self.levels[0].pres if self.levels else -9999.0

    def level_at(self, target_mb: float) -> Optional[SoundingLevel]:
        """Find closest level to a target pressure."""
        if not self.levels:
            return None
        return min(self.levels, key=lambda l: abs(l.pres - target_mb))

    def temp_at(self, target_mb: float) -> float:
        """Temperature (°C) at a pressure level."""
        lev = self.level_at(target_mb)
        return lev.tmpc if lev else -9999.0

    def wind_at(self, target_mb: float) -> tuple[float, float]:
        """(dir°, speed_kt) at a pressure level."""
        lev = self.level_at(target_mb)
        return (lev.drct, lev.sknt) if lev else (0, 0)

    def lapse_rate(self, p_top: float = 500, p_bot: float = 700) -> float:
        """Lapse rate (°C/km) between two pressure levels."""
        top = self.level_at(p_top)
        bot = self.level_at(p_bot)
        if not top or not bot or top.hght == bot.hght:
            return -9999.0
        return (bot.tmpc - top.tmpc) / ((top.hght - bot.hght) / 1000)

    def freezing_level(self) -> float:
        """Height (m) of 0°C isotherm."""
        for i in range(len(self.levels) - 1):
            if self.levels[i].tmpc >= 0 and self.levels[i+1].tmpc < 0:
                # Linear interpolation
                t1, h1 = self.levels[i].tmpc, self.levels[i].hght
                t2, h2 = self.levels[i+1].tmpc, self.levels[i+1].hght
                if t1 == t2:
                    return h1
                frac = t1 / (t1 - t2)
                return h1 + frac * (h2 - h1)
        return -9999.0

    def inversion(self) -> Optional[tuple[float, float, float]]:
        """Find lowest temperature inversion. Returns (base_mb, top_mb, strength_C) or None."""
        for i in range(len(self.levels) - 1):
            if self.levels[i+1].tmpc > self.levels[i].tmpc:
                # Found inversion — find top
                base = self.levels[i]
                top = self.levels[i+1]
                for j in range(i+2, len(self.levels)):
                    if self.levels[j].tmpc < self.levels[j-1].tmpc:
                        break
                    top = self.levels[j]
                return (base.pres, top.pres, top.tmpc - base.tmpc)
        return None

    def precipitable_water_calc(self) -> float:
        """Calculate PWAT from sounding (mm). Cross-check vs STNPRM PWAT."""
        # Simplified: integrate mixing ratio through column
        pw = 0.0
        for i in range(len(self.levels) - 1):
            dp = self.levels[i].pres - self.levels[i+1].pres
            if dp <= 0:
                continue
            # Average dewpoint depression → mixing ratio approximation
            td_avg = (self.levels[i].dwpc + self.levels[i+1].dwpc) / 2
            t_avg = (self.levels[i].tmpc + self.levels[i+1].tmpc) / 2
            if td_avg < -100:
                continue
            # Bolton's equation for saturation vapor pressure
            es = 6.112 * 2.71828 ** (17.67 * td_avg / (td_avg + 243.5))
            p_avg = (self.levels[i].pres + self.levels[i+1].pres) / 2
            w = 0.622 * es / (p_avg - es)  # mixing ratio kg/kg
            pw += w * dp * 100 / 9.81  # convert to mm
        return round(pw, 1)

    def max_wind(self) -> tuple[float, float, float]:
        """Find max wind in the column. Returns (pres_mb, dir°, speed_kt)."""
        if not self.levels:
            return (0, 0, 0)
        mx = max(self.levels, key=lambda l: l.sknt)
        return (mx.pres, mx.drct, mx.sknt)

    def wind_shear_0_6km(self) -> float:
        """Bulk wind shear magnitude (kt) from surface to ~6km AGL."""
        if len(self.levels) < 2:
            return 0
        sfc = self.levels[0]
        sfc_hgt = sfc.hght
        # Find level closest to 6km AGL
        target_hgt = sfc_hgt + 6000
        upper = min(self.levels, key=lambda l: abs(l.hght - target_hgt))

        import math
        u_sfc = -sfc.sknt * math.sin(math.radians(sfc.drct))
        v_sfc = -sfc.sknt * math.cos(math.radians(sfc.drct))
        u_top = -upper.sknt * math.sin(math.radians(upper.drct))
        v_top = -upper.sknt * math.cos(math.radians(upper.drct))

        return round(math.sqrt((u_top - u_sfc)**2 + (v_top - v_sfc)**2), 1)

    def cloud_layers(self) -> list[tuple[float, float, float]]:
        """Identify cloud layers from CFRL. Returns [(base_mb, top_mb, max_fraction), ...]."""
        layers = []
        in_cloud = False
        base = top = max_frac = 0

        for lev in self.levels:
            if lev.cfrl > 5:
                if not in_cloud:
                    base = lev.pres
                    max_frac = lev.cfrl
                    in_cloud = True
                else:
                    max_frac = max(max_frac, lev.cfrl)
                top = lev.pres
            else:
                if in_cloud:
                    layers.append((base, top, max_frac))
                    in_cloud = False

        if in_cloud:
            layers.append((base, top, max_frac))
        return layers


@dataclass
class BufkitFile:
    """Complete parsed BUFKIT file with all forecasts and metadata."""
    model: str
    station: str
    soundings: list[ForecastSounding] = field(default_factory=list)
    snparm_fields: list[str] = field(default_factory=list)
    stnprm_fields: list[str] = field(default_factory=list)

    @property
    def n_hours(self) -> int:
        return len(set(s.fhr for s in self.soundings))

    @property
    def n_members(self) -> int:
        return len(set(s.member for s in self.soundings)) or 1

    @property
    def is_ensemble(self) -> bool:
        return self.n_members > 1

    @property
    def forecast_hours(self) -> list[int]:
        return sorted(set(s.fhr for s in self.soundings))

    def at_fhr(self, fhr: int, member: int = 0) -> Optional[ForecastSounding]:
        """Get sounding at a specific forecast hour (and member)."""
        for s in self.soundings:
            if s.fhr == fhr and s.member == member:
                return s
        return None

    def surface_timeseries(self, member: int = 0) -> list[dict]:
        """Extract surface temperature timeseries."""
        return [
            {"fhr": s.fhr, "valid": s.valid_utc, "temp_f": s.sfc_temp_f,
             "dp_f": s.sfc_dewpoint_f, "wind": s.sfc_wind,
             "cape": s.surface.cape, "pwat": s.surface.pwat}
            for s in self.soundings if s.member == member
        ]

    def forecast_high(self, member: int = 0) -> tuple[float, datetime]:
        """Max surface temp across all forecast hours."""
        candidates = [s for s in self.soundings if s.member == member and s.sfc_temp_f > -900]
        if not candidates:
            return (-9999.0, datetime.now(timezone.utc))
        best = max(candidates, key=lambda s: s.sfc_temp_f)
        return (best.sfc_temp_f, best.valid_utc)

    def forecast_low(self, member: int = 0) -> tuple[float, datetime]:
        """Min surface temp across all forecast hours."""
        candidates = [s for s in self.soundings if s.member == member and s.sfc_temp_f > -900]
        if not candidates:
            return (-9999.0, datetime.now(timezone.utc))
        best = min(candidates, key=lambda s: s.sfc_temp_f)
        return (best.sfc_temp_f, best.valid_utc)

    def ensemble_spread(self, fhr: int) -> dict:
        """For ensemble models: spread statistics at a forecast hour."""
        members = [s for s in self.soundings if s.fhr == fhr]
        if len(members) < 2:
            return {}
        temps = [s.sfc_temp_f for s in members if s.sfc_temp_f > -900]
        if not temps:
            return {}
        import statistics
        return {
            "mean": round(statistics.mean(temps), 1),
            "std": round(statistics.stdev(temps), 1),
            "min": round(min(temps), 1),
            "max": round(max(temps), 1),
            "range": round(max(temps) - min(temps), 1),
            "n": len(temps),
        }

    def time_height_data(self, field_name: str = "tmpc", member: int = 0) -> dict:
        """
        Extract time-height cross-section data (like BUFKIT's contour display).
        Returns {fhr: [(pres, value), ...]} for plotting.
        """
        result = {}
        for s in self.soundings:
            if s.member != member:
                continue
            profile = []
            for lev in s.levels:
                val = getattr(lev, field_name, None)
                if val is not None and val > -9990:
                    profile.append((lev.pres, val))
            result[s.fhr] = profile
        return result


def parse(raw: bytes, model: str = "unknown") -> BufkitFile:
    """
    Parse a complete BUFKIT file into structured objects.

    Handles all models: HRRR, RAP, NAM, NAMNEST, GFS, SREF.
    For SREF, assigns member numbers based on block sequence.
    """
    try:
        text = raw.decode("utf-8", errors="replace")
    except:
        text = raw.decode("latin-1", errors="replace")

    lines = text.split('\n')
    result = BufkitFile(model=model, station="")

    # Parse field definitions
    for line in lines:
        s = line.strip()
        if s.startswith("SNPARM"):
            raw_f = s.split("=", 1)[1] if "=" in s else ""
            result.snparm_fields = [f.strip() for f in raw_f.replace(";", " ").split() if f.strip()]
        elif s.startswith("STNPRM"):
            raw_f = s.split("=", 1)[1] if "=" in s else ""
            result.stnprm_fields = [f.strip() for f in raw_f.replace(";", " ").split() if f.strip()]

    if not result.snparm_fields:
        return result

    n_sn = len(result.snparm_fields)
    sn_idx = {f: i for i, f in enumerate(result.snparm_fields)}

    # For SREF ensemble: track member counter per forecast hour
    fhr_member_count = {}

    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if not ("STID =" in line and "TIME =" in line):
            i += 1
            continue

        # ── Parse header ──
        stid_m = re.search(r'STID\s*=\s*(\S+)', line)
        stnm_m = re.search(r'STNM\s*=\s*(\d+)', line)
        time_m = re.search(r'TIME\s*=\s*(\d{6})/(\d{4})', line)

        if not time_m:
            i += 1
            continue

        station = stid_m.group(1) if stid_m else ""
        station_num = int(stnm_m.group(1)) if stnm_m else 0
        result.station = station

        ymd, hm = time_m.groups()
        yr = int(ymd[:2]) + (2000 if int(ymd[:2]) < 50 else 1900)
        try:
            valid_utc = datetime(yr, int(ymd[2:4]), int(ymd[4:6]),
                                 int(hm[:2]), int(hm[2:4]), tzinfo=timezone.utc)
        except:
            i += 1
            continue

        # Next line: SLAT, SLON, SELV
        i += 1
        lat = lon = elev = 0.0
        if i < len(lines):
            sl = lines[i].strip()
            lat_m = re.search(r'SLAT\s*=\s*(-?[\d.]+)', sl)
            lon_m = re.search(r'SLON\s*=\s*(-?[\d.]+)', sl)
            elv_m = re.search(r'SELV\s*=\s*(-?[\d.]+)', sl)
            if lat_m: lat = float(lat_m.group(1))
            if lon_m: lon = float(lon_m.group(1))
            if elv_m: elev = float(elv_m.group(1))
            i += 1

        # STIM line
        fhr = 0
        while i < len(lines) and lines[i].strip():
            stim_m = re.search(r'STIM\s*=\s*(\d+)', lines[i].strip())
            if stim_m:
                fhr = int(stim_m.group(1))
            i += 1
        # Skip blank
        while i < len(lines) and not lines[i].strip():
            i += 1

        # ── Parse STNPRM (KEY = value format) ──
        sfc = SurfaceParams()
        sfc_dict = {}
        while i < len(lines) and lines[i].strip():
            sl = lines[i].strip()
            # If it looks like sounding header, stop
            if sl.startswith("PRES") or (sl.split() and sl.split()[0] in result.snparm_fields):
                break
            for km in re.finditer(r'(\w+)\s*=\s*(-?[\d.]+)', sl):
                sfc_dict[km.group(1)] = float(km.group(2))
            i += 1

        sfc.show = sfc_dict.get("SHOW", -9999.0)
        sfc.lift = sfc_dict.get("LIFT", -9999.0)
        sfc.swet = sfc_dict.get("SWET", -9999.0)
        sfc.kinx = sfc_dict.get("KINX", -9999.0)
        sfc.lclp = sfc_dict.get("LCLP", -9999.0)
        sfc.pwat = sfc_dict.get("PWAT", -9999.0)
        sfc.totl = sfc_dict.get("TOTL", -9999.0)
        sfc.cape = sfc_dict.get("CAPE", -9999.0)
        sfc.lclt = sfc_dict.get("LCLT", -9999.0)
        sfc.cins = sfc_dict.get("CINS", -9999.0)
        sfc.eqlv = sfc_dict.get("EQLV", -9999.0)
        sfc.lfct = sfc_dict.get("LFCT", -9999.0)
        sfc.brch = sfc_dict.get("BRCH", -9999.0)

        # Skip blank lines
        while i < len(lines) and not lines[i].strip():
            i += 1

        # ── Skip sounding header ──
        while i < len(lines):
            sl = lines[i].strip()
            if not sl:
                i += 1
                continue
            try:
                float(sl.split()[0])
                break  # numeric = start of data
            except:
                i += 1

        # ── Parse ALL vertical levels ──
        levels = []
        while i < len(lines):
            sl = lines[i].strip()
            if not sl or "STID" in sl:
                break
            try:
                float(sl.split()[0])
            except:
                break

            # Accumulate values for one level (may span 2 lines)
            vals = []
            vals.extend(float(x) for x in sl.split())
            i += 1
            while len(vals) < n_sn and i < len(lines):
                sl2 = lines[i].strip()
                if not sl2:
                    break
                try:
                    vals.extend(float(x) for x in sl2.split())
                    i += 1
                except:
                    break

            if len(vals) >= n_sn:
                lev = SoundingLevel(
                    pres=vals[sn_idx.get("PRES", 0)],
                    tmpc=vals[sn_idx.get("TMPC", 1)],
                    tmwc=vals[sn_idx.get("TMWC", 2)],
                    dwpc=vals[sn_idx.get("DWPC", 3)],
                    thte=vals[sn_idx.get("THTE", 4)],
                    drct=vals[sn_idx.get("DRCT", 5)],
                    sknt=vals[sn_idx.get("SKNT", 6)],
                    omeg=vals[sn_idx.get("OMEG", 7)],
                    hght=vals[sn_idx.get("HGHT", n_sn - 1)],
                    cfrl=vals[sn_idx["CFRL"]] if "CFRL" in sn_idx else 0.0,
                )
                levels.append(lev)

        # Assign ensemble member number
        member = 0
        if fhr not in fhr_member_count:
            fhr_member_count[fhr] = 0
        else:
            fhr_member_count[fhr] += 1
        member = fhr_member_count[fhr]

        sounding = ForecastSounding(
            station=station, station_num=station_num,
            valid_utc=valid_utc, fhr=fhr,
            lat=lat, lon=lon, elev=elev,
            surface=sfc, levels=levels, member=member,
        )
        result.soundings.append(sounding)

    return result


def load(filepath: str, model: str = "unknown") -> BufkitFile:
    """Load and parse a BUFKIT file from disk."""
    with open(filepath, "rb") as f:
        return parse(f.read(), model)


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("Usage: python3 bufkit.py <file.buf> [model_name]")
        sys.exit(1)

    fname = sys.argv[1]
    model = sys.argv[2] if len(sys.argv) > 2 else "unknown"
    bf = load(fname, model)

    print(f"\n  BUFKIT File: {fname}")
    print(f"  Model: {bf.model}  Station: {bf.station}")
    print(f"  SNPARM: {', '.join(bf.snparm_fields)}")
    print(f"  STNPRM: {', '.join(bf.stnprm_fields)}")
    print(f"  Soundings: {len(bf.soundings)}  Hours: {bf.n_hours}  Members: {bf.n_members}")
    print(f"  Forecast hours: {bf.forecast_hours[:10]}{'...' if bf.n_hours > 10 else ''}")

    hi_f, hi_t = bf.forecast_high()
    lo_f, lo_t = bf.forecast_low()
    print(f"\n  Forecast High: {hi_f:.1f}°F at {hi_t.strftime('%m/%d %H:%MZ')}")
    print(f"  Forecast Low:  {lo_f:.1f}°F at {lo_t.strftime('%m/%d %H:%MZ')}")

    # Show first sounding detail
    s0 = bf.soundings[0] if bf.soundings else None
    if s0:
        print(f"\n  First sounding: fhr={s0.fhr} valid={s0.valid_utc.strftime('%m/%d %HZ')}")
        print(f"    Surface: {s0.sfc_temp_f:.1f}°F  Td={s0.sfc_dewpoint_f:.1f}°F  Wind={s0.sfc_wind[0]:.0f}°/{s0.sfc_wind[1]:.0f}kt  P={s0.sfc_pres:.0f}mb")
        print(f"    CAPE={s0.surface.cape:.0f}  CIN={s0.surface.cins:.0f}  PWAT={s0.surface.pwat:.1f}mm  LI={s0.surface.lift:.1f}  KI={s0.surface.kinx:.1f}")
        print(f"    Convective risk: {s0.surface.convective_risk}")
        print(f"    Freezing level: {s0.freezing_level():.0f}m ({s0.freezing_level()*3.281:.0f}ft)")
        print(f"    0-6km shear: {s0.wind_shear_0_6km():.0f}kt")
        inv = s0.inversion()
        if inv:
            print(f"    Inversion: {inv[0]:.0f}-{inv[1]:.0f}mb ({inv[2]:+.1f}°C)")
        clouds = s0.cloud_layers()
        if clouds:
            for base, top, frac in clouds:
                print(f"    Cloud: {base:.0f}-{top:.0f}mb ({frac:.0f}%)")
        mx = s0.max_wind()
        print(f"    Max wind: {mx[2]:.0f}kt at {mx[0]:.0f}mb ({mx[1]:.0f}°)")
        print(f"    700-500 lapse rate: {s0.lapse_rate(500, 700):.1f}°C/km")

        print(f"\n    Key levels:")
        for p in [925, 850, 700, 500, 300, 250]:
            lev = s0.level_at(p)
            if lev and abs(lev.pres - p) < 30:
                print(f"      {lev.pres:6.0f}mb: {lev.tmpc:+6.1f}°C  Td={lev.dwpc:+6.1f}°C  RH={lev.rh:.0f}%  Wind={lev.drct:.0f}°/{lev.sknt:.0f}kt  Hgt={lev.hght:.0f}m  Ω={lev.omeg:+.2f}")

    if bf.is_ensemble:
        print(f"\n  Ensemble spread at fhr=12:")
        sp = bf.ensemble_spread(12)
        if sp:
            print(f"    Mean={sp['mean']}°F  Std={sp['std']}°F  Range={sp['min']}-{sp['max']}°F  N={sp['n']}")
