#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<USAGE
Usage: $0 <new-version> [release-date]

Examples:
  $0 0.8.0
  $0 0.8.0 2026-03-02
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage >&2
  exit 1
fi

NEW_VERSION="$1"
RELEASE_DATE="${2:-$(date +%Y-%m-%d)}"
SEMVER_RE='^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$'

if [[ ! "$NEW_VERSION" =~ $SEMVER_RE ]]; then
  echo "error: '$NEW_VERSION' is not semver-like" >&2
  exit 1
fi

if [[ ! "$RELEASE_DATE" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  echo "error: release-date must be YYYY-MM-DD" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f config.h || ! -f CHANGELOG.md ]]; then
  echo "error: run this script from a dsco-cli checkout" >&2
  exit 1
fi

TMP_FILE="$(mktemp)"
awk -v version="$NEW_VERSION" '
BEGIN { updated = 0 }
{
  if ($0 ~ /^#define[[:space:]]+DSCO_VERSION[[:space:]]+"/) {
    print "#define DSCO_VERSION \"" version "\""
    updated = 1
  } else {
    print
  }
}
END {
  if (!updated) {
    exit 2
  }
}
' config.h > "$TMP_FILE"
mv "$TMP_FILE" config.h

if grep -Eq "^## \[$NEW_VERSION\]([[:space:]]|$)" CHANGELOG.md; then
  echo "changelog section [$NEW_VERSION] already exists"
else
  TMP_FILE="$(mktemp)"
  awk -v version="$NEW_VERSION" -v date="$RELEASE_DATE" '
  function print_release_block() {
    print "## [" version "] - " date
    print ""
    print "### Added"
    print ""
    print "- N/A"
    print ""
    print "### Changed"
    print ""
    print "- N/A"
    print ""
    print "### Fixed"
    print ""
    print "- N/A"
    print ""
  }

  BEGIN { inserted = 0 }

  /^## Historical Notes/ {
    if (!inserted) {
      print_release_block()
      inserted = 1
    }
  }

  { print }

  END {
    if (!inserted) {
      print ""
      print_release_block()
    }
  }
  ' CHANGELOG.md > "$TMP_FILE"
  mv "$TMP_FILE" CHANGELOG.md
fi

./scripts/check_version_consistency.sh

echo "version bumped to $NEW_VERSION"
echo "next: update [Unreleased] entries, then run make lint && make test"
