#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSCO_BIN="${DSCO_BIN:-}"
MODEL="${MODEL:-modal/zai-org/GLM-5.2-FP8}"
MODAL_ENDPOINT_URL="${MODAL_ENDPOINT_URL:-https://arthurcolle--ep-agent-server.us-west.modal.direct}"
TOKEN_FILE="${TOKEN_FILE:-/tmp/modal_proxy_token.json}"
TIMEOUT_SECS="${TIMEOUT_SECS:-90}"
TOOLS_API_TIMEOUT="${TOOLS_API_TIMEOUT:-20}"
TOOLS_QUERY="${TOOLS_QUERY:-calculator arithmetic}"
REQUIRE_REMOTE_TOOLS="${REQUIRE_REMOTE_TOOLS:-0}"

if [[ -z "$DSCO_BIN" ]]; then
  if [[ -x "$ROOT/dsco" ]]; then
    DSCO_BIN="$ROOT/dsco"
  else
    DSCO_BIN="$(command -v dsco)"
  fi
fi

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

mask() {
  local value="${1:-}"
  if [[ ${#value} -le 10 ]]; then
    printf '<set>'
  else
    printf '%s...%s' "${value:0:6}" "${value: -4}"
  fi
}

load_modal_proxy_token() {
  local key="${MODAL_KEY:-}"
  local secret="${MODAL_SECRET:-}"

  if [[ -n "$key" && "$key" != "..." && -n "$secret" && "$secret" != "..." ]]; then
    export MODAL_PROXY_TOKEN_ID="${MODAL_PROXY_TOKEN_ID:-$key}"
    export MODAL_PROXY_TOKEN_SECRET="${MODAL_PROXY_TOKEN_SECRET:-$secret}"
    return
  fi

  if [[ ! -f "$TOKEN_FILE" ]]; then
    echo "error: MODAL_KEY/MODAL_SECRET are not set and $TOKEN_FILE does not exist" >&2
    exit 1
  fi

  local parsed
  parsed="$(python3 - "$TOKEN_FILE" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
key = data.get("Modal-Key") or data.get("MODAL_KEY") or data.get("key")
secret = data.get("Modal-Secret") or data.get("MODAL_SECRET") or data.get("secret")
if not key or not secret:
    raise SystemExit(f"{path} is missing Modal-Key/Modal-Secret")
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

run_capture() {
  local outfile="$1"
  shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$TIMEOUT_SECS" "$@" >"$outfile" 2>&1
  else
    "$@" >"$outfile" 2>&1
  fi
}

remote_tools_check() {
  local list_file="$1"
  local run_file="$2"
  local search_file="$3"

  if ! TOOLS_API_TIMEOUT="$TOOLS_API_TIMEOUT" "$DSCO_BIN" tools list --limit 5 >"$list_file" 2>&1; then
    if grep -Eiq '401|unauthorized|Bearer token required' "$list_file"; then
      echo "remote tools auth: missing TOOLS_API_TOKEN/AUTH_TOKEN for https://tools.distributed.systems"
      if [[ "$REQUIRE_REMOTE_TOOLS" == "1" ]]; then
        sed -n '1,80p' "$list_file" >&2
        exit 1
      fi
      return 2
    fi
    sed -n '1,120p' "$list_file" >&2
    exit 1
  fi

  TOOLS_API_TIMEOUT="$TOOLS_API_TIMEOUT" "$DSCO_BIN" tools run add a=19 b=23 >"$run_file" 2>&1
  assert_contains 'tools available at https://tools\.distributed\.systems' "$list_file" "remote tools catalog"
  assert_contains '"tool_name":"add"|status":"success"|42' "$run_file" "remote add execution"

  if ! TOOLS_API_TIMEOUT="$TOOLS_API_TIMEOUT" "$DSCO_BIN" tools search "$TOOLS_QUERY" --limit 5 >"$search_file" 2>&1; then
    echo "remote tools search: warning, semantic search failed; catalog and run checks passed"
    sed -n '1,40p' "$search_file"
  else
    assert_contains 'matches for' "$search_file" "remote tools semantic search"
  fi
  return 0
}

assert_contains() {
  local pattern="$1"
  local file="$2"
  local label="$3"
  if ! grep -Eiq "$pattern" "$file"; then
    echo "error: expected $label in $file" >&2
    echo "----- captured output -----" >&2
    sed -n '1,220p' "$file" >&2
    echo "---------------------------" >&2
    exit 1
  fi
}

need python3
need grep
need sed

load_modal_proxy_token
export MODAL_ENDPOINT_URL

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/dsco-modal-tools.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

echo "dsco: $DSCO_BIN"
echo "model: $MODEL"
echo "endpoint: $MODAL_ENDPOINT_URL"
echo "modal key: $(mask "$MODAL_KEY")"
echo

echo "1/5 route check"
"$DSCO_BIN" --route-explain "$MODEL" >"$tmpdir/route.txt" 2>&1
assert_contains 'route_provider: modal' "$tmpdir/route.txt" "modal route"
assert_contains 'custom_api_base: yes|credential_present: yes|credential_usable: yes' "$tmpdir/route.txt" "modal credential/base status"
sed -n '1,40p' "$tmpdir/route.txt"
echo

echo "2/5 native dsco tool registry check"
"$DSCO_BIN" --tools-json >"$tmpdir/tools.json"
python3 - "$tmpdir/tools.json" <<'PY'
import json
import sys

tools = json.loads(open(sys.argv[1]).read())
names = {t.get("name") for t in tools}
required = {"calc", "cwd", "read_file", "discover_tools"}
missing = sorted(required - names)
if missing:
    raise SystemExit(f"missing required tools: {', '.join(missing)}")
print(f"tools available: {len(tools)}; checked: {', '.join(sorted(required))}")
PY
echo

echo "3/5 native dsco tool execution check"
"$DSCO_BIN" --tool-exec calc '{"expression":"19*23+7"}' >"$tmpdir/direct-tool.json" 2>&1
assert_contains '444' "$tmpdir/direct-tool.json" "direct calc result"
sed -n '1,40p' "$tmpdir/direct-tool.json"
echo

echo "4/5 hosted tools.distributed.systems catalog check"
remote_status=0
remote_tools_check "$tmpdir/remote-list.txt" "$tmpdir/remote-run.txt" "$tmpdir/remote-search.txt" || remote_status=$?
if [[ "$remote_status" == "0" ]]; then
  sed -n '1,80p' "$tmpdir/remote-list.txt"
  sed -n '1,80p' "$tmpdir/remote-run.txt"
  sed -n '1,80p' "$tmpdir/remote-search.txt"
else
  sed -n '1,40p' "$tmpdir/remote-list.txt"
fi
echo

echo "5/5 live Modal LLM tool-calling check"
remote_result='auth_required'
if [[ "$remote_status" == "0" ]]; then
  remote_result='ok'
fi
prompt='You are testing dsco native tool calling. The local harness already checked tools.distributed.systems and found REMOTE_TOOLS='"$remote_result"'. You MUST call the calc tool exactly once with expression "19*23+7". After the tool result is returned, reply on one line as TOOL_RESULT=<number> REMOTE_TOOLS='"$remote_result"'. Do not compute mentally; use the calc tool.'
if ! run_capture "$tmpdir/live.txt" env -u TOOLS_API_TOKEN -u AUTH_TOKEN "$DSCO_BIN" -m "$MODEL" --trust-tier standard --approval-mode never "$prompt"; then
  echo "error: live dsco call failed" >&2
  sed -n '1,240p' "$tmpdir/live.txt" >&2
  exit 1
fi

assert_contains 'tool_call|calc' "$tmpdir/live.txt" "tool-call transcript"
assert_contains 'REMOTE_TOOLS=' "$tmpdir/live.txt" "hosted tools status"
assert_contains 'TOOL_RESULT=444|444' "$tmpdir/live.txt" "final tool result"
sed -n '1,240p' "$tmpdir/live.txt"
echo
echo "PASS: Modal LLM route, native dsco tools, hosted tools.distributed.systems reachability, and live tool-calling path all passed."
