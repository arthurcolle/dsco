#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSCO_BIN="${DSCO_BIN:-}"
MODAL_ENDPOINT_URL="${MODAL_ENDPOINT_URL:-https://arthurcolle--ep-agent-server.us-west.modal.direct}"
MODAL_MODEL="${MODAL_MODEL:-zai-org/GLM-5.2-FP8}"
TOKEN_FILE="${MODAL_PROXY_TOKEN_FILE:-/tmp/modal_proxy_token.json}"
TOOLS_API_TIMEOUT="${TOOLS_API_TIMEOUT:-20}"

if [[ -z "$DSCO_BIN" ]]; then
  if [[ -x "$ROOT/dsco" ]]; then
    DSCO_BIN="$ROOT/dsco"
  else
    DSCO_BIN="$(command -v dsco)"
  fi
fi

if [[ ! -x "$DSCO_BIN" ]]; then
  echo "error: dsco binary not executable: $DSCO_BIN" >&2
  exit 1
fi

if [[ ! -r "$TOKEN_FILE" ]]; then
  echo "error: Modal proxy token file not readable: $TOKEN_FILE" >&2
  exit 1
fi

read_modal_creds() {
  python3 - "$TOKEN_FILE" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], "r", encoding="utf-8"))
key = data.get("Modal-Key") or data.get("MODAL_KEY") or data.get("modal_key")
secret = data.get("Modal-Secret") or data.get("MODAL_SECRET") or data.get("modal_secret")
if not key or not secret:
    raise SystemExit("missing Modal-Key/Modal-Secret in token file")
print(key)
print(secret)
PY
}

mapfile -t MODAL_CREDS < <(read_modal_creds)
MODAL_KEY_VALUE="${MODAL_CREDS[0]}"
MODAL_SECRET_VALUE="${MODAL_CREDS[1]}"

echo "== dsco provisioned infrastructure qualification =="
echo "dsco:     $DSCO_BIN"
echo "modal:    $MODAL_ENDPOINT_URL"
echo "model:    modal/$MODAL_MODEL"
echo "token:    $TOKEN_FILE (loaded, values hidden)"
echo

echo "== tools.distributed.systems via dsco =="
if [[ -z "${TOOLS_API_TOKEN:-${AUTH_TOKEN:-}}" ]]; then
  echo "tools: SKIP (set TOOLS_API_TOKEN or AUTH_TOKEN)"
  tools_ok=skip
else
  TOOLS_API_TIMEOUT="$TOOLS_API_TIMEOUT" "$DSCO_BIN" tools list --limit 5
  TOOLS_API_TIMEOUT="$TOOLS_API_TIMEOUT" "$DSCO_BIN" tools run add a=19 b=23
  TOOLS_API_TIMEOUT="$TOOLS_API_TIMEOUT" "$DSCO_BIN" tools run multiply a=42 b=10
  tools_ok=yes
fi
echo

echo "== Modal endpoint raw HTTP =="
modal_raw_status="$(
  python3 - "$MODAL_ENDPOINT_URL" "$MODAL_KEY_VALUE" "$MODAL_SECRET_VALUE" "$MODAL_MODEL" <<'PY'
import json
import sys
import urllib.error
import urllib.request

base, key, secret, model = sys.argv[1:5]
headers = {"Modal-Key": key, "Modal-Secret": secret}
statuses = []

def record(label, code, body=""):
    statuses.append(code)
    body = (body or "").replace("\n", " ")[:240]
    print(f"{label}: HTTP {code} {body}")

try:
    req = urllib.request.Request(base.rstrip("/") + "/v1/models", headers=headers, method="GET")
    with urllib.request.urlopen(req, timeout=30) as r:
        record("/v1/models", r.status, r.read(500).decode("utf-8", "replace"))
except urllib.error.HTTPError as e:
    record("/v1/models", e.code, e.read(500).decode("utf-8", "replace"))

payload = {
    "model": model,
    "messages": [{"role": "user", "content": "Reply with exactly OK."}],
    "max_tokens": 8,
    "temperature": 0,
    "stream": False,
}
try:
    req = urllib.request.Request(
        base.rstrip("/") + "/v1/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={**headers, "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        body = r.read(2000).decode("utf-8", "replace")
        record("/v1/chat/completions", r.status, body)
except urllib.error.HTTPError as e:
    record("/v1/chat/completions", e.code, e.read(500).decode("utf-8", "replace"))

print("RAW_OK=" + ("yes" if any(200 <= s < 300 for s in statuses) else "no"))
PY
)"
echo "$modal_raw_status"
modal_raw_ok="$(printf '%s\n' "$modal_raw_status" | sed -n 's/^RAW_OK=//p' | tail -1)"
echo

echo "== Modal route through dsco =="
MODAL_ENDPOINT_URL="$MODAL_ENDPOINT_URL" \
MODAL_KEY="$MODAL_KEY_VALUE" \
MODAL_SECRET="$MODAL_SECRET_VALUE" \
MODAL_PROXY_TOKEN_ID="$MODAL_KEY_VALUE" \
MODAL_PROXY_TOKEN_SECRET="$MODAL_SECRET_VALUE" \
"$DSCO_BIN" --route-explain "modal/$MODAL_MODEL"
echo

echo "== Modal generation through dsco =="
set +e
modal_dsco_output="$(
  MODAL_ENDPOINT_URL="$MODAL_ENDPOINT_URL" \
  MODAL_KEY="$MODAL_KEY_VALUE" \
  MODAL_SECRET="$MODAL_SECRET_VALUE" \
  MODAL_PROXY_TOKEN_ID="$MODAL_KEY_VALUE" \
  MODAL_PROXY_TOKEN_SECRET="$MODAL_SECRET_VALUE" \
  DSCO_OR_DISABLE_TOOLS=1 \
  DSCO_OR_MAX_TOOLS=0 \
  DSCO_MAX_TOKENS=128 \
  "$DSCO_BIN" -m "modal/$MODAL_MODEL" "Reply with exactly OK." 2>&1
)"
modal_dsco_rc=$?
set -e
printf '%s\n' "$modal_dsco_output"
echo "dsco_modal_exit=$modal_dsco_rc"
echo

echo "== summary =="
echo "tools_distributed_systems=$tools_ok"
echo "modal_endpoint_raw=$modal_raw_ok"
echo "modal_dsco_generation=$([[ "$modal_dsco_rc" == "0" ]] && echo yes || echo no)"

if [[ "$modal_raw_ok" != "yes" || "$modal_dsco_rc" != "0" ]]; then
  echo "result=degraded"
  exit 2
fi

echo "result=ok"
