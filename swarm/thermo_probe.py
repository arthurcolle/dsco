#!/usr/bin/env python3
"""Free-energy instrumentation for the continual swarm.

Every LLM turn funnels through Swarm.call_lane with a `messages` list and a binary
success/failure outcome. This module snapshots that context and logs the thermodynamic
state functionals (U, S, T, F, N) per call, so the §7 falsification test can run on live
swarm traffic: is lane success monotone in free energy F rather than token count N?

Reuses the thermo_context package (~/Dsco/thermo-context). Off unless THERMO_PROBE=1.
Structural mode (default): logs N and redundancy R with no extra model calls. Full mode
(THERMO_SCORER_MODEL set): also estimates U via probe answerability through a cheap lane,
giving the complete F = U - T*S. Measurement is done off the event loop and never raises
into the swarm.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence

# --- bootstrap the thermo_context package -----------------------------------------
try:
    import thermo_context  # noqa: F401
except Exception:  # pragma: no cover - path bootstrap
    _TC = os.path.expanduser("~/Dsco/thermo-context")
    if _TC not in sys.path:
        sys.path.insert(0, _TC)

from thermo_context.atoms import Atom
from thermo_context.backends import HashingEmbedder, MockNLI
from thermo_context.functionals import (
    Probe,
    contradiction_mass,
    estimate_temperature,
    redundancy,
)

_SENT = re.compile(r"(?<=[.!?])\s+")


def _split(text: str) -> List[str]:
    return [p.strip() for p in _SENT.split((text or "").strip()) if p.strip()]


def oai_messages_to_atoms(messages: Sequence[dict]) -> List[Atom]:
    """Segment an OpenAI/Anthropic-style message list into sentence-level atoms."""
    atoms: List[Atom] = []
    for i, m in enumerate(messages):
        role = m.get("role", "user") if isinstance(m, dict) else "user"
        content = m.get("content") if isinstance(m, dict) else str(m)
        if isinstance(content, list):  # content-parts (multimodal / tool blocks)
            text = " ".join(
                (p.get("text") or p.get("content") or "") if isinstance(p, dict) else str(p)
                for p in content
            )
        else:
            text = content or ""
        for j, s in enumerate(_split(text)):
            atoms.append(Atom(id=f"m{i}s{j}", text=s, role=str(role)))
    return atoms


# --- probe scoring (full mode) ----------------------------------------------------
class OpenAIProbeScorer:
    """Scores probe answerability from context through a cheap OpenAI-compatible lane."""

    def __init__(self, client, model: str):
        self.client = client
        self.model = model

    def answer_prob(self, question: str, context: Sequence[Atom]) -> float:
        ctx = "\n".join(f"- {a.text}" for a in context) or "(empty)"
        try:
            r = self.client.chat.completions.create(
                model=self.model,
                max_tokens=64,
                messages=[
                    {
                        "role": "system",
                        "content": "Answer strictly from the context. If the context does not "
                        "contain the answer, reply exactly UNKNOWN. Reply with only the answer.",
                    },
                    {"role": "user", "content": f"Context:\n{ctx}\n\nQuestion: {question}"},
                ],
            )
            ans = (r.choices[0].message.content or "").strip().lower()
        except Exception:
            return 0.0
        return 0.0 if (not ans or ans.startswith("unknown")) else 1.0


def make_probe_provider(client, model: str, k: int = 4) -> Callable[[Sequence[dict]], List[Probe]]:
    """Build a cached provider that derives k checkpoint probes from a call's task text."""
    cache: Dict[str, List[Probe]] = {}

    def task_text(messages: Sequence[dict]) -> str:
        users = [m for m in messages if isinstance(m, dict) and m.get("role") in ("user", "system")]
        src = users[0] if users else (messages[0] if messages else {})
        c = src.get("content") if isinstance(src, dict) else str(src)
        return c if isinstance(c, str) else json.dumps(c)[:2000]

    def provider(messages: Sequence[dict]) -> List[Probe]:
        txt = task_text(messages)
        key = hashlib.md5(txt.encode()).hexdigest()
        if key in cache:
            return cache[key]
        try:
            r = client.chat.completions.create(
                model=model,
                max_tokens=256,
                messages=[
                    {
                        "role": "system",
                        "content": f"List exactly {k} short factual checkpoint questions a "
                        "competent agent must be able to answer to complete the task. "
                        "One per line, no numbering.",
                    },
                    {"role": "user", "content": txt[:2000]},
                ],
            )
            lines = [l.strip("-*0123456789. \t") for l in (r.choices[0].message.content or "").splitlines()]
            probes = [Probe(question=q) for q in lines if q][:k]
        except Exception:
            probes = []
        if not probes:
            probes = [Probe(question=txt[:200])]
        cache[key] = probes
        return probes

    return provider


