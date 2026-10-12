#!/usr/bin/env python3
"""Offline integration check for the swarm free-energy wiring.

Drives the REAL Swarm.call_lane with a fake OpenAI client (no network) and a mock-backed
ThermoProbe, over synthetic contexts whose composition decouples free energy F from token
count N. Success is drawn from sigmoid(F_true) with no N dependence. Verifies:
  1. the wired call_lane -> _thermo_record -> measure path writes rows,
  2. the §7 report finds F dominant and N collapsing,
  3. the realtime panel renders.
"""
import asyncio
import os
import random
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.expanduser("~/Dsco/thermo-context"))
sys.path.insert(0, str(Path(__file__).parent))

os.environ.setdefault("SWARM_BASE_URL", "http://127.0.0.1:9/v1")  # never dialed
import continual_swarm as cs  # noqa: E402
import thermo_probe as tp  # noqa: E402
from thermo_context.backends import HashingEmbedder, MockScorer  # noqa: E402
from thermo_context.functionals import Probe  # noqa: E402


class _Msg:
    def __init__(self, content):
        self.message = type("M", (), {"content": content})


class _Resp:
    def __init__(self, content):
        self.choices = [_Msg(content)]


class FakeCompletions:
    """Returns non-empty content on 'success', empty (-> call_lane failure) otherwise."""

    def __init__(self, rng):
        self.rng = rng
        self.next_ok = True

    async def create(self, **kw):
        await asyncio.sleep(0)
        return _Resp("ANSWER: ok" if self.next_ok else "")


class FakeClient:
    def __init__(self, rng):
        self.chat = type("C", (), {"completions": FakeCompletions(rng)})()

    async def close(self):
        pass


def build_messages(rng, K=6):
    facts = rng.sample(range(K), rng.randint(1, K))
    vals = {k: rng.randint(10, 99) for k in facts}
    parts = [f"Measured quantity {k}. The value of quantity {k} is {vals[k]}. <<FACT:{k}:{vals[k]}>>" for k in facts]
    n_dup = rng.randint(0, 5)
    for _ in range(n_dup):
        k = rng.choice(facts)
        parts.append(f"Measured quantity {k}. The value of quantity {k} is {vals[k]}. <<FACT:{k}:{vals[k]}>>")
    n_filler = rng.randint(0, 20)  # decouples N from F
    parts += ["Vendor migration prose and on-call rotation notes, unrelated background." for _ in range(n_filler)]
    rng.shuffle(parts)
    U_true = len(facts) / K
    S_true = 0.12 * n_dup
    F_true = U_true - 0.5 * S_true
    msgs = [{"role": "user", "content": p} for p in parts]
    return msgs, F_true


async def main():
    rng = random.Random(20260806)
    tmp = Path(tempfile.mkdtemp()) / "thermo.jsonl"

    store = cs.Store(Path(tempfile.mkdtemp()) / "s.db")
    lanes = [{"name": "fake/haiku", "model": "m", "key_env": ""}]
    swarm = cs.Swarm(store, lanes, workers=1)
    swarm.client = FakeClient(rng)
    swarm.thermo = tp.ThermoProbe(
        embedder=HashingEmbedder(),
        log_path=tmp,
        scorer=MockScorer(),
        probe_provider=lambda m: [Probe(question=f"<<Q:{k}>> value of {k}?") for k in range(6)],
        fixed_T=0.5,
    )

    a, b, n = -1.3, 6.0, 220
    for _ in range(n):
        msgs, F_true = build_messages(rng)
        p = 1.0 / (1.0 + np.exp(-(a + b * F_true)))
        swarm.client.chat.completions.next_ok = rng.random() < p
        try:
            await swarm.call_lane(lanes[0], msgs)
        except Exception:
            pass

    # drain the fire-and-forget measurement tasks
    for _ in range(200):
        pending = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
        if not pending:
            break
        await asyncio.gather(*pending, return_exceptions=True)
    for _ in range(50):
        if tmp.exists() and sum(1 for _ in open(tmp)) >= n:
            break
        await asyncio.sleep(0.05)

    n_rows = sum(1 for _ in open(tmp)) if tmp.exists() else 0
    print(f"logged {n_rows}/{n} thermo rows -> {tmp}\n")

    print("--- realtime panel (render_once) ---")
    rows = [__import__("json").loads(l) for l in open(tmp)]
    print(tp.render_panel(rows))
    print("\n--- §7 report ---")
    tp.report(tmp)

    y = np.array([r["ok"] for r in rows], float)
    F = np.array([r["F"] for r in rows], float)
    N = np.array([r["N"] for r in rows], float)
    from thermo_context.stats import logistic_fit, standardize

    both = logistic_fit(np.column_stack([np.ones(len(y)), standardize(F), standardize(N)]), y)
    n_only = logistic_fit(np.column_stack([np.ones(len(y)), standardize(N)]), y)
    bF, bN, pF = both["beta"][1], both["beta"][2], both["p"][1]
    ok = (n_rows >= n) and (bF > 0 and pF < 0.05) and (abs(bF) > 2 * abs(bN)) and (n_only["pseudo_r2"] < 0.05)
    print("\nSELFTEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
