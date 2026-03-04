#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/API_REFERENCE.md"
TMP="$(mktemp)"
CHECK=0

if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

mapfile -t HEADERS < <(cd "$ROOT" && ls *.h 2>/dev/null | sort)

{
  echo "# API Reference"
  echo
  echo "This file is auto-generated from header declarations in the repository root."
  echo
  echo "- Generator: \`./scripts/gen_api_reference.sh\`"
  echo "- Headers scanned: ${#HEADERS[@]}"
  echo
  echo "## Regeneration"
  echo
  echo "\`\`\`bash"
  echo "./scripts/gen_api_reference.sh"
  echo "\`\`\`"

  for h in "${HEADERS[@]}"; do
    path="$ROOT/$h"
    echo
    echo "## \`$h\`"
    echo

    # Count function-like declarations for a quick summary.
    count=$(awk '
      BEGIN { inblock=0; buf=""; c=0 }
      {
        line=$0
        sub(/\/\/.*$/, "", line)
        if (inblock) {
          if (line ~ /\*\//) { sub(/^.*\*\//, "", line); inblock=0 } else next
        }
        while (line ~ /\/\*/) {
          pre=line; sub(/\/\*.*/, "", pre)
          if (line ~ /\*\//) { post=line; sub(/^.*\*\//, "", post); line=pre " " post }
          else { line=pre; inblock=1; break }
        }
        if (line ~ /^[[:space:]]*#/ || line ~ /^[[:space:]]*$/) next
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
        if (line == "") next
        buf = (buf == "" ? line : buf " " line)
        if (line ~ /;[[:space:]]*$/) {
          s=buf
          gsub(/[[:space:]]+/, " ", s)
          if (s ~ /\(.*\)[[:space:]]*;/ && s !~ /^typedef[[:space:]]+struct/ && s !~ /^typedef[[:space:]]+enum/ && s !~ /^typedef[[:space:]]+union/) c++
          buf=""
        }
      }
      END { print c }
    ' "$path")

    echo "Function-like declarations: $count"
    echo
    echo "### Declarations"
    echo

    awk '
      BEGIN { inblock=0; buf="" }
      {
        line=$0
        sub(/\/\/.*$/, "", line)
        if (inblock) {
          if (line ~ /\*\//) { sub(/^.*\*\//, "", line); inblock=0 } else next
        }
        while (line ~ /\/\*/) {
          pre=line; sub(/\/\*.*/, "", pre)
          if (line ~ /\*\//) { post=line; sub(/^.*\*\//, "", post); line=pre " " post }
          else { line=pre; inblock=1; break }
        }
        if (line ~ /^[[:space:]]*#/ || line ~ /^[[:space:]]*$/) next
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
        if (line == "") next
        buf = (buf == "" ? line : buf " " line)
        if (line ~ /;[[:space:]]*$/) {
          s=buf
          gsub(/[[:space:]]+/, " ", s)
          if (s ~ /\(.*\)[[:space:]]*;/) {
            print "- `" s "`"
          }
          buf=""
        }
      }
    ' "$path"
  done
} > "$TMP"

if [[ $CHECK -eq 1 ]]; then
  if ! cmp -s "$TMP" "$OUT"; then
    echo "docs drift: $OUT is out of date. Run ./scripts/gen_api_reference.sh" >&2
    rm -f "$TMP"
    exit 1
  fi
  rm -f "$TMP"
  exit 0
fi

mv "$TMP" "$OUT"
echo "wrote $OUT"
