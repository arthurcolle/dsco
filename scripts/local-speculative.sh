#!/bin/sh
# Space-safe llama.cpp launcher for a local target + draft model.
# It never downloads, copies, converts, or quantizes model weights.

set -eu

TARGET_DEFAULT="$HOME/.lmstudio/models/local/Qwythos-9B-Claude-Mythos-5-1M/Qwythos-9B-Claude-Mythos-5-1M-uncensored-heretic-Q4_K_M.gguf"
DRAFT_DEFAULT="$HOME/.lmstudio/models/lmstudio-community/Qwen3.5-2B-GGUF/Qwen3.5-2B-Q8_0.gguf"
LLAMA_DIR_DEFAULT="$HOME/native_tools/llama.cpp/build/bin"

TARGET=${DSCO_SPEC_TARGET_MODEL:-$TARGET_DEFAULT}
DRAFT=${DSCO_SPEC_DRAFT_MODEL:-$DRAFT_DEFAULT}
LLAMA_DIR=${DSCO_LLAMACPP_DIR:-$LLAMA_DIR_DEFAULT}
SERVER="$LLAMA_DIR/llama-server"
PORT=${DSCO_SPEC_PORT:-8080}
CTX=${DSCO_SPEC_CTX:-4096}
SLOTS=${DSCO_SPEC_SLOTS:-1}
KV_TYPE=${DSCO_SPEC_KV_TYPE:-q4_0}
BATCH=${DSCO_SPEC_BATCH:-2048}
UBATCH=${DSCO_SPEC_UBATCH:-256}
UNIFIED_KV=0
DRAFT_MAX=${DSCO_SPEC_DRAFT_MAX:-4}
MIN_GAIN=${DSCO_SPEC_MIN_GAIN:-1.03}
STATE_DIR=${DSCO_SPEC_STATE_DIR:-$HOME/.dsco/local-speculative}
PID_FILE="$STATE_DIR/server.pid"
LOG_FILE="$STATE_DIR/server.log"
MODE_FILE="$STATE_DIR/mode"
BENCH_PROMPT=${DSCO_SPEC_BENCH_PROMPT:-Write a detailed technical explanation of speculative decoding for local language models, including tradeoffs, in continuous prose.}

usage() {
    cat <<'EOF'
usage: scripts/local-speculative.sh [auto|serve|serve-target|serve-throughput|bench|status|stop|print]

  auto          benchmark once, then foreground the faster configuration (default)
  serve         foreground target + draft speculative decoding
  serve-target  foreground the target alone
  serve-throughput  foreground four-slot continuous batching
  bench         compare target-only and target + draft without leaving a server running
  status        report the background server created by start-* commands
  stop          stop that background server
  print         print resolved paths and the two llama-server commands
  start-auto    benchmark once, then start the winner in the background
  start-spec    start target + draft in the background
  start-target  start the target alone in the background
  start-throughput  start four-slot continuous batching in the background

No command downloads or duplicates model weights. Override paths with:
  DSCO_SPEC_TARGET_MODEL=/path/target.gguf
  DSCO_SPEC_DRAFT_MODEL=/path/draft.gguf
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

validate_common() {
    [ -x "$SERVER" ] || die "llama-server not executable: $SERVER"
    [ -f "$TARGET" ] || die "target GGUF not found: $TARGET"
    is_uint "$PORT" && [ "$PORT" -gt 0 ] && [ "$PORT" -lt 65536 ] || die "invalid DSCO_SPEC_PORT: $PORT"
    is_uint "$CTX" && [ "$CTX" -ge 512 ] || die "invalid DSCO_SPEC_CTX: $CTX"
    is_uint "$SLOTS" && [ "$SLOTS" -ge 1 ] || die "invalid DSCO_SPEC_SLOTS: $SLOTS"
    is_uint "$BATCH" && [ "$BATCH" -ge 1 ] || die "invalid DSCO_SPEC_BATCH: $BATCH"
    is_uint "$UBATCH" && [ "$UBATCH" -ge 1 ] && [ "$UBATCH" -le "$BATCH" ] ||
        die "invalid DSCO_SPEC_UBATCH: $UBATCH (must be <= DSCO_SPEC_BATCH)"
    case "$KV_TYPE" in
        f32|f16|bf16|q8_0|q4_0|q4_1|iq4_nl|q5_0|q5_1) ;;
        *) die "invalid DSCO_SPEC_KV_TYPE: $KV_TYPE" ;;
    esac
}

