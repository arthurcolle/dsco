#!/usr/bin/env python3
"""continual_swarm — durable continual-processing swarm over OpenAI-compatible lanes.

Engine-agnostic by construction: every lane is (base_url, model, key) resolved
from env, defaulting to the local dsco router proxy. No provider is hardcoded.

    uv run --with openai python continual_swarm.py run --workers 8
    uv run --with openai python continual_swarm.py add ensemble --prompt "..." --lanes 3
    uv run --with openai python continual_swarm.py add llm --prompt "..."
    uv run --with openai python continual_swarm.py pulse --prompt "..." --interval 900
    uv run --with openai python continual_swarm.py watch inbox.jsonl   # tail file -> tasks
    uv run --with openai python continual_swarm.py status
    uv run --with openai python continual_swarm.py result <task_id>

Task graph semantics:
    llm       one lane call (adaptive lane pick unless pinned)
    ensemble  fan out prompt across N distinct lanes -> spawns judge join-task
    map       one llm child per item -> spawns reduce join-task
    judge     joins children, scores/synthesizes via best lane
    reduce    joins children, merges via best lane
    pulse     recurring task: re-enqueues itself every `interval` seconds

Env:
    SWARM_BASE_URL   default http://127.0.0.1:3141/v1 (dsco router proxy)
    SWARM_API_KEY    key for SWARM_BASE_URL
    SWARM_LANES      JSON list of lane specs; else built from available key envs
    SWARM_DB         sqlite path, default ~/.dsco/swarm/continual.db
"""

import argparse
import asyncio
import json
import os
import random
import re
import signal
import sqlite3
import sys
import time
import uuid
from pathlib import Path
from typing import Any, Optional

import openai

DB_PATH = Path(os.getenv("SWARM_DB", str(Path.home() / ".dsco" / "swarm" / "continual.db")))
BASE_URL = os.getenv("SWARM_BASE_URL", "http://127.0.0.1:3141/v1")
API_KEY = os.getenv("SWARM_API_KEY", "")
EMA = 0.25
MAX_TOKENS_DEFAULT = 4000
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_THINK_RE = re.compile(r"<think>[\s\S]*?</think>\s*", re.IGNORECASE)


def clean_output(text: str) -> str:
    """Strip terminal styling (exec lanes) and leaked reasoning blocks."""
    return _THINK_RE.sub("", _ANSI_RE.sub("", text)).strip()

# ── Lanes ────────────────────────────────────────────────────────────────────

DSCO_BIN = Path(os.getenv("DSCO_BIN", str(Path.home() / "dsco" / "dsco-cli" / "dsco")))


