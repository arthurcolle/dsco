#!/usr/bin/env bash
# pipeline.sh — Sequential dsco pipeline: each step feeds the next
set -euo pipefail

DSCO="./dsco"
MODEL="${DSCO_MODEL:-claude-sonnet-4-6}"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

log_step() {
  local n="$1"
  local prompt="$2"
  local label="$3"
  echo "═══ Step ${n} ═══" >&2
  echo "  ${label}" >&2
  echo "  prompt: ${prompt:0:120}..." >&2
}

run_step() {
  local n="$1"
  shift
  local prompt="$1"
  local out_file="$TMPDIR/step${n}.out"
  local err_file="$TMPDIR/step${n}.err"

  log_step "$n" "$prompt" "running"
  if ! "$DSCO" -m "$MODEL" -p "$prompt" >"$out_file" 2>"$err_file"; then
    echo "  command failed. stderr:" >&2
    cat "$err_file" >&2
    exit 1
  fi

  if [ -s "$err_file" ]; then
    # Keep stderr visible in case dsco prints diagnostics
    cat "$err_file" >&2
  fi

  # Strip ANSI escape sequences if present, then return file contents
  sed -E $'s/\x1B\\[[0-9;]*[A-Za-z]//g' "$out_file"
}

extract_script_body() {
  local raw="$1"
  local in_block=0
  local body=""
  local fence='```'

  while IFS= read -r line; do
    if [[ "$line" == "${fence}"* ]]; then
      if (( in_block == 0 )); then
        in_block=1
        continue
      else
        break
      fi
    fi
    if (( in_block == 1 )); then
      body+="${body:+$'\n'}$line"
    fi
  done <<< "$raw"

  if [ -n "$body" ]; then
    printf '%s\n' "$body"
  else
    printf '%s\n' "$raw"
  fi
}

non_empty_or_fail() {
  local label="$1"
  local value="$2"
  if [ -z "${value//[[:space:]]/}" ]; then
    echo "  ${label} came back empty. Halting to avoid cascading failures." >&2
    exit 1
  fi
}

# Step 1: Gather system info
STEP1_RAW="$(run_step 1 "Give me a concise JSON object with: hostname, OS, kernel version, CPU architecture, total RAM, and current uptime of this machine. Output ONLY valid JSON, no markdown.")"
STEP1="$(echo "$STEP1_RAW" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')"
non_empty_or_fail "Step 1" "$STEP1"
echo "$STEP1" > "$TMPDIR/step1.json"
echo "  result saved" >&2

# Step 2: Analyze the system info
STEP2_RAW="$(run_step 2 "Given this system info: $STEP1 — Identify what kind of workloads this machine is best suited for. List 3 specific recommendations. Be concise, no markdown fences.")"
STEP2="$(echo "$STEP2_RAW" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')"
non_empty_or_fail "Step 2" "$STEP2"
echo "$STEP2" > "$TMPDIR/step2.txt"
echo "  result saved" >&2

# Step 3: Generate a monitoring script based on analysis
STEP3_PROMPT="Given these workload recommendations for my machine: $STEP2 — write a short bash monitoring script for the most relevant metrics. Include CPU, memory, disk, and network I/O.
Output ONLY a bash script in a fenced code block, and nothing else."
STEP3_RAW="$(run_step 3 "$STEP3_PROMPT")"
STEP3="$(extract_script_body "$STEP3_RAW")"
STEP3="$(echo "$STEP3" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')"
non_empty_or_fail "Step 3" "$STEP3"
echo "$STEP3" > "$TMPDIR/step3.sh"
chmod +x "$TMPDIR/step3.sh"
echo "  result saved" >&2

# Step 4: Review and critique the generated script
STEP4_RAW="$(run_step 4 "Review this bash monitoring script for correctness and portability on macOS. Point out any bugs or missing commands. Be concise.

Script:
$STEP3")"
STEP4="$(echo "$STEP4_RAW" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')"
non_empty_or_fail "Step 4" "$STEP4"
echo "$STEP4" > "$TMPDIR/step4.txt"
echo "  result saved" >&2

# Step 5: Produce final improved version
STEP5_RAW="$(run_step 5 "Apply these fixes to the monitoring script and output the final corrected version. Output ONLY the script in a fenced block:

Original script:
$STEP3

Review feedback:
$STEP4")"
STEP5="$(extract_script_body "$STEP5_RAW")"
non_empty_or_fail "Step 5" "$STEP5"

# Final output
echo ""
echo "╔════════════════════════════════════════╗"
echo "║   dsco pipeline complete (5 steps)     ║"
echo "╚════════════════════════════════════════╝"
echo ""
echo "── Step 1: System Info ──"
echo "$STEP1"
echo ""
echo "── Step 2: Workload Analysis ──"
echo "$STEP2"
echo ""
echo "── Step 3: Generated Monitor Script ──"
echo "$STEP3"
echo ""
echo "── Step 4: Code Review ──"
echo "$STEP4"
echo ""
echo "── Step 5: Final Script ──"
echo "$STEP5"
