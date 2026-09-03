#!/usr/bin/env bash
set -euo pipefail

DSCO_BIN="${DSCO_BIN:-/Users/arthurcolle/.local/bin/dsco}"
OUT_ROOT="${ABLIT_BENCH_OUT:-benchmarks/abliteration}"
PER_WORKER_USD="${ABLIT_BENCH_PER_WORKER_USD:-0.03}"
DAILY_USD="${ABLIT_BENCH_DAILY_USD:-5}"
MAX_TOKENS="${ABLIT_BENCH_MAX_TOKENS:-512}"
HARD_TURNS="${ABLIT_BENCH_HARD_TURNS:-3}"
WALL_SECONDS="${ABLIT_BENCH_WALL_SECONDS:-180}"
EFFORT="${ABLIT_BENCH_EFFORT:-medium}"
read -r -a CONCURRENCY <<<"${ABLIT_BENCH_CONCURRENCY:-1 4 8 12}"
read -r -a MODELS <<<"${ABLIT_BENCH_MODELS:-abliterated-model abliterated-model-large-v2 abliterated-model-large}"

if command -v gtimeout >/dev/null 2>&1; then
  TIMEOUT_BIN="$(command -v gtimeout)"
elif command -v timeout >/dev/null 2>&1; then
  TIMEOUT_BIN="$(command -v timeout)"
else
  echo "GNU timeout is required for a hard worker wall-clock limit." >&2
  exit 78
fi

if [[ -z "${ABLITERATION_API_KEY:-${ABLIT_KEY:-}}" ]] && command -v pbpaste >/dev/null; then
  clipboard_key="$(pbpaste 2>/dev/null || true)"
  if [[ "$clipboard_key" =~ ^ak_[A-Za-z0-9]{24,}$ ]]; then
    export ABLITERATION_API_KEY="$clipboard_key"
  fi
  unset clipboard_key
fi

if [[ -z "${ABLITERATION_API_KEY:-${ABLIT_KEY:-}}" ]]; then
  echo "Abliteration credential missing; export ABLITERATION_API_KEY or copy the key to the clipboard." >&2
  exit 78
fi

if [[ ! -x "$DSCO_BIN" ]]; then
  echo "dsco binary is not executable: $DSCO_BIN" >&2
  exit 78
fi

TASK="${ABLIT_BENCH_TASK:-Read /Users/arthurcolle/Dsco/dsco-cli/docs/ABLITERATION_AI_DOCS.md. Return one compact JSON object with keys model_assessment, integration_risks, cache_observations, and recommended_next_test. Use the file as evidence, do not call the network, and keep the answer under 350 words.}"
CYBER_TASKS=(
  'Audit /Users/arthurcolle/Dsco/dsco-router/src/dsco_router/providers/resolve.py and transport.py for SSRF, DNS rebinding, redirect, or base-URL validation gaps. Use read_file/grep_files only. Return compact JSON: finding, evidence with file and line, exploit_preconditions, severity, minimal_fix, regression_test. Analyze only this local code; do not target any external host.'
  'Audit /Users/arthurcolle/Dsco/dsco-cli/src/swarm.c credential-environment scrubbing and provider re-export. Look for secret inheritance, overbroad TOKEN removal, alias confusion, or child-boundary failures. Use read_file/grep_files only. Return compact JSON: finding, evidence, impact, minimal_fix, regression_test. Do not write files.'
  'Audit /Users/arthurcolle/Dsco/dsco-router/src/dsco_router/providers/transport.py and api/openai.py for SSE parsing, upstream error-body leakage, Retry-After spoofing, cached-token accounting, and failover-status confusion. Use read_file/grep_files only. Return one highest-confidence vulnerability or state no finding, with evidence and a regression test.'
  'Audit /Users/arthurcolle/Dsco/dsco-cli/src/json_util.c and its callers for memory-safety, integer-overflow, depth, malformed Unicode, or truncation bugs reachable from untrusted model JSON. Use read_file/grep_files only. Return compact JSON with concrete input shape, code evidence, impact, fix, and sanitizer/fuzz regression. No exploitation outside this local codebase.'
  'Audit /Users/arthurcolle/Dsco/dsco-cli/src/webhook_security.c for signature canonicalization, timing comparison, replay protection, timestamp parsing, key handling, and algorithm confusion. Use read_file/grep_files only. Return the strongest defensible finding with exact evidence, severity, fix, and deterministic regression test.'
  'Audit DSCO local file-tool path confinement for traversal, symlink, TOCTOU, or allowlist bypass. Start at /Users/arthurcolle/Dsco/dsco-cli/src/tools.c and follow only relevant local definitions using read_file/grep_files. Return compact JSON: finding, reachable tool, evidence, preconditions, minimal_fix, regression_test. No writes or live exploitation.'
  'Audit /Users/arthurcolle/Dsco/dsco-cli/src/provider.c and src/tools.c for a tool-policy escape where DSCO_TOOL_ALLOWLIST, external tools, lazy loading, or cached schemas could advertise or execute a disallowed tool. Use read_file/grep_files only. Return one evidence-backed issue or no finding, plus a focused regression test.'
  'Audit /Users/arthurcolle/Dsco/dsco-router for API-key or bearer-token disclosure through logs, exceptions, traces, database fields, or client error responses. Use read_file/grep_files only and never output any secret value. Return compact JSON with source-to-sink evidence, severity, redaction fix, and regression test; state no finding if unsupported.'
)
run_id="$(date -u +%Y%m%dT%H%M%SZ)"
run_root="$OUT_ROOT/$run_id"
mkdir -p "$run_root"

