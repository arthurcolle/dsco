#!/usr/bin/env bash
# Mode dispatch + durability wrapper for the dsco container.
#
# dsco has no single built-in "run forever" daemon mode — it's argv-driven
# (see README.md / src/main.c: one-shot prompt, `-i` REPL, `mcp serve`,
# `acp serve`, `--timeline-server`, `swarmd`). This script picks one of those
# and keeps it alive, because container orchestrators restart *containers*,
# not the process inside them if it hangs without exiting — and a stdio-based
# server like `mcp serve` has no network port to bridge to a Service unless
# something bridges it.
#
# DSCO_MODE selects what runs (default: mcp):
#   mcp       dsco mcp serve --toolsets ... --tier ...   (the persistent agent)
#   acp       dsco acp serve
#   timeline  dsco --timeline-server --timeline-port ... (observability UI)
#   swarmd    dsco swarmd                                (fleet daemon)
#   repl      dsco -i                                    (interactive; for
#                                                          `docker run -it` /
#                                                          `kubectl exec -it`)
#   *         anything else: exec dsco "$@" verbatim — lets a Job override
#             CMD for a one-shot task using the same image.
#
# DSCO_BIND_PORT, if set, makes the container reachable from outside its own
# network namespace (required for a Kubernetes Service to reach it):
#   - mcp/acp are stdio protocols, not network servers. socat's EXEC mode
#     spawns a fresh `dsco mcp serve` per TCP connection, wired to that
#     connection's stdin/stdout — the standard way to expose a stdio server
#     over the network without a code change. Each client gets an isolated
#     process; persistent state under ~/.dsco is still shared across them.
#   - timeline already opens a real TCP server, but only on 127.0.0.1
#     (src/baseline.c) — loopback is unreachable from outside the pod's own
#     network namespace, so socat here just forwards DSCO_BIND_PORT on all
#     interfaces to that loopback port instead of spawning anything.
set -euo pipefail

DSCO_MODE="${DSCO_MODE:-mcp}"
DSCO_TOOLSETS="${DSCO_TOOLSETS:-}"
DSCO_TIER="${DSCO_TIER:-agent}"
DSCO_TIMELINE_PORT="${DSCO_TIMELINE_PORT:-8421}"
DSCO_BIND_PORT="${DSCO_BIND_PORT:-}"

mkdir -p "$HOME/.dsco"

log() { printf '[dsco-entrypoint] %s\n' "$*" >&2; }

# Builds the `dsco mcp serve ...` command line as a single plain string for
# socat's EXEC: address, which splits on whitespace itself (no shell in
# between) — so this deliberately does NOT shell-quote: DSCO_TOOLSETS/
# DSCO_TIER are simple comma-lists/identifiers by contract, and quote
# characters here would land literally in dsco's argv instead of being
# stripped, since nothing interprets them.
mcp_exec_line() {
  local line="dsco mcp serve"
  [[ -n "$DSCO_TOOLSETS" ]] && line+=" --toolsets $DSCO_TOOLSETS"
  [[ -n "$DSCO_TIER" ]] && line+=" --tier $DSCO_TIER"
  printf '%s' "$line"
}

# Retries the given command with exponential backoff, capped so a
# persistently broken command (bad API key, unreachable provider) surfaces as
# a real container exit — and k8s's own CrashLoopBackOff / compose's
# `restart: unless-stopped` — rather than an entrypoint that retries forever
# and hides the failure from the orchestrator.
run_with_backoff() {
  local attempt=0 max_attempts=8 backoff
  while (( attempt < max_attempts )); do
    attempt=$((attempt + 1))
    log "starting (attempt $attempt/$max_attempts): $*"
    if "$@"; then
      log "exited cleanly"
      return 0
    fi
    local status=$?
    if (( attempt >= max_attempts )); then
      log "exited with status $status after $attempt attempts — giving up, letting the orchestrator restart the container"
      return "$status"
    fi
    backoff=$(( attempt < 6 ? 2 ** attempt : 60 ))
    log "exited with status $status — retrying in ${backoff}s"
    sleep "$backoff"
  done
}

# Runs two processes (an app + its socat bridge) and exits the moment either
# one dies, propagating its status — so a dead app doesn't leave a container
# that looks "up" with nothing actually listening, and a dead bridge doesn't
# leave the app running unreachable.
run_pair_and_exit_on_either() {
  bash -c "$1" & local pid_a=$!
  bash -c "$2" & local pid_b=$!
  local status=0
  if wait -n "$pid_a" "$pid_b"; then status=0; else status=$?; fi
  kill "$pid_a" "$pid_b" 2>/dev/null || true
  exit "$status"
}

case "$DSCO_MODE" in
  mcp)
    if [[ -n "$DSCO_BIND_PORT" ]]; then
      log "bridging MCP (stdio) to TCP :$DSCO_BIND_PORT via socat, one dsco process per connection"
      exec socat TCP-LISTEN:"$DSCO_BIND_PORT",reuseaddr,fork EXEC:"$(mcp_exec_line)"
    fi
    run_with_backoff dsco mcp serve ${DSCO_TOOLSETS:+--toolsets "$DSCO_TOOLSETS"} ${DSCO_TIER:+--tier "$DSCO_TIER"}
    ;;
  acp)
    if [[ -n "$DSCO_BIND_PORT" ]]; then
      log "bridging ACP (stdio) to TCP :$DSCO_BIND_PORT via socat, one dsco process per connection"
      exec socat TCP-LISTEN:"$DSCO_BIND_PORT",reuseaddr,fork EXEC:"dsco acp serve"
    fi
    run_with_backoff dsco acp serve
    ;;
  timeline)
    if [[ -n "$DSCO_BIND_PORT" ]]; then
      log "timeline server binds 127.0.0.1 only; forwarding TCP :$DSCO_BIND_PORT -> 127.0.0.1:$DSCO_TIMELINE_PORT via socat"
      run_pair_and_exit_on_either \
        "dsco --timeline-server --timeline-port $DSCO_TIMELINE_PORT" \
        "socat TCP-LISTEN:$DSCO_BIND_PORT,fork,reuseaddr TCP:127.0.0.1:$DSCO_TIMELINE_PORT"
    fi
    run_with_backoff dsco --timeline-server --timeline-port "$DSCO_TIMELINE_PORT"
    ;;
  swarmd)
    run_with_backoff dsco swarmd
    ;;
  repl)
    exec dsco -i
    ;;
  *)
    log "DSCO_MODE=$DSCO_MODE not recognized as a built-in mode; passing args through to dsco directly"
    exec dsco "$@"
    ;;
esac
