#!/usr/bin/env python3
"""Quick DSCO scenario modeling engine.

Local-first, stdlib-only statistical scenario workflow:
- JSON scenario spec
- monthly GTM/revenue simulation helper
- Monte Carlo
- 2D parameter sweep
- sensitivity ranking
- ASCII visuals
- Markdown + CSV/JSON artifacts
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import statistics
from dataclasses import dataclass
from typing import Any, Dict, List, Tuple


@dataclass
class Dist:
    kind: str
    args: Dict[str, float]

    def sample(self, rng: random.Random) -> float:
        k = self.kind.lower()
        a = self.args
        if k in ("const", "constant"):
            return float(a.get("value", a.get("v", 0.0)))
        if k == "uniform":
            return rng.uniform(float(a["lo"]), float(a["hi"]))
        if k == "normal":
            return rng.gauss(float(a.get("mu", a.get("mean", 0.0))), float(a.get("sigma", a.get("sd", 1.0))))
        if k == "lognormal":
            return rng.lognormvariate(float(a.get("mu", 0.0)), float(a.get("sigma", 1.0)))
        if k == "beta":
            return rng.betavariate(float(a.get("alpha", 1.0)), float(a.get("beta", 1.0)))
        if k == "triangular":
            return rng.triangular(float(a["lo"]), float(a["hi"]), float(a.get("mode", (a["lo"] + a["hi"]) / 2)))
        if k == "poisson":
            # Knuth; fine for small lambdas used in quick business scenarios.
            lam = float(a.get("lambda", a.get("lam", 1.0)))
            L, p, n = math.exp(-lam), 1.0, 0
            while p > L:
                n += 1
                p *= rng.random()
            return float(n - 1)
        raise ValueError(f"unsupported distribution: {self.kind}")


def parse_dist(x: Any) -> Dist:
    if isinstance(x, (int, float)):
        return Dist("constant", {"value": float(x)})
    if isinstance(x, dict):
        k = x.get("dist", x.get("kind", "constant"))
        args = {kk: vv for kk, vv in x.items() if kk not in ("dist", "kind", "unit", "evidence", "confidence", "desc")}
        return Dist(str(k), {kk: float(vv) for kk, vv in args.items()})
    raise TypeError(f"bad distribution spec: {x!r}")


def quantile(xs: List[float], q: float) -> float:
    if not xs:
        return float("nan")
    ys = sorted(xs)
    pos = (len(ys) - 1) * q
    lo = int(math.floor(pos)); hi = int(math.ceil(pos))
    if lo == hi:
        return ys[lo]
    return ys[lo] * (hi - pos) + ys[hi] * (pos - lo)


def pearson(a: List[float], b: List[float]) -> float:
    if len(a) < 3 or len(a) != len(b):
        return 0.0
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    da = [x - ma for x in a]
    db = [y - mb for y in b]
    va = sum(x * x for x in da)
    vb = sum(y * y for y in db)
    if va <= 0 or vb <= 0:
        return 0.0
    return sum(x * y for x, y in zip(da, db)) / math.sqrt(va * vb)


def rankdata(xs: List[float]) -> List[float]:
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    ranks = [0.0] * len(xs)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        r = (i + j) / 2 + 1
        for k in range(i, j + 1):
            ranks[order[k]] = r
        i = j + 1
    return ranks


def spearman(a: List[float], b: List[float]) -> float:
    return pearson(rankdata(a), rankdata(b))


def model_gtm(sample: Dict[str, float], spec: Dict[str, Any]) -> Dict[str, float]:
    months = int(spec.get("months", 12))
    customers = float(spec.get("initial_customers", 0))
    traffic = float(sample.get("traffic_monthly", 0))
    traffic_growth = float(sample.get("traffic_growth_monthly", 0))
    activation = float(sample.get("activation_rate", 0))
    conversion = float(sample.get("paid_conversion_rate", sample.get("conversion_rate", 0)))
    arpa = float(sample.get("arpa", 0))
    churn = float(sample.get("churn_monthly", 0))
    expansion = float(sample.get("expansion_monthly", 0))
    enterprise_deals = float(sample.get("enterprise_deals", 0))
    enterprise_acv = float(sample.get("enterprise_acv", 0))

    mrr = 0.0
    for _m in range(1, months + 1):
        new_paid = traffic * activation * conversion
        customers = customers * max(0.0, 1.0 - churn) + new_paid
        arpa = arpa * (1.0 + expansion)
        mrr = customers * arpa
        traffic = traffic * (1.0 + traffic_growth)

    self_serve_arr = 12.0 * mrr
    enterprise_arr = enterprise_deals * enterprise_acv
    arr = self_serve_arr + enterprise_arr
    return {
        "arr_12m": arr,
        "self_serve_arr": self_serve_arr,
        "enterprise_arr": enterprise_arr,
        "customers_12m": customers,
        "mrr_12m": mrr,
    }


def simulate(spec: Dict[str, Any], n: int, seed: int, overrides: Dict[str, float] | None = None) -> Tuple[List[Dict[str, float]], List[Dict[str, float]]]:
    rng = random.Random(seed)
    params = spec["parameters"]
    dists = {name: parse_dist(cfg) for name, cfg in params.items()}
    samples: List[Dict[str, float]] = []
    outputs: List[Dict[str, float]] = []
    overrides = overrides or {}
    for _ in range(n):
        s = {name: dist.sample(rng) for name, dist in dists.items()}
        s.update(overrides)
        # Clamp common probability params.
        for k in list(s):
            if k.endswith("_rate") or k in ("activation_rate", "conversion_rate", "paid_conversion_rate", "churn_monthly", "expansion_monthly"):
                s[k] = min(1.0, max(0.0, s[k]))
        out = model_gtm(s, spec) if spec.get("model", "gtm_arr") == "gtm_arr" else model_gtm(s, spec)
        samples.append(s); outputs.append(out)
    return samples, outputs


def summarize(values: List[float], target: float) -> Dict[str, float]:
    return {
        "n": float(len(values)),
        "mean": statistics.fmean(values),
        "median": quantile(values, 0.5),
        "p05": quantile(values, 0.05),
        "p25": quantile(values, 0.25),
        "p75": quantile(values, 0.75),
        "p95": quantile(values, 0.95),
        "min": min(values),
        "max": max(values),
        "p_success": sum(1 for v in values if v >= target) / len(values),
        "target": target,
    }


def hist(values: List[float], width: int = 56, bins: int = 18) -> str:
    lo, hi = min(values), max(values)
    if hi <= lo:
        return "all values identical"
    counts = [0] * bins
    for v in values:
        i = min(bins - 1, int((v - lo) / (hi - lo) * bins))
        counts[i] += 1
    mx = max(counts) or 1
    lines = []
    for i, c in enumerate(counts):
        a = lo + (hi - lo) * i / bins
        b = lo + (hi - lo) * (i + 1) / bins
        bar = "█" * max(1, int(c / mx * width)) if c else ""
        lines.append(f"${a/1000:7.0f}k-${b/1000:7.0f}k | {bar} {c}")
    return "\n".join(lines)


def heatmap(matrix: List[List[float]], xs: List[float], ys: List[float], title: str) -> str:
    chars = " .:-=+*#%@"
    vals = [v for row in matrix for v in row]
    lo, hi = min(vals), max(vals)
    lines = [title, f"y rows low→high, x cols low→high; range {lo:.2%}..{hi:.2%}"]
    for row in reversed(matrix):
        line = ""
        for v in row:
            t = 0 if hi == lo else (v - lo) / (hi - lo)
            line += chars[min(len(chars) - 1, max(0, int(t * (len(chars) - 1))))]
        lines.append(line)
    lines.append(f"x: {xs[0]:.4g} → {xs[-1]:.4g}; y: {ys[0]:.4g} → {ys[-1]:.4g}")
    return "\n".join(lines)


def sweep2d(spec: Dict[str, Any], xname: str, yname: str, nx: int, ny: int, n: int, seed: int) -> Tuple[List[float], List[float], List[List[float]]]:
    def bounds(name: str) -> Tuple[float, float]:
        cfg = spec["parameters"][name]
        if "sweep" in cfg:
            return float(cfg["sweep"][0]), float(cfg["sweep"][1])
        d = parse_dist(cfg)
        a = d.args
        if d.kind == "beta":
            m = a.get("alpha", 1) / (a.get("alpha", 1) + a.get("beta", 1))
            return max(0.0, m * 0.25), min(1.0, m * 2.5)
        if "lo" in a and "hi" in a:
            return float(a["lo"]), float(a["hi"])
        v = a.get("value", a.get("mu", 1.0))
        return float(v) * 0.5, float(v) * 1.5
    xlo, xhi = bounds(xname); ylo, yhi = bounds(yname)
    xs = [xlo + (xhi - xlo) * i / max(1, nx - 1) for i in range(nx)]
    ys = [ylo + (yhi - ylo) * j / max(1, ny - 1) for j in range(ny)]
    target = float(spec.get("target", 300000))
    mat: List[List[float]] = []
    for y in ys:
        row = []
        for x in xs:
            _s, outs = simulate(spec, n=n, seed=seed + len(row) + 1000 * len(mat), overrides={xname: x, yname: y})
            vals = [o["arr_12m"] for o in outs]
            row.append(sum(1 for v in vals if v >= target) / len(vals))
        mat.append(row)
    return xs, ys, mat


def markdown_report(spec: Dict[str, Any], summary: Dict[str, float], sens: List[Tuple[str, float, float]], hist_text: str, heat_text: str | None) -> str:
    lines = []
    lines.append(f"# Scenario Report: {spec.get('name', 'unnamed')}\n")
    lines.append(f"Question: **{spec.get('question', '')}**\n")
    lines.append("## Outcome Summary\n")
    for k in ["target", "p_success", "mean", "median", "p05", "p25", "p75", "p95", "min", "max"]:
        v = summary[k]
        if k == "p_success":
            lines.append(f"- `{k}`: **{v:.2%}**")
        elif k == "target":
            lines.append(f"- `{k}`: ${v:,.0f}")
        else:
            lines.append(f"- `{k}`: ${v:,.0f}")
    lines.append("\n## Sensitivity Ranking\n")
    lines.append("| rank | parameter | spearman | pearson |")
    lines.append("|---:|---|---:|---:|")
    for i, (name, sp, pe) in enumerate(sens, 1):
        lines.append(f"| {i} | `{name}` | {sp:+.3f} | {pe:+.3f} |")
    lines.append("\n## ARR Distribution\n")
    lines.append("```text")
    lines.append(hist_text)
    lines.append("```")
    if heat_text:
        lines.append("\n## Parameter-Space Heatmap\n")
        lines.append("```text")
        lines.append(heat_text)
        lines.append("```")
    lines.append("\n## First-Principles Prompts\n")
    lines.append("- What primitive variable dominates the target probability?")
    lines.append("- Which high-sensitivity variable is least measured?")
    lines.append("- Where is the break-even frontier? What operational lever moves it?")
    lines.append("- What data would reduce uncertainty fastest?")
    lines.append("- What decision changes if this model is true?")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="DSCO quick scenario modeling engine")
    ap.add_argument("spec", help="scenario JSON file")
    ap.add_argument("--n", type=int, default=5000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--outdir", default="reports/scenario_modeling/latest")
    ap.add_argument("--sweep", nargs=2, metavar=("X", "Y"))
    ap.add_argument("--grid", nargs=2, type=int, default=[36, 18])
    args = ap.parse_args()

    with open(args.spec, "r", encoding="utf-8") as f:
        spec = json.load(f)
    os.makedirs(args.outdir, exist_ok=True)

    samples, outputs = simulate(spec, args.n, args.seed)
    vals = [o["arr_12m"] for o in outputs]
    target = float(spec.get("target", 300000))
    summ = summarize(vals, target)

    sens = []
    for name in spec["parameters"]:
        xs = [s[name] for s in samples]
        sens.append((name, spearman(xs, vals), pearson(xs, vals)))
    sens.sort(key=lambda t: abs(t[1]), reverse=True)

    heat_text = None
    if args.sweep:
        xs, ys, mat = sweep2d(spec, args.sweep[0], args.sweep[1], args.grid[0], args.grid[1], max(200, args.n // 20), args.seed)
        heat_text = heatmap(mat, xs, ys, f"P(success) heatmap: y={args.sweep[1]}, x={args.sweep[0]}")
        with open(os.path.join(args.outdir, "sweep.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f); w.writerow([args.sweep[1] + "\\" + args.sweep[0], *xs])
            for y, row in zip(ys, mat):
                w.writerow([y, *row])

    hist_text = hist(vals)
    report = markdown_report(spec, summ, sens, hist_text, heat_text)

    with open(os.path.join(args.outdir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"summary": summ, "sensitivity": sens[:20]}, f, indent=2)
    with open(os.path.join(args.outdir, "samples.csv"), "w", newline="", encoding="utf-8") as f:
        fieldnames = list(samples[0].keys()) + list(outputs[0].keys())
        w = csv.DictWriter(f, fieldnames=fieldnames); w.writeheader()
        for s, o in zip(samples, outputs):
            row = dict(s); row.update(o); w.writerow(row)
    with open(os.path.join(args.outdir, "report.md"), "w", encoding="utf-8") as f:
        f.write(report)
    print(report)
    print(f"Artifacts: {args.outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
