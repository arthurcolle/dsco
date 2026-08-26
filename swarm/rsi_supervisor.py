#!/usr/bin/env python3
"""RSI supervisor — the OUTER loop over the continual swarm's INNER loop.

Topology:
    OUTER (this process)                       INNER (engine subprocess)
    observe -> propose -> gate -> hot-swap     queue -> workers -> lanes -> results

The engine (continual_swarm.py) is deployed as immutable VERSIONS under
~/.dsco/swarm/rsi/versions/vNNNN/. One version runs as PRIMARY (initial
server) against the live queue; the previously-good version is retained as
BACKUP. Self-improvement is a *gated experiment*:

    1. PROPOSE  — enqueue an ensemble task so the swarm reviews its own source;
                  a judge lane consolidates candidate patches (find/replace).
    2. BUILD    — apply verbatim-matching patches to a new candidate version.
    3. GATE     — candidate runs in an ISOLATED sandbox (own DB, --drain):
                    G1 syntax   py_compile
                    G2 smoke    golden tasks incl. ensemble->judge join, 0 dead
                    G3 durable  seeded orphaned 'running' task must be recovered
                    G4 perf     smoke wall-time within budget
    4. SWAP     — all gates green: primary demoted to backup, candidate started
                  as new primary (durable queue + lease janitor make this a
                  zero-loss hot swap). Probation window; crash => auto-rollback.

    uv run --with openai python rsi_supervisor.py --cycles 1 --workers 8
Env: SWARM_DB (live queue), SWARM_API_KEY, SWARM_RSI_HOME, lanes as usual.
"""

import argparse
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import Any, Optional

sys.path.insert(0, str(Path(__file__).parent))
from continual_swarm import Store, DB_PATH as LIVE_DB  # noqa: E402

RSI_HOME = Path(os.getenv("SWARM_RSI_HOME", str(Path.home() / ".dsco" / "swarm" / "rsi")))
ENGINE_SRC = Path(__file__).parent / "continual_swarm.py"
RSI_DB = RSI_HOME / "rsi.db"
RUNTIME = RSI_HOME / "runtime.json"
UV = shutil.which("uv") or "uv"

SCHEMA = """
CREATE TABLE IF NOT EXISTS versions (
    v INTEGER PRIMARY KEY, path TEXT NOT NULL, status TEXT NOT NULL, note TEXT,
    created_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS gates (
    id INTEGER PRIMARY KEY AUTOINCREMENT, v INTEGER NOT NULL, gate TEXT NOT NULL,
    ok INTEGER NOT NULL, detail TEXT, ts REAL NOT NULL);
CREATE TABLE IF NOT EXISTS swaps (
    ts REAL NOT NULL, old_v INTEGER, new_v INTEGER, reason TEXT);
CREATE TABLE IF NOT EXISTS cycles (
    id TEXT PRIMARY KEY, started REAL, finished REAL, stage TEXT, outcome TEXT, detail TEXT);
"""

IMPROVE_PROMPT = """You are reviewing production Python from a continual-processing LLM swarm
(you ARE this swarm — this is your own engine). Find the TWO highest-impact
defects or improvements (correctness, async blocking, durability, efficiency — not style).
Reply ONLY with JSON:
{{"improvements":[{{"title":"...","rationale":"...","find":"<EXACT substring>","replace":"<replacement>"}}]}}
"find" must be copied verbatim from the file (unique; multi-line ok). Keep patches minimal
and behavior-preserving for existing CLI/API.

FILE: continual_swarm.py
```python
{code}
```"""