def default_lanes() -> list[dict[str, Any]]:
    """HTTP lanes are BYOK through the router proxy. Exec lanes shell out to
    the dsco binary, which owns the subscription backends (ChatGPT/Claude
    OAuth, sakana, zai) — $0 marginal cost."""
    cands = [
        {"name": "openrouter/gpt-5.2",   "model": "openai/gpt-5.2",      "key_env": "OPENROUTER_API_KEY"},
        {"name": "openrouter/oss-120b",  "model": "openai/gpt-oss-120b", "key_env": "OPENROUTER_API_KEY"},
        {"name": "openrouter/kimi-k3",   "model": "moonshotai/kimi-k3",  "key_env": "OPENROUTER_API_KEY"},
        {"name": "xai/grok-4.5",         "model": "grok-4.5",            "provider": "xai", "key_env": "XAI_API_KEY"},
        {"name": "xai/grok-4.3",         "model": "grok-4.3",            "provider": "xai", "key_env": "XAI_API_KEY"},
        {"name": "cerebras/gemma-4-31b", "model": "gemma-4-31b",         "provider": "cerebras", "key_env": "CEREBRAS_API_KEY"},
        {"name": "groq/llama-3.3-70b", "model": "llama-3.3-70b-versatile",
         "provider": "groq", "key_env": "GROQ_API_KEY"},
        {"name": "groq/qwen3.6-27b",   "model": "qwen/qwen3.6-27b",
         "provider": "groq", "key_env": "GROQ_API_KEY"},
        # Provider-native model ids: slash-ids without an explicit provider are
        # routed to OpenRouter by the gateway's detect_provider().
        {"name": "deepseek/v4-flash",  "model": "deepseek-v4-flash",
         "provider": "deepseek", "key_env": "DEEPSEEK_API_KEY"},
        {"name": "deepseek/v4-pro",    "model": "deepseek-v4-pro",
         "provider": "deepseek", "key_env": "DEEPSEEK_API_KEY"},
        # Together lanes removed 2026-07-19: account has no credits (402) and
        # Qwen3.7-Plus additionally requires streaming. Re-add when funded.
        {"name": "openai/gpt-5.4-mini", "model": "gpt-5.4-mini",
         "provider": "openai", "key_env": "OPENAI_API_KEY"},  # 429s until quota tops up
    ]
    lanes = [c for c in cands if os.getenv(c["key_env"], "")]
    if DSCO_BIN.exists() and os.getenv("SWARM_SUBSCRIPTION_LANES", "1") != "0":
        lanes += [
            {"name": "claude/sonnet-5",  "exec": True, "provider": "anthropic", "model": "claude-sonnet-5"},
            {"name": "claude/fable-5",   "exec": True, "provider": "anthropic", "model": "claude-fable-5"},
            {"name": "claude/haiku-4.5", "exec": True, "provider": "anthropic", "model": "claude-haiku-4-5-20251001"},
            {"name": "zai/glm-5.2",      "exec": True, "provider": "zai",       "model": "glm-5.2"},
            {"name": "sakana/fugu",      "exec": True, "provider": "sakana",    "model": "fugu"},
        ]
        # ChatGPT one-shot backend rejects max_output_tokens as of 2026-07-16
        # (-e codex is a full agent harness, unusable for headless fan-out).
        # Re-enable when upstream recovers: SWARM_CODEX_LANES=1
        if os.getenv("SWARM_CODEX_LANES", "0") == "1":
            lanes += [
                {"name": "codex/gpt-5.6-sol",   "exec": True, "provider": "openai-codex", "model": "gpt-5.6-sol"},
                {"name": "codex/gpt-5.6-terra", "exec": True, "provider": "openai-codex", "model": "gpt-5.6-terra"},
                {"name": "codex/gpt-5.5",       "exec": True, "provider": "openai-codex", "model": "gpt-5.5"},
            ]
    return lanes

def load_lanes() -> list[dict[str, Any]]:
    raw = os.getenv("SWARM_LANES", "")
    lanes = json.loads(raw) if raw else default_lanes()
    if not lanes:
        sys.exit("no lanes: set SWARM_LANES or export provider key envs")
    return lanes

# ── Store ────────────────────────────────────────────────────────────────────

SCHEMA = """
CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY, kind TEXT NOT NULL, payload TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'pending',      -- pending|running|done|failed|dead
    priority INTEGER NOT NULL DEFAULT 5, run_after REAL NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL DEFAULT 3,
    parent_id TEXT, lane TEXT, result TEXT, error TEXT,
    created_at REAL NOT NULL, updated_at REAL NOT NULL);
CREATE INDEX IF NOT EXISTS idx_tasks_ready ON tasks (state, run_after, priority);
CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks (parent_id);
CREATE TABLE IF NOT EXISTS lane_stats (
    lane TEXT PRIMARY KEY, calls INTEGER NOT NULL DEFAULT 0, errors INTEGER NOT NULL DEFAULT 0,
    ema_latency REAL NOT NULL DEFAULT 0, ema_success REAL NOT NULL DEFAULT 1,
    last_error TEXT, updated_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS events (
    ts REAL NOT NULL, task_id TEXT, lane TEXT, event TEXT NOT NULL, detail TEXT);
"""

