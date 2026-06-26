#!/usr/bin/env bash
# Publish `dsco` as a Homebrew formula in a personal tap so that
#   brew install arthurcolle/dsco/dsco       (or, after `brew tap`, just `brew install dsco`)
# builds dsco from a tagged source release.
#
# Prereqs: `gh auth login` (working GitHub auth) and a clean dsco repo at a
# released commit. Re-runnable: bumps the tag if VERSION changes.
set -euo pipefail

GH_USER="${GH_USER:-arthurcolle}"
DSCO_REPO_DIR="${DSCO_REPO_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
TAP_NAME="${TAP_NAME:-homebrew-dsco}"          # -> tap is arthurcolle/dsco
VERSION="$(sed -nE 's/^#define DSCO_VERSION "([^"]+)".*/\1/p' "$DSCO_REPO_DIR/include/config.h")"
TAG="v${VERSION}"
TARBALL_NAME="dsco-${VERSION}.tar.gz"
TARBALL_URL="https://github.com/${GH_USER}/dsco/releases/download/${TAG}/${TARBALL_NAME}"
WORK="$(mktemp -d)"

echo ">> dsco ${VERSION} -> tag ${TAG} -> tap ${GH_USER}/${TAP_NAME#homebrew-}"

command -v gh  >/dev/null || { echo "gh CLI required"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "run: gh auth login"; exit 1; }

# 1. Tag the source release (idempotent) and push it.
git -C "$DSCO_REPO_DIR" rev-parse "$TAG" >/dev/null 2>&1 \
  || git -C "$DSCO_REPO_DIR" tag -a "$TAG" -m "dsco ${VERSION}"
git -C "$DSCO_REPO_DIR" push origin "$TAG"

# 2. Create/upload the deterministic source tarball consumed by Homebrew.
git -C "$DSCO_REPO_DIR" archive --format=tar.gz --prefix="dsco-${VERSION}/" \
  -o "$WORK/${TARBALL_NAME}" "$TAG"
SHA="$(shasum -a 256 "$WORK/${TARBALL_NAME}" | awk '{print $1}')"
echo ">> sha256 ${SHA}"

gh release view "$TAG" -R "${GH_USER}/dsco" >/dev/null 2>&1 \
  || gh release create "$TAG" -R "${GH_USER}/dsco" -t "dsco ${VERSION}" -n "dsco ${VERSION}"

if ! gh release view "$TAG" -R "${GH_USER}/dsco" --json assets --jq '.assets[].name' | grep -Fx "$TARBALL_NAME" >/dev/null; then
  gh release upload "$TAG" "$WORK/${TARBALL_NAME}" -R "${GH_USER}/dsco"
fi

# 3. Render the formula with the real URL and sha256.
FORMULA_SRC="$DSCO_REPO_DIR/Formula/dsco.rb"
mkdir -p "$WORK/${TAP_NAME}/Formula"
sed -E \
  -e "s|^[[:space:]]*url \".*\"|  url \"${TARBALL_URL}\"|" \
  -e "s|^[[:space:]]*sha256 \".*\"|  sha256 \"${SHA}\"|" \
  "$FORMULA_SRC" \
  > "$WORK/${TAP_NAME}/Formula/dsco.rb"

# 4. Create the tap repo if needed and push the formula.
if ! gh repo view "${GH_USER}/${TAP_NAME}" >/dev/null 2>&1; then
  gh repo create "${GH_USER}/${TAP_NAME}" --public \
    -d "Homebrew tap for dsco and friends"
fi
cd "$WORK/${TAP_NAME}"
git init -q
git checkout -q -b main
git add Formula/dsco.rb
git -c user.name=dsco -c user.email=agent@distributed.systems \
    commit -q -m "dsco ${VERSION} (new formula)"
git remote add origin "git@github.com:${GH_USER}/${TAP_NAME}.git"
git push -fu origin main

cat <<EOF

Done. Users can now run:

    brew tap ${GH_USER}/${TAP_NAME#homebrew-}
    brew install dsco

  or in one shot:

    brew install ${GH_USER}/${TAP_NAME#homebrew-}/dsco

  Audit before announcing:

    brew audit --strict --online ${GH_USER}/${TAP_NAME#homebrew-}/dsco
    brew test  ${GH_USER}/${TAP_NAME#homebrew-}/dsco
EOF