enable_throughput_profile() {
    SLOTS=${DSCO_SPEC_THROUGHPUT_SLOTS:-4}
    if [ "${DSCO_SPEC_CTX+x}" != x ]; then
        CTX=$((4096 * SLOTS))
    fi
    UNIFIED_KV=1
}

validate_draft() {
    [ -f "$DRAFT" ] || die "draft GGUF not found: $DRAFT"
    is_uint "$DRAFT_MAX" && [ "$DRAFT_MAX" -ge 1 ] || die "invalid DSCO_SPEC_DRAFT_MAX: $DRAFT_MAX"
}

common_args() {
    printf '%s\n' \
        -m "$TARGET" \
        --host 127.0.0.1 --port "$PORT" \
        -c "$CTX" -np "$SLOTS" \
        -ngl all -fa on \
        -ctk "$KV_TYPE" -ctv "$KV_TYPE" \
        --no-host --fit off \
        --cont-batching -b "$BATCH" -ub "$UBATCH" \
        --metrics
    if [ "$UNIFIED_KV" -eq 1 ]; then
        printf '%s\n' --kv-unified
    fi
}

print_command() {
    kind=$1
    printf "DYLD_LIBRARY_PATH='%s' LD_LIBRARY_PATH='%s' '%s'" "$LLAMA_DIR" "$LLAMA_DIR" "$SERVER"
    common_args | while IFS= read -r arg; do printf " '%s'" "$arg"; done
    if [ "$kind" = spec ]; then
        printf " -md '%s' --spec-type draft-simple --spec-draft-n-max '%s'" "$DRAFT" "$DRAFT_MAX"
        printf " --spec-draft-n-min '1' --spec-draft-p-min '0.0' -ngld 'all' -ctkd '%s' -ctvd '%s'" "$KV_TYPE" "$KV_TYPE"
    fi
    printf '\n'
}

run_server() {
    kind=$1
    set -- -m "$TARGET" --host 127.0.0.1 --port "$PORT" -c "$CTX" -np "$SLOTS" \
        -ngl all -fa on -ctk "$KV_TYPE" -ctv "$KV_TYPE" --no-host --fit off \
        --cont-batching -b "$BATCH" -ub "$UBATCH" --metrics
    if [ "$UNIFIED_KV" -eq 1 ]; then
        set -- "$@" --kv-unified
    fi
    if [ "$kind" = spec ]; then
        set -- "$@" -md "$DRAFT" --spec-type draft-simple \
            --spec-draft-n-max "$DRAFT_MAX" --spec-draft-n-min 1 --spec-draft-p-min 0.0 \
            -ngld all -ctkd "$KV_TYPE" -ctvd "$KV_TYPE"
    fi
    DYLD_LIBRARY_PATH="$LLAMA_DIR" LD_LIBRARY_PATH="$LLAMA_DIR" exec "$SERVER" "$@"
}