class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(path))
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA busy_timeout=30000")
        self.db.executescript(SCHEMA)
        self.db.commit()

    def add_task(self, kind: str, payload: dict, *, priority=5, run_after=0.0,
                 parent_id: Optional[str] = None, max_attempts=3, task_id: Optional[str] = None) -> str:
        tid = task_id or uuid.uuid4().hex[:16]
        now = time.time()
        self.db.execute(
            "INSERT OR IGNORE INTO tasks (id,kind,payload,priority,run_after,parent_id,max_attempts,created_at,updated_at)"
            " VALUES (?,?,?,?,?,?,?,?,?)",
            (tid, kind, json.dumps(payload), priority, run_after, parent_id, max_attempts, now, now))
        self.db.commit()
        return tid

    def claim(self) -> Optional[sqlite3.Row]:
        """Atomically claim the highest-priority ready task.

        One-step UPDATE..RETURNING is atomic across processes; the rollback
        guard prevents a poisoned connection if it throws mid-transaction.
        Converged by the swarm itself across engine v0003 -> v0006."""
        now = time.time()
        try:
            cur = self.db.execute(
                "UPDATE tasks SET state='running', updated_at=? WHERE id = ("
                " SELECT id FROM tasks WHERE state='pending' AND run_after<=?"
                " ORDER BY priority ASC, created_at ASC LIMIT 1) RETURNING *", (now, now))
            row = cur.fetchone()
            self.db.commit()
            return row
        except Exception:
            self.db.rollback()
            raise

    def finish(self, tid: str, result: Any, lane: Optional[str]):
        self.db.execute("UPDATE tasks SET state='done', result=?, lane=?, updated_at=? WHERE id=?",
                        (json.dumps(result), lane, time.time(), tid))
        self.db.commit()

    def reschedule(self, tid: str, delay: float, *, count_attempt=False, error: Optional[str] = None):
        self.db.execute(
            "UPDATE tasks SET state='pending', run_after=?, attempts=attempts+?, error=?, updated_at=? WHERE id=?",
            (time.time() + delay, 1 if count_attempt else 0, error, time.time(), tid))
        self.db.commit()

    def fail(self, tid: str, error: str):
        row = self.db.execute("SELECT attempts, max_attempts FROM tasks WHERE id=?", (tid,)).fetchone()
        dead = row and row["attempts"] + 1 >= row["max_attempts"]
        if dead:
            self.db.execute("UPDATE tasks SET state='dead', attempts=attempts+1, error=?, updated_at=? WHERE id=?",
                            (error, time.time(), tid))
        else:
            backoff = min(60.0, 2.0 ** (row["attempts"] + 1)) if row else 2.0
            self.db.execute(
                "UPDATE tasks SET state='pending', attempts=attempts+1, run_after=?, error=?, updated_at=? WHERE id=?",
                (time.time() + backoff, error, time.time(), tid))
        self.db.commit()
        return bool(dead)

    def recover_stale(self, lease: float = 300.0) -> int:
        """Requeue tasks orphaned in 'running' by a killed worker process."""
        now = time.time()
        cur = self.db.execute(
            "UPDATE tasks SET state='pending', run_after=?, updated_at=? WHERE state='running' AND updated_at < ?",
            (now, now, now - lease))
        self.db.commit()
        return cur.rowcount

    def children(self, parent_id: str) -> list[sqlite3.Row]:
        return self.db.execute("SELECT * FROM tasks WHERE parent_id=?", (parent_id,)).fetchall()

    def get(self, tid: str) -> Optional[sqlite3.Row]:
        return self.db.execute("SELECT * FROM tasks WHERE id=?", (tid,)).fetchone()

    def lane_update(self, lane: str, ok: bool, latency: float, error: Optional[str] = None):
        row = self.db.execute("SELECT * FROM lane_stats WHERE lane=?", (lane,)).fetchone()
        if row:
            el = EMA * latency + (1 - EMA) * row["ema_latency"] if row["ema_latency"] > 0 else latency
            es = EMA * (1.0 if ok else 0.0) + (1 - EMA) * row["ema_success"]
            self.db.execute(
                "UPDATE lane_stats SET calls=calls+1, errors=errors+?, ema_latency=?, ema_success=?, last_error=?, updated_at=? WHERE lane=?",
                (0 if ok else 1, el, es, error, time.time(), lane))
        else:
            self.db.execute(
                "INSERT INTO lane_stats (lane,calls,errors,ema_latency,ema_success,last_error,updated_at) VALUES (?,?,?,?,?,?,?)",
                (lane, 1, 0 if ok else 1, latency, 1.0 if ok else 0.0, error, time.time()))
        self.db.commit()

    def lane_scores(self) -> dict[str, float]:
        out = {}
        for r in self.db.execute("SELECT * FROM lane_stats"):
            out[r["lane"]] = r["ema_success"] / (1.0 + r["ema_latency"] / 10.0)
        return out

    def event(self, task_id: Optional[str], lane: Optional[str], event: str, detail: str = ""):
        self.db.execute("INSERT INTO events (ts,task_id,lane,event,detail) VALUES (?,?,?,?,?)",
                        (time.time(), task_id, lane, event, detail[:500]))
        self.db.commit()

