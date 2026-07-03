#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSCO_BIN="${DSCO_BIN:-$ROOT/dsco}"
MODEL="${MODEL:-modal/zai-org/GLM-5.2-FP8}"
MODAL_ENDPOINT_URL="${MODAL_ENDPOINT_URL:-https://arthurcolle--ep-agent-server.us-west.modal.direct}"
TOKEN_FILE="${TOKEN_FILE:-/tmp/modal_proxy_token.json}"
TIMEOUT_SECS="${TIMEOUT_SECS:-60}"

PARAMS="${DSCO_OPENAI_PARAMS:-}"
if [[ -z "$PARAMS" ]]; then
  PARAMS='{"temperature":0,"logprobs":true,"top_logprobs":5,"stream_options":{"include_usage":true},"parallel_tool_calls":false}'
fi

if [[ ! -x "$DSCO_BIN" ]]; then
  DSCO_BIN="$(command -v dsco)"
fi

load_modal_proxy_token() {
  if [[ -n "${MODAL_KEY:-}" && "${MODAL_KEY:-}" != "..." && -n "${MODAL_SECRET:-}" && "${MODAL_SECRET:-}" != "..." ]]; then
    export MODAL_PROXY_TOKEN_ID="${MODAL_PROXY_TOKEN_ID:-$MODAL_KEY}"
    export MODAL_PROXY_TOKEN_SECRET="${MODAL_PROXY_TOKEN_SECRET:-$MODAL_SECRET}"
    return
  fi
  if [[ ! -f "$TOKEN_FILE" ]]; then
    echo "error: set MODAL_KEY/MODAL_SECRET or provide $TOKEN_FILE" >&2
    exit 1
  fi
  local parsed
  parsed="$(python3 - "$TOKEN_FILE" <<'PY'
import json
import sys
from pathlib import Path

data = json.loads(Path(sys.argv[1]).read_text())
key = data.get("Modal-Key") or data.get("MODAL_KEY") or data.get("key")
secret = data.get("Modal-Secret") or data.get("MODAL_SECRET") or data.get("secret")
if not key or not secret:
    raise SystemExit("token file is missing Modal-Key/Modal-Secret")
print(key)
print(secret)
PY
)"
  MODAL_KEY="$(printf '%s\n' "$parsed" | sed -n '1p')"
  MODAL_SECRET="$(printf '%s\n' "$parsed" | sed -n '2p')"
  export MODAL_KEY MODAL_SECRET
  export MODAL_PROXY_TOKEN_ID="$MODAL_KEY"
  export MODAL_PROXY_TOKEN_SECRET="$MODAL_SECRET"
}

load_modal_proxy_token
export MODAL_ENDPOINT_URL
export DSCO_OPENAI_PARAMS="$PARAMS"
export DSCO_OR_DISABLE_TOOLS=1
export DSCO_OR_MAX_TOOLS=0

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/dsco-modal-params.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

echo "dsco: $DSCO_BIN"
echo "model: $MODEL"
echo "endpoint: $MODAL_ENDPOINT_URL"
echo "params: $DSCO_OPENAI_PARAMS"
echo "tools: disabled for this provider-parameter smoke"
echo

echo "1/2 request-shape debug"
DSCO_DEBUG_REQUEST=1 "$DSCO_BIN" -m "$MODEL" "Reply with exactly OK." >"$tmpdir/out.txt" 2>&1 || {
  sed -n '1,160p' "$tmpdir/out.txt" >&2
  exit 1
}
sed -n '1,120p' "$tmpdir/out.txt"
echo

echo "2/2 verify params were accepted by provider"
if ! grep -q 'OK' "$tmpdir/out.txt"; then
  echo "error: provider did not return expected OK response" >&2
  sed -n '1,160p' "$tmpdir/out.txt" >&2
  exit 1
fi

echo "PASS: Modal accepted DSCO_OPENAI_PARAMS. Inspect ~/.dsco/debug for saved request JSON when DSCO_DEBUG_REQUEST=1 is enabled."
