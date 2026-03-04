#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f config.h ]]; then
  echo "error: config.h not found" >&2
  exit 1
fi

if [[ ! -f CHANGELOG.md ]]; then
  echo "error: CHANGELOG.md not found" >&2
  exit 1
fi

DSCO_VERSION="$(sed -nE 's/^#define[[:space:]]+DSCO_VERSION[[:space:]]+"([^"]+)".*/\1/p' config.h | head -n1)"
if [[ -z "$DSCO_VERSION" ]]; then
  echo "error: unable to parse DSCO_VERSION from config.h" >&2
  exit 1
fi

SEMVER_RE='^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$'
if [[ ! "$DSCO_VERSION" =~ $SEMVER_RE ]]; then
  echo "error: DSCO_VERSION '$DSCO_VERSION' is not semver-like" >&2
  exit 1
fi

if ! grep -Eq '^## \[Unreleased\]' CHANGELOG.md; then
  echo "error: CHANGELOG.md must contain an [Unreleased] section" >&2
  exit 1
fi

if ! grep -Eq "^## \[$DSCO_VERSION\]([[:space:]]|$)" CHANGELOG.md; then
  echo "error: CHANGELOG.md is missing section for DSCO_VERSION [$DSCO_VERSION]" >&2
  exit 1
fi

echo "version consistency check passed: DSCO_VERSION=$DSCO_VERSION"
