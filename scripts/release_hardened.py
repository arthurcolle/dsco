#!/usr/bin/env python3
"""Create a hardened release artifact with evidence.

The script is intentionally conservative:
- copies the input binary into a private debug artifact;
- copies it again into a public artifact;
- strips the public artifact when a compatible strip tool can do so;
- audits symbols before and after using scripts/elf_symbol_graph_audit.py;
- emits a signed-by-hash manifest and HTML evidence bundle.

It does not mutate the source binary.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import html
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_IN = ROOT / "dsco.aarch64.elf"
DEFAULT_OUT_ROOT = ROOT / "build" / "release_hardened"
AUDIT_SCRIPT = ROOT / "scripts" / "elf_symbol_graph_audit.py"


def run(argv: list[str], timeout: int = 60) -> dict[str, Any]:
    try:
        p = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=timeout)
        return {"argv": argv, "returncode": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
    except Exception as e:
        return {"argv": argv, "returncode": -999, "stdout": "", "stderr": repr(e)}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_audit_module():
    spec = importlib.util.spec_from_file_location("elf_symbol_graph_audit", AUDIT_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {AUDIT_SCRIPT}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["elf_symbol_graph_audit"] = mod
    spec.loader.exec_module(mod)
    return mod


def audit_binary(binary: pathlib.Path, out_html: pathlib.Path, out_json: pathlib.Path) -> dict[str, Any]:
    mod = load_audit_module()
    meta = {"path": str(binary), "bytes": binary.stat().st_size, "sha256": sha256_file(binary), "kind": mod.file_kind(binary)}
    raw_symbols, nm_meta = mod.parse_nm(binary)
    symbols = mod.classify_symbols(raw_symbols)
    meta["nm"] = nm_meta
    summary = mod.summarize(symbols, meta)
    graph = mod.build_graph(symbols, meta)
    metrics = mod.graph_metrics(graph)
    out_html.parent.mkdir(parents=True, exist_ok=True)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_html.write_text(mod.render(summary, graph, metrics))
    out_json.write_text(json.dumps({"summary": summary, "graph": graph, "metrics": metrics}, indent=2, sort_keys=True) + "\n")
    return {"summary": summary, "metrics": metrics, "html": str(out_html), "json": str(out_json)}


def choose_strip_tool() -> str | None:
    candidates = [os.environ.get("STRIP"), "llvm-strip", "aarch64-linux-gnu-strip", "strip"]
    for c in candidates:
        if c and shutil.which(c):
            return shutil.which(c)
    return None


def strip_public_artifact(path: pathlib.Path) -> dict[str, Any]:
    tool = choose_strip_tool()
    if not tool:
        return {"ok": False, "tool": None, "reason": "no strip tool found"}
    attempts = [
        [tool, "--strip-unneeded", str(path)],
        [tool, "-x", str(path)],
        [tool, str(path)],
    ]
    results = []
    for argv in attempts:
        before = sha256_file(path)
        r = run(argv, timeout=60)
        after = sha256_file(path)
        r["sha_before"] = before
        r["sha_after"] = after
        results.append(r)
        if r["returncode"] == 0:
            return {"ok": True, "tool": tool, "argv": argv, "attempts": results}
    return {"ok": False, "tool": tool, "reason": "all strip attempts failed", "attempts": results}


def write_manifest(out_dir: pathlib.Path, manifest: dict[str, Any]) -> pathlib.Path:
    p = out_dir / "release_manifest.json"
    # The manifest signature is a self-contained hash over all fields except manifest_sha256.
    body = dict(manifest)
    body.pop("manifest_sha256", None)
    digest = hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    manifest["manifest_sha256"] = digest
    p.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return p


def render_report(manifest: dict[str, Any], before: dict[str, Any], after: dict[str, Any]) -> str:
    b = before["summary"]
    a = after["summary"]
    def esc(x): return html.escape(str(x))
    rows = [
        ("Bytes", b["binary"]["bytes"], a["binary"]["bytes"], a["binary"]["bytes"] - b["binary"]["bytes"]),
        ("Symbols", b["symbol_count"], a["symbol_count"], a["symbol_count"] - b["symbol_count"]),
        ("Exported-ish", b["exported_like_count"], a["exported_like_count"], a["exported_like_count"] - b["exported_like_count"]),
        ("Debug symbols", b["debug_symbol_count"], a["debug_symbol_count"], a["debug_symbol_count"] - b["debug_symbol_count"]),
        ("Long names", b["long_name_count"], a["long_name_count"], a["long_name_count"] - b["long_name_count"]),
    ]
    diag = "".join(f"<li><b>{esc(d['severity'])}</b>: {esc(d['finding'])}<br><span>{esc(d['action'])}</span></li>" for d in after.get("release_diagnosis", []))
    strip = manifest.get("strip", {})
    return f"""<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>DSCO Hardened Release Evidence</title>
