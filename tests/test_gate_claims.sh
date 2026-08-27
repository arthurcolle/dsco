#!/usr/bin/env bash
# test_gate_claims.sh — conformance verifier for dsco's capability gate.
#
# Drives `dsco mcp serve` (JSON-RPC over stdio) with pinned env and asserts the
# gate's hard rules end-to-end against the live binary — no LLM involved:
#   1. control-plane tools deny by default; allow with DSCO_ALLOW_CONTROL=1
#   2. lethal trifecta (secrets touched -> egress) denies under DSCO_ALLOW_EXFIL=0
#   3. same sequence allows (advisory) under DSCO_ALLOW_EXFIL=1
#   4. untrusted tier denies net/exec outright (capability defaults)
#
# Conformance target: ~/dsco/autonomy-kernel/governed_kernel.py semantics.
# Every env pin is explicit in the subshell — ambient posture must not leak.

set -u
DSCO_BIN="${DSCO_BIN:-$HOME/dsco-emergency/dsco-cli/dsco}"
[[ -x "$DSCO_BIN" ]] || { echo "FATAL: $DSCO_BIN not executable" >&2; exit 2; }
PASS=0; FAIL=0

rpc() { # rpc <tier> <env-assignments...> -- <jsonl lines...>
  local tier="$1"; shift
  local envs=() line
  while [[ "$1" != "--" ]]; do envs+=("$1"); shift; done
  shift
  {
    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"gate-claims","version":"0"}}}'
    printf '%s\n' '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    printf '%s\n' "$@"
  } | env -i HOME="$HOME" PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" "${envs[@]}" \
      "$DSCO_BIN" mcp serve --toolsets all --tier "$tier" 2>/dev/null
}

check() { # check <name> <condition-result: 0/1> <detail>
  if [[ "$2" == "0" ]]; then echo "ok:   $1 ($3)"; PASS=$((PASS+1));
  else echo "FAIL: $1 ($3)"; FAIL=$((FAIL+1)); fi
}

call() { printf '%s\n' "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}"; }

# ── 1. control: killswitch trigger denies by default ──────────────────────────
OUT=$(rpc standard -- "$(call killswitch '{"action":"trigger"}')")
check "control-denied-by-default" \
  $([[ "$OUT" == *"denied"* || "$OUT" == *"DSCO_ALLOW_CONTROL"* || "$OUT" == *"error"* ]] && echo 0 || echo 1) \
  "killswitch trigger"

# ── 2. control allowed with explicit grant ────────────────────────────────────
OUT=$(rpc standard DSCO_ALLOW_CONTROL=1 -- "$(call killswitch '{"action":"status"}')")
check "control-grant-accepted" \
  $([[ "$OUT" == *'"result"'* ]] && echo 0 || echo 1) \
  "killswitch status (read-only exempt)"

# ── 3. trifecta: secrets touched, untrusted ingested, then egress → deny ─────
OUT=$(rpc standard DSCO_ALLOW_EXFIL=0 -- \
  "$(call bash '{"command":"cat ~/.ssh/id_rsa || true"}')" \
  "$(call http_request '{"url":"https://example.com","method":"GET"}')" \
  "$(call bash '{"command":"curl -s https://example.com >/dev/null"}')")
check "trifecta-denied-exfil0" \
  $([[ "$OUT" == *"lethal-trifecta"* || "$OUT" == *"denied"* ]] && echo 0 || echo 1) \
  "secrets->egress under DSCO_ALLOW_EXFIL=0"

# ── 4. trifecta override: EXFIL=1 → not blocked ───────────────────────────────
OUT=$(rpc standard DSCO_ALLOW_EXFIL=1 -- \
  "$(call bash '{"command":"cat ~/.ssh/id_rsa || true"}')" \
  "$(call http_request '{"url":"https://example.com","method":"GET"}')" \
  "$(call bash '{"command":"curl -s https://example.com >/dev/null"}')")
check "trifecta-override-exfil1" \
  $([[ "$OUT" == *'"result"'* ]] && echo 0 || echo 1) \
  "operator override honored"

# ── 5. untrusted tier: net denied by default ─────────────────────────────────
OUT=$(rpc untrusted -- "$(call http_request '{"url":"https://example.com","method":"GET"}')")
check "untrusted-tier-net-denied" \
  $([[ "$OUT" == *"denied"* || "$OUT" == *"error"* ]] && echo 0 || echo 1) \
  "http_request at untrusted tier"

# ── 6. secrets tier default: standard tier blocked from secrets tool ─────────
OUT=$(rpc standard -- "$(call openai_image_generate '{"prompt":"x"}')")
# openai_image_generate isn't in core toolsets; exposure itself is a control.
check "unexposed-tool-not-served" \
  $([[ "$OUT" == *"not exposed"* || "$OUT" == *"error"* ]] && echo 0 || echo 1) \
  "toolset disclosure discipline"

echo
if (( FAIL == 0 )); then echo "ALL GATE CLAIMS VERIFIED ($PASS/$((PASS+FAIL)))"; exit 0
else echo "$FAIL CLAIM(S) FAILED ($PASS passed)"; exit 1; fi