class Rsi:
    def __init__(self):
        RSI_HOME.mkdir(parents=True, exist_ok=True)
        (RSI_HOME / "versions").mkdir(exist_ok=True)
        self.db = sqlite3.connect(str(RSI_DB))
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.commit()
        self.primary_proc: Optional[subprocess.Popen] = None

    # ── version bookkeeping ─────────────────────────────────────────────
    def _next_v(self) -> int:
        r = self.db.execute("SELECT COALESCE(MAX(v),0)+1 n FROM versions").fetchone()
        return r["n"]

    def new_version(self, src: Path, status: str, note: str) -> int:
        v = self._next_v()
        vdir = RSI_HOME / "versions" / f"v{v:04d}"
        vdir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, vdir / "continual_swarm.py")
        self.db.execute("INSERT INTO versions (v,path,status,note,created_at) VALUES (?,?,?,?,?)",
                        (v, str(vdir), status, note, time.time()))
        self.db.commit()
        return v

    def vpath(self, v: int) -> Path:
        r = self.db.execute("SELECT path FROM versions WHERE v=?", (v,)).fetchone()
        return Path(r["path"]) / "continual_swarm.py"

    def set_status(self, v: int, status: str, note: Optional[str] = None):
        self.db.execute("UPDATE versions SET status=?, note=COALESCE(?,note) WHERE v=?",
                        (status, note, v))
        self.db.commit()

    def by_status(self, status: str) -> Optional[int]:
        r = self.db.execute("SELECT v FROM versions WHERE status=? ORDER BY v DESC", (status,)).fetchone()
        return r["v"] if r else None

    def gate(self, v: int, gate: str, ok: bool, detail: str):
        self.db.execute("INSERT INTO gates (v,gate,ok,detail,ts) VALUES (?,?,?,?,?)",
                        (v, gate, int(ok), detail[:400], time.time()))
        self.db.commit()
        print(f"  [{gate}] {'PASS' if ok else 'FAIL'} — {detail[:120]}")

    def stage(self, cid: str, stage: str, outcome: str = "", detail: str = ""):
        self.db.execute("UPDATE cycles SET stage=?, outcome=?, detail=?, finished=? WHERE id=?",
                        (stage, outcome, detail[:400], time.time(), cid))
        self.db.commit()
        print(f"[outer] stage={stage} {outcome} {detail[:100]}")

    def write_runtime(self, **extra):
        pv, bv = self.by_status("primary"), self.by_status("backup")
        data = {"primary_v": pv, "backup_v": bv,
                "primary_pid": self.primary_proc.pid if self.primary_proc else None,
                "primary_started": getattr(self, "_primary_started", None),
                "updated": time.time(), **extra}
        RUNTIME.write_text(json.dumps(data))

    # ── engine process control ──────────────────────────────────────────
    def spawn(self, engine: Path, db: Path, workers: int, drain: bool,
              extra_env: Optional[dict] = None) -> subprocess.Popen:
        env = {**os.environ, "SWARM_DB": str(db)}
        env.update(extra_env or {})
        cmd = [UV, "run", "--with", "openai", "python", str(engine), "run",
               "--workers", str(workers)] + (["--drain"] if drain else [])
        return subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, start_new_session=True)

    def start_primary(self, v: int, workers: int):
        self.primary_proc = self.spawn(self.vpath(v), LIVE_DB, workers, drain=False)
        self._primary_started = time.time()
        print(f"[outer] primary v{v} up (pid {self.primary_proc.pid})")
        self.write_runtime()

    def stop_primary(self):
        p = self.primary_proc
        if p and p.poll() is None:
            os.killpg(os.getpgid(p.pid), signal.SIGTERM)
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        self.primary_proc = None

    # ── PROPOSE via the inner loop ──────────────────────────────────────
    def propose(self, current_engine: Path, timeout: float = 420,
                focus: str = "") -> list[dict]:
        store = Store(LIVE_DB)
        code = current_engine.read_text()
        prompt = IMPROVE_PROMPT.format(code=code)
        if focus:
            prompt = (f"FOCUS: address ONLY this specific, evidence-backed defect — do not "
                      f"propose unrelated changes:\n{focus}\n\n" + prompt)
        eid = store.add_task("ensemble", {
            "prompt": prompt, "lanes": 3, "max_tokens": 6000,
            "criteria": "patch correctness; find-string verbatim; impact over style; no CLI/API regressions",
        }, priority=1)
        print(f"[outer] proposed self-review ensemble {eid} on inner loop")
        t0 = time.time()
        while time.time() - t0 < timeout:
            j = store.db.execute(
                "SELECT id,state,result FROM tasks WHERE parent_id=? AND kind='judge'", (eid,)).fetchone()
            if j and j["state"] == "done":
                verdict = json.loads(j["result"]).get("verdict", "")
                return self.parse_patches(verdict)
            if j and j["state"] == "dead":
                return []
            time.sleep(4)
        print("[outer] propose timed out")
        return []

    @staticmethod
    def parse_patches(verdict: str) -> list[dict]:
        for fence in re.findall(r"```(?:json)?\s*([\s\S]*?)```", verdict):
            try:
                d = json.loads(fence)
                if isinstance(d, dict) and d.get("improvements"):
                    return d["improvements"]
            except Exception:
                pass
        m = re.findall(r"\{[\s\S]*\"improvements\"[\s\S]*\}", verdict)
        for cand in m:
            # trim to last closing brace that parses
            for end in range(len(cand), 0, -1):
                if cand[end - 1] != "}":
                    continue
                try:
                    d = json.loads(cand[:end])
                    return d.get("improvements", [])
                except Exception:
                    continue
        return []

    # ── BUILD candidate ─────────────────────────────────────────────────
    @staticmethod
    def _compiles(src: str) -> bool:
        try:
            compile(src, "<candidate>", "exec")
            return True
        except SyntaxError:
            return False

    def build_candidate(self, base: Path, patches: list[dict]) -> tuple[Optional[int], list[str]]:
        src = base.read_text()
        applied = []
        for p in patches:
            f, r = p.get("find", ""), p.get("replace", "")
            if not (f and src.count(f) == 1):
                continue
            trial = src.replace(f, r)
            if not self._compiles(trial):          # drop individually-broken patches
                print(f"  [build] dropped non-compiling patch: {p.get('title','?')[:60]}")
                continue
            src = trial
            applied.append(p.get("title", "untitled"))
        if not applied:
            return None, []
        v = self._next_v()
        vdir = RSI_HOME / "versions" / f"v{v:04d}"
        vdir.mkdir(parents=True)
        (vdir / "continual_swarm.py").write_text(src)
        self.db.execute("INSERT INTO versions (v,path,status,note,created_at) VALUES (?,?,?,?,?)",
                        (v, str(vdir), "candidate", "; ".join(applied)[:200], time.time()))
        self.db.commit()
        return v, applied

    # ── GATES (isolated sandbox) ────────────────────────────────────────
    def run_gates(self, v: int) -> bool:
        engine = self.vpath(v)

        # G1 syntax
        r = subprocess.run([sys.executable, "-m", "py_compile", str(engine)],
                           capture_output=True, text=True)
        self.gate(v, "G1-syntax", r.returncode == 0, r.stderr.strip() or "compiles clean")
        if r.returncode != 0:
            return False

        # G2 smoke — golden tasks in a fresh sandbox DB, incl. join semantics
        sandbox = RSI_HOME / f"exp_v{v}.db"
        for suf in ("", "-wal", "-shm"):
            Path(str(sandbox) + suf).unlink(missing_ok=True)
        os.environ["SWARM_DB_SANDBOX"] = str(sandbox)  # marker only
        s = sqlite3.connect(str(sandbox))
        s.close()
        seed = subprocess.run(
            [UV, "run", "--with", "openai", "python", str(engine), "add", "ensemble",
             "--prompt", "Compute 12*11. Number only.", "--lanes", "2"],
            env={**os.environ, "SWARM_DB": str(sandbox)}, capture_output=True, text=True)
        for prompt in ("Say GOLD-1.", "Say GOLD-2."):
            subprocess.run([UV, "run", "--with", "openai", "python", str(engine), "add", "llm",
                            "--prompt", prompt],
                           env={**os.environ, "SWARM_DB": str(sandbox)}, capture_output=True, text=True)
        t0 = time.time()
        proc = self.spawn(engine, sandbox, workers=4, drain=True)
        try:
            proc.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            self.gate(v, "G2-smoke", False, "sandbox run did not drain in 240s")
            return False
        wall = time.time() - t0
        db = sqlite3.connect(str(sandbox))
        done = db.execute("SELECT COUNT(*) FROM tasks WHERE state='done'").fetchone()[0]
        dead = db.execute("SELECT COUNT(*) FROM tasks WHERE state='dead'").fetchone()[0]
        judged = db.execute("SELECT COUNT(*) FROM tasks WHERE kind='judge' AND state='done'").fetchone()[0]
        ok = done >= 5 and dead == 0 and judged >= 1
        self.gate(v, "G2-smoke", ok, f"done={done} dead={dead} judge={judged} wall={wall:.0f}s"
                  + (f" seed_err={seed.stderr[:80]}" if seed.returncode else ""))
        if not ok:
            return False

        # G3 durability — seeded orphan 'running' task must be recovered & finished
        now = time.time()
        tid = uuid.uuid4().hex[:16]
        db.execute("INSERT INTO tasks (id,kind,payload,state,priority,run_after,attempts,max_attempts,"
                   "created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
                   (tid, "llm", json.dumps({"prompt": "Say RECOVERED."}), "running",
                    5, 0, 0, 3, now - 9999, now - 9999))
        db.commit()
        proc = self.spawn(engine, sandbox, workers=2, drain=True,
                          extra_env={"SWARM_LEASE_SEC": "60"})
        try:
            proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        state = db.execute("SELECT state FROM tasks WHERE id=?", (tid,)).fetchone()[0]
        self.gate(v, "G3-durability", state == "done", f"orphaned task -> {state}")
        db.close()
        if state != "done":
            return False

        # G4 perf — smoke wall time within budget
        ok = wall <= 180
        self.gate(v, "G4-perf", ok, f"smoke wall {wall:.0f}s (budget 180s)")
        return ok

    # ── HOT SWAP ────────────────────────────────────────────────────────
    def hot_swap(self, new_v: int, workers: int, probation: float = 25) -> bool:
        old_v = self.by_status("primary")
        print(f"[outer] hot swap: v{old_v} -> v{new_v}")
        prev_backup = self.by_status("backup")
        if prev_backup is not None:
            self.set_status(prev_backup, "retired")
        self.stop_primary()                       # durable queue: nothing lost
        if old_v is not None:
            self.set_status(old_v, "backup")
        self.set_status(new_v, "primary")
        self.start_primary(new_v, workers)
        self.db.execute("INSERT INTO swaps (ts,old_v,new_v,reason) VALUES (?,?,?,?)",
                        (time.time(), old_v, new_v, "gates green"))
        self.db.commit()
        time.sleep(probation)
        if self.primary_proc and self.primary_proc.poll() is None:
            print(f"[outer] v{new_v} survived probation — swap committed")
            self.write_runtime()
            return True
        # rollback
        print(f"[outer] v{new_v} DIED in probation — rolling back to v{old_v}")
        self.set_status(new_v, "failed")
        if old_v is not None:
            self.set_status(old_v, "primary")
            self.start_primary(old_v, workers)
        self.db.execute("INSERT INTO swaps (ts,old_v,new_v,reason) VALUES (?,?,?,?)",
                        (time.time(), new_v, old_v, "probation rollback"))
        self.db.commit()
        return False

    # ── one full outer-loop cycle ───────────────────────────────────────
    def cycle(self, workers: int, focus: str = "") -> str:
        cid = uuid.uuid4().hex[:10]
        self.db.execute("INSERT INTO cycles (id,started,stage) VALUES (?,?,?)",
                        (cid, time.time(), "observe"))
        self.db.commit()
        pv = self.by_status("primary")
        if pv is None:
            pv = self.new_version(ENGINE_SRC, "primary", "initial deployment from repo")
        if self.primary_proc is None or self.primary_proc.poll() is not None:
            self.start_primary(pv, workers)

        self.stage(cid, "propose")
        patches = self.propose(self.vpath(pv), focus=focus)
        if not patches:
            self.stage(cid, "done", "no-op", "no parseable patches from judge")
            return "no-op"

        self.stage(cid, "build", detail=f"{len(patches)} patch(es) proposed")
        cv, applied = self.build_candidate(self.vpath(pv), patches)
        if cv is None:
            self.stage(cid, "done", "no-op", "no patch matched verbatim")
            return "no-op"

        self.stage(cid, "gate", detail=f"candidate v{cv}: {'; '.join(applied)[:120]}")
        if not self.run_gates(cv):
            self.set_status(cv, "gated_fail")
            self.stage(cid, "done", "gated_fail", f"candidate v{cv} rejected by gates")
            self.write_runtime()
            return "gated_fail"
        self.set_status(cv, "gated_pass")

        self.stage(cid, "swap")
        ok = self.hot_swap(cv, workers)
        self.stage(cid, "done", "swapped" if ok else "rolled_back", f"v{pv} -> v{cv}")
        return "swapped" if ok else "rolled_back"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=1)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--interval", type=float, default=30, help="pause between cycles")
    ap.add_argument("--hold", type=float, default=0,
                    help="keep primary serving N seconds after the last cycle")
    ap.add_argument("--focus", type=str, default="",
                    help="direct cycles at one named, evidence-backed defect")
    args = ap.parse_args()

    r = Rsi()
    print(f"RSI supervisor · live_db={LIVE_DB} · home={RSI_HOME}")
    try:
        for i in range(args.cycles):
            print(f"\n══ OUTER CYCLE {i+1}/{args.cycles} ══")
            outcome = r.cycle(args.workers, focus=args.focus)
            print(f"══ cycle outcome: {outcome} ══")
            if i + 1 < args.cycles:
                time.sleep(args.interval)
        if args.hold:
            print(f"[outer] holding primary for {args.hold:.0f}s")
            time.sleep(args.hold)
    finally:
        r.stop_primary()
        r.write_runtime(stopped=True)
        print("[outer] supervisor exit — primary stopped, state persisted")


if __name__ == "__main__":
    main()