# ── Swarm ────────────────────────────────────────────────────────────────────

class Swarm:
    def __init__(self, store: Store, lanes: list[dict], workers: int = 8, lane_cap: int = 4):
        self.store, self.lanes, self.workers = store, lanes, workers
        self.client = openai.AsyncOpenAI(base_url=BASE_URL, api_key=API_KEY or "unused", timeout=180)
        self.sems = {l["name"]: asyncio.Semaphore(lane_cap) for l in lanes}
        # global cap on heavy subprocess tasks (topologies spawn 3-15 agents each)
        self.exec_sem = asyncio.Semaphore(int(os.getenv("SWARM_EXEC_CAP", "4")))
        self.stop = asyncio.Event()
        self.handlers = {"llm": self.h_llm, "ensemble": self.h_ensemble, "map": self.h_map,
                         "judge": self.h_judge, "reduce": self.h_reduce, "pulse": self.h_pulse,
                         "agent": self.h_agent, "topology": self.h_topology}
        # optional free-energy instrumentation (off unless THERMO_PROBE=1)
        try:
            from thermo_probe import ThermoProbe
            self.thermo = ThermoProbe.from_env(BASE_URL, API_KEY)
        except Exception:
            self.thermo = None

    # lane selection: score-ranked with epsilon exploration; exclude named lanes
    def pick_lanes(self, n=1, exclude: tuple = (), pin: Optional[str] = None) -> list[dict]:
        if pin:
            return [l for l in self.lanes if l["name"] == pin][:1] or self.lanes[:1]
        scores = self.store.lane_scores()
        pool = [l for l in self.lanes if l["name"] not in exclude]
        if random.random() < 0.15:
            random.shuffle(pool)
        else:
            pool.sort(key=lambda l: scores.get(l["name"], 1.0), reverse=True)
        return pool[:n]

    async def _call_exec(self, lane: dict, messages: list[dict]) -> str:
        """Subscription lane: one-shot through the dsco binary (OAuth backends)."""
        prompt = "\n\n".join(m.get("content", "") for m in messages if m.get("content"))
        args = [str(DSCO_BIN), "--profile", "lite", "--provider", lane["provider"],
                "-m", lane["model"], "-p", prompt]
        proc = await asyncio.create_subprocess_exec(
            *args,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
            start_new_session=True)
        try:
            stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=240)
        except asyncio.TimeoutError:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            raise RuntimeError("exec lane timeout (240s)")
        if proc.returncode != 0:
            raise RuntimeError(f"dsco exit {proc.returncode}")
        return stdout.decode(errors="replace").strip()

    async def call_lane(self, lane: dict, messages: list[dict], **kw) -> str:
        t0 = time.time()
        async with self.sems[lane["name"]]:
            try:
                if lane.get("exec"):
                    out = await self._call_exec(lane, messages)
                else:
                    extra: dict[str, Any] = {"api_key": os.getenv(lane.get("key_env", ""), "")}
                    if lane.get("provider"):
                        extra["provider"] = lane["provider"]
                    r = await self.client.chat.completions.create(
                        model=lane["model"], messages=messages,
                        max_tokens=kw.get("max_tokens", MAX_TOKENS_DEFAULT),
                        extra_body=extra)
                    out = (r.choices[0].message.content or "").strip()
                out = clean_output(out)
                if not out:
                    raise RuntimeError("empty completion")
                self.store.lane_update(lane["name"], True, time.time() - t0)
                self._thermo_record(lane["name"], messages, True, kw.get("task_id"))
                return out
            except Exception as e:
                self.store.lane_update(lane["name"], False, time.time() - t0, str(e)[:200])
                self._thermo_record(lane["name"], messages, False, kw.get("task_id"))
                raise

    def _thermo_record(self, lane: str, messages: list[dict], ok: bool, task_id=None) -> None:
        """Fire-and-forget free-energy snapshot of this LLM turn; never blocks or raises."""
        if not getattr(self, "thermo", None):
            return
        try:
            snap = [{"role": m.get("role"), "content": m.get("content")} for m in messages]
            asyncio.create_task(asyncio.to_thread(self.thermo.measure, snap, ok, lane, task_id))
        except Exception:
            pass

    # ── handlers ─────────────────────────────────────────────────────────
    async def h_llm(self, task, payload) -> Any:
        msgs = payload.get("messages") or [{"role": "user", "content": payload["prompt"]}]
        exclude = tuple(payload.get("exclude_lanes", ()))
        last_err = None
        for lane in self.pick_lanes(n=len(self.lanes), exclude=exclude, pin=payload.get("lane")):
            try:
                out = await self.call_lane(lane, msgs, max_tokens=payload.get("max_tokens", MAX_TOKENS_DEFAULT))
                return {"lane": lane["name"], "content": out}
            except Exception as e:
                last_err = e
                self.store.event(task["id"], lane["name"], "lane_failover", str(e)[:200])
        raise RuntimeError(f"all lanes failed: {last_err}")

    async def h_ensemble(self, task, payload) -> Any:
        n = min(int(payload.get("lanes", 3)), len(self.lanes))
        picked = self.pick_lanes(n=n)
        child_ids = [self.store.add_task("llm", {"prompt": payload["prompt"], "lane": l["name"],
                                                 "max_tokens": payload.get("max_tokens", MAX_TOKENS_DEFAULT)},
                                         parent_id=task["id"], priority=task["priority"])
                     for l in picked]
        jid = self.store.add_task("judge", {"prompt": payload["prompt"], "child_ids": child_ids,
                                            "criteria": payload.get("criteria", "correctness, completeness, calibration"),
                                            "then": payload.get("then")},
                                  parent_id=task["id"], priority=task["priority"], run_after=time.time() + 2)
        return {"spawned": child_ids, "judge": jid}

    async def h_map(self, task, payload) -> Any:
        items = payload["items"]
        child_ids = [self.store.add_task("llm", {"prompt": payload["template"].replace("{item}", str(it))},
                                         parent_id=task["id"], priority=task["priority"])
                     for it in items]
        rid = self.store.add_task("reduce", {"child_ids": child_ids, "instruction": payload.get(
            "reduce", "Merge the item results into one coherent, deduplicated summary.")},
            parent_id=task["id"], priority=task["priority"], run_after=time.time() + 2)
        return {"spawned": child_ids, "reduce": rid}

    def _join(self, task, child_ids) -> Optional[list[sqlite3.Row]]:
        """Return children if all settled, else reschedule this join-task."""
        rows = [r for c in child_ids if (r := self.store.get(c)) is not None]
        if len(rows) != len(child_ids):
            raise RuntimeError("missing child task")
        if all(r["state"] in ("done", "dead") for r in rows):
            return rows
        self.store.reschedule(task["id"], 3.0)
        return None

    async def h_judge(self, task, payload) -> Any:
        rows = self._join(task, payload["child_ids"])
        if rows is None:
            return None  # rescheduled
        answers = [(r["lane"] or "?", json.loads(r["result"])["content"])
                   for r in rows if r["state"] == "done" and r["result"]]
        if not answers:
            raise RuntimeError("no ensemble children succeeded")
        bundle = "\n\n".join(f"### Candidate {i+1} (lane {l})\n{a}" for i, (l, a) in enumerate(answers))
        msgs = [{"role": "system", "content":
                 f"You are a strict judge. Criteria: {payload['criteria']}. Score each candidate 0-10 "
                 "with one-line reasons, name the winner, then produce a FINAL consolidated answer "
                 "that takes the best of all candidates. Format: SCORES / WINNER / FINAL sections."},
                {"role": "user", "content": f"Task:\n{payload['prompt']}\n\n{bundle}"}]
        lane = self.pick_lanes(1)[0]
        verdict = await self.call_lane(lane, msgs)
        # continual: verdict can seed a follow-up task
        nxt = payload.get("then")
        if nxt:
            self.store.add_task(nxt["kind"], {**nxt.get("payload", {}), "prompt": nxt.get(
                "prompt_template", "{final}").replace("{final}", verdict)},
                parent_id=task["id"], priority=task["priority"])
        return {"judge_lane": lane["name"], "n_candidates": len(answers), "verdict": verdict}

    async def h_reduce(self, task, payload) -> Any:
        rows = self._join(task, payload["child_ids"])
        if rows is None:
            return None
        parts = [json.loads(r["result"])["content"] for r in rows if r["state"] == "done" and r["result"]]
        if not parts:
            raise RuntimeError("no reduce children succeeded")
        msgs = [{"role": "system", "content": payload["instruction"]},
                {"role": "user", "content": "\n\n---\n\n".join(parts)}]
        lane = self.pick_lanes(1)[0]
        return {"reduce_lane": lane["name"], "n_parts": len(parts), "content": await self.call_lane(lane, msgs)}

    async def h_agent(self, task, payload) -> Any:
        """Full dsco executor: agentic run with tools (shell, files, …), not a
        text completion. cwd defaults to the swarm directory so 'improve X'
        acts on the real code."""
        backend = payload.get("backend", "anthropic")
        model = payload.get("model", "claude-sonnet-5")
        if backend == "codex":
            args = [str(DSCO_BIN), "-e", "codex", "-m", payload.get("model", "gpt-5.6-sol")]
        else:
            args = [str(DSCO_BIN), "--provider", backend, "-m", model]
        args += ["-p", payload["prompt"]]
        cwd = payload.get("cwd", str(Path(__file__).parent))
        proc = await asyncio.create_subprocess_exec(
            *args, cwd=cwd, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL, start_new_session=True)
        t0 = time.time()
        try:
            stdout, _ = await asyncio.wait_for(proc.communicate(),
                                               timeout=float(payload.get("timeout", 900)))
        except asyncio.TimeoutError:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            raise RuntimeError("agent timeout")
        out = clean_output(stdout.decode(errors="replace"))
        lane = f"dsco/agent·{model.split('/')[-1]}"
        self.store.lane_update(lane, proc.returncode == 0 and bool(out), time.time() - t0)
        if proc.returncode != 0 and not out:
            raise RuntimeError(f"agent exit {proc.returncode}")
        return {"lane": lane, "content": out[-8000:], "exit": proc.returncode}

    async def h_topology(self, task, payload) -> Any:
        """Run one of dsco's 60 agent topologies (chains, fan-outs, meshes,
        tribunals, tournaments, …) as a single swarm task."""
        name = str(payload.get("topology", "auto"))
        model = payload.get("model", "claude-sonnet-5")
        args = [str(DSCO_BIN), "-m", model]
        args += ["--topology-auto"] if name == "auto" else ["--topology", name]
        args += ["-p", payload["prompt"]]
        async with self.exec_sem:
            proc = await asyncio.create_subprocess_exec(
                *args, cwd=str(Path(__file__).parent),
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
                start_new_session=True)
            t0 = time.time()
            try:
                stdout, _ = await asyncio.wait_for(proc.communicate(),
                                                   timeout=float(payload.get("timeout", 1200)))
            except asyncio.TimeoutError:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
                raise RuntimeError(f"topology {name} timeout")
        out = clean_output(stdout.decode(errors="replace"))
        out = "\n".join(l for l in out.splitlines() if l.strip() != "<>").strip()
        lane = f"topo/{name}"
        self.store.lane_update(lane, proc.returncode == 0 and bool(out), time.time() - t0)
        if proc.returncode != 0 or not out:
            raise RuntimeError(f"topology {name} failed (exit {proc.returncode})")
        return {"lane": lane, "content": out[-8000:], "topology": name}

    async def h_pulse(self, task, payload) -> Any:
        """Recurring heartbeat task: run inner prompt, then re-enqueue self."""
        inner = await self.h_llm(task, payload)
        interval = float(payload.get("interval", 900))
        left = int(payload.get("remaining", -1))
        if left != 0:
            nxt = dict(payload)
            if left > 0:
                nxt["remaining"] = left - 1
            self.store.add_task("pulse", nxt, priority=task["priority"],
                                run_after=time.time() + interval, max_attempts=task["max_attempts"])
        return inner

    # ── run loop ─────────────────────────────────────────────────────────
    async def worker(self, wid: int):
        while not self.stop.is_set():
            row = self.store.claim()
            if row is None:
                await asyncio.sleep(0.5)
                continue
            payload = json.loads(row["payload"])
            handler = self.handlers.get(row["kind"])
            if handler is None:
                self.store.fail(row["id"], f"unknown kind {row['kind']}")
                continue
            try:
                result = await handler(row, payload)
                if result is not None:
                    lane = (result.get("lane") or result.get("judge_lane") or result.get("reduce_lane")) \
                        if isinstance(result, dict) else None
                    self.store.finish(row["id"], result, lane)
                    self.store.event(row["id"], lane, "done", row["kind"])
                    print(f"[w{wid}] done {row['kind']}:{row['id']}" + (f" via {lane}" if lane else ""))
                else:
                    # join not ready (handler re-queued it): yield so in-flight I/O progresses
                    await asyncio.sleep(0.05)
            except Exception as e:
                dead = self.store.fail(row["id"], str(e)[:300])
                self.store.event(row["id"], None, "dead" if dead else "retry", str(e)[:200])
                print(f"[w{wid}] {'DEAD' if dead else 'retry'} {row['kind']}:{row['id']}: {str(e)[:120]}")

    async def tail_inbox(self, path: Path):
        """Continual source: any JSON line appended to `path` becomes a task.
        Line shape: {"kind": "...", "payload": {...}, "priority": 5}"""
        path.touch(exist_ok=True)
        with open(path) as f:
            f.seek(0, 2)
            while not self.stop.is_set():
                line = f.readline()
                if not line:
                    await asyncio.sleep(1.0)
                    continue
                try:
                    spec = json.loads(line)
                    tid = self.store.add_task(spec["kind"], spec.get("payload", {}),
                                              priority=int(spec.get("priority", 5)))
                    print(f"[inbox] queued {spec['kind']}:{tid}")
                except Exception as e:
                    print(f"[inbox] bad line: {e}")

    async def janitor(self, lease: float):
        while not self.stop.is_set():
            n = self.store.recover_stale(lease)
            if n:
                print(f"[janitor] recovered {n} stale running task(s)")
            await asyncio.sleep(30)

    async def drain_monitor(self):
        """--drain mode: stop once the queue is empty for 3 consecutive checks."""
        idle = 0
        while not self.stop.is_set():
            n = self.store.db.execute(
                "SELECT COUNT(*) FROM tasks WHERE state IN ('pending','running')").fetchone()[0]
            idle = idle + 1 if n == 0 else 0
            if idle >= 3:
                print("[drain] queue empty — stopping")
                self.stop.set()
                return
            await asyncio.sleep(1.5)

    async def run(self, inbox: Optional[Path] = None, drain: bool = False):
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, self.stop.set)
            except NotImplementedError:      # e.g. Windows event loop
                signal.signal(sig, lambda *_: self.stop.set())
        lease = float(os.getenv("SWARM_LEASE_SEC", "300"))
        n = self.store.recover_stale(lease)
        if n:
            print(f"[startup] recovered {n} stale running task(s)")
        tasks = [asyncio.create_task(self.worker(i)) for i in range(self.workers)]
        tasks.append(asyncio.create_task(self.janitor(lease)))
        if drain:
            tasks.append(asyncio.create_task(self.drain_monitor()))
        if inbox:
            tasks.append(asyncio.create_task(self.tail_inbox(inbox)))
        print(f"swarm up: {self.workers} workers, {len(self.lanes)} lanes "
              f"({', '.join(l['name'] for l in self.lanes)}), db={DB_PATH}")
        await self.stop.wait()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        try:
            await self.client.close()
        finally:
            self.store.db.close()
        print("swarm stopped")

