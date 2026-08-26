#!/bin/sh
# Run the existing Qwen3.5 2B and Qwythos 9B as an adaptive local cascade.
# No model downloads or copies are performed.

set -eu

ROOT=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)
LLAMA_DIR=${DSCO_LLAMACPP_DIR:-$HOME/native_tools/llama.cpp/build/bin}
LLAMA_SERVER="$LLAMA_DIR/llama-server"
ROUTER=${DSCO_SMART_ROUTER_BIN:-$ROOT/local-smart-router}
FAST_MODEL=${DSCO_SMART_FAST_MODEL:-$HOME/.lmstudio/models/lmstudio-community/Qwen3.5-2B-GGUF/Qwen3.5-2B-Q8_0.gguf}
SMART_MODEL=${DSCO_SMART_MODEL:-$HOME/.lmstudio/models/local/Qwythos-9B-Claude-Mythos-5-1M/Qwythos-9B-Claude-Mythos-5-1M-uncensored-heretic-Q4_K_M.gguf}

PUBLIC_PORT=${DSCO_SMART_PORT:-8080}
FAST_PORT=${DSCO_SMART_FAST_PORT:-8081}
SMART_PORT=${DSCO_SMART_MODEL_PORT:-8082}
FAST_SLOTS=${DSCO_SMART_FAST_SLOTS:-2}
FAST_CTX_PER_SLOT=${DSCO_SMART_FAST_CTX:-8192}
SMART_CTX=${DSCO_SMART_CTX:-16384}
THRESHOLD=${DSCO_SMART_THRESHOLD:-1800}
REASONING_BUDGET=${DSCO_SMART_REASONING_BUDGET:-64}
STATE_DIR=${DSCO_SMART_STATE_DIR:-$HOME/.dsco/local-smart}
SUPERVISOR_PID_FILE="$STATE_DIR/supervisor.pid"
FAST_PID_FILE="$STATE_DIR/fast.pid"
SMART_PID_FILE="$STATE_DIR/smart.pid"
SUPERVISOR_LOG="$STATE_DIR/supervisor.log"
FAST_LOG="$STATE_DIR/fast.log"
SMART_LOG="$STATE_DIR/smart.log"

ROUTER_PID=0
FAST_PID=0
SMART_PID=0

