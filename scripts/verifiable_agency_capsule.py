#!/usr/bin/env python3
"""Generate a verifiable agency capsule for the current DSCO workspace.

The capsule is a local, replayable evidence bundle: git provenance, dirty-file
hashes, diff statistics, test-selection hints, and an HTML/Markdown report.
It is intentionally read-only with respect to the repository. All outputs go
under build/agency_capsule/<timestamp>/.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import html
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, asdict
from typing import Iterable

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "agency_capsule"


@dataclass
class CommandResult:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str


@dataclass
class FileEvidence:
    path: str
    status: str
    exists: bool
    bytes: int | None
    sha256: str | None
    additions: int = 0
    deletions: int = 0
    hunks: int = 0
    risk: str = "low"
    suggested_tests: list[str] | None = None


def run(argv: list[str], *, timeout: int = 20, check: bool = False) -> CommandResult:
    p = subprocess.run(
        argv,
        cwd=ROOT,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    r = CommandResult(argv=argv, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr)
    if check and p.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(argv)}\n{p.stderr}")
    return r


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def short_status() -> list[tuple[str, str]]:
    r = run(["git", "status", "--porcelain=v1", "-z"], check=True)
    raw = r.stdout.encode() if isinstance(r.stdout, str) else r.stdout
    # subprocess text=True decodes NULs fine; split on string NUL.
    items = r.stdout.split("\0")
    out: list[tuple[str, str]] = []
    i = 0
    while i < len(items):
        entry = items[i]
        i += 1
        if not entry:
            continue
        status = entry[:2]
        path = entry[3:]
        # Rename/copy porcelain includes a second NUL path; use destination path.
        if status[0] in {"R", "C"} and i < len(items):
            path = items[i]
            i += 1
        out.append((status, path))
    return out


def numstat() -> dict[str, tuple[int, int]]:
    r = run(["git", "diff", "--numstat", "--", "."])
    stats: dict[str, tuple[int, int]] = {}
    for line in r.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3:
            a = 0 if parts[0] == "-" else int(parts[0])
            d = 0 if parts[1] == "-" else int(parts[1])
            stats[parts[2]] = (a, d)
    return stats


def hunk_counts() -> dict[str, int]:
    r = run(["git", "diff", "--unified=0", "--", "."], timeout=30)
    counts: dict[str, int] = {}
    current: str | None = None
    for line in r.stdout.splitlines():
        if line.startswith("diff --git "):
            m = re.match(r"diff --git a/(.*?) b/(.*)", line)
            current = m.group(2) if m else None
        elif current and line.startswith("@@ "):
            counts[current] = counts.get(current, 0) + 1
    return counts


def risk_for(path: str, additions: int, deletions: int, hunks: int) -> str:
    hot = (
        "src/main.c", "src/tools.c", "src/agent.c", "src/provider.c",
        "src/llm.c", "src/tui.c", "include/config.h", "Makefile",
    )
    score = additions + deletions + hunks * 12
    if path in hot:
        score += 120
    if path.startswith("src/") or path.startswith("include/"):
        score += 40
    if path == "Makefile" or path.startswith("web/"):
        score += 35
    if score >= 350:
        return "critical"
    if score >= 160:
        return "high"
    if score >= 60:
        return "medium"
    return "low"


def tests_for(path: str) -> list[str]:
    rules = [
        (("src/stateful_atoms.c", "include/stateful_atoms.h", "tests/test_stateful_atoms.c"), "make test_stateful_atoms"),
        (("src/plan_cache.c", "include/plan_cache.h", "tests/test_plan_cache.c"), "make test_plan_cache"),
        (("src/plan_optimizer.c", "include/plan_optimizer.h", "tests/test_plan_optimizer.c"), "make test_plan_optimizer"),
        (("src/recovery.c", "include/recovery.h", "tests/test_recovery.c"), "make test_recovery"),
        (("src/session_memory.c", "include/session_memory.h", "tests/test_session_memory.c"), "make test_session_memory"),
        (("src/memory_tier.c", "include/memory_tier.h", "tests/test_memory_keep_score.c"), "make test_memory_keep_score"),
        (("src/control_flow.c", "include/control_flow.h", "tests/test_control_flow.c"), "make test_control_flow"),
        (("src/avian.c", "include/avian.h", "tests/test_avian.c"), "make test_avian"),
        (("src/learned_cost.c", "include/learned_cost.h", "tests/test_learned_cost.c"), "make test_learned_cost"),
        (("src/math_fastpath.c", "include/math_fastpath.h", "tests/test_math_corpus.c"), "make test_math_corpus"),
    ]
    out: list[str] = []
    for paths, target in rules:
        if path in paths:
            out.append(target)
    if path in {"src/tui.c", "include/tui.h"} or path.startswith("tests/test_tui_"):
        out.append("make test_tui_snapshots")
    if path in {"Makefile", "src/main.c", "src/tools.c", "src/agent.c", "src/provider.c", "src/llm.c"}:
        out.append("make test")
    if path.startswith("web/") or path.startswith("tests/test_web"):
        out.append("python3 -m pytest tests/test_web_server.py")
    return out or ["make changed-tests"]


def collect_evidence() -> dict:
    status = short_status()
    ns = numstat()
    hc = hunk_counts()
    files: list[FileEvidence] = []
    for st, rel in status:
        p = ROOT / rel
        adds, dels = ns.get(rel, (0, 0))
        hunks = hc.get(rel, 0)
        ev = FileEvidence(
            path=rel,
            status=st,
            exists=p.exists(),
            bytes=p.stat().st_size if p.exists() and p.is_file() else None,
            sha256=sha256_file(p) if p.exists() and p.is_file() else None,
            additions=adds,
            deletions=dels,
            hunks=hunks,
            risk=risk_for(rel, adds, dels, hunks),
            suggested_tests=tests_for(rel),
        )
        files.append(ev)

    branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"]).stdout.strip()
    head = run(["git", "rev-parse", "HEAD"]).stdout.strip()
    head_subject = run(["git", "log", "-1", "--pretty=%s"]).stdout.strip()
    diff_stat = run(["git", "diff", "--stat"]).stdout
    name_only = run(["git", "diff", "--name-only"]).stdout.splitlines()
    untracked = [p for st, p in status if st == "??"]
    tracked_dirty = [p for st, p in status if st != "??"]

    test_matrix: dict[str, list[str]] = {}
    for ev in files:
        for t in ev.suggested_tests or []:
            test_matrix.setdefault(t, []).append(ev.path)

    return {
        "generated_at_utc": _dt.datetime.now(_dt.UTC).isoformat(),
        "root": str(ROOT),
        "branch": branch,
        "head": head,
        "head_subject": head_subject,
        "dirty_count": len(files),
        "tracked_dirty_count": len(tracked_dirty),
        "untracked_count": len(untracked),
        "diff_stat": diff_stat,
        "diff_name_only": name_only,
        "files": [asdict(f) for f in files],
        "test_matrix": test_matrix,
    }


def capsule_digest(evidence: dict) -> str:
    blob = json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest()


def severity_counts(files: Iterable[dict]) -> dict[str, int]:
    out = {"critical": 0, "high": 0, "medium": 0, "low": 0}
    for f in files:
        out[f["risk"]] = out.get(f["risk"], 0) + 1
    return out


def write_markdown(out: pathlib.Path, evidence: dict, digest: str) -> None:
    counts = severity_counts(evidence["files"])
    lines = [
        "# DSCO Verifiable Agency Capsule",
        "",
        f"- Generated UTC: `{evidence['generated_at_utc']}`",
        f"- Repo: `{evidence['root']}`",
        f"- Branch: `{evidence['branch']}`",
        f"- HEAD: `{evidence['head']}` — {evidence['head_subject']}",
        f"- Capsule digest: `{digest}`",
        f"- Dirty files: **{evidence['dirty_count']}** ({evidence['tracked_dirty_count']} tracked, {evidence['untracked_count']} untracked)",
        f"- Risk counts: critical={counts.get('critical',0)}, high={counts.get('high',0)}, medium={counts.get('medium',0)}, low={counts.get('low',0)}",
        "",
        "## Diff stat",
        "",
        "```",
        evidence["diff_stat"].rstrip() or "(no tracked diff)",
        "```",
        "",
        "## Recommended verification commands",
        "",
    ]
    for cmd, paths in sorted(evidence["test_matrix"].items()):
        lines.append(f"- `{cmd}`  ← {len(paths)} file(s)")
    if not evidence["test_matrix"]:
        lines.append("- No dirty files; no verification commands selected.")
    lines += ["", "## File evidence", "", "| Risk | Status | +/- | Hunks | Bytes | SHA-256 | Path |", "|---|---:|---:|---:|---:|---|---|"]
    order = {"critical": 0, "high": 1, "medium": 2, "low": 3}
    for f in sorted(evidence["files"], key=lambda x: (order.get(x["risk"], 9), x["path"])):
        pm = f"+{f['additions']}/-{f['deletions']}"
        sha = f["sha256"][:16] + "…" if f["sha256"] else ""
        lines.append(f"| {f['risk']} | `{f['status']}` | {pm} | {f['hunks']} | {f['bytes'] or ''} | `{sha}` | `{f['path']}` |")
    lines += [
        "",
        "## Replay contract",
        "",
        "1. Re-check HEAD and branch.",
        "2. Compare `capsule.json` digest against the capsule digest above.",
        "3. Verify each dirty-file SHA-256 before trusting conclusions.",
        "4. Run the recommended verification commands relevant to the files you intend to commit.",
    ]
    (out / "REPORT.md").write_text("\n".join(lines) + "\n")


def color_for(risk: str) -> str:
    return {"critical": "#ef4444", "high": "#f97316", "medium": "#eab308", "low": "#22c55e"}.get(risk, "#94a3b8")


def write_html(out: pathlib.Path, evidence: dict, digest: str) -> None:
    counts = severity_counts(evidence["files"])
    rows = []
    order = {"critical": 0, "high": 1, "medium": 2, "low": 3}
    for f in sorted(evidence["files"], key=lambda x: (order.get(x["risk"], 9), x["path"])):
        sha = f["sha256"] or ""
        tests = "<br>".join(html.escape(t) for t in (f.get("suggested_tests") or []))
        rows.append(
            f"<tr><td><span class='pill' style='background:{color_for(f['risk'])}'>{html.escape(f['risk'])}</span></td>"
            f"<td><code>{html.escape(f['status'])}</code></td>"
            f"<td class='num'>+{f['additions']} / -{f['deletions']}</td>"
            f"<td class='num'>{f['hunks']}</td>"
            f"<td class='num'>{f['bytes'] or ''}</td>"
            f"<td><code title='{html.escape(sha)}'>{html.escape(sha[:24])}{'…' if sha else ''}</code></td>"
            f"<td><code>{html.escape(f['path'])}</code></td><td>{tests}</td></tr>"
        )
    tests = []
    for cmd, paths in sorted(evidence["test_matrix"].items()):
        tests.append(f"<li><code>{html.escape(cmd)}</code><span>{len(paths)} file(s)</span></li>")
    html_doc = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DSCO Verifiable Agency Capsule</title>
<style>
:root {{ color-scheme: dark; --bg:#071018; --card:#0f172a; --muted:#94a3b8; --text:#e2e8f0; --line:#1e293b; }}
body {{ margin:0; background:radial-gradient(circle at top left,#172554 0,#071018 45%); color:var(--text); font:14px/1.45 ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }}
main {{ max-width:1180px; margin:0 auto; padding:36px 22px 80px; }}
h1 {{ font-size:34px; margin:0 0 8px; letter-spacing:-.04em; }}
.subtitle {{ color:var(--muted); margin-bottom:24px; }}
.grid {{ display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:12px; margin:22px 0; }}
.card {{ background:rgba(15,23,42,.82); border:1px solid var(--line); border-radius:16px; padding:16px; box-shadow:0 18px 40px rgba(0,0,0,.25); }}
.k {{ color:var(--muted); font-size:12px; text-transform:uppercase; letter-spacing:.08em; }}
.v {{ font-size:22px; font-weight:700; margin-top:4px; overflow:hidden; text-overflow:ellipsis; }}
code, pre {{ font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono",monospace; }}
pre {{ white-space:pre-wrap; background:#020617; border:1px solid var(--line); border-radius:14px; padding:14px; overflow:auto; }}
table {{ width:100%; border-collapse:collapse; background:rgba(15,23,42,.75); border:1px solid var(--line); border-radius:16px; overflow:hidden; }}
th,td {{ border-bottom:1px solid var(--line); padding:10px; vertical-align:top; text-align:left; }}
th {{ color:#cbd5e1; background:#111827; position:sticky; top:0; }}
.num {{ text-align:right; white-space:nowrap; }}
.pill {{ display:inline-block; color:#020617; font-weight:800; border-radius:999px; padding:3px 9px; font-size:12px; text-transform:uppercase; }}
ul.tests {{ padding:0; list-style:none; display:grid; gap:8px; }}
ul.tests li {{ display:flex; justify-content:space-between; gap:12px; background:#020617; border:1px solid var(--line); padding:10px 12px; border-radius:12px; }}
.footer {{ color:var(--muted); margin-top:24px; }}
@media (max-width:800px) {{ .grid {{ grid-template-columns:1fr 1fr; }} table {{ font-size:12px; }} }}
</style></head><body><main>
<h1>DSCO Verifiable Agency Capsule</h1>
<div class="subtitle">A local evidence bundle for replayable, governable agent work.</div>
<div class="grid">
  <div class="card"><div class="k">Branch</div><div class="v">{html.escape(evidence['branch'])}</div></div>
  <div class="card"><div class="k">Dirty files</div><div class="v">{evidence['dirty_count']}</div></div>
  <div class="card"><div class="k">Critical / High</div><div class="v">{counts.get('critical',0)} / {counts.get('high',0)}</div></div>
  <div class="card"><div class="k">Capsule digest</div><div class="v" title="{digest}">{digest[:16]}…</div></div>
</div>
<div class="card"><div class="k">HEAD</div><p><code>{html.escape(evidence['head'])}</code> — {html.escape(evidence['head_subject'])}</p><div class="k">Generated UTC</div><p>{html.escape(evidence['generated_at_utc'])}</p></div>
<h2>Recommended verification</h2><ul class="tests">{''.join(tests) or '<li>No dirty files detected.</li>'}</ul>
<h2>Diff stat</h2><pre>{html.escape(evidence['diff_stat'].rstrip() or '(no tracked diff)')}</pre>
<h2>File evidence</h2>
<table><thead><tr><th>Risk</th><th>Status</th><th>+/-</th><th>Hunks</th><th>Bytes</th><th>SHA-256</th><th>Path</th><th>Suggested tests</th></tr></thead><tbody>{''.join(rows)}</tbody></table>
<p class="footer">Replay contract: verify HEAD, verify capsule digest, verify file hashes, then run selected tests. Generated without modifying tracked source.</p>
</main></body></html>"""
    (out / "REPORT.html").write_text(html_doc)