start_temp() {
    kind=$1
    log=$2
    set -- -m "$TARGET" --host 127.0.0.1 --port "$PORT" -c "$CTX" -np "$SLOTS" \
        -ngl all -fa on -ctk "$KV_TYPE" -ctv "$KV_TYPE" --no-host --fit off \
        --cont-batching -b "$BATCH" -ub "$UBATCH" --metrics --no-warmup
    if [ "$UNIFIED_KV" -eq 1 ]; then
        set -- "$@" --kv-unified
    fi
    if [ "$kind" = spec ]; then
        set -- "$@" -md "$DRAFT" --spec-type draft-simple \
            --spec-draft-n-max "$DRAFT_MAX" --spec-draft-n-min 1 --spec-draft-p-min 0.0 \
            -ngld all -ctkd "$KV_TYPE" -ctvd "$KV_TYPE"
    fi
    DYLD_LIBRARY_PATH="$LLAMA_DIR" LD_LIBRARY_PATH="$LLAMA_DIR" "$SERVER" "$@" >"$log" 2>&1 &
    TEMP_PID=$!
    export TEMP_PID
}

stop_temp() {
    if [ "${TEMP_PID:-0}" -gt 0 ] 2>/dev/null; then
        kill "$TEMP_PID" 2>/dev/null || true
        wait "$TEMP_PID" 2>/dev/null || true
    fi
    TEMP_PID=0
}