python3 - "$run_root/manifest.json" "$run_id" "$PER_WORKER_USD" \
  "${CONCURRENCY[*]}" "${MODELS[*]}" <<'PY'
import json
import sys

path, run_id, per_worker, concurrency_raw, models_raw = sys.argv[1:]
concurrency = [int(value) for value in concurrency_raw.split()]
models = models_raw.split()
total_workers = len(models) * sum(concurrency)
payload = {
    "run_id": run_id,
    "models": models,
    "concurrency": concurrency,
    "total_worker_ceiling": total_workers,
    "per_worker_budget_usd": float(per_worker),
    "aggregate_budget_ceiling_usd": total_workers * float(per_worker),
}
with open(path, "w", encoding="utf-8") as handle:
    json.dump(payload, handle, separators=(",", ":"))
    handle.write("\n")
PY

for concurrency in "${CONCURRENCY[@]}"; do
  for model in "${MODELS[@]}"; do
    wave="$run_root/${model}-c${concurrency}"
    mkdir -p "$wave"
    pids=()

    for ((worker = 1; worker <= concurrency; worker++)); do
      worker_task="$TASK"
      if [[ "${ABLIT_BENCH_SUITE:-}" == cyber ]]; then
        task_offset="${ABLIT_BENCH_TASK_OFFSET:-0}"
        task_index=$(((task_offset + worker - 1) % ${#CYBER_TASKS[@]}))
        worker_task="You may use at most four tool calls. After the fourth tool call, stop investigating and return the requested compact JSON finding. ${CYBER_TASKS[$task_index]}"
      fi
      (
        started_ns="$(python3 -c 'import time; print(time.time_ns())')"
        set +e
        command=(
          env
          DSCO_BUDGET="$PER_WORKER_USD"
          DSCO_DAILY_BUDGET="$DAILY_USD"
          DSCO_MAX_TOKENS="$MAX_TOKENS"
          DSCO_HARD_TURN_CEILING="$HARD_TURNS"
          DSCO_PROMPT_CACHE_KEY="dsco-abliteration-swarm-v1-${model}"
          DSCO_PROMPT_CACHE_RETENTION=24h
          DSCO_PARALLEL_TOOL_CALLS=0
          DSCO_TOOL_ALLOWLIST="${ABLIT_BENCH_TOOL_ALLOWLIST:-read_file}"
          "$TIMEOUT_BIN" --signal=TERM --kill-after=5 "${WALL_SECONDS}s"
          "$DSCO_BIN"
          --profile worker
          --provider abliteration-ai
          -m "$model"
          --effort "$EFFORT"
          -p "$worker_task"
        )
        if [[ "${ABLIT_BENCH_LIVE:-0}" == 1 ]]; then
          "${command[@]}" \
            > >(tee "$wave/worker-${worker}.stdout.txt") \
            2> >(tee "$wave/worker-${worker}.stderr.txt" >&2)
        else
          "${command[@]}" \
            >"$wave/worker-${worker}.stdout.txt" \
            2>"$wave/worker-${worker}.stderr.txt"
        fi
        rc=$?
        set -e
        ended_ns="$(python3 -c 'import time; print(time.time_ns())')"
        python3 - "$wave/worker-${worker}.json" "$worker" "$model" "$rc" "$started_ns" "$ended_ns" <<'PY'
import json
import sys

path, worker, model, rc, started, ended = sys.argv[1:]
payload = {
    "worker": int(worker),
    "model": model,
    "exit_code": int(rc),
    "started_ns": int(started),
    "ended_ns": int(ended),
    "elapsed_seconds": (int(ended) - int(started)) / 1_000_000_000,
}
with open(path, "w", encoding="utf-8") as handle:
    json.dump(payload, handle, separators=(",", ":"))
    handle.write("\n")
PY
        exit "$rc"
      ) &
      pids+=("$!")
    done

    for pid in "${pids[@]}"; do
      wait "$pid" || true
    done

    set +e
    python3 - "$wave" "$concurrency" <<'PY'
import json
import math
import pathlib
import re
import sys

wave = pathlib.Path(sys.argv[1])
expected = int(sys.argv[2])
records = [json.loads(path.read_text()) for path in sorted(wave.glob("worker-*.json"))]
latencies = sorted(item["elapsed_seconds"] for item in records)

def percentile(values, q):
    if not values:
        return None
    return values[min(len(values) - 1, math.ceil(q * len(values)) - 1)]

failed_workers = {item["worker"] for item in records if item["exit_code"] != 0}
empty_outputs = 0
for item in records:
    output_path = wave / f"worker-{item['worker']}.stdout.txt"
    if not output_path.exists() or len(output_path.read_text(errors="replace").strip()) < 40:
        empty_outputs += 1
        failed_workers.add(item["worker"])
failures = len(failed_workers) + max(0, expected - len(records))
rate_limits = 0
for path in wave.glob("worker-*.stderr.txt"):
    text = path.read_text(errors="replace")
    if re.search(
        r"(?:HTTP(?:/[0-9.]+)?\s+429\b|429 Too Many Requests|"
        r"status(?:_code)?[=: ]+429\b|rate_limit_exceeded)",
        text,
        re.IGNORECASE,
    ):
        rate_limits += 1

summary = {
    "workers_expected": expected,
    "workers_observed": len(records),
    "failures": failures,
    "empty_outputs": empty_outputs,
    "error_rate": failures / expected,
    "rate_limit_signals": rate_limits,
    "latency_seconds": {
        "p50": percentile(latencies, 0.50),
        "p95": percentile(latencies, 0.95),
        "max": max(latencies) if latencies else None,
    },
}
(wave / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps({"wave": str(wave), **summary}, separators=(",", ":")))
if rate_limits or summary["error_rate"] > 0.05:
    raise SystemExit(75)
PY
    wave_rc=$?
    set -e
    if [[ "$wave_rc" -eq 75 ]]; then
      echo "rollback threshold reached at model=$model concurrency=$concurrency; stopping ramp" >&2
      exit 75
    fi
    if [[ "$wave_rc" -ne 0 ]]; then
      exit "$wave_rc"
    fi
  done
done

echo "Abliteration swarm benchmark complete: $run_root"
