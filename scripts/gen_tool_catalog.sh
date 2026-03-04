#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/tools.c"
OUT="$ROOT/docs/TOOL_CATALOG.md"
TMP="$(mktemp)"
CHECK=0

if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

{
  echo "# Built-in Tool Catalog"
  echo
  echo "This catalog is generated from \`tools.c\` tool registrations (\`.name\` / \`.description\` pairs)."
  echo
  echo "- Source: \`tools.c\`"

  awk '
  /\.name = "/ {
    name=$0
    sub(/.*\.name = "/, "", name)
    sub(/".*/, "", name)
    pending=name
    next
  }
  /\.description = "/ {
    if (pending != "") {
      desc=$0
      sub(/.*\.description = "/, "", desc)
      sub(/".*/, "", desc)
      pairs[pending]=desc
      pending=""
    }
  }
  END {
    n=0
    for (k in pairs) {
      names[++n]=k
    }

    # stable lexical sort
    for (i=1; i<=n; i++) {
      for (j=i+1; j<=n; j++) {
        if (names[i] > names[j]) {
          t=names[i]
          names[i]=names[j]
          names[j]=t
        }
      }
    }

    printf("- Total built-in tools: %d\n", n)
    printf("\n")
    printf("Regeneration:\n\n")
    printf("```bash\n")
    printf("./scripts/gen_tool_catalog.sh\n")
    printf("```\n\n")
    printf("| Tool | Description |\n")
    printf("|---|---|\n")

    for (i=1; i<=n; i++) {
      k=names[i]
      v=pairs[k]
      gsub(/\|/, "\\\\|", k)
      gsub(/\|/, "\\\\|", v)
      printf("| <code>%s</code> | %s |\n", k, v)
    }
  }
  ' "$SRC"
} > "$TMP"

if [[ $CHECK -eq 1 ]]; then
  if ! cmp -s "$TMP" "$OUT"; then
    echo "docs drift: $OUT is out of date. Run ./scripts/gen_tool_catalog.sh" >&2
    rm -f "$TMP"
    exit 1
  fi
  rm -f "$TMP"
  exit 0
fi

mv "$TMP" "$OUT"
echo "wrote $OUT"