# --- the instrument ---------------------------------------------------------------
@dataclass
class ThermoProbe:
    embedder: Any
    log_path: Path
    scorer: Optional[Any] = None  # OpenAIProbeScorer or thermo_context Scorer; None => structural
    nli: Optional[Any] = None
    probe_provider: Optional[Callable[[Sequence[dict]], List[Probe]]] = None
    w_r: float = 1.0
    w_x: float = 1.0
    enable_x: bool = False
    fixed_T: Optional[float] = 0.5  # independent calls => constant T; None enables online T
    _base: Dict[str, float] = field(default_factory=dict)
    _dU: Dict[str, list] = field(default_factory=dict)
    _dS: Dict[str, list] = field(default_factory=dict)
    _prev: Dict[str, tuple] = field(default_factory=dict)

    def measure(self, messages: Sequence[dict], ok: bool, lane: str = "?", task_id: Optional[str] = None) -> dict:
        atoms = oai_messages_to_atoms(messages)
        N = sum(a.tokens for a in atoms)
        R = redundancy(atoms, self.embedder)
        X = contradiction_mass(atoms, self.nli) if (self.enable_x and self.nli) else 0.0
        S = self.w_r * R + self.w_x * X

        U = None
        if self.scorer is not None and self.probe_provider is not None and atoms:
            probes = self.probe_provider(messages)
            key = "|".join(p.question for p in probes)
            if key not in self._base:
                self._base[key] = float(
                    sum(self.scorer.answer_prob(p.question, []) for p in probes) / max(1, len(probes))
                )
            full = float(
                sum(self.scorer.answer_prob(p.question, atoms) for p in probes) / max(1, len(probes))
            )
            U = full - self._base[key]

        # Temperature. Across independent calls (the swarm choke point) T is a constant
        # hyperparameter; online dU/dS estimation only makes sense within one accumulating
        # loop (set fixed_T=None for that).
        if self.fixed_T is not None:
            T = self.fixed_T
        else:
            T = 0.5
            if U is not None:
                pv = self._prev.get(lane)
                if pv is not None:
                    self._dU.setdefault(lane, []).append(U - pv[0])
                    self._dS.setdefault(lane, []).append(S - pv[1])
                    self._dU[lane] = self._dU[lane][-25:]
                    self._dS[lane] = self._dS[lane][-25:]
                self._prev[lane] = (U, S)
                T = estimate_temperature(self._dU.get(lane, []), self._dS.get(lane, []))

        F = (U if U is not None else 0.0) - T * S
        row = {
            "ts": time.time(),
            "lane": lane,
            "task_id": task_id,
            "ok": int(bool(ok)),
            "N": N,
            "R": round(R, 6),
            "X": round(X, 6),
            "S": round(S, 6),
            "T": round(T, 6),
            "U": None if U is None else round(U, 6),
            "F": round(F, 6),
            "mode": "full" if U is not None else "structural",
        }
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.log_path, "a") as fh:
            fh.write(json.dumps(row) + "\n")
        if os.getenv("THERMO_STREAM"):
            u = "  U=--- " if U is None else f"U={U:+.3f}"
            mark = "\033[32m✓\033[0m" if ok else "\033[31m✗\033[0m"
            print(
                f"\033[2m[thermo]\033[0m {mark} {lane:<22.22} "
                f"{u} S={S:6.3f} T={T:4.2f} F={F:+.3f} N={N:<5d}",
                flush=True,
            )
        return row

    # ----------------------------------------------------------------- construction
    @classmethod
    def from_env(cls, base_url: str, api_key: str, log_path: Optional[Path] = None) -> Optional["ThermoProbe"]:
        if os.getenv("THERMO_PROBE", "") not in ("1", "true", "yes", "on"):
            return None
        log_path = log_path or Path(os.getenv("THERMO_LOG", str(Path.home() / ".dsco" / "swarm" / "thermo.jsonl")))
        scorer_model = os.getenv("THERMO_SCORER_MODEL", "").strip()
        scorer = provider = None
        if scorer_model:
            import openai  # lazy; only needed in full mode

            client = openai.OpenAI(base_url=base_url, api_key=api_key or "unused", timeout=60)
            scorer = OpenAIProbeScorer(client, scorer_model)
            provider = make_probe_provider(client, scorer_model)
        return cls(
            embedder=HashingEmbedder(),
            log_path=log_path,
            scorer=scorer,
            nli=MockNLI() if os.getenv("THERMO_NLI") else None,
            probe_provider=provider,
            enable_x=bool(os.getenv("THERMO_NLI")),
            fixed_T=float(os.getenv("THERMO_T", "0.5")),
        )


