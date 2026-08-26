#!/usr/bin/env python3
"""Swarm console — FastAPI + SPA over the continual_swarm SQLite state.

    uv run --with fastapi --with 'uvicorn[standard]' python dashboard.py --port 8422
    SWARM_DB=~/.dsco/swarm/continual.db  (same env the swarm uses)
"""

import argparse
import json
import os
import sqlite3
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Optional

import re
import subprocess

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse

DB_PATH = Path(os.getenv("SWARM_DB", str(Path.home() / ".dsco" / "swarm" / "continual.db")))
RSI_HOME = Path(os.getenv("SWARM_RSI_HOME", str(Path.home() / ".dsco" / "swarm" / "rsi")))
STATIC = Path(__file__).parent / "static"

app = FastAPI(title="swarm-console", docs_url=None, redoc_url=None)


_lock = threading.Lock()
_db_conn: Optional[sqlite3.Connection] = None


def q(sql: str, args: tuple = ()) -> list[dict[str, Any]]:
    global _db_conn
    with _lock:
        if _db_conn is None:
            _db_conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=5,
                                       check_same_thread=False)
            _db_conn.row_factory = sqlite3.Row
        return [dict(r) for r in _db_conn.execute(sql, args).fetchall()]


@app.get("/")
def index():
    return FileResponse(STATIC / "index.html")


@app.get("/api/overview")
def overview():
    now = time.time()
    states = {r["state"]: r["n"] for r in q("SELECT state, COUNT(*) n FROM tasks GROUP BY state")}
    lanes = q("SELECT * FROM lane_stats ORDER BY lane")

    # done-per-minute, last 60 minutes
    t0 = now - 3600
    buckets = {int(r["b"]): r["n"] for r in q(
        "SELECT CAST((ts - ?) / 60 AS INTEGER) b, COUNT(*) n FROM events"
        " WHERE event='done' AND ts >= ? GROUP BY b", (t0, t0))}
    throughput = [{"t": t0 + i * 60, "n": buckets.get(i, 0)} for i in range(61)]

    # completion dots for the swimlane board, last 30 minutes
    dots = q("SELECT id, kind, lane, updated_at t, state FROM tasks"
             " WHERE updated_at >= ? AND state IN ('done','dead') AND lane IS NOT NULL"
             " ORDER BY updated_at", (now - 1800,))

    recent = q("SELECT id, kind, state, lane, parent_id, attempts,"
               " substr(COALESCE(error,''),1,110) error, created_at, updated_at"
               " FROM tasks ORDER BY updated_at DESC LIMIT 40")
    events = q("SELECT ts, task_id, lane, event, substr(COALESCE(detail,''),1,90) detail"
               " FROM events ORDER BY ts DESC LIMIT 12")
    kinds = q("SELECT kind, COUNT(*) n FROM tasks GROUP BY kind ORDER BY n DESC")
    return {"now": now, "db": str(DB_PATH), "states": states, "lanes": lanes,
            "throughput": throughput, "dots": dots, "recent": recent,
            "events": events, "kinds": kinds}


@app.get("/api/task/{tid}")
def task(tid: str):
    rows = q("SELECT * FROM tasks WHERE id=?", (tid,))
    if not rows:
        return JSONResponse({"error": "not found"}, status_code=404)
    t = rows[0]
    for k in ("payload", "result"):
        if t.get(k):
            try:
                t[k] = json.loads(t[k])
            except Exception:
                pass
    t["children"] = q("SELECT id, kind, state, lane, attempts, substr(COALESCE(error,''),1,80) error"
                      " FROM tasks WHERE parent_id=? ORDER BY created_at", (tid,))
    parent: Optional[str] = t.get("parent_id")
    t["ancestry"] = []
    if parent:
        t["ancestry"] = q(
            "WITH RECURSIVE anc(id, kind, state, parent_id, depth) AS ("
            " SELECT id, kind, state, parent_id, 0 FROM tasks WHERE id=?"
            " UNION ALL"
            " SELECT p.id, p.kind, p.state, p.parent_id, a.depth+1"
            " FROM tasks p JOIN anc a ON p.id = a.parent_id WHERE a.depth < 32"
            ") SELECT id, kind, state FROM anc ORDER BY depth", (parent,))
    return t


DSCO_BIN = Path(os.getenv("DSCO_BIN", str(Path.home() / "dsco" / "dsco-cli" / "dsco")))
_TOPOLOGIES: list[dict[str, Any]] = []


def load_topologies() -> list[dict[str, Any]]:
    global _TOPOLOGIES
    if _TOPOLOGIES or not DSCO_BIN.exists():
        return _TOPOLOGIES
    try:
        r = subprocess.run([str(DSCO_BIN), "--topology-list"], capture_output=True,
                           text=True, timeout=20)
        for line in r.stdout.splitlines():
            m = re.match(r"\s*(T\d+)\s+(\S+)\s+(\S+)\s+agents=(\d+)\s+latency=([\d.]+)x", line)
            if m:
                _TOPOLOGIES.append({"id": m.group(1), "name": m.group(2), "category": m.group(3),
                                    "agents": int(m.group(4)), "latency": float(m.group(5))})
    except Exception:
        pass
    return _TOPOLOGIES


