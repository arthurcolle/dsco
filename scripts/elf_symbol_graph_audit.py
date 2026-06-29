#!/usr/bin/env python3
"""Generate a standalone ELF/Mach-O symbol exposure graph UI.

This is a local reverse-engineering surface for release hygiene: it fingerprints a
binary, extracts symbols with nm when available, classifies exposure, computes
small graph-theory metrics, and emits a pretty standalone HTML report.
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import html
import json
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "dsco.aarch64.elf"
DEFAULT_OUT = ROOT / "web" / "static" / "elf_symbols.html"
MAX_SYMBOL_NODES = 420

TYPE_NAMES = {
    "T": "text/global-code", "t": "text/local-code",
    "D": "data/global", "d": "data/local",
    "B": "bss/global", "b": "bss/local",
    "R": "rodata/global", "r": "rodata/local",
    "W": "weak", "w": "weak/local", "U": "undefined",
    "N": "debug", "A": "absolute", "V": "weak-object", "v": "weak-object/local",
    "S": "small-bss", "s": "small-data", "C": "common",
}
TYPE_COLORS = {
    "T": "#38bdf8", "t": "#0ea5e9", "D": "#f97316", "d": "#fb923c",
    "B": "#a78bfa", "b": "#8b5cf6", "R": "#22c55e", "r": "#16a34a",
    "U": "#ef4444", "N": "#64748b", "W": "#eab308", "w": "#ca8a04",
}

SECRET_PATTERNS = [
    ("key", re.compile(r"key|secret|token|passwd|password|credential|auth", re.I)),
    ("crypto", re.compile(r"crypto|cipher|nonce|sign|verify|hmac|sha|aes|chacha|sodium|hkdf", re.I)),
    ("network", re.compile(r"http|tls|ssl|socket|mesh|peer|net|server|client|bridge", re.I)),
    ("agent", re.compile(r"agent|swarm|tool|orchestr|plan|memory|provider|llm|talons|avian", re.I)),
    ("debug", re.compile(r"debug|trace|assert|test|log|dump", re.I)),
]


def run(argv: list[str], timeout: int = 20) -> dict[str, Any]:
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


def file_kind(path: pathlib.Path) -> str:
    file_bin = shutil.which("file")
    if file_bin:
        r = run([file_bin, str(path)], timeout=10)
        if r["returncode"] == 0:
            return r["stdout"].strip()
    head = path.read_bytes()[:8]
    if head.startswith(b"\x7fELF"):
        return "ELF binary"
    if head[:4] in {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"}:
        return "Mach-O binary"
    return "unknown binary"


def parse_nm(path: pathlib.Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    nm = shutil.which("nm")
    if not nm:
        return [], {"error": "nm not found"}
    r = run([nm, "-a", str(path)], timeout=40)
    symbols: list[dict[str, Any]] = []
    # BSD nm formats commonly: "000000080001df98 T CPU_COUNT" or "                 U _foo"
    line_re = re.compile(r"^(?:(?P<addr>[0-9A-Fa-f]+)|\s+)\s+(?P<typ>[A-Za-z?])\s+(?P<name>.+?)\s*$")
    for line in r["stdout"].splitlines():
        m = line_re.match(line)
        if not m:
            continue
        name = m.group("name").strip()
        typ = m.group("typ")
        addr = m.group("addr") or ""
        if not name:
            continue
        symbols.append({
            "address": addr,
            "type": typ,
            "type_name": TYPE_NAMES.get(typ, "other"),
            "name": name,
            "length": len(name),
        })
    return symbols, {"tool": nm, "returncode": r["returncode"], "stderr": r["stderr"][-2000:], "stdout_lines": len(r["stdout"].splitlines())}


def tokenise_symbol(name: str) -> list[str]:
    s = re.sub(r"[^A-Za-z0-9_]+", "_", name)
    parts = []
    for p in s.split("_"):
        if not p:
            continue
        # split camel-ish fragments lightly
        bits = re.findall(r"[A-Z]?[a-z]+|[A-Z]+(?=[A-Z]|$)|\d+", p)
        parts.extend(bits or [p])
    return [p.lower() for p in parts if len(p) >= 2]


def module_guess(name: str) -> str:
    toks = tokenise_symbol(name)
    if not toks:
        return "misc"
    known = ["dsco", "agent", "tool", "memory", "provider", "llm", "tui", "mesh", "crypto", "json", "plan", "swarm", "avian", "talons", "http", "tls", "sqlite", "curl", "cosmo", "pthread", "malloc", "arena", "vecstore", "mcp", "net", "audit", "trace"]
    for k in known:
        if k in toks or any(t.startswith(k) for t in toks):
            return k
    if name.startswith("__"):
        return "runtime"
    if name.startswith("_"):
        return "external"
    return toks[0]


def exposure_score(sym: dict[str, Any]) -> float:
    name = sym["name"]
    typ = sym["type"]
    score = 0.0
    if typ.isupper():
        score += 2.0
    if typ in {"T", "D", "B", "R", "W", "V"}:
        score += 2.0
    if typ == "N":
        score += 0.4
    score += min(4.0, len(name) / 20.0)
    toks = set(tokenise_symbol(name))
    score += min(3.0, len(toks) / 3.0)
    for _, pat in SECRET_PATTERNS:
        if pat.search(name):
            score += 2.0
    if any(x in name.lower() for x in ["password", "secret", "token", "credential", "private"]):
        score += 4.0
    return round(score, 3)


def exposure_class(score: float) -> str:
    if score >= 9:
        return "critical"
    if score >= 6:
        return "high"
    if score >= 3:
        return "medium"
    return "low"


def classify_symbols(symbols: list[dict[str, Any]]) -> list[dict[str, Any]]:
    out = []
    for s in symbols:
        ss = dict(s)
        ss["module"] = module_guess(ss["name"])
        ss["tokens"] = tokenise_symbol(ss["name"])[:12]
        ss["score"] = exposure_score(ss)
        ss["risk"] = exposure_class(ss["score"])
        ss["tags"] = [tag for tag, pat in SECRET_PATTERNS if pat.search(ss["name"])]
        out.append(ss)
    return out


def add_node(nodes: dict[str, dict], node_id: str, label: str, kind: str, **attrs):
    if node_id not in nodes:
        nodes[node_id] = {"id": node_id, "label": label, "kind": kind, **attrs}
    else:
        nodes[node_id].update(attrs)


def add_edge(edges: list[dict], source: str, target: str, relation: str, weight: float = 1.0, **attrs):
    edges.append({"source": source, "target": target, "relation": relation, "weight": weight, **attrs})


def build_graph(symbols: list[dict[str, Any]], binary_meta: dict[str, Any]) -> dict[str, Any]:
    nodes: dict[str, dict] = {}
    edges: list[dict] = []
    bin_attrs = dict(binary_meta)
    bin_attrs["file_kind"] = bin_attrs.pop("kind", "")
    add_node(nodes, "binary", pathlib.Path(binary_meta["path"]).name, "binary", layer=0, color="#22d3ee", **bin_attrs)
    add_node(nodes, "section:symtab", ".symtab / symbol names", "section", layer=1, color="#a78bfa")
    add_node(nodes, "section:strtab", ".strtab / name strings", "section", layer=1, color="#8b5cf6")
    add_node(nodes, "ship:strip", "strip --strip-unneeded", "hardening", layer=5, color="#22c55e")
    add_node(nodes, "ship:visibility", "-fvisibility=hidden / version script", "hardening", layer=5, color="#14b8a6")
    add_node(nodes, "ship:debuglink", "split debug file", "hardening", layer=5, color="#0ea5e9")
    add_edge(edges, "binary", "section:symtab", "contains", 4)
    add_edge(edges, "section:symtab", "section:strtab", "names_in", 3)
    add_edge(edges, "section:symtab", "ship:strip", "removed_by", 3)
    add_edge(edges, "section:strtab", "ship:strip", "names_removed_by", 3)
    add_edge(edges, "binary", "ship:debuglink", "can_split", 2)
    add_edge(edges, "binary", "ship:visibility", "release_policy", 2)

    ranked = sorted(symbols, key=lambda s: (s["score"], len(s["name"])), reverse=True)[:MAX_SYMBOL_NODES]
    modules = collections.Counter(s["module"] for s in symbols)
    types = collections.Counter(s["type"] for s in symbols)
    risks = collections.Counter(s["risk"] for s in symbols)
    for m, count in modules.most_common(60):
        add_node(nodes, f"module:{m}", m, "module", layer=2, count=count, color="#64748b")
        add_edge(edges, "section:symtab", f"module:{m}", "clusters", min(8, 1 + math.log1p(count)))
    for t, count in types.most_common():
        add_node(nodes, f"type:{t}", f"{t} · {TYPE_NAMES.get(t, 'other')}", "symbol_type", layer=2, count=count, color=TYPE_COLORS.get(t, "#94a3b8"))
        add_edge(edges, "section:symtab", f"type:{t}", "has_type", min(8, 1 + math.log1p(count)))
    for r, count in risks.items():
        color = {"critical":"#ef4444","high":"#f97316","medium":"#eab308","low":"#22c55e"}.get(r,"#94a3b8")
        add_node(nodes, f"risk:{r}", f"{r} exposure", "risk", layer=4, count=count, color=color)
        add_edge(edges, f"risk:{r}", "ship:strip", "mitigated_by", 2)

    for s in ranked:
        sid = "sym:" + hashlib.sha1((s["type"] + "\0" + s["name"]).encode()).hexdigest()[:14]
        add_node(nodes, sid, s["name"], "symbol", layer=3, color=TYPE_COLORS.get(s["type"], "#94a3b8"), **s)
        add_edge(edges, f"module:{s['module']}", sid, "defines", max(1, s["score"] / 2), risk=s["risk"])
        add_edge(edges, f"type:{s['type']}", sid, "typed_as", 1.5)
        add_edge(edges, sid, f"risk:{s['risk']}", "exposes", max(1, s["score"] / 2), risk=s["risk"])
        for tag in s.get("tags", [])[:3]:
            tid = f"tag:{tag}"
            add_node(nodes, tid, tag, "tag", layer=4, color="#f43f5e")
            add_edge(edges, sid, tid, "matches", 2.5, risk=s["risk"])
            add_edge(edges, tid, "ship:visibility", "policy_input", 1.5)
    return {"nodes": list(nodes.values()), "edges": edges}


def graph_metrics(graph: dict[str, Any]) -> dict[str, Any]:
    ids = [n["id"] for n in graph["nodes"]]
    adj: dict[str, set[str]] = {i: set() for i in ids}
    weighted = collections.Counter()
    out = collections.defaultdict(list)
    for e in graph["edges"]:
        a, b = e["source"], e["target"]
        adj.setdefault(a, set()).add(b); adj.setdefault(b, set()).add(a)
        weighted[a] += e.get("weight", 1); weighted[b] += e.get("weight", 1)
        out[a].append(b)
    # pagerank
    n = max(1, len(ids)); pr = {i: 1/n for i in ids}
    for _ in range(35):
        new = {i: 0.15/n for i in ids}
        sink = sum(pr[i] for i in ids if not out.get(i))
        for i in ids: new[i] += 0.85 * sink / n
        for u, vs in out.items():
            for v in vs: new[v] += 0.85 * pr.get(u, 0) / max(1, len(vs))
        pr = new
    # cheap closeness via BFS from each node (small enough)
    closeness = {}
    for s in ids:
        q = collections.deque([s]); dist = {s: 0}
        while q:
            u = q.popleft()
            for v in adj.get(u, ()):
                if v not in dist:
                    dist[v] = dist[u] + 1; q.append(v)
        closeness[s] = (len(dist)-1) / sum(dist.values()) if sum(dist.values()) else 0
    by_id = {n["id"]: n for n in graph["nodes"]}
    for node in graph["nodes"]:
        i = node["id"]
        node["degree"] = len(adj.get(i, ()))
        node["weighted_degree"] = round(weighted[i], 3)
        node["pagerank"] = round(pr[i], 6)
        node["closeness"] = round(closeness[i], 6)
    top = lambda k: sorted(graph["nodes"], key=lambda x: x.get(k, 0), reverse=True)[:12]
    return {
        "nodes": len(ids),
        "edges": len(graph["edges"]),
        "density": round(len(graph["edges"]) / max(1, len(ids) * (len(ids)-1)), 6),
        "top_degree": [{"label": x["label"], "kind": x["kind"], "value": x["degree"]} for x in top("degree")],
        "top_pagerank": [{"label": x["label"], "kind": x["kind"], "value": x["pagerank"]} for x in top("pagerank")],
        "top_closeness": [{"label": x["label"], "kind": x["kind"], "value": x["closeness"]} for x in top("closeness")],
    }


def summarize(symbols: list[dict[str, Any]], binary_meta: dict[str, Any]) -> dict[str, Any]:
    type_counts = collections.Counter(s["type"] for s in symbols)
    risk_counts = collections.Counter(s["risk"] for s in symbols)
    module_counts = collections.Counter(s["module"] for s in symbols)
    tag_counts = collections.Counter(tag for s in symbols for tag in s.get("tags", []))
    exported = sum(1 for s in symbols if s["type"].isupper() and s["type"] not in {"N", "U"})
    debug = type_counts.get("N", 0)
    long_names = sum(1 for s in symbols if len(s["name"]) >= 24)
    return {
        "generated_at_utc": dt.datetime.now(dt.UTC).isoformat(),
        "binary": binary_meta,
        "symbol_count": len(symbols),
        "exported_like_count": exported,
        "debug_symbol_count": debug,
        "long_name_count": long_names,
        "type_counts": dict(type_counts.most_common()),
        "risk_counts": {k: risk_counts.get(k, 0) for k in ["critical", "high", "medium", "low"]},
        "module_counts": dict(module_counts.most_common(30)),
        "tag_counts": dict(tag_counts.most_common()),
        "top_exposed": sorted(symbols, key=lambda s: s["score"], reverse=True)[:80],
        "release_diagnosis": release_diagnosis(binary_meta, symbols, type_counts),
    }


def release_diagnosis(binary_meta: dict[str, Any], symbols: list[dict[str, Any]], type_counts: collections.Counter) -> list[dict[str, str]]:
    kind = binary_meta.get("kind", "").lower()
    out = []
    if "not stripped" in kind or type_counts.get("N", 0) > 0:
        out.append({"severity": "critical", "finding": "Binary appears unstripped or contains debug symbols.", "action": "For release: split debug info, then strip --strip-unneeded / release-strip equivalent."})
    if symbols:
        out.append({"severity": "high", "finding": f"{len(symbols)} names are recoverable through nm/symbol metadata.", "action": "Do not ship development symbol tables in public artifacts unless intentionally exposing an ABI."})
    if any(s.get("tags") for s in symbols):
        out.append({"severity": "medium", "finding": "Some symbol names reveal security, crypto, network, or agent internals.", "action": "Treat names as intelligence leakage; visibility controls reduce exported surface but obfuscation is separate."})
    if "dynamically linked" in kind:
        out.append({"severity": "medium", "finding": "Dynamic symbol table must remain for imported/exported runtime linkage.", "action": "For shared libraries, hide non-API with -fvisibility=hidden and linker version scripts."})
    else:
        out.append({"severity": "info", "finding": "Fully linked executable can usually lose .symtab entirely and still run.", "action": "Keep a private debug artifact for crash forensics; ship stripped runtime artifact."})
    return out


def render(summary: dict[str, Any], graph: dict[str, Any], metrics: dict[str, Any]) -> str:
    payload = json.dumps({"summary": summary, "graph": graph, "metrics": metrics}, separators=(",", ":"))
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DSCO Binary Symbol Exposure Graph</title>
<style>
:root{{--bg:#020617;--panel:#08111f;--panel2:#0f172a;--line:#1e293b;--text:#e5e7eb;--muted:#94a3b8;--cyan:#22d3ee;--red:#ef4444;--orange:#f97316;--yellow:#eab308;--green:#22c55e;--purple:#a78bfa}}
*{{box-sizing:border-box}} body{{margin:0;background:radial-gradient(circle at 12% 0%,rgba(239,68,68,.17),transparent 28%),radial-gradient(circle at 88% 0%,rgba(34,211,238,.18),transparent 31%),linear-gradient(180deg,#020617,#030712);color:var(--text);font:14px/1.45 ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}} code{{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace}} .shell{{max-width:1540px;margin:0 auto;padding:28px}} .hero{{display:grid;grid-template-columns:1.25fr .75fr;gap:18px}} .card{{background:linear-gradient(180deg,rgba(15,23,42,.9),rgba(8,17,31,.88));border:1px solid rgba(148,163,184,.18);border-radius:22px;box-shadow:0 24px 80px rgba(0,0,0,.35);overflow:hidden}} .pad{{padding:20px}} .eyebrow{{color:var(--cyan);font-size:12px;letter-spacing:.16em;text-transform:uppercase;font-weight:900}} h1{{font-size:48px;line-height:.96;letter-spacing:-.06em;margin:10px 0 14px}} h2{{margin:0 0 12px;font-size:18px}} .lead{{font-size:16px;color:#cbd5e1;max-width:900px}} .digest{{background:#020617;border:1px solid var(--line);border-radius:14px;padding:12px;margin-top:14px;overflow:hidden;text-overflow:ellipsis}} .stats{{display:grid;grid-template-columns:repeat(5,1fr);gap:10px;margin-top:18px}} .stat{{background:#020617;border:1px solid var(--line);border-radius:16px;padding:13px}} .k{{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.1em}} .v{{font-size:24px;font-weight:950;margin-top:4px}} .tabs{{display:flex;gap:10px;margin:20px 0;flex-wrap:wrap}} .tab{{border:1px solid var(--line);background:rgba(15,23,42,.8);color:#cbd5e1;padding:10px 14px;border-radius:999px;cursor:pointer}} .tab.active{{border-color:rgba(34,211,238,.7);color:white;box-shadow:0 0 0 3px rgba(34,211,238,.1) inset}} .view{{display:none}} .view.active{{display:block}} .layout{{display:grid;grid-template-columns:1fr 390px;gap:18px}} #graph{{width:100%;height:760px;display:block;background:radial-gradient(circle at 50% 45%,rgba(167,139,250,.08),transparent 38%),#020617;border-radius:22px}} .side{{display:grid;gap:14px;align-content:start}} input,select,button{{font:inherit}} input,select{{background:#020617;border:1px solid var(--line);color:var(--text);border-radius:12px;padding:10px;outline:none}} .controls{{display:flex;gap:8px;flex-wrap:wrap}} .btn{{background:linear-gradient(180deg,#164e63,#0e7490);border:1px solid rgba(34,211,238,.35);color:white;padding:9px 12px;border-radius:12px;cursor:pointer}} .nodeList{{display:grid;gap:8px;max-height:315px;overflow:auto}} .nodeItem{{display:grid;grid-template-columns:10px 1fr auto;gap:10px;align-items:center;padding:9px;background:#020617;border:1px solid var(--line);border-radius:12px;cursor:pointer}} .dot{{width:10px;height:10px;border-radius:99px}} .muted{{color:var(--muted)}} .small{{font-size:12px}} .kv{{display:grid;grid-template-columns:118px 1fr;gap:7px;margin:8px 0}} .pill{{display:inline-flex;border-radius:999px;padding:2px 8px;font-size:11px;font-weight:900;color:#020617;text-transform:uppercase}} .riskRow{{display:grid;grid-template-columns:74px 1fr 48px;gap:10px;align-items:center;margin:10px 0}} .bar{{height:10px;background:#111827;border-radius:999px;overflow:hidden}} .bar>i{{display:block;height:100%;border-radius:999px}} .path{{display:grid;grid-template-columns:repeat(5,1fr);gap:12px}} .lane{{min-height:570px;background:rgba(2,6,23,.58);border:1px solid var(--line);border-radius:18px;padding:12px}} .lane h3{{margin:0 0 10px;color:#cbd5e1;font-size:12px;text-transform:uppercase;letter-spacing:.1em}} .pnode{{border:1px solid rgba(148,163,184,.22);background:rgba(15,23,42,.82);border-radius:13px;padding:10px;margin:8px 0;cursor:pointer}} .pnode strong{{display:block;overflow:hidden;text-overflow:ellipsis}} table{{width:100%;border-collapse:collapse;background:#020617;border-radius:16px;overflow:hidden}} th,td{{padding:10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}} th{{background:#0b1220;color:#cbd5e1}} .tables{{display:grid;grid-template-columns:repeat(3,1fr);gap:16px}} .diag{{display:grid;gap:10px}} .finding{{border:1px solid var(--line);background:#020617;border-radius:14px;padding:12px}} .tooltip{{position:fixed;display:none;pointer-events:none;z-index:20;background:#020617;border:1px solid rgba(148,163,184,.35);border-radius:12px;padding:10px;max-width:420px;box-shadow:0 20px 60px rgba(0,0,0,.5)}} @media(max-width:1100px){{.hero,.layout,.tables{{grid-template-columns:1fr}}.path{{grid-template-columns:1fr 1fr}}h1{{font-size:38px}}.stats{{grid-template-columns:1fr 1fr}}}}
</style></head><body><div class="shell">
<section class="hero"><div class="card pad"><div class="eyebrow">DSCO // Binary Intelligence</div><h1>Symbol exposure graph for the shipping artifact.</h1><p class="lead">You pasted the ELF symbol-table question. This is the answer made concrete: DSCO’s binary metadata is rendered as a graph of symbol tables, modules, symbol types, exposure risks, and release-hardening pathways.</p><div class="digest"><b id="binaryName"></b><br><code id="binaryKind"></code><br><code id="sha"></code></div><div class="stats" id="stats"></div></div><div class="card pad"><h2>Exposure diagnosis</h2><div class="diag" id="diagnosis"></div><h2 style="margin-top:18px">Risk mass</h2><div id="riskBars"></div></div></section>
<nav class="tabs"><button class="tab active" data-view="graph">Symbol graph</button><button class="tab" data-view="pathway">Hardening pathways</button><button class="tab" data-view="metrics">Graph theory</button><button class="tab" data-view="symbols">Top exposed symbols</button></nav>
<section id="view-graph" class="view active"><div class="layout"><div class="card"><svg id="graph"></svg></div><aside class="side"><div class="card pad"><h2>Controls</h2><div class="controls"><input id="search" placeholder="search symbol/module/type"><select id="kindFilter"><option value="all">all kinds</option></select><select id="riskFilter"><option value="all">all risks</option><option>critical</option><option>high</option><option>medium</option><option>low</option></select><button class="btn" id="reheat">reheat</button></div></div><div class="card pad"><h2>Selected</h2><div id="details" class="muted">Click a node.</div></div><div class="card pad"><h2>Central nodes</h2><div id="nodeList" class="nodeList"></div></div></aside></div></section>
<section id="view-pathway" class="view"><div class="card pad"><h2>Release hardening pathway</h2><div class="path" id="pathway"></div></div></section>
<section id="view-metrics" class="view"><div class="tables"><div class="card pad"><h2>Top degree</h2><table id="degreeTable"></table></div><div class="card pad"><h2>Top PageRank</h2><table id="prTable"></table></div><div class="card pad"><h2>Top closeness</h2><table id="closeTable"></table></div></div></section>
<section id="view-symbols" class="view"><div class="card pad"><h2>Top exposed recoverable names</h2><table id="symbolTable"></table></div></section>
</div><div class="tooltip" id="tooltip"></div><script id="payload" type="application/json">{payload}</script><script>
const DATA=JSON.parse(document.getElementById('payload').textContent), S=DATA.summary, G=DATA.graph, M=DATA.metrics;
const riskColors={{critical:'#ef4444',high:'#f97316',medium:'#eab308',low:'#22c55e'}}, kindColors={{binary:'#22d3ee',section:'#a78bfa',module:'#64748b',symbol_type:'#60a5fa',symbol:'#cbd5e1',risk:'#f97316',tag:'#f43f5e',hardening:'#22c55e'}};
const byId=new Map(G.nodes.map(n=>[n.id,n])); const out=new Map(), inc=new Map(); G.edges.forEach(e=>{{if(!out.has(e.source))out.set(e.source,[]);out.get(e.source).push(e);if(!inc.has(e.target))inc.set(e.target,[]);inc.get(e.target).push(e)}});
function esc(x){{return String(x??'').replace(/[&<>"']/g,m=>({{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}}[m]))}} function color(n){{return n.color||riskColors[n.risk]||kindColors[n.kind]||'#94a3b8'}} function rad(n){{return 5+Math.min(18,Math.sqrt((n.weighted_degree||n.degree||1))*2.2)+(n.kind==='binary'?8:0)}}
document.getElementById('binaryName').textContent=S.binary.path; document.getElementById('binaryKind').textContent=S.binary.kind; document.getElementById('sha').textContent='sha256 '+S.binary.sha256;
document.getElementById('stats').innerHTML=[['Symbols',S.symbol_count],['Exported-ish',S.exported_like_count],['Debug',S.debug_symbol_count],['Long names',S.long_name_count],['Graph',M.nodes+' / '+M.edges]].map(([k,v])=>`<div class="stat"><div class="k">${{k}}</div><div class="v">${{v}}</div></div>`).join('');
document.getElementById('diagnosis').innerHTML=S.release_diagnosis.map(d=>`<div class="finding"><span class="pill" style="background:${{riskColors[d.severity]||'#64748b'}}">${{esc(d.severity)}}</span><p><b>${{esc(d.finding)}}</b></p><p class="muted">${{esc(d.action)}}</p></div>`).join(''); const maxRisk=Math.max(1,...Object.values(S.risk_counts)); document.getElementById('riskBars').innerHTML=['critical','high','medium','low'].map(r=>`<div class="riskRow"><b style="color:${{riskColors[r]}}">${{r}}</b><div class="bar"><i style="width:${{100*(S.risk_counts[r]||0)/maxRisk}}%;background:${{riskColors[r]}}"></i></div><code>${{S.risk_counts[r]||0}}</code></div>`).join('');
for(const k of [...new Set(G.nodes.map(n=>n.kind))].sort()) document.getElementById('kindFilter').insertAdjacentHTML('beforeend',`<option>${{k}}</option>`);
document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));t.classList.add('active');document.querySelectorAll('.view').forEach(v=>v.classList.remove('active'));document.getElementById('view-'+t.dataset.view).classList.add('active');resize()}});
const svg=document.getElementById('graph'); let W=1000,H=760,selected=null; let nodes=G.nodes.map((n,i)=>Object.assign({{x:80+(i%12)*70,y:80+Math.floor(i/12)*50,vx:0,vy:0,visible:true}},n)); let nmap=new Map(nodes.map(n=>[n.id,n])); let links=G.edges.map(e=>Object.assign({{a:nmap.get(e.source),b:nmap.get(e.target)}},e)); function resize(){{const r=svg.getBoundingClientRect();W=r.width||1000;H=r.height||760;svg.setAttribute('viewBox',`0 0 ${{W}} ${{H}}`)}} window.addEventListener('resize',resize); resize();
function tick(){{const vis=nodes.filter(n=>n.visible); for(const n of vis){{let tx=(n.layer??3)*(W/6)+55; n.vx+=(tx-n.x)*.003; n.vy+=(H/2-n.y)*.001}} for(let i=0;i<vis.length;i++)for(let j=i+1;j<vis.length;j++){{let a=vis[i],b=vis[j],dx=a.x-b.x,dy=a.y-b.y,d2=dx*dx+dy*dy+.1,f=850/d2;if(d2<7200){{a.vx+=dx*f;a.vy+=dy*f;b.vx-=dx*f;b.vy-=dy*f}}}} for(const l of links)if(l.a?.visible&&l.b?.visible){{let a=l.a,b=l.b,dx=b.x-a.x,dy=b.y-a.y,d=Math.sqrt(dx*dx+dy*dy)+.01,des=90+12*Math.abs((a.layer||0)-(b.layer||0)),f=(d-des)*.006*(l.weight||1);a.vx+=dx/d*f;a.vy+=dy/d*f;b.vx-=dx/d*f;b.vy-=dy/d*f}} for(const n of vis){{n.vx*=.84;n.vy*=.84;n.x=Math.max(24,Math.min(W-24,n.x+n.vx));n.y=Math.max(24,Math.min(H-24,n.y+n.vy))}} draw(); requestAnimationFrame(tick)}}
function draw(){{const q=document.getElementById('search').value.toLowerCase(), k=document.getElementById('kindFilter').value, r=document.getElementById('riskFilter').value; for(const n of nodes)n.visible=(!q||n.label.toLowerCase().includes(q)||n.id.toLowerCase().includes(q)||(n.module||'').includes(q))&&(k==='all'||n.kind===k)&&(r==='all'||n.risk===r||n.kind!=='symbol'); const vs=new Set(nodes.filter(n=>n.visible).map(n=>n.id)); const ls=links.filter(l=>vs.has(l.source)&&vs.has(l.target)).map(l=>`<line x1="${{l.a.x}}" y1="${{l.a.y}}" x2="${{l.b.x}}" y2="${{l.b.y}}" stroke="${{l.risk?riskColors[l.risk]:'#334155'}}" stroke-opacity="${{selected&&(l.source===selected.id||l.target===selected.id)?.9:.28}}" stroke-width="${{Math.max(1,Math.min(7,l.weight||1))}}"/>`).join(''); const ns=nodes.filter(n=>n.visible).map(n=>`<g class="gn" data-id="${{esc(n.id)}}" transform="translate(${{n.x}},${{n.y}})"><circle r="${{rad(n)}}" fill="${{color(n)}}" stroke="${{selected?.id===n.id?'#fff':'#020617'}}" stroke-width="${{selected?.id===n.id?4:1.5}}" opacity=".94"/><text x="${{rad(n)+5}}" y="4" fill="#dbeafe" font-size="11">${{esc(n.label.length>34?n.label.slice(0,34)+'…':n.label)}}</text></g>`).join(''); svg.innerHTML=ls+ns; svg.querySelectorAll('.gn').forEach(g=>{{g.onclick=()=>select(nmap.get(g.dataset.id));g.onmousemove=e=>tip(e,nmap.get(g.dataset.id));g.onmouseleave=()=>document.getElementById('tooltip').style.display='none'}})}}
function tip(e,n){{let t=document.getElementById('tooltip');t.style.display='block';t.style.left=e.clientX+14+'px';t.style.top=e.clientY+14+'px';t.innerHTML=`<b>${{esc(n.label)}}</b><br><span class="muted">${{esc(n.kind)}} · degree ${{n.degree}} · PR ${{n.pagerank}}</span>`}} function select(n){{selected=n;details(n);list();draw()}} function details(n){{let outs=(out.get(n.id)||[]).slice(0,16).map(e=>`<li><code>${{esc(e.relation)}}</code> → ${{esc(byId.get(e.target)?.label||e.target)}}</li>`).join(''), ins=(inc.get(n.id)||[]).slice(0,16).map(e=>`<li>${{esc(byId.get(e.source)?.label||e.source)}} → <code>${{esc(e.relation)}}</code></li>`).join(''); document.getElementById('details').innerHTML=`<div class="kv"><b>Label</b><span>${{esc(n.label)}}</span><b>Kind</b><span>${{esc(n.kind)}}</span><b>Type</b><span>${{esc(n.type_name||n.type||'—')}}</span><b>Risk</b><span>${{n.risk?`<span class="pill" style="background:${{riskColors[n.risk]}}">${{n.risk}}</span>`:'—'}}</span><b>Score</b><span>${{n.score??'—'}}</span><b>Degree</b><span>${{n.degree}}</span><b>PageRank</b><span>${{n.pagerank}}</span><b>Module</b><span>${{esc(n.module||'—')}}</span></div><hr style="border-color:#1e293b"><b>Outgoing</b><ul>${{outs||'<li>none</li>'}}</ul><b>Incoming</b><ul>${{ins||'<li>none</li>'}}</ul>`}}
function list(){{let xs=[...nodes].sort((a,b)=>(b.pagerank-a.pagerank)||(b.degree-a.degree)).slice(0,28);document.getElementById('nodeList').innerHTML=xs.map(n=>`<div class="nodeItem" data-id="${{esc(n.id)}}"><i class="dot" style="background:${{color(n)}}"></i><div><b>${{esc(n.label.length>38?n.label.slice(0,38)+'…':n.label)}}</b><div class="small muted">${{n.kind}} · degree ${{n.degree}}</div></div><code>${{n.pagerank}}</code></div>`).join('');document.querySelectorAll('.nodeItem').forEach(el=>el.onclick=()=>select(nmap.get(el.dataset.id)))}} ['search','kindFilter','riskFilter'].forEach(id=>document.getElementById(id).addEventListener('input',draw)); document.getElementById('reheat').onclick=()=>nodes.forEach(n=>{{n.vx+=(Math.random()-.5)*18;n.vy+=(Math.random()-.5)*18}}); list(); tick(); select(nmap.get('binary'));
function table(id,rows){{document.getElementById(id).innerHTML='<tr><th>Node</th><th>Kind</th><th>Value</th></tr>'+rows.map(r=>`<tr><td>${{esc(r.label)}}</td><td><code>${{esc(r.kind)}}</code></td><td><b>${{r.value}}</b></td></tr>`).join('')}} table('degreeTable',M.top_degree); table('prTable',M.top_pagerank); table('closeTable',M.top_closeness);
document.getElementById('symbolTable').innerHTML='<tr><th>Risk</th><th>Score</th><th>Type</th><th>Module</th><th>Name</th><th>Tags</th></tr>'+S.top_exposed.map(s=>`<tr><td><span class="pill" style="background:${{riskColors[s.risk]}}">${{s.risk}}</span></td><td>${{s.score}}</td><td><code>${{esc(s.type)}} ${{esc(s.type_name)}}</code></td><td>${{esc(s.module)}}</td><td><code>${{esc(s.name)}}</code></td><td>${{(s.tags||[]).map(esc).join(', ')}}</td></tr>`).join('');
const lanes=[['Binary',['binary']],['Tables',['section:symtab','section:strtab']],['Exposure',G.nodes.filter(n=>n.kind==='risk').map(n=>n.id)],['Policy',['ship:visibility','ship:debuglink']],['Shipping',['ship:strip']]]; document.getElementById('pathway').innerHTML=lanes.map(([title,ids])=>`<div class="lane"><h3>${{title}}</h3>${{ids.map(id=>{{let n=byId.get(id);return `<div class="pnode" data-id="${{esc(id)}}"><strong>${{esc(n.label)}}</strong><span class="small muted">${{esc(n.kind)}} · degree ${{n.degree}}</span></div>`}}).join('')}}</div>`).join(''); document.querySelectorAll('.pnode').forEach(el=>el.onclick=()=>{{document.querySelector('[data-view="graph"]').click();select(nmap.get(el.dataset.id))}});
</script></body></html>"""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate a symbol exposure graph UI for a binary.")
    ap.add_argument("binary", nargs="?", default=str(DEFAULT_BINARY))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--json-out", default=str(ROOT / "build" / "symbol_audit" / "symbol_audit.json"))
    args = ap.parse_args(argv)
    binary = pathlib.Path(args.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"binary not found: {binary}")
    meta = {"path": str(binary), "bytes": binary.stat().st_size, "sha256": sha256_file(binary), "kind": file_kind(binary)}
    raw_symbols, nm_meta = parse_nm(binary)
    symbols = classify_symbols(raw_symbols)
    meta["nm"] = nm_meta
    summary = summarize(symbols, meta)
    graph = build_graph(symbols, meta)
    metrics = graph_metrics(graph)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(summary, graph, metrics))
    jout = pathlib.Path(args.json_out)
    jout.parent.mkdir(parents=True, exist_ok=True)
    jout.write_text(json.dumps({"summary": summary, "graph": graph, "metrics": metrics}, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"out": str(out), "json": str(jout), "symbols": len(symbols), "nodes": metrics["nodes"], "edges": metrics["edges"], "binary_kind": meta["kind"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