usage() {
    cat <<'EOF'
usage: scripts/local-smart.sh [serve|start|stop|status|test|route|print]

  serve   run the adaptive endpoint in the foreground
  start   start it in the background
  stop    stop the background supervisor and both model servers
  status  show router/backend health
  test    send one automatic fast request and one automatic smart request
  route   explain the model decision for a prompt without running inference
  print   show resolved models, ports, and routing policy

Endpoint: http://127.0.0.1:8080/v1
  model=auto       heuristic routing (default)
  model=fast       force Qwen3.5 2B, thinking off
  model=smart      force Qwythos 9B, bounded thinking on
  X-DSCO-Lane      fast or smart; overrides the model and heuristic
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

is_positive_uint() {
    is_uint "$1" && [ "$1" -gt 0 ] 2>/dev/null
}

is_port() {
    is_positive_uint "$1" && [ "$1" -le 65535 ] 2>/dev/null
}

check_config() {
    [ -x "$LLAMA_SERVER" ] || die "llama-server not executable: $LLAMA_SERVER"
    [ -f "$FAST_MODEL" ] || die "fast model not found: $FAST_MODEL"
    [ -f "$SMART_MODEL" ] || die "smart model not found: $SMART_MODEL"
    for value in "$PUBLIC_PORT" "$FAST_PORT" "$SMART_PORT"; do
        is_port "$value" || die "expected a TCP port, got: $value"
    done
    for value in "$FAST_SLOTS" "$FAST_CTX_PER_SLOT" "$SMART_CTX" "$THRESHOLD"; do
        is_positive_uint "$value" || die "expected a positive integer, got: $value"
    done
    is_uint "$REASONING_BUDGET" || die "expected a non-negative reasoning budget"
    [ "$PUBLIC_PORT" -ne "$FAST_PORT" ] && [ "$PUBLIC_PORT" -ne "$SMART_PORT" ] &&
        [ "$FAST_PORT" -ne "$SMART_PORT" ] || die "router and backend ports must be distinct"
}

build_router() {
    if [ ! -x "$ROUTER" ] || [ "$ROOT/src/local_smart_router.c" -nt "$ROUTER" ] ||
        [ "$ROOT/include/local_smart_router.h" -nt "$ROUTER" ]; then
        make -s -C "$ROOT" local-smart-router
    fi
    [ -x "$ROUTER" ] || die "router build failed: $ROUTER"
}

backend_ready() {
    port=$1
    pid=$2
    i=0
    while [ "$i" -lt 120 ]; do
        if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
            return 0
        fi
        kill -0 "$pid" 2>/dev/null || return 1
        i=$((i + 1))
        sleep 1
    done
    return 1
}

cleanup_children() {
    if [ "$ROUTER_PID" -gt 0 ] 2>/dev/null; then
        kill "$ROUTER_PID" 2>/dev/null || true
    fi
    if [ "$FAST_PID" -gt 0 ] 2>/dev/null; then
        kill "$FAST_PID" 2>/dev/null || true
    fi
    if [ "$SMART_PID" -gt 0 ] 2>/dev/null; then
        kill "$SMART_PID" 2>/dev/null || true
    fi
    [ "$ROUTER_PID" -gt 0 ] 2>/dev/null && wait "$ROUTER_PID" 2>/dev/null || true
    [ "$FAST_PID" -gt 0 ] 2>/dev/null && wait "$FAST_PID" 2>/dev/null || true
    [ "$SMART_PID" -gt 0 ] 2>/dev/null && wait "$SMART_PID" 2>/dev/null || true
    rm -f "$FAST_PID_FILE" "$SMART_PID_FILE"
}

serve() {
    check_config
    build_router
    mkdir -p "$STATE_DIR"
    trap 'cleanup_children' EXIT HUP INT TERM

    fast_total_ctx=$((FAST_CTX_PER_SLOT * FAST_SLOTS))
    DYLD_LIBRARY_PATH="$LLAMA_DIR" LD_LIBRARY_PATH="$LLAMA_DIR" \
        "$LLAMA_SERVER" -m "$FAST_MODEL" --host 127.0.0.1 --port "$FAST_PORT" \
        -c "$fast_total_ctx" -np "$FAST_SLOTS" -ngl all -fa on \
        -ctk q4_0 -ctv q4_0 --no-host --fit off --cont-batching --kv-unified \
        -b 2048 -ub 256 --reasoning off --no-warmup >"$FAST_LOG" 2>&1 &
    FAST_PID=$!
    printf '%s\n' "$FAST_PID" >"$FAST_PID_FILE"

    DYLD_LIBRARY_PATH="$LLAMA_DIR" LD_LIBRARY_PATH="$LLAMA_DIR" \
        "$LLAMA_SERVER" -m "$SMART_MODEL" --host 127.0.0.1 --port "$SMART_PORT" \
        -c "$SMART_CTX" -np 1 -ngl all -fa on -ctk q4_0 -ctv q4_0 \
        --no-host --fit off --cont-batching -b 2048 -ub 256 \
        --reasoning auto --reasoning-budget "$REASONING_BUDGET" \
        --reasoning-preserve --no-warmup >"$SMART_LOG" 2>&1 &
    SMART_PID=$!
    printf '%s\n' "$SMART_PID" >"$SMART_PID_FILE"

    if ! backend_ready "$FAST_PORT" "$FAST_PID"; then
        die "fast backend failed; see $FAST_LOG"
    fi
    if ! backend_ready "$SMART_PORT" "$SMART_PID"; then
        die "smart backend failed; see $SMART_LOG"
    fi

    printf 'adaptive local endpoint: http://127.0.0.1:%s/v1\n' "$PUBLIC_PORT" >&2
    "$ROUTER" --listen-port "$PUBLIC_PORT" --fast-port "$FAST_PORT" \
        --smart-port "$SMART_PORT" --threshold "$THRESHOLD" &
    ROUTER_PID=$!
    wait "$ROUTER_PID"
}

start_background() {
    check_config
    build_router
    mkdir -p "$STATE_DIR"
    if [ -f "$SUPERVISOR_PID_FILE" ]; then
        old=$(sed -n '1p' "$SUPERVISOR_PID_FILE")
        if is_uint "$old" && kill -0 "$old" 2>/dev/null; then
            die "adaptive server already running as PID $old"
        fi
    fi
    nohup "$0" _serve >>"$SUPERVISOR_LOG" 2>&1 &
    pid=$!
    printf '%s\n' "$pid" >"$SUPERVISOR_PID_FILE"
    if ! backend_ready "$PUBLIC_PORT" "$pid"; then
        kill "$pid" 2>/dev/null || true
        die "adaptive server failed; see $SUPERVISOR_LOG"
    fi
    printf 'started adaptive server: PID %s, http://127.0.0.1:%s/v1\n' "$pid" "$PUBLIC_PORT"
}

stop_background() {
    [ -f "$SUPERVISOR_PID_FILE" ] || die "no adaptive supervisor pid file"
    pid=$(sed -n '1p' "$SUPERVISOR_PID_FILE")
    if is_uint "$pid" && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 30 ]; do
            i=$((i + 1))
            sleep 1
        done
    fi
    for file in "$FAST_PID_FILE" "$SMART_PID_FILE"; do
        if [ -f "$file" ]; then
            child=$(sed -n '1p' "$file")
            is_uint "$child" && kill "$child" 2>/dev/null || true
        fi
    done
    rm -f "$SUPERVISOR_PID_FILE" "$FAST_PID_FILE" "$SMART_PID_FILE"
    printf 'stopped adaptive server\n'
}