@app.get("/api/topologies")
def topologies():
    topos = load_topologies()
    usage = {r["lane"][5:]: {"calls": r["calls"], "errors": r["errors"]}
             for r in q("SELECT lane, calls, errors FROM lane_stats WHERE lane LIKE 'topo/%'")}
    return {"topologies": [{**t, **usage.get(t["name"], {"calls": 0, "errors": 0})} for t in topos]}


@app.post("/api/chat")
async def chat_submit(request: Request):
    body = await request.json()
    prompt = str(body.get("prompt", "")).strip()
    if not prompt:
        return JSONResponse({"error": "empty prompt"}, status_code=400)
    mode = str(body.get("mode", "")) or ("council" if body.get("council") else "llm")
    kind = {"council": "ensemble", "agent": "agent", "topology": "topology"}.get(mode, "llm")
    payload: dict[str, Any] = {"prompt": prompt, "chat": True}
    if kind == "ensemble":
        payload["lanes"] = int(body.get("lanes", 4))
    if kind == "agent":
        payload["timeout"] = float(body.get("timeout", 900))
    if kind == "topology":
        payload["topology"] = str(body.get("topology", "auto"))
    tid = uuid.uuid4().hex[:16]
    now = time.time()
    db = sqlite3.connect(str(DB_PATH), timeout=10)
    try:
        db.execute("PRAGMA busy_timeout=10000")
        db.execute(
            "INSERT INTO tasks (id,kind,payload,priority,run_after,max_attempts,created_at,updated_at)"
            " VALUES (?,?,?,?,?,?,?,?)",
            (tid, kind, json.dumps(payload), 2, 0, 2 if kind == "agent" else 3, now, now))
        db.commit()
    finally:
        db.close()
    return {"id": tid, "kind": kind}


@app.get("/api/chat/feed")
def chat_feed():
    rows = q("SELECT id, kind, state, lane, parent_id, payload, result, created_at, updated_at"
             " FROM tasks WHERE kind IN ('llm','ensemble','judge','reduce','pulse','agent','topology')"
             " ORDER BY created_at DESC LIMIT 36")
    feed = []
    for r in rows:
        try:
            payload = json.loads(r["payload"] or "{}")
        except Exception:
            payload = {}
        prompt = str(payload.get("prompt", ""))
        out, judged_by = "", None
        if r.get("result"):
            try:
                res = json.loads(r["result"])
                out = str(res.get("verdict") or res.get("content") or "")
                judged_by = res.get("judge_lane") or res.get("reduce_lane")
            except Exception:
                pass
        feed.append({
            "id": r["id"], "kind": r["kind"], "state": r["state"], "lane": r["lane"],
            "parent_id": r["parent_id"], "chat": bool(payload.get("chat")),
            "prompt": (prompt[:220] + "…") if len(prompt) > 220 else prompt,
            "big_prompt": len(prompt) > 2000,
            "out": (out[:1200] + "…") if len(out) > 1200 else out,
            "judged_by": judged_by, "t": r["updated_at"],
        })
    feed.reverse()
    return {"feed": feed}


@app.get("/api/rsi")
def rsi_state():
    rsi_db = RSI_HOME / "rsi.db"
    if not rsi_db.exists():
        return {"enabled": False}
    db = sqlite3.connect(f"file:{rsi_db}?mode=ro", uri=True, timeout=5)
    db.row_factory = sqlite3.Row
    try:
        versions = [dict(r) for r in db.execute("SELECT * FROM versions ORDER BY v")]
        gates = [dict(r) for r in db.execute("SELECT * FROM gates ORDER BY id DESC LIMIT 16")]
        swaps = [dict(r) for r in db.execute("SELECT * FROM swaps ORDER BY ts DESC LIMIT 8")]
        cycles = [dict(r) for r in db.execute("SELECT * FROM cycles ORDER BY started DESC LIMIT 6")]
    finally:
        db.close()
    runtime: dict[str, Any] = {}
    try:
        runtime = json.loads((RSI_HOME / "runtime.json").read_text())
    except Exception:
        pass
    alive = False
    if runtime.get("primary_pid") and not runtime.get("stopped"):
        try:
            os.kill(int(runtime["primary_pid"]), 0)
            alive = True
        except Exception:
            alive = False
    return {"enabled": True, "versions": versions, "gates": gates, "swaps": swaps,
            "cycles": cycles, "runtime": runtime, "primary_alive": alive}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8422)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    print(f"swarm console → http://{args.host}:{args.port}  (db={DB_PATH})")
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")


if __name__ == "__main__":
    main()
