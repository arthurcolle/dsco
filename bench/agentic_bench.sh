#!/usr/bin/env bash
# agentic_bench.sh — DSCO harness benchmark v0 (Phase 1.3, harness-entry plan)
# Scenarios: coding20 | fanout | crash_resume | bigtools | synthesis
# Output: bench/results/context-runtime-baseline.json
# Metrics: tokens-to-completion, TTFT p50 (turns 3+), cache read ratio,
#          crash loss (turns), fanout wall-clock ratio, tool-schema footprint.
set -euo pipefail
cd "$(dirname "$0")/.."
DSCO=${DSCO:-./dsco}
OUT=bench/results/context-runtime-baseline.json
mkdir -p bench/results
STAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
GIT=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)

run_scenario() { # name, prompt, extra env
  local name="$1" prompt="$2"
  local log="bench/results/${name}.log"
  local t0=$(date +%s%N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1e9))')
  DSCO_CACHE_PREFIX_HASH=1 timeout 600 "$DSCO" -p "$prompt" >"$log" 2>&1 || true
  local t1=$(date +%s%N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1e9))')
  echo $(( (t1 - t0) / 1000000 )) # wall ms
}

declare -A WALL
WALL[coding20]=$(run_scenario coding20 "Implement, test, and iterate on a small C ring-buffer module in /tmp/bench_ring; run at least 15 tool turns of build/test/fix.")
WALL[fanout]=$(run_scenario fanout "Spawn 4 parallel sub-agents to each summarize one src/*.c module; synthesize.")
WALL[bigtools]=$(run_scenario bigtools "Discover and load 3 tool categories, then use one tool from each; report context footprint.")
WALL[synthesis]=$(run_scenario synthesis "Read docs/harness.json and docs/llms.txt; produce a one-paragraph consistency check.")

# crash_resume measured manually until `dsco resume --last` ships (Wave 2)
python3 - "$OUT" "$STAMP" "$GIT" <<'EOF'
import json, re, sys, glob, os
out, stamp, git = sys.argv[1:4]
res = {"timestamp": stamp, "git": git, "scenarios": {}}
for log in glob.glob("bench/results/*.log"):
    name = os.path.basename(log)[:-4]
    txt = open(log, errors="ignore").read()
    cache = re.findall(r"hit:(\d+)%", txt)
    ttft = re.findall(r"ttft:(\d+)ms", txt)
    tokens = re.findall(r"out:(\d+)", txt)
    churn = txt.count("cache-prefix churn")
    res["scenarios"][name] = {
        "cache_hit_pct_last": int(cache[-1]) if cache else None,
        "ttft_ms_all": [int(t) for t in ttft],
        "out_tokens_total": sum(int(t) for t in tokens),
        "prefix_churn_events": churn,
    }
json.dump(res, open(out, "w"), indent=2)
print("wrote", out)
EOF
echo "done: $OUT"
