#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSCO_BIN="${DSCO_BIN:-$ROOT/dsco}"
TOKEN_FILE="${MODAL_PROXY_TOKEN_FILE:-/tmp/modal_proxy_token.json}"
ENDPOINT_URL="${MODAL_ENDPOINT_URL:-https://arthurcolle--ep-agent-server.us-west.modal.direct}"
MODEL="${MODAL_MODEL:-zai-org/GLM-5.2-FP8}"
PROMPT="${1:-Reply with exactly OK and no other text.}"

if [[ ! -x "$DSCO_BIN" ]]; then
  echo "error: dsco binary not executable: $DSCO_BIN" >&2
  echo "hint: run 'make dsco' or set DSCO_BIN=/path/to/dsco" >&2
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

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

key = data.get("Modal-Key") or data.get("MODAL_KEY") or data.get("modal_key")
secret = data.get("Modal-Secret") or data.get("MODAL_SECRET") or data.get("modal_secret")
if not key or not secret:
    raise SystemExit("missing Modal-Key/Modal-Secret in token file")

print(key)
print(secret)
PY
}

mapfile -t CREDS < <(read_modal_creds)
MODAL_KEY_VALUE="${CREDS[0]}"
MODAL_SECRET_VALUE="${CREDS[1]}"

echo "== Modal endpoint =="
echo "endpoint: $ENDPOINT_URL"
echo "model:    $MODEL"
echo "token:    $TOKEN_FILE (loaded, values hidden)"
echo

echo "== Probe /v1/models =="
python3 - "$ENDPOINT_URL" "$MODAL_KEY_VALUE" "$MODAL_SECRET_VALUE" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

base, key, secret = sys.argv[1:4]
req = urllib.request.Request(
    base.rstrip("/") + "/v1/models",
    headers={"Modal-Key": key, "Modal-Secret": secret},
    method="GET",
)
t0 = time.time()
try:
    with urllib.request.urlopen(req, timeout=30) as r:
        body = r.read(4000).decode("utf-8", "replace")
        print(f"status={r.status} latency_ms={int((time.time() - t0) * 1000)}")
        obj = json.loads(body)
        ids = [m.get("id") for m in obj.get("data", []) if m.get("id")]
        print("models=" + ", ".join(ids))
except urllib.error.HTTPError as e:
    body = e.read(1000).decode("utf-8", "replace")
    print(f"error: HTTP {e.code}: {body}", file=sys.stderr)
    raise SystemExit(1)
PY
echo

echo "== Direct OpenAI-compatible chat smoke =="
python3 - "$ENDPOINT_URL" "$MODAL_KEY_VALUE" "$MODAL_SECRET_VALUE" "$MODEL" "$PROMPT" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

base, key, secret, model, prompt = sys.argv[1:6]
payload = {
    "model": model,
    "messages": [{"role": "user", "content": prompt}],
    "max_tokens": 128,
    "temperature": 0,
}
req = urllib.request.Request(
    base.rstrip("/") + "/v1/chat/completions",
    data=json.dumps(payload).encode("utf-8"),
    headers={
        "Content-Type": "application/json",
        "Modal-Key": key,
        "Modal-Secret": secret,
    },
    method="POST",
)
t0 = time.time()
try:
    with urllib.request.urlopen(req, timeout=180) as r:
        body = r.read(20000).decode("utf-8", "replace")
        obj = json.loads(body)
        choice = (obj.get("choices") or [{}])[0]
        msg = choice.get("message") or {}
        content = msg.get("content") or ""
        reasoning = msg.get("reasoning_content") or ""
        print(f"status={r.status} latency_ms={int((time.time() - t0) * 1000)} finish_reason={choice.get('finish_reason')}")
        print(f"content={content!r}")
        print(f"reasoning_chars={len(reasoning)} usage={obj.get('usage')}")
except urllib.error.HTTPError as e:
    body = e.read(2000).decode("utf-8", "replace")
    print(f"error: HTTP {e.code}: {body}", file=sys.stderr)
    raise SystemExit(1)
PY
echo

echo "== dsco route explain =="
MODAL_ENDPOINT_URL="$ENDPOINT_URL" \
MODAL_KEY="$MODAL_KEY_VALUE" \
MODAL_SECRET="$MODAL_SECRET_VALUE" \
DSCO_SECURE_STORE_NO_PROMPT=1 \
"$DSCO_BIN" --route-explain "modal/$MODEL"
echo

echo "== dsco live LLM smoke =="
MODAL_ENDPOINT_URL="$ENDPOINT_URL" \
MODAL_KEY="$MODAL_KEY_VALUE" \
MODAL_SECRET="$MODAL_SECRET_VALUE" \
DSCO_SECURE_STORE_NO_PROMPT=1 \
DSCO_NO_SUPERVISE=1 \
DSCO_NO_AUTO_SUPERVISE=1 \
DSCO_MAX_TOKENS="${DSCO_MAX_TOKENS:-128}" \
"$DSCO_BIN" -m "modal/$MODEL" "$PROMPT"