def verify_capsule(capsule_dir: pathlib.Path) -> int:
    capsule_path = capsule_dir / "capsule.json"
    data = json.loads(capsule_path.read_text())
    expected = data.pop("capsule_digest_sha256", None)
    actual = capsule_digest(data)
    hash_mismatches = []
    missing = []
    for f in data.get("files", []):
        if not f.get("sha256"):
            continue
        p = ROOT / f["path"]
        if not p.exists() or not p.is_file():
            missing.append(f["path"])
            continue
        got = sha256_file(p)
        if got != f["sha256"]:
            hash_mismatches.append({"path": f["path"], "expected": f["sha256"], "actual": got})
    ok = expected == actual and not hash_mismatches and not missing
    print(json.dumps({
        "ok": ok,
        "capsule": str(capsule_dir),
        "digest_expected": expected,
        "digest_actual": actual,
        "missing_files": missing,
        "hash_mismatches": hash_mismatches,
    }, indent=2))
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate or verify a verifiable agency capsule for this workspace.")
    ap.add_argument("--out-root", default=str(DEFAULT_OUT_ROOT), help="directory that will contain timestamped capsules")
    ap.add_argument("--latest", action="store_true", help="also update build/agency_capsule/latest symlink/copy")
    ap.add_argument("--verify", metavar="CAPSULE_DIR", help="verify capsule digest and recorded file hashes")
    args = ap.parse_args(argv)

    if args.verify:
        return verify_capsule(pathlib.Path(args.verify))

    stamp = _dt.datetime.now(_dt.UTC).strftime("%Y%m%dT%H%M%SZ")
    out_root = pathlib.Path(args.out_root)
    out = out_root / stamp
    out.mkdir(parents=True, exist_ok=False)

    evidence = collect_evidence()
    digest = capsule_digest(evidence)
    evidence["capsule_digest_sha256"] = digest

    (out / "capsule.json").write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    write_markdown(out, evidence, digest)
    write_html(out, evidence, digest)

    latest = out_root / "latest"
    if args.latest:
        if latest.exists() or latest.is_symlink():
            if latest.is_symlink() or latest.is_file():
                latest.unlink()
            else:
                shutil.rmtree(latest)
        try:
            latest.symlink_to(out.name, target_is_directory=True)
        except OSError:
            shutil.copytree(out, latest)

    print(json.dumps({"capsule": str(out), "digest": digest, "dirty_files": evidence["dirty_count"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
