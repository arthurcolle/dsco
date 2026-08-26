#!/usr/bin/env python3
"""secret_scan.py — credential-leak detector shared by repo scan and bake_data gate.

Usage:
  python3 scripts/secret_scan.py [paths...]        # scan; exit 1 on hit
  python3 -c 'import secret_scan'                  # use SUSPICIOUS_RE elsewhere

Heuristics only: pattern-shaped tokens (key prefixes, long high-entropy base64
assignments). Verified credentials never live in git; anything this flags that is
a known-safe fixture should be added to ALLOWLIST below with a justification.
"""
import os, re, sys

SECRET_PATTERNS = [
    r"sk-(?:ant|proj|svcacct)-[A-Za-z0-9_\-]{20,}",   # anthropic-style API keys
    r"\bsk-[A-Za-z0-9]{30,}",                          # generic sk-...
    r"gh[pousr]_[A-Za-z0-9]{20,}",
    r"github_pat_[A-Za-z0-9_]{20,}",
    r"AKIA[0-9A-Z]{16}",
    r"xox[bpars]-[A-Za-z0-9\-]{10,}",
    r"AIza[0-9A-Za-z_\-]{30,}",
    r"glpat-[A-Za-z0-9_\-]{15,}",
    r"(api[_-]?key|apikey|secret|token|password)\s*[:=]\s*[\"\047][A-Za-z0-9+/_\-]{24,}[\"\047]",
]
SUSPICIOUS_RE = re.compile("|".join(f"(?:{p})" for p in SECRET_PATTERNS), re.IGNORECASE)

# Known-safe false positives (substring match on file path or the matched token).
ALLOWLIST = {
    "sk-ant-oat01-session",  # tests/: deliberate fake fixture prefix
}

SKIP_DIRS = {".git", "node_modules", ".venv", ".test-venv", ".web-venv",
             "ic-procurement-reading-list", "__pycache__"}
BINARY_EXTS = {".png", ".jpg", ".o", ".a", ".dylib", ".so", ".npy", ".bin", ".woff2"}

def scan_path(root):
    hits = []
    if os.path.isfile(root):
        walk = [(os.path.dirname(root) or ".", [os.path.basename(root)])]
    else:
        walk = []
        for dp, dns, fns in os.walk(root):
            dns[:] = [d for d in dns if d not in SKIP_DIRS]
            walk.append((dp, fns))
    for dp, fns in walk:
        for fn in fns:
            p = os.path.join(dp, fn)
            if os.path.splitext(fn)[1].lower() in BINARY_EXTS:
                continue
            try:
                if os.path.getsize(p) > 64 * 1024 * 1024:
                    continue
                txt = open(p, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            if "\x00" in txt[:4096]:
                continue
            for m in SUSPICIOUS_RE.finditer(txt):
                tok = m.group(0)
                if any(a in tok for a in ALLOWLIST):
                    continue
                line = txt.count("\n", 0, m.start()) + 1
                hits.append((p, line, tok[:18] + "…"))
    return hits

def main(argv):
    roots = argv[1:] or ["."]
    hits = []
    for r in roots:
        hits += scan_path(r)
    for p, ln, tok in hits:
        print(f"{p}:{ln}: {tok}")
    print(f"\n{len(hits)} suspicious hit(s)" + (" — FAIL" if hits else " — clean"))
    return 1 if hits else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
