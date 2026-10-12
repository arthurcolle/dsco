#!/usr/bin/env python3
"""Benchmark real local DSCO modules, including SIGKILL/WAL recovery.

No inference, external messaging, provider substitutes, or saved configuration
changes. All databases and processes belong to this bounded benchmark.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import sqlite3
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def summary(values):
    ordered = sorted(values)
    return {"count": len(values), "p50": statistics.median(values),
            "p95": ordered[int((len(ordered) - 1) * .95)]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if list(out.glob("*.sqlite")):
        parser.error("use a fresh output directory to preserve previous benchmark evidence")
    binary = out / "native_runtime_bench"
    sources = [ROOT / "bench/native_runtime_bench.c"] + [ROOT / f"src/{name}.c"
               for name in ["ipc", "json_util", "env_config", "event_loop", "scheduler"]]
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-O2", "-std=c11", "-D_DARWIN_C_SOURCE", "-D_POSIX_C_SOURCE=200809L",
        "-I", str(ROOT / "include"), *map(str, sources), "-lsqlite3", "-lm", "-o", str(binary)]
    subprocess.run(command, check=True, timeout=60)
    records = []

    def run(mode, db, count=1000):
        argv = [str(binary), mode, str(db), str(count)]
        start = time.perf_counter()
        result = subprocess.run(argv, capture_output=True, text=True, timeout=30, check=True)
        record = {"mode": mode, "argv": argv, "process_ms": (time.perf_counter() - start) * 1000,
                  "result": json.loads(result.stdout), "stderr": result.stderr}
        records.append(record)
        (out / "records.json").write_text(json.dumps(records, indent=2) + "\n")
        return record

    # Cold = a new native process and IPC connection for every item, reusing DB.
    cold_db = out / "cold.sqlite"
    run("hot", cold_db, 1)  # initialize schema before measuring process restarts
    cold = [run("hot", cold_db, 1) for _ in range(25)]
    hot = [run("hot", out / f"hot-{i}.sqlite", 1000) for i in range(5)]
    queues = run("two-queue", out / "two-queue.sqlite", 1000)
    scheduler = [run("scheduler", "unused") for _ in range(5)]

    recovery_db = out / "recovery.sqlite"
    setup = run("setup", recovery_db)
    holder = subprocess.Popen([str(binary), "hold", str(recovery_db)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        import selectors
        with selectors.DefaultSelector() as selector:
            selector.register(holder.stdout, selectors.EVENT_READ)
            assert selector.select(timeout=10), "worker failed to claim within deadline"
        line = holder.stdout.readline()
        assert line.strip(), holder.stderr.read()
        claimed = json.loads(line)
        started = time.perf_counter()
        os.kill(holder.pid, signal.SIGSTOP)
        stopped_pid, stopped_status = os.waitpid(holder.pid, os.WUNTRACED)
        assert stopped_pid == holder.pid and os.WIFSTOPPED(stopped_status)
        pause_ms = (time.perf_counter() - started) * 1000
        steering = run("steer", recovery_db)
        os.kill(holder.pid, signal.SIGKILL)
        assert holder.wait(timeout=5) == -signal.SIGKILL
        restarted = run("recover", recovery_db)
        assert restarted["result"]["completed_task"] == setup["result"]["task_id"] == claimed["claimed_task"]
    finally:
        if holder.poll() is None:
            holder.kill()
            holder.wait(timeout=5)
    with sqlite3.connect(recovery_db) as db:
        task = db.execute("SELECT id,status,result,target_agent_id FROM tasks").fetchall()
        checkpoints = db.execute("SELECT COUNT(*) FROM agent_checkpoint").fetchone()[0]
        messages = db.execute("SELECT COUNT(*),COUNT(read_at) FROM messages").fetchone()
        assert len(task) == 1 and task[0][1] == "done" and checkpoints == 1 and messages == (1, 1)

    result = {
        "schema": "dsco.native_runtime_bench.v1",
        "scope": "Native C control-plane operations on local SQLite and cooperative scheduler; no inference or competitor comparison",
        "compiler_argv": command,
        "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        "cold_process_task_plus_message_ms": summary([r["process_ms"] for r in cold]),
        "cold_ipc_init_ms": summary([r["result"]["init_ms"] for r in cold]),
        "hot_task_lifecycle_p50_ms": summary([r["result"]["task_lifecycle"]["p50_ms"] for r in hot]),
        "hot_task_lifecycles_per_second": summary([r["result"]["task_lifecycle"]["ops_per_second"] for r in hot]),
        "hot_message_roundtrip_p50_ms": summary([r["result"]["message_send_receive_ack"]["p50_ms"] for r in hot]),
        "two_targeted_queues": queues["result"],
        "scheduler_noop_dispatches_per_second": summary([r["result"]["dispatches_per_second"] for r in scheduler]),
        "deferred_callbacks_per_second": summary([r["result"]["callbacks_per_second"] for r in scheduler]),
        "cancelled_task_executions": [r["result"]["cancelled_task_executions"] for r in scheduler],
        "process_recovery": {"sigstop_ms": pause_ms, "claimed": claimed,
            "steering_written_while_stopped": steering["result"], "sigkill_confirmed": True,
            "restarted": restarted, "final_tasks": task, "checkpoints": checkpoints, "messages_total_and_acked": messages},
        "limits": ["No model/inference speed claim", "Cold process timing includes IPC open/schema checks, one task lifecycle, one message roundtrip, cleanup and OS process overhead",
            "Two targeted queues are composed by this harness, not proof of an integrated durable planning scheduler",
            "Task lease is explicitly expired through ipc_task_requeue_stale(0) only after confirmed worker death; no claim of automatic immediate recovery",
            "SQLite WAL uses synchronous=NORMAL: process-crash recovery tested, power-loss durability not tested",
            "Scheduler callbacks are cooperative and process-local; SIGSTOP/SIGKILL exercise OS process interruption",
            "Queued cancellation tested; running callback preemption and completion-attempt fencing are not provided by these measurements",
            "Deferred callbacks are native event-loop callbacks, not HTTP webhook deliveries"],
        "records": records,
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ["records", "compiler_argv", "source_sha256"]}, indent=2))


if __name__ == "__main__":
    main()