status() {
    health=$(curl -fsS --max-time 2 "http://127.0.0.1:$PUBLIC_PORT/health") ||
        die "adaptive endpoint is down"
    printf '%s\n' "$health"
}

test_routes() {
    fast=$(curl -fsS -D "$STATE_DIR/test-fast.headers" \
        "http://127.0.0.1:$PUBLIC_PORT/v1/chat/completions" \
        -H 'Content-Type: application/json' \
        -d '{"model":"auto","messages":[{"role":"user","content":"Reply exactly FAST_ROUTE_OK"}],"temperature":0,"max_tokens":32,"stream":false}')
    smart=$(curl -fsS -D "$STATE_DIR/test-smart.headers" \
        "http://127.0.0.1:$PUBLIC_PORT/v1/chat/completions" \
        -H 'Content-Type: application/json' \
        -d '{"model":"auto","messages":[{"role":"user","content":"Analyze this architecture carefully, then reply exactly SMART_ROUTE_OK"}],"chat_template_kwargs":{"enable_thinking":false},"temperature":0,"max_tokens":32,"stream":false}')
    fast_lane=$(sed -n 's/^X-DSCO-Local-Lane: //Ip' "$STATE_DIR/test-fast.headers" | tr -d '\r')
    fast_score=$(sed -n 's/^X-DSCO-Route-Score: //Ip' "$STATE_DIR/test-fast.headers" | tr -d '\r')
    fast_reason=$(sed -n 's/^X-DSCO-Route-Reason: //Ip' "$STATE_DIR/test-fast.headers" | tr -d '\r')
    smart_lane=$(sed -n 's/^X-DSCO-Local-Lane: //Ip' "$STATE_DIR/test-smart.headers" | tr -d '\r')
    smart_score=$(sed -n 's/^X-DSCO-Route-Score: //Ip' "$STATE_DIR/test-smart.headers" | tr -d '\r')
    smart_reason=$(sed -n 's/^X-DSCO-Route-Reason: //Ip' "$STATE_DIR/test-smart.headers" | tr -d '\r')
    printf 'fast:  %s (%s score=%s reason=%s)\n' \
        "$(printf '%s' "$fast" | jq -r '.choices[0].message.content')" \
        "$fast_lane" "$fast_score" "$fast_reason"
    printf 'smart: %s (%s score=%s reason=%s)\n' \
        "$(printf '%s' "$smart" | jq -r '.choices[0].message.content')" \
        "$smart_lane" "$smart_score" "$smart_reason"
    rm -f "$STATE_DIR/test-fast.headers" "$STATE_DIR/test-smart.headers"
}

explain_route() {
    prompt=${1:-Hello}
    body=$(jq -nc --arg prompt "$prompt" \
        '{model:"auto",messages:[{role:"user",content:$prompt}]}')
    curl -fsS "http://127.0.0.1:$PUBLIC_PORT/v1/route" \
        -H 'Content-Type: application/json' -d "$body"
    printf '\n'
}

print_config() {
    printf 'router: %s\nfast:   %s @ 127.0.0.1:%s (%s slots)\n' \
        "$ROUTER" "$FAST_MODEL" "$FAST_PORT" "$FAST_SLOTS"
    printf 'smart:  %s @ 127.0.0.1:%s (reasoning budget %s)\n' \
        "$SMART_MODEL" "$SMART_PORT" "$REASONING_BUDGET"
    printf 'public: http://127.0.0.1:%s/v1 (smart user-message threshold %s bytes)\n' \
        "$PUBLIC_PORT" "$THRESHOLD"
}

cmd=${1:-serve}
case "$cmd" in
    serve|_serve) serve ;;
    start) start_background ;;
    stop) stop_background ;;
    status) status ;;
    test) mkdir -p "$STATE_DIR"; test_routes ;;
    route) shift; explain_route "${*:-Hello}" ;;
    print) check_config; build_router; print_config ;;
    -h|--help|help) usage ;;
    *) usage >&2; exit 2 ;;
esac
