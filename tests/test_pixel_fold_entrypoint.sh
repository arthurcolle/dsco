#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENTRY="$ROOT/scripts/pixel-fold/dsco-device"
DOCTOR="$ROOT/scripts/pixel-fold/doctor"
INSTALLER="$ROOT/scripts/pixel-fold/install-termux"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for script in "$ENTRY" "$DOCTOR" "$INSTALLER"; do
  bash -n "$script"
done

mkdir -p "$TMP/root" "$TMP/bin"
cat >"$TMP/root/dsco" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$DSCO_DEVICE|$DSCO_DEVICE_LAYOUT|$DSCO_PROFILE|$DSCO_ALLOW_NET|$DSCO_ALLOW_RUN|$DSCO_ALLOW_SECRETS|$DSCO_ALLOW_CONTROL|$*"
EOF
chmod +x "$TMP/root/dsco"
cat >"$TMP/bin/tput" <<'EOF'
#!/usr/bin/env bash
case "$1" in cols) printf '%s\n' "${TEST_COLS:-80}" ;; lines) printf '%s\n' "${TEST_ROWS:-24}" ;; esac
EOF
chmod +x "$TMP/bin/tput"

run_case() {
  expected="$1" cols="$2"
  output="$(HOME="$TMP/home" PATH="$TMP/bin:$PATH" DSCO_ROOT="$TMP/root" TEST_COLS="$cols" TEST_ROWS=30 bash "$ENTRY" --version)"
  case "$output" in
    "pixel-fold|$expected|lite|0|0|0|0|--version") ;;
    *) printf 'unexpected entrypoint output: %s\n' "$output" >&2; exit 1 ;;
  esac
  grep -q "DSCO_DEVICE_LAYOUT=$expected" "$TMP/home/.dsco/device/pixel-fold.env"
}

run_case compact 60
run_case medium 90
run_case expanded 140

output="$(HOME="$TMP/home" PATH="$TMP/bin:$PATH" DSCO_ROOT="$TMP/root" TEST_COLS=90 DSCO_ALLOW_NET=1 bash "$ENTRY" hello)"
case "$output" in 'pixel-fold|medium|lite|1|0|0|0|hello') ;; *) exit 1 ;; esac

printf 'pixel fold entrypoint tests passed\n'