<style>
:root{{color-scheme:dark;--bg:#020617;--panel:#0f172a;--line:#1e293b;--text:#e5e7eb;--muted:#94a3b8;--green:#22c55e;--red:#ef4444;--cyan:#22d3ee}}
body{{margin:0;background:radial-gradient(circle at 10% 0%,rgba(34,211,238,.18),transparent 30%),linear-gradient(180deg,#020617,#030712);color:var(--text);font:14px/1.45 ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}}
main{{max-width:1180px;margin:0 auto;padding:34px 22px 80px}} .card{{background:rgba(15,23,42,.86);border:1px solid var(--line);border-radius:20px;padding:18px;margin:14px 0;box-shadow:0 24px 70px rgba(0,0,0,.32)}}
h1{{font-size:42px;letter-spacing:-.05em;margin:0 0 10px}} .muted{{color:var(--muted)}} code{{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace}} table{{width:100%;border-collapse:collapse;background:#020617;border-radius:14px;overflow:hidden}} th,td{{padding:11px;border-bottom:1px solid var(--line);text-align:left}} th{{background:#111827}} .good{{color:var(--green)}} .bad{{color:var(--red)}} .grid{{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}} .stat{{background:#020617;border:1px solid var(--line);border-radius:14px;padding:13px}} .k{{font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted)}} .v{{font-size:22px;font-weight:900;margin-top:4px}} a{{color:var(--cyan)}}
</style></head><body><main>
<h1>DSCO Hardened Release Evidence</h1><p class='muted'>A release artifact with private debug retention, public stripping attempt, symbol audit before/after, and hash provenance.</p>
<div class='card'><div class='grid'><div class='stat'><div class='k'>Input</div><div class='v'>{esc(pathlib.Path(manifest['input']['path']).name)}</div></div><div class='stat'><div class='k'>Strip</div><div class='v {'good' if strip.get('effective') else 'bad'}'>{'EFFECTIVE' if strip.get('effective') else ('NO CHANGE' if strip.get('ok') else 'FAILED/SKIPPED')}</div></div><div class='stat'><div class='k'>Manifest SHA</div><div class='v'><code>{esc(manifest.get('manifest_sha256','')[:16])}…</code></div></div></div>{f"<p class='bad'>{esc(strip.get('warning'))}</p>" if strip.get('warning') else ''}</div>
<div class='card'><h2>Before / after</h2><table><tr><th>Metric</th><th>Before</th><th>After</th><th>Delta</th></tr>{''.join(f'<tr><td>{esc(k)}</td><td>{esc(v1)}</td><td>{esc(v2)}</td><td>{esc(d)}</td></tr>' for k,v1,v2,d in rows)}</table></div>
<div class='card'><h2>Artifacts</h2><ul><li>Public artifact: <code>{esc(manifest['public']['path'])}</code></li><li>Private debug artifact: <code>{esc(manifest['debug']['path'])}</code></li><li>Manifest: <code>release_manifest.json</code></li><li>Before audit: <a href='audit_before.html'>audit_before.html</a></li><li>After audit: <a href='audit_after.html'>audit_after.html</a></li></ul></div>
<div class='card'><h2>Post-hardening diagnosis</h2><ul>{diag}</ul></div>
<div class='card'><h2>Replay</h2><pre>python3 scripts/release_hardened.py {esc(manifest['input']['path'])}</pre></div>
</main></body></html>"""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Create a hardened release artifact and evidence bundle.")
    ap.add_argument("binary", nargs="?", default=str(DEFAULT_IN), help="input binary; source is not mutated")
    ap.add_argument("--out-root", default=str(DEFAULT_OUT_ROOT))
    ap.add_argument("--name", default=None, help="public artifact basename")
    ap.add_argument("--no-strip", action="store_true", help="copy/audit only; do not strip public artifact")
    args = ap.parse_args(argv)

    src = pathlib.Path(args.binary).resolve()
    if not src.exists():
        raise SystemExit(f"binary not found: {src}")
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")
    out_dir = pathlib.Path(args.out_root) / stamp
    out_dir.mkdir(parents=True, exist_ok=False)
    public_name = args.name or src.name
    public = out_dir / public_name
    debug = out_dir / f"{public_name}.debug"
    shutil.copy2(src, debug)
    shutil.copy2(src, public)

    before = audit_binary(src, out_dir / "audit_before.html", out_dir / "audit_before.json")
    strip_result = {"ok": False, "reason": "--no-strip"} if args.no_strip else strip_public_artifact(public)
    after = audit_binary(public, out_dir / "audit_after.html", out_dir / "audit_after.json")

    input_sha = sha256_file(src)
    public_sha = sha256_file(public)
    before_symbols = before["summary"]["symbol_count"]
    after_symbols = after["summary"]["symbol_count"]
    before_debug_symbols = before["summary"]["debug_symbol_count"]
    after_debug_symbols = after["summary"]["debug_symbol_count"]
    strip_result["effective"] = bool(
        strip_result.get("ok") and (
            public_sha != input_sha or
            after_symbols < before_symbols or
            after_debug_symbols < before_debug_symbols or
            public.stat().st_size < src.stat().st_size
        )
    )
    if strip_result.get("ok") and not strip_result["effective"]:
        strip_result["warning"] = "strip command returned success but artifact hash/size/symbol counts did not improve; tool may not support this binary format"

    manifest = {
        "generated_at_utc": dt.datetime.now(dt.UTC).isoformat(),
        "policy": "release-hardened-v1",
        "input": {"path": str(src), "bytes": src.stat().st_size, "sha256": input_sha},
        "debug": {"path": str(debug), "bytes": debug.stat().st_size, "sha256": sha256_file(debug)},
        "public": {"path": str(public), "bytes": public.stat().st_size, "sha256": public_sha},
        "strip": strip_result,
        "audit_before": {"html": "audit_before.html", "json": "audit_before.json", "symbol_count": before_symbols, "debug_symbol_count": before_debug_symbols},
        "audit_after": {"html": "audit_after.html", "json": "audit_after.json", "symbol_count": after_symbols, "debug_symbol_count": after_debug_symbols},
    }
    manifest_path = write_manifest(out_dir, manifest)
    (out_dir / "REPORT.html").write_text(render_report(manifest, before, after))
    latest = pathlib.Path(args.out_root) / "latest"
    if latest.exists() or latest.is_symlink():
        if latest.is_symlink() or latest.is_file():
            latest.unlink()
        else:
            shutil.rmtree(latest)
    try:
        latest.symlink_to(out_dir.name, target_is_directory=True)
    except OSError:
        shutil.copytree(out_dir, latest)
    print(json.dumps({"out": str(out_dir), "latest": str(latest), "manifest": str(manifest_path), "public": str(public), "debug": str(debug), "strip_ok": strip_result.get("ok"), "symbols_before": before["summary"]["symbol_count"], "symbols_after": after["summary"]["symbol_count"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