wait_ready() {
    i=0
    while [ "$i" -lt 120 ]; do
        if curl -fsS --max-time 1 "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$TEMP_PID" 2>/dev/null; then
            return 1
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

bench_one() {
    kind=$1
    log="$STATE_DIR/bench-$kind.log"
    start_temp "$kind" "$log"
    if ! wait_ready; then
        stop_temp
        die "$kind server failed to start; see $log"
    fi

    body=$(jq -nc --arg prompt "$BENCH_PROMPT" \
        '{model:"local",messages:[{role:"user",content:$prompt}],temperature:0,max_tokens:64,cache_prompt:false,stream:false}')
    result=$(curl -fsS --max-time 180 "http://127.0.0.1:$PORT/v1/chat/completions" \
        -H 'Content-Type: application/json' -d "$body") || {
        stop_temp
        die "$kind benchmark request failed; see $log"
    }
    rate=$(printf '%s' "$result" | jq -er '.timings.predicted_per_second') || {
        stop_temp
        die "$kind response did not include llama.cpp timings"
    }
    prefill=$(printf '%s' "$result" | jq -er '.timings.prompt_per_second')
    total_ms=$(printf '%s' "$result" | jq -er '(.timings.prompt_ms + .timings.predicted_ms)')
    e2e=$(printf '%s' "$result" | jq -er \
        '((.timings.prompt_n + .timings.predicted_n) * 1000 / (.timings.prompt_ms + .timings.predicted_ms))')
    accepted=$(printf '%s' "$result" | jq -r \
        'if .timings.draft_n then " draft=" + (.timings.draft_n_accepted|tostring) + "/" + (.timings.draft_n|tostring) else "" end')
    printf '%-7s decode=%7.2f prefill=%7.2f e2e=%7.2f tok/s total=%7.1f ms%s\n' \
        "$kind" "$rate" "$prefill" "$e2e" "$total_ms" "$accepted" >&2
    stop_temp
    printf '%s\n' "$e2e"
}

choose_mode() {
    validate_draft
    command -v curl >/dev/null 2>&1 || die "curl is required for auto/bench"
    command -v jq >/dev/null 2>&1 || die "jq is required for auto/bench"
    mkdir -p "$STATE_DIR"
    trap 'stop_temp' EXIT HUP INT TERM
    target_rate=$(bench_one target)
    spec_rate=$(bench_one spec)
    winner=$(awk -v target="$target_rate" -v spec="$spec_rate" -v gain="$MIN_GAIN" \
        'BEGIN { print (spec >= target * gain) ? "spec" : "target" }')
    trap - EXIT HUP INT TERM
    printf 'winner  %s (spec must be at least %.2fx target)\n' "$winner" "$MIN_GAIN" >&2
    printf '%s\n' "$winner"
}

start_background() {
    kind=$1
    mkdir -p "$STATE_DIR"
    if [ -f "$PID_FILE" ]; then
        old_pid=$(sed -n '1p' "$PID_FILE")
        if is_uint "$old_pid" && kill -0 "$old_pid" 2>/dev/null; then
            die "server already running as PID $old_pid"
        fi
    fi
    set -- -m "$TARGET" --host 127.0.0.1 --port "$PORT" -c "$CTX" -np "$SLOTS" \
        -ngl all -fa on -ctk "$KV_TYPE" -ctv "$KV_TYPE" --no-host --fit off \
        --cont-batching -b "$BATCH" -ub "$UBATCH" --metrics
    if [ "$UNIFIED_KV" -eq 1 ]; then
        set -- "$@" --kv-unified
    fi
    if [ "$kind" = spec ]; then
        set -- "$@" -md "$DRAFT" --spec-type draft-simple \
            --spec-draft-n-max "$DRAFT_MAX" --spec-draft-n-min 1 --spec-draft-p-min 0.0 \
            -ngld all -ctkd "$KV_TYPE" -ctvd "$KV_TYPE"
    fi
    DYLD_LIBRARY_PATH="$LLAMA_DIR" LD_LIBRARY_PATH="$LLAMA_DIR" \
        nohup "$SERVER" "$@" >>"$LOG_FILE" 2>&1 &
    pid=$!
    TEMP_PID=$pid
    if ! wait_ready; then
        stop_temp
        die "$kind server failed to become healthy; see $LOG_FILE"
    fi
    TEMP_PID=0
    printf '%s\n' "$pid" >"$PID_FILE"
    printf '%s\n' "$kind" >"$MODE_FILE"
    printf 'started %s server: PID %s, http://127.0.0.1:%s/v1\nlog: %s\n' "$kind" "$pid" "$PORT" "$LOG_FILE"
}

status_server() {
    [ -f "$PID_FILE" ] || die "no background server pid file: $PID_FILE"
    pid=$(sed -n '1p' "$PID_FILE")
    mode=$(sed -n '1p' "$MODE_FILE" 2>/dev/null || printf unknown)
    if is_uint "$pid" && kill -0 "$pid" 2>/dev/null; then
        printf 'running: PID %s, mode=%s, http://127.0.0.1:%s/v1\n' "$pid" "$mode" "$PORT"
        return 0
    fi
    die "stale pid file (PID $pid is not running)"
}

stop_server() {
    [ -f "$PID_FILE" ] || die "no background server pid file: $PID_FILE"
    pid=$(sed -n '1p' "$PID_FILE")
    is_uint "$pid" || die "invalid pid file: $PID_FILE"
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid"
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 30 ]; do
            i=$((i + 1))
            sleep 1
        done
        kill -0 "$pid" 2>/dev/null && die "PID $pid did not stop"
    else
        printf 'cleaning stale PID %s\n' "$pid"
    fi
    rm -f "$PID_FILE" "$MODE_FILE"
    printf 'stopped PID %s\n' "$pid"
}

cmd=${1:-auto}
case "$cmd" in
    -h|--help|help) usage ;;
    print)
        validate_common
        validate_draft
        printf 'target: %s\ndraft:  %s\nllama:  %s\n\n' "$TARGET" "$DRAFT" "$SERVER"
        print_command target
        print_command spec
        ;;
    bench)
        validate_common
        choose_mode >/dev/null
        ;;
    auto)
        validate_common
        winner=$(choose_mode)
        run_server "$winner"
        ;;
    serve)
        validate_common
        validate_draft
        run_server spec
        ;;
    serve-target)
        validate_common
        run_server target
        ;;
    serve-throughput)
        enable_throughput_profile
        validate_common
        run_server target
        ;;
    start-auto)
        validate_common
        winner=$(choose_mode)
        start_background "$winner"
        ;;
    start-spec)
        validate_common
        validate_draft
        start_background spec
        ;;
    start-target)
        validate_common
        start_background target
        ;;
    start-throughput)
        enable_throughput_profile
        validate_common
        start_background throughput
        ;;
    status) status_server ;;
    stop) stop_server ;;
    *) usage >&2; exit 2 ;;
esac