# --- §7 falsification report ------------------------------------------------------
def report(log_path: Path) -> int:
    import numpy as np

    from thermo_context.stats import logistic_fit, standardize

    rows = []
    if not log_path.exists():
        print(f"no thermo log at {log_path}")
        return 1
    with open(log_path) as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    rows = [r for r in rows if "ok" in r]
    if len(rows) < 20:
        print(f"need >=20 logged calls, have {len(rows)} (run the swarm with THERMO_PROBE=1)")
        return 1

    y = np.array([r["ok"] for r in rows], float)
    N = np.array([r["N"] for r in rows], float)
    full = all(r.get("U") is not None for r in rows)
    print("=" * 66)
    print(f"  Swarm free-energy report  ({len(rows)} calls, {'full' if full else 'structural'} mode)")
    print(f"  success rate = {y.mean():.2f}")
    print("=" * 66)

    def show(fit, names):
        for nm, b, se, z, p in zip(names, fit["beta"], fit["se"], fit["z"], fit["p"]):
            star = "***" if p < 0.001 else "**" if p < 0.01 else "*" if p < 0.05 else ""
            print(f"    {nm:<10} beta={b:+.3f}  se={se:.3f}  z={z:+6.2f}  p={p:.3g} {star}")
        print(f"    pseudo-R^2 = {fit['pseudo_r2']:.3f}")

    inter = np.ones(len(y))
    if full:
        F = np.array([r["F"] for r in rows], float)
        zF, zN = standardize(F), standardize(N)
        print("\n  logistic  ok ~ 1 + z(F) + z(N):")
        both = logistic_fit(np.column_stack([inter, zF, zN]), y)
        show(both, ["intercept", "F", "N"])
        print("\n  logistic  ok ~ 1 + z(F):")
        show(logistic_fit(np.column_stack([inter, zF]), y), ["intercept", "F"])
        print("\n  logistic  ok ~ 1 + z(N):")
        show(logistic_fit(np.column_stack([inter, zN]), y), ["intercept", "N"])
        bF, bN, pF, pN = both["beta"][1], both["beta"][2], both["p"][1], both["p"][2]
        ok = (bF > 0 and pF < 0.05) and (abs(bF) > 2 * abs(bN) or pN > 0.05)
        print("\n  VERDICT:", "F predicts success; N collapses given F." if ok else "inconclusive on this data.")
    else:
        R = np.array([r["R"] for r in rows], float)
        zN, zR = standardize(N), standardize(R)
        print("\n  structural mode (no U): logistic ok ~ 1 + z(N) + z(R):")
        show(logistic_fit(np.column_stack([inter, zN, zR]), y), ["intercept", "N", "R"])
        print("\n  (set THERMO_SCORER_MODEL to a cheap lane for full U/F free-energy analysis)")
    print("=" * 66)
    return 0


# --- realtime watch ---------------------------------------------------------------
_SPARK = "▁▂▃▄▅▆▇█"