# ── CLI ──────────────────────────────────────────────────────────────────────

def cmd_status(store: Store):
    for r in store.db.execute("SELECT state, COUNT(*) n FROM tasks GROUP BY state"):
        print(f"{r['state']:8s} {r['n']}")
    print("\nlane                       calls errs  ema_lat  ema_ok")
    for r in store.db.execute("SELECT * FROM lane_stats ORDER BY ema_success DESC"):
        print(f"{r['lane']:26s} {r['calls']:5d} {r['errors']:4d}  {r['ema_latency']:6.1f}s  {r['ema_success']:.2f}")
    print("\nrecent:")
    for r in store.db.execute(
            "SELECT id,kind,state,lane,substr(error,1,60) e FROM tasks ORDER BY updated_at DESC LIMIT 10"):
        print(f"  {r['id']} {r['kind']:9s} {r['state']:7s} {r['lane'] or '-':24s} {r['e'] or ''}")

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run"); p.add_argument("--workers", type=int, default=16)
    p.add_argument("--inbox", type=str, default=None)
    p.add_argument("--drain", action="store_true", help="exit when the queue is empty")
    p = sub.add_parser("add"); p.add_argument("kind"); p.add_argument("--prompt", default=None)
    p.add_argument("--json", dest="payload_json", default=None); p.add_argument("--lanes", type=int, default=3)
    p.add_argument("--priority", type=int, default=5)
    p = sub.add_parser("pulse"); p.add_argument("--prompt", required=True)
    p.add_argument("--interval", type=float, default=900); p.add_argument("--count", type=int, default=-1)
    sub.add_parser("status")
    p = sub.add_parser("result"); p.add_argument("task_id")
    p = sub.add_parser("watch"); p.add_argument("inbox")
    p = sub.add_parser("thermo-report"); p.add_argument("--log", default=None)
    p = sub.add_parser("thermo-watch"); p.add_argument("--log", default=None)
    args = ap.parse_args()

    if args.cmd in ("thermo-report", "thermo-watch"):
        from thermo_probe import report as thermo_report, watch as thermo_watch
        log = Path(args.log) if args.log else Path(
            os.getenv("THERMO_LOG", str(Path.home() / ".dsco" / "swarm" / "thermo.jsonl")))
        return sys.exit(thermo_watch(log) if args.cmd == "thermo-watch" else thermo_report(log))

    store = Store(DB_PATH)
    if args.cmd == "status":
        return cmd_status(store)
    if args.cmd == "result":
        row = store.get(args.task_id)
        if not row:
            sys.exit("no such task")
        print(f"{row['kind']} {row['state']} lane={row['lane']}")
        if row["result"]:
            print(json.dumps(json.loads(row["result"]), indent=2))
        if row["error"]:
            print("error:", row["error"])
        for c in store.children(args.task_id):
            print(f"  child {c['id']} {c['kind']} {c['state']} lane={c['lane']}")
        return
    if args.cmd == "add":
        payload = json.loads(args.payload_json) if args.payload_json else {}
        if args.prompt:
            payload["prompt"] = args.prompt
        if args.kind == "ensemble":
            payload.setdefault("lanes", args.lanes)
        tid = store.add_task(args.kind, payload, priority=args.priority)
        print(tid)
        return
    if args.cmd == "pulse":
        tid = store.add_task("pulse", {"prompt": args.prompt, "interval": args.interval,
                                       "remaining": args.count}, priority=3)
        print(tid)
        return
    swarm = Swarm(store, load_lanes(), workers=getattr(args, "workers", 8))
    inbox = Path(args.inbox) if getattr(args, "inbox", None) else None
    if args.cmd == "watch":
        inbox = Path(args.inbox)
    asyncio.run(swarm.run(inbox=inbox, drain=getattr(args, "drain", False)))

if __name__ == "__main__":
    main()
