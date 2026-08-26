#!/usr/bin/env bash
set -euo pipefail

bin="${1:-${DSCO_BIN:-./dsco}}"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

run_stdout() {
  local err out rc
  err="$(mktemp)"
  set +e
  out="$(env \
    DSCO_NO_AUTO_SUPERVISE=1 \
    DSCO_SETUP_NO_AUTO_BOOTSTRAP=1 \
    DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT=1 \
    DSCO_SECURE_STORE_NO_PROMPT=1 \
    "$bin" "$@" 2>"$err")"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    cat "$err" >&2
    rm -f "$err"
    fail "$bin $* exited $rc"
  fi
  if grep -q "unknown option" "$err"; then
    cat "$err" >&2
    rm -f "$err"
    fail "$bin $* reported unknown option"
  fi
  rm -f "$err"
  printf '%s\n' "$out"
}

assert_json_array() {
  case "$1" in
    \[*\]) ;;
    *) fail "$2 did not return a JSON array: $1" ;;
  esac
}

out="$(run_stdout agents list --json)"
assert_json_array "$out" "agents list --json"

out="$(run_stdout --systems-agent agents list --json)"
assert_json_array "$out" "--systems-agent agents list --json"

out="$(run_stdout --approval-mode never --trust-tier trusted agents list --json)"
assert_json_array "$out" "--approval-mode/--trust-tier agents list --json"

out="$(run_stdout --sandbox-mode workspace-write agents list --json)"
assert_json_array "$out" "--sandbox-mode workspace-write agents list --json"

out="$(run_stdout --sandbox-mode agents list --json)"
assert_json_array "$out" "--sandbox-mode agents list --json"

out="$(run_stdout --systems-agent agents --help)"
printf '%s\n' "$out" | grep -q "agents tui" ||
  fail "--systems-agent agents --help did not reach agents usage"

out="$(run_stdout --systems-agent runtime status --json)"
printf '%s\n' "$out" | grep -q '"systems_agent": true' ||
  fail "--systems-agent runtime status --json did not apply systems-agent mode"

out="$(run_stdout --sandbox-mode workspace-write runtime status --json)"
printf '%s\n' "$out" | grep -q '"DSCO_TRUST_TIER": "untrusted"' ||
  fail "--sandbox-mode workspace-write did not set untrusted tier"

out="$(run_stdout mcp --help)"
printf '%s\n' "$out" | grep -q "mcp serve" ||
  fail "mcp --help did not reach MCP-specific usage"

runs_dir="$(mktemp -d)"
DSCO_RUNS_DIR="$runs_dir" run_stdout runs list >/dev/null
if find "$runs_dir" -name journal.wal -print -quit | grep -q .; then
  rm -rf "$runs_dir"
  fail "runs list created a Chronicle journal while inspecting runs"
fi
rm -rf "$runs_dir"

runs_dir="$(mktemp -d)/not-created"
DSCO_RUNS_DIR="$runs_dir" run_stdout runs list >/dev/null
[ ! -e "$runs_dir" ] || fail "runs list created a missing runs directory"
rm -rf "${runs_dir%/not-created}"

out="$(run_stdout runs --help)"
printf '%s\n' "$out" | grep -q "Chronicle run journals" ||
  fail "runs --help did not reach runs-specific usage"

printf 'cli global flag dispatch: ok\n'