def _spark(vals: Sequence[float], width: int = 40) -> str:
    vals = list(vals)[-width:]
    if not vals:
        return ""
    lo, hi = min(vals), max(vals)
    rng = hi - lo or 1.0
    return "".join(_SPARK[min(7, int((v - lo) / rng * 7))] for v in vals)


def render_panel(rows: Sequence[dict], width: int = 48) -> str:
    """Render a live free-energy panel from the most recent rows."""
    import numpy as np

    rows = list(rows)
    if not rows:
        return "  (waiting for swarm LLM calls… run with THERMO_PROBE=1)"
    full = all(r.get("U") is not None for r in rows)
    F = [r["F"] for r in rows]
    S = [r["S"] for r in rows]
    N = [float(r["N"]) for r in rows]
    ok = [r["ok"] for r in rows]
    last = rows[-1]
    win = ok[-30:]
    rate = sum(win) / len(win)
    L = []
    L.append("\033[1m  ⚡ swarm free-energy — realtime\033[0m   " f"{len(rows)} calls · {last['mode']} mode")
    L.append("  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈")
    if full:
        L.append(f"  F  {_spark(F, width)}  {last['F']:+.3f}")
    L.append(f"  S  {_spark(S, width)}  {last['S']:.3f}")
    L.append(f"  N  {_spark(N, width)}  {int(last['N'])}")
    bar = "█" * int(rate * 20) + "░" * (20 - int(rate * 20))
    L.append(f"  ok {bar} {rate*100:4.0f}%   last: {'✓' if last['ok'] else '✗'} {last['lane'][:24]}")
    # live regression once enough data
    if len(rows) >= 20:
        from thermo_context.stats import logistic_fit, standardize

        y = np.array(ok, float)
        inter = np.ones(len(y))
        if full and np.std(F) > 0 and np.std(N) > 0:
            fit = logistic_fit(np.column_stack([inter, standardize(F), standardize(N)]), y)
            bF, bN, pF = fit["beta"][1], fit["beta"][2], fit["p"][1]
            flag = "\033[32mF≫N\033[0m" if (bF > 0 and pF < 0.05 and abs(bF) > 2 * abs(bN)) else "…"
            L.append(f"  live  βF={bF:+.2f}(p={pF:.2g})  βN={bN:+.2f}   {flag}")
        elif np.std(N) > 0:
            fit = logistic_fit(np.column_stack([inter, standardize(N)]), y)
            L.append(f"  live  βN={fit['beta'][1]:+.2f}(p={fit['p'][1]:.2g})  [structural]")
    return "\n".join(L)


def follow(path: Path, from_start: bool = True):
    """Generator yielding parsed rows as they are appended to the log (tail -f)."""
    while not path.exists():
        time.sleep(0.2)
    with open(path) as fh:
        if not from_start:
            fh.seek(0, 2)
        buf = ""
        while True:
            line = fh.readline()
            if not line:
                time.sleep(0.15)
                continue
            buf += line
            if buf.endswith("\n"):
                try:
                    yield json.loads(buf.strip())
                except Exception:
                    pass
                buf = ""


def watch(path: Path, keep: int = 200) -> int:
    """Live panel: follow the log and redraw the free-energy dashboard in place."""
    from collections import deque

    rows: deque = deque(maxlen=keep)
    # seed with existing rows
    if path.exists():
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line:
                    try:
                        rows.append(json.loads(line))
                    except Exception:
                        pass
    sys.stdout.write("\033[2J")

    def draw():
        sys.stdout.write("\033[H")  # cursor home
        sys.stdout.write(render_panel(rows) + "\033[J\n")
        sys.stdout.flush()

    draw()
    try:
        for row in follow(path, from_start=False):
            rows.append(row)
            draw()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    default_log = Path(os.getenv("THERMO_LOG", str(Path.home() / ".dsco" / "swarm" / "thermo.jsonl")))
    if args and args[0] == "watch":
        sys.exit(watch(Path(args[1]) if len(args) > 1 else default_log))
    sys.exit(report(Path(args[0]) if args else default_log))
