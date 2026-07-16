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

# ── Compile-time baked-in posture defaults ────────────────────────────────
# The issued binary bakes in --gov-model none + --autonomous. runtime status
# with no flags and a clean env must reflect that default posture.
run_stdout_clean() {
  local err out rc
  err="$(mktemp)"
  set +e
  out="$(env -u DSCO_GOV_MODEL -u DSCO_GOV_BYPASS -u DSCO_TRUST_TIER \
    -u DSCO_APPROVAL_MODE -u DSCO_APPROVAL_NEVER -u DSCO_NO_APPROVAL_PROMPTS \
    DSCO_NO_AUTO_SUPERVISE=1 \
    DSCO_SETUP_NO_AUTO_BOOTSTRAP=1 \
    DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT=1 \
    DSCO_SECURE_STORE_NO_PROMPT=1 \
    "$bin" "$@" 2>"$err")"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    cat "$err" >&2; rm -f "$err"; fail "$bin $* exited $rc"
  fi
  rm -f "$err"
  printf '%s\n' "$out"
}

out="$(run_stdout_clean runtime status --json)"
printf '%s\n' "$out" | grep -q '"governance_model": "none"' ||
  fail "default posture: governance_model is not 'none' (baked-in --gov-model none regressed)"
printf '%s\n' "$out" | grep -q '"DSCO_APPROVAL_MODE": "never"' ||
  fail "default posture: DSCO_APPROVAL_MODE is not 'never' (baked-in --autonomous regressed)"
printf '%s\n' "$out" | grep -q '"DSCO_TRUST_TIER": "trusted"' ||
  fail "default posture: DSCO_TRUST_TIER is not 'trusted' (baked-in --autonomous regressed)"

# Explicit CLI flag must still override the baked-in default.
out="$(run_stdout_clean --gov-model standard runtime status --json)"
printf '%s\n' "$out" | grep -q '"governance_model": "standard"' ||
  fail "override: --gov-model standard did not override baked-in default"

# Pre-set environment variable must still override the baked-in default.
out="$(env DSCO_GOV_MODEL=paranoid DSCO_NO_AUTO_SUPERVISE=1 \
  DSCO_SETUP_NO_AUTO_BOOTSTRAP=1 DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT=1 \
  DSCO_SECURE_STORE_NO_PROMPT=1 "$bin" runtime status --json 2>/dev/null)"
printf '%s\n' "$out" | grep -q '"governance_model": "paranoid"' ||
  fail "override: DSCO_GOV_MODEL=paranoid env did not override baked-in default"

printf 'cli global flag dispatch: ok\n'
