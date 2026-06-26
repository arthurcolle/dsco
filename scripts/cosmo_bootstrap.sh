#!/usr/bin/env bash
set -euo pipefail

# Fetch and pin Cosmopolitan's cosmocc toolchain for DSCO portable builds.
# The archive is intentionally kept under build/ (gitignored) while this script
# and the Makefile integration are tracked. This keeps the repository light and
# reproducible without vendoring a ~440MB compiler zip.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${DSCO_COSMOCC_VERSION:-${COSMOCC_VERSION:-4.0.2}}"
URL="${DSCO_COSMOCC_URL:-https://github.com/jart/cosmopolitan/releases/download/${VERSION}/cosmocc-${VERSION}.zip}"
EXPECTED_SHA256="${DSCO_COSMOCC_SHA256:-${COSMOCC_SHA256:-85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44}}"
CACHE_DIR="${DSCO_COSMO_CACHE_DIR:-$ROOT/build/cache}"
TOOLS_DIR="${DSCO_COSMO_TOOLS_DIR:-$ROOT/build/cosmocc}"
ZIP="$CACHE_DIR/cosmocc-${VERSION}.zip"
DEST="$TOOLS_DIR/${VERSION}"
CURRENT="$TOOLS_DIR/current"

mkdir -p "$CACHE_DIR" "$TOOLS_DIR"

need_download=0
if [[ ! -s "$ZIP" ]]; then
  need_download=1
elif [[ "${DSCO_COSMO_FORCE_DOWNLOAD:-0}" == "1" ]]; then
  need_download=1
fi

if [[ "$need_download" == "1" ]]; then
  echo "[cosmo] downloading cosmocc ${VERSION}" >&2
  echo "[cosmo] url: $URL" >&2
  tmp="$ZIP.tmp.$$"
  rm -f "$tmp"
  curl -L --fail --retry 3 --retry-delay 2 -o "$tmp" "$URL"
  mv "$tmp" "$ZIP"
fi

if [[ -n "$EXPECTED_SHA256" && "${DSCO_COSMO_VERIFY:-1}" != "0" ]]; then
  actual="$(shasum -a 256 "$ZIP" | awk '{print $1}')"
  if [[ "$actual" != "$EXPECTED_SHA256" ]]; then
    cat >&2 <<EOF
[cosmo] SHA256 mismatch for $ZIP
[cosmo] expected: $EXPECTED_SHA256
[cosmo] actual:   $actual
[cosmo] Refusing to continue. If intentionally upgrading cosmocc, set
[cosmo] DSCO_COSMOCC_SHA256=$actual after independently verifying the release.
EOF
    exit 1
  fi
  echo "$actual  $ZIP" > "$ZIP.sha256"
fi

if [[ ! -x "$DEST/bin/cosmocc" || "${DSCO_COSMO_FORCE_UNPACK:-0}" == "1" ]]; then
  echo "[cosmo] unpacking $ZIP -> $DEST" >&2
  rm -rf "$DEST.tmp" "$DEST"
  mkdir -p "$DEST.tmp"
  unzip -q "$ZIP" -d "$DEST.tmp"
  mv "$DEST.tmp" "$DEST"
fi

ln -sfn "$DEST" "$CURRENT"
chmod +x "$DEST/bin/cosmocc" 2>/dev/null || true

cat <<EOF
$DEST/bin/cosmocc
EOF
