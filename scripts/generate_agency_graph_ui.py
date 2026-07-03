#!/usr/bin/env python3
"""Generate a standalone graph-theoretic UI for a DSCO agency capsule."""
from __future__ import annotations

import argparse
import html
import json
import math
import pathlib
import sys
from collections import defaultdict, deque

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_CAPSULE = ROOT / "build" / "agency_capsule" / "latest" / "capsule.json"
DEFAULT_OUT = ROOT / "web" / "static" / "agency_graph.html"

RISK_WEIGHT = {"low": 1.0, "medium": 2.0, "high": 3.5, "critical": 5.0}
RISK_COLOR = {"low": "#22c55e", "medium": "#eab308", "high": "#f97316", "critical": "#ef4444"}


def gate_for_command(cmd: str) -> str:
    c = cmd.lower()
    if "pytest" in c or "web" in c:
        return "gate:web-regression"
    if "tui" in c or "snapshot" in c:
        return "gate:snapshot-regression"
    if "changed-tests" in c:
        return "gate:targeted-regression"
    if "make test" == c.strip():
        return "gate:full-regression"
    return "gate:unit-regression"


def subsystem_for_path(path: str) -> str:
    parts = path.split("/")
    if len(parts) == 1:
        return "subsystem:repo-root"
    if parts[0] in {"src", "include"} and len(parts) > 1:
        stem = pathlib.Path(parts[1]).stem
        return f"subsystem:{stem}"
    return f"subsystem:{parts[0]}"


def node(nodes: dict, node_id: str, label: str, kind: str, **attrs):
    if node_id not in nodes:
        nodes[node_id] = {"id": node_id, "label": label, "kind": kind, **attrs}
    else:
        nodes[node_id].update(attrs)
    return nodes[node_id]


def add_edge(edges: list, source: str, target: str, relation: str, weight: float = 1.0, **attrs):
    edges.append({"source": source, "target": target, "relation": relation, "weight": weight, **attrs})


def build_graph(capsule: dict) -> dict:
    nodes: dict[str, dict] = {}
    edges: list[dict] = []

    node(nodes, "state:workspace", "Workspace state", "state", layer=0, color="#38bdf8")
    node(nodes, "capsule:digest", "Capsule digest", "evidence", layer=4, color="#a78bfa", digest=capsule.get("capsule_digest_sha256"))
    node(nodes, "gate:commit-readiness", "Commit readiness", "gate", layer=6, color="#14b8a6")
    node(nodes, "gate:audit-replay", "Replay / audit gate", "gate", layer=5, color="#8b5cf6")
    add_edge(edges, "state:workspace", "capsule:digest", "summarized_by", 3.0)
    add_edge(edges, "capsule:digest", "gate:audit-replay", "verifies", 4.0)
    add_edge(edges, "gate:audit-replay", "gate:commit-readiness", "unlocks", 3.5)

    test_matrix = capsule.get("test_matrix", {})
    file_to_tests: dict[str, list[str]] = defaultdict(list)
    for cmd, paths in test_matrix.items():
        for p in paths:
            file_to_tests[p].append(cmd)

    for f in capsule.get("files", []):
        path = f["path"]
        risk = f.get("risk", "low")
        weight = RISK_WEIGHT.get(risk, 1.0)
        fid = f"file:{path}"
        sid = subsystem_for_path(path)
        subsystem_label = sid.split(":", 1)[1]
        node(nodes, sid, subsystem_label, "subsystem", layer=1, color="#64748b")
        node(nodes, fid, path, "file", layer=2, color=RISK_COLOR.get(risk, "#94a3b8"), **f)
        add_edge(edges, "state:workspace", sid, "contains", 1.0)
        add_edge(edges, sid, fid, "changed_file", weight, risk=risk)
        add_edge(edges, fid, "capsule:digest", "fingerprinted", weight, risk=risk)
        for cmd in file_to_tests.get(path, ["make changed-tests"]):
            tid = f"test:{cmd}"
            gate = gate_for_command(cmd)
            node(nodes, tid, cmd, "test", layer=3, color="#60a5fa", command=cmd)
            node(nodes, gate, gate.split(":", 1)[1].replace("-", " ").title(), "gate", layer=5, color="#f59e0b")
            add_edge(edges, fid, tid, "should_verify_with", weight, risk=risk)
            add_edge(edges, tid, gate, "feeds_gate", 2.0)
            add_edge(edges, gate, "gate:commit-readiness", "supports", 2.5)

    return {"nodes": list(nodes.values()), "edges": edges}


def undirected_adjacency(graph: dict):
    adj = defaultdict(set)
    weighted = defaultdict(float)
    for e in graph["edges"]:
        a, b = e["source"], e["target"]
        adj[a].add(b)
        adj[b].add(a)
        weighted[a] += float(e.get("weight", 1.0))
        weighted[b] += float(e.get("weight", 1.0))
    return adj, weighted


def components(adj: dict[str, set[str]], node_ids: list[str]) -> list[list[str]]:
    seen = set()
    out = []
    for n in node_ids:
        if n in seen:
            continue
        q = deque([n])
        seen.add(n)
        comp = []
        while q:
            u = q.popleft()
            comp.append(u)
            for v in adj.get(u, ()):  # isolated-safe
                if v not in seen:
                    seen.add(v)
                    q.append(v)
        out.append(comp)
    return out


def pagerank(graph: dict, iterations: int = 40, damping: float = 0.85) -> dict[str, float]:
    ids = [n["id"] for n in graph["nodes"]]
    n = max(1, len(ids))
    outlinks = defaultdict(list)
    for e in graph["edges"]:
        outlinks[e["source"]].append(e["target"])
    pr = {i: 1.0 / n for i in ids}
    for _ in range(iterations):
        new = {i: (1 - damping) / n for i in ids}
        sink = sum(pr[i] for i in ids if not outlinks.get(i))
        for i in ids:
            new[i] += damping * sink / n
        for u, vs in outlinks.items():
            share = damping * pr[u] / max(1, len(vs))
            for v in vs:
                new[v] += share
        pr = new
    return pr


def betweenness(adj: dict[str, set[str]], node_ids: list[str]) -> dict[str, float]:
    cb = {v: 0.0 for v in node_ids}
    for s in node_ids:
        stack = []
        pred = {w: [] for w in node_ids}
        sigma = dict.fromkeys(node_ids, 0.0)
        dist = dict.fromkeys(node_ids, -1)
        sigma[s] = 1.0
        dist[s] = 0
        q = deque([s])
        while q:
            v = q.popleft()
            stack.append(v)
            for w in adj.get(v, ()):
                if dist[w] < 0:
                    q.append(w)
                    dist[w] = dist[v] + 1
                if dist[w] == dist[v] + 1:
                    sigma[w] += sigma[v]
                    pred[w].append(v)
        delta = dict.fromkeys(node_ids, 0.0)
        while stack:
            w = stack.pop()
            for v in pred[w]:
                if sigma[w]:
                    delta[v] += (sigma[v] / sigma[w]) * (1 + delta[w])
            if w != s:
                cb[w] += delta[w]
    # undirected normalization by 2
    return {k: v / 2.0 for k, v in cb.items()}


def articulation_points(adj: dict[str, set[str]], node_ids: list[str]) -> list[str]:
    time = 0
    disc = {u: -1 for u in node_ids}
    low = {u: -1 for u in node_ids}
    parent = {u: None for u in node_ids}
    aps = set()

    def dfs(u: str):
        nonlocal time
        children = 0
        disc[u] = low[u] = time
        time += 1
        for v in adj.get(u, ()):
            if disc[v] == -1:
                parent[v] = u
                children += 1
                dfs(v)
                low[u] = min(low[u], low[v])
                if parent[u] is None and children > 1:
                    aps.add(u)
                if parent[u] is not None and low[v] >= disc[u]:
                    aps.add(u)
            elif v != parent[u]:
                low[u] = min(low[u], disc[v])

    for u in node_ids:
        if disc[u] == -1:
            dfs(u)
    return sorted(aps)


def enrich_metrics(graph: dict) -> dict:
    ids = [n["id"] for n in graph["nodes"]]
    by_id = {n["id"]: n for n in graph["nodes"]}
    adj, weighted = undirected_adjacency(graph)
    comps = components(adj, ids)
    pr = pagerank(graph)
    btw = betweenness(adj, ids)
    aps = articulation_points(adj, ids)
    edge_count = len(graph["edges"])
    node_count = len(ids)
    density = edge_count / max(1, node_count * (node_count - 1))
    for n in graph["nodes"]:
        nid = n["id"]
        n["degree"] = len(adj.get(nid, ()))
        n["weighted_degree"] = round(weighted.get(nid, 0.0), 3)
        n["pagerank"] = round(pr.get(nid, 0.0), 6)
        n["betweenness"] = round(btw.get(nid, 0.0), 3)
        n["articulation"] = nid in aps
    top = lambda key: sorted(graph["nodes"], key=lambda x: x.get(key, 0), reverse=True)[:10]
    return {
        "node_count": node_count,
        "edge_count": edge_count,
        "density": round(density, 5),
        "component_count": len(comps),
        "largest_component": max((len(c) for c in comps), default=0),
        "articulation_points": [{"id": x, "label": by_id[x]["label"], "kind": by_id[x]["kind"]} for x in aps],
        "top_degree": [{"id": n["id"], "label": n["label"], "kind": n["kind"], "value": n["degree"]} for n in top("degree")],
        "top_pagerank": [{"id": n["id"], "label": n["label"], "kind": n["kind"], "value": n["pagerank"]} for n in top("pagerank")],
        "top_betweenness": [{"id": n["id"], "label": n["label"], "kind": n["kind"], "value": n["betweenness"]} for n in top("betweenness")],
    }


def risk_counts(files):
    out = {"critical": 0, "high": 0, "medium": 0, "low": 0}
    for f in files:
        out[f.get("risk", "low")] = out.get(f.get("risk", "low"), 0) + 1
    return out


def render(capsule: dict, graph: dict, metrics: dict) -> str:
    payload = {"capsule": capsule, "graph": graph, "metrics": metrics, "riskCounts": risk_counts(capsule.get("files", []))}
    payload_json = json.dumps(payload, separators=(",", ":"))
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>DSCO Agency Graph Console</title>
<style>
:root {{
  color-scheme: dark;
  --bg:#030712; --panel:#08111f; --panel2:#0f172a; --line:#1e293b; --text:#e5e7eb; --muted:#94a3b8;
  --cyan:#22d3ee; --blue:#60a5fa; --purple:#a78bfa; --green:#22c55e; --yellow:#eab308; --orange:#f97316; --red:#ef4444;
}}
* {{ box-sizing:border-box; }}
body {{ margin:0; min-height:100vh; font:14px/1.45 Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; color:var(--text); background:
  radial-gradient(circle at 15% 0%, rgba(34,211,238,.22), transparent 28%),
  radial-gradient(circle at 85% 8%, rgba(167,139,250,.18), transparent 30%),
  linear-gradient(180deg,#020617,#030712 34%,#020617); }}
button,input,select {{ font:inherit; }}
code {{ font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; }}
.shell {{ max-width:1500px; margin:0 auto; padding:28px; }}
.hero {{ display:grid; grid-template-columns:1.35fr .65fr; gap:18px; align-items:stretch; }}
.card {{ background:linear-gradient(180deg,rgba(15,23,42,.88),rgba(8,17,31,.88)); border:1px solid rgba(148,163,184,.18); border-radius:22px; box-shadow:0 24px 80px rgba(0,0,0,.35); overflow:hidden; }}
.card.pad {{ padding:20px; }}
.eyebrow {{ color:var(--cyan); text-transform:uppercase; letter-spacing:.16em; font-size:12px; font-weight:800; }}
h1 {{ font-size:48px; line-height:.96; letter-spacing:-.06em; margin:10px 0 14px; }}
.lead {{ color:#cbd5e1; font-size:16px; max-width:850px; }}
.digest {{ margin-top:16px; padding:12px 14px; border-radius:14px; background:#020617; border:1px solid var(--line); color:#cbd5e1; overflow:hidden; text-overflow:ellipsis; }}
.stats {{ display:grid; grid-template-columns:repeat(4,1fr); gap:10px; margin-top:18px; }}
.stat {{ background:rgba(2,6,23,.7); border:1px solid var(--line); border-radius:16px; padding:13px; }}
.stat .k {{ color:var(--muted); font-size:11px; text-transform:uppercase; letter-spacing:.1em; }}
.stat .v {{ font-size:25px; font-weight:900; margin-top:4px; }}
.riskStack {{ display:grid; gap:9px; }}
.riskRow {{ display:grid; grid-template-columns:82px 1fr 34px; gap:10px; align-items:center; }}
.bar {{ height:10px; border-radius:999px; background:#111827; overflow:hidden; }}
.bar > i {{ display:block; height:100%; border-radius:999px; }}
.tabs {{ display:flex; gap:10px; margin:20px 0; flex-wrap:wrap; }}
.tab {{ border:1px solid var(--line); color:#cbd5e1; background:rgba(15,23,42,.75); padding:10px 14px; border-radius:999px; cursor:pointer; }}
.tab.active {{ border-color:rgba(34,211,238,.65); color:white; box-shadow:0 0 0 3px rgba(34,211,238,.1) inset; }}
.view {{ display:none; }} .view.active {{ display:block; }}
.layout {{ display:grid; grid-template-columns:1fr 390px; gap:18px; }}
#graphCanvas {{ width:100%; height:720px; display:block; background:radial-gradient(circle at 50% 40%,rgba(34,211,238,.08),transparent 38%), #020617; border-radius:22px; }}
.side {{ display:grid; gap:14px; align-content:start; }}
.panelTitle {{ display:flex; justify-content:space-between; gap:12px; align-items:center; margin-bottom:12px; }}
.panelTitle h2 {{ margin:0; font-size:18px; }}
.controls {{ display:flex; gap:8px; flex-wrap:wrap; }}
.controls input,.controls select {{ background:#020617; border:1px solid var(--line); color:var(--text); border-radius:12px; padding:10px 11px; outline:none; }}
.btn {{ background:linear-gradient(180deg,#164e63,#0e7490); border:1px solid rgba(34,211,238,.35); color:white; padding:9px 12px; border-radius:12px; cursor:pointer; }}
.nodeList {{ display:grid; gap:8px; max-height:260px; overflow:auto; padding-right:3px; }}
.nodeItem {{ display:grid; grid-template-columns:10px 1fr auto; gap:10px; align-items:center; padding:9px; border:1px solid var(--line); border-radius:12px; background:#020617; cursor:pointer; }}
.dot {{ width:10px; height:10px; border-radius:99px; }}
.muted {{ color:var(--muted); }}
.small {{ font-size:12px; }}
.kv {{ display:grid; grid-template-columns:130px 1fr; gap:8px; margin:7px 0; }}
.kv b {{ color:#cbd5e1; }}
.pathway {{ display:grid; grid-template-columns:repeat(6,1fr); gap:12px; align-items:start; }}
.lane {{ min-height:560px; background:rgba(2,6,23,.55); border:1px solid var(--line); border-radius:18px; padding:12px; position:relative; }}
.lane h3 {{ margin:0 0 10px; color:#cbd5e1; font-size:13px; text-transform:uppercase; letter-spacing:.08em; }}
.pnode {{ border:1px solid rgba(148,163,184,.22); background:rgba(15,23,42,.78); border-radius:13px; padding:10px; margin:8px 0; cursor:pointer; box-shadow:0 10px 30px rgba(0,0,0,.18); }}
.pnode strong {{ display:block; overflow:hidden; text-overflow:ellipsis; }}
.pill {{ display:inline-flex; align-items:center; border-radius:999px; padding:2px 8px; font-size:11px; font-weight:800; color:#020617; text-transform:uppercase; }}
.tables {{ display:grid; grid-template-columns:repeat(3,1fr); gap:16px; }}
table {{ width:100%; border-collapse:collapse; background:rgba(2,6,23,.6); border-radius:16px; overflow:hidden; }}
th,td {{ padding:10px; border-bottom:1px solid var(--line); text-align:left; vertical-align:top; }}
th {{ background:#0b1220; color:#cbd5e1; }}
.execGrid {{ display:grid; grid-template-columns:1fr 1fr; gap:16px; }}
.command {{ display:flex; justify-content:space-between; gap:12px; align-items:center; border:1px solid var(--line); background:#020617; padding:12px; border-radius:14px; margin:8px 0; }}
.copy {{ border:1px solid var(--line); background:#111827; color:#cbd5e1; padding:6px 9px; border-radius:10px; cursor:pointer; }}
svg text {{ user-select:none; }}
.tooltip {{ position:fixed; pointer-events:none; background:#020617; border:1px solid rgba(148,163,184,.35); border-radius:12px; padding:10px; color:#e5e7eb; max-width:360px; box-shadow:0 20px 60px rgba(0,0,0,.45); display:none; z-index:20; }}
@media (max-width:1100px) {{ .hero,.layout,.execGrid,.tables {{ grid-template-columns:1fr; }} .pathway {{ grid-template-columns:1fr 1fr; }} h1 {{ font-size:38px; }} }}
</style>
</head>
<body>
<div class="shell">
  <section class="hero">
    <div class="card pad">
      <div class="eyebrow">DSCO // Verifiable Agency Graph</div>
      <h1>Execution pathways, risk topology, and proof-carrying work.</h1>
      <p class="lead">This is the current workspace state rendered as a graph: changed files flow into tests, gates, capsule evidence, replay verification, and commit readiness. The UI computes degree, PageRank, betweenness, density, components, and articulation points from the actual capsule.</p>
      <div class="digest"><code id="digest"></code></div>
      <div class="stats" id="topStats"></div>
    </div>
    <div class="card pad">
      <div class="panelTitle"><h2>Risk mass</h2><span class="muted small">dirty-file topology</span></div>
      <div class="riskStack" id="riskStack"></div>
      <div class="digest small" style="margin-top:16px"><b>HEAD</b><br><code id="head"></code></div>
    </div>
  </section>

  <nav class="tabs">
    <button class="tab active" data-view="graph">Force graph</button>
    <button class="tab" data-view="pathways">Execution pathways</button>
    <button class="tab" data-view="metrics">Graph theory</button>
    <button class="tab" data-view="commands">Verification console</button>
  </nav>

  <section id="view-graph" class="view active">
    <div class="layout">
      <div class="card"><svg id="graphCanvas"></svg></div>
      <aside class="side">
        <div class="card pad">
          <div class="panelTitle"><h2>Graph controls</h2></div>
          <div class="controls">
            <input id="search" placeholder="search node / file / command" />
            <select id="kindFilter"><option value="all">all kinds</option></select>
            <select id="riskFilter"><option value="all">all risks</option><option>critical</option><option>high</option><option>medium</option><option>low</option></select>
            <button class="btn" id="reheat">reheat layout</button>
          </div>
        </div>
        <div class="card pad">
          <div class="panelTitle"><h2>Selected node</h2><span class="muted small">click graph/list</span></div>
          <div id="details" class="muted">No node selected.</div>
        </div>
        <div class="card pad">
          <div class="panelTitle"><h2>High-signal nodes</h2><span class="muted small">centrality sorted</span></div>
          <div id="nodeList" class="nodeList"></div>
        </div>
      </aside>
    </div>
  </section>

  <section id="view-pathways" class="view">
    <div class="card pad">
      <div class="panelTitle"><h2>Observe → Fingerprint → Verify → Gate → Commit</h2><span class="muted small">DAG view</span></div>
      <div class="pathway" id="pathway"></div>
    </div>
  </section>

  <section id="view-metrics" class="view">
    <div class="tables">
      <div class="card pad"><h2>Top degree</h2><table id="degreeTable"></table></div>
      <div class="card pad"><h2>Top PageRank</h2><table id="pagerankTable"></table></div>
      <div class="card pad"><h2>Top betweenness</h2><table id="betweenTable"></table></div>
    </div>
    <div class="card pad" style="margin-top:16px"><h2>Articulation points / bottlenecks</h2><div id="articulation"></div></div>
  </section>

  <section id="view-commands" class="view">
    <div class="execGrid">
      <div class="card pad"><h2>Recommended verification commands</h2><div id="commands"></div></div>
      <div class="card pad"><h2>Replay contract</h2><div class="command"><code>scripts/verifiable_agency_capsule.py --verify build/agency_capsule/latest</code><button class="copy">copy</button></div><div class="command"><code>open web/static/agency_graph.html</code><button class="copy">copy</button></div><p class="muted">Verify digest, verify dirty-file hashes, run selected tests, then commit only what passes.</p></div>
    </div>
  </section>
</div>
<div class="tooltip" id="tooltip"></div>
<script id="payload" type="application/json">{payload_json}</script>
<script>
const DATA = JSON.parse(document.getElementById('payload').textContent);
const graph = DATA.graph;
const metrics = DATA.metrics;
const capsule = DATA.capsule;
const riskColors = {{low:'#22c55e', medium:'#eab308', high:'#f97316', critical:'#ef4444'}};
const kindColors = {{state:'#22d3ee', subsystem:'#64748b', file:'#60a5fa', test:'#a78bfa', gate:'#f59e0b', evidence:'#8b5cf6'}};
const byId = new Map(graph.nodes.map(n => [n.id,n]));
const outgoing = new Map(); const incoming = new Map();
graph.edges.forEach(e => {{ if(!outgoing.has(e.source)) outgoing.set(e.source,[]); outgoing.get(e.source).push(e); if(!incoming.has(e.target)) incoming.set(e.target,[]); incoming.get(e.target).push(e); }});
function esc(s) {{ return String(s ?? '').replace(/[&<>"']/g, m => ({{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}}[m])); }}
function label(id) {{ return byId.get(id)?.label || id; }}
function color(n) {{ return n.color || riskColors[n.risk] || kindColors[n.kind] || '#94a3b8'; }}
function radius(n) {{ return 6 + Math.min(16, Math.sqrt((n.weighted_degree||n.degree||1))*2.2) + (n.risk==='critical'?4:0); }}

document.getElementById('digest').textContent = capsule.capsule_digest_sha256;
document.getElementById('head').textContent = `${{capsule.branch}} @ ${{capsule.head?.slice(0,12)}}`;
document.getElementById('topStats').innerHTML = [
 ['Nodes', metrics.node_count], ['Edges', metrics.edge_count], ['Density', metrics.density], ['Components', metrics.component_count]
].map(([k,v])=>`<div class="stat"><div class="k">${{k}}</div><div class="v">${{v}}</div></div>`).join('');
const maxRisk = Math.max(1,...Object.values(DATA.riskCounts));
document.getElementById('riskStack').innerHTML = ['critical','high','medium','low'].map(r => `<div class="riskRow"><b style="color:${{riskColors[r]}}">${{r}}</b><div class="bar"><i style="width:${{100*DATA.riskCounts[r]/maxRisk}}%;background:${{riskColors[r]}}"></i></div><code>${{DATA.riskCounts[r]}}</code></div>`).join('');

for (const k of [...new Set(graph.nodes.map(n=>n.kind))].sort()) document.getElementById('kindFilter').insertAdjacentHTML('beforeend', `<option>${{k}}</option>`);

// Tabs
for (const tab of document.querySelectorAll('.tab')) tab.onclick = () => {{
 document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active')); tab.classList.add('active');
 document.querySelectorAll('.view').forEach(v=>v.classList.remove('active')); document.getElementById('view-'+tab.dataset.view).classList.add('active');
 if (tab.dataset.view === 'graph') setTimeout(resizeGraph, 50);
}};

// Force graph
const svg = document.getElementById('graphCanvas');
let W=1000,H=720, selected=null;
let nodes = graph.nodes.map((n,i)=>Object.assign({{x:120+(i%8)*95,y:90+Math.floor(i/8)*70,vx:0,vy:0,visible:true}}, n));
let nodeMap = new Map(nodes.map(n=>[n.id,n]));
let links = graph.edges.map(e=>Object.assign({{sourceNode:nodeMap.get(e.source), targetNode:nodeMap.get(e.target)}}, e));
function resizeGraph() {{ const r=svg.getBoundingClientRect(); W=r.width||1000; H=r.height||720; svg.setAttribute('viewBox',`0 0 ${{W}} ${{H}}`); }}
window.addEventListener('resize', resizeGraph); resizeGraph();
function tick() {{
 const visibleNodes = nodes.filter(n=>n.visible); const visibleSet = new Set(visibleNodes.map(n=>n.id));
 for (const n of visibleNodes) {{
   const targetX = (n.layer ?? 3) * (W/7) + 60; n.vx += (targetX-n.x)*0.003; n.vy += (H/2-n.y)*0.001;
 }}
 for (let i=0;i<visibleNodes.length;i++) for (let j=i+1;j<visibleNodes.length;j++) {{
   const a=visibleNodes[i], b=visibleNodes[j]; let dx=a.x-b.x, dy=a.y-b.y; let d2=dx*dx+dy*dy+0.01; let f=900/d2; if(d2<9000){{ a.vx += dx*f; a.vy += dy*f; b.vx -= dx*f; b.vy -= dy*f; }}
 }}
 for (const l of links) if (l.sourceNode?.visible && l.targetNode?.visible) {{
   const a=l.sourceNode,b=l.targetNode; let dx=b.x-a.x,dy=b.y-a.y,d=Math.sqrt(dx*dx+dy*dy)+0.01; let desired=90+18*(Math.abs((a.layer||0)-(b.layer||0))); let f=(d-desired)*0.006*(l.weight||1);
   a.vx += dx/d*f; a.vy += dy/d*f; b.vx -= dx/d*f; b.vy -= dy/d*f;
 }}
 for (const n of visibleNodes) {{ n.vx*=0.82; n.vy*=0.82; n.x=Math.max(25,Math.min(W-25,n.x+n.vx)); n.y=Math.max(25,Math.min(H-25,n.y+n.vy)); }}
 draw(); requestAnimationFrame(tick);
}}
function draw() {{
 const q = document.getElementById('search').value.toLowerCase(); const kind = document.getElementById('kindFilter').value; const risk = document.getElementById('riskFilter').value;
 for (const n of nodes) n.visible = (!q || n.label.toLowerCase().includes(q) || n.id.toLowerCase().includes(q)) && (kind==='all'||n.kind===kind) && (risk==='all'||n.risk===risk||n.kind!=='file');
 const visibleSet = new Set(nodes.filter(n=>n.visible).map(n=>n.id));
 const linkSvg = links.filter(l=>visibleSet.has(l.source)&&visibleSet.has(l.target)).map(l => `<line x1="${{l.sourceNode.x}}" y1="${{l.sourceNode.y}}" x2="${{l.targetNode.x}}" y2="${{l.targetNode.y}}" stroke="${{l.risk?riskColors[l.risk]:'#334155'}}" stroke-opacity="${{selected && (l.source===selected.id||l.target===selected.id) ? .9 : .32}}" stroke-width="${{Math.max(1,Math.min(7,l.weight||1))}}" />`).join('');
 const nodeSvg = nodes.filter(n=>n.visible).map(n => `<g class="gnode" data-id="${{esc(n.id)}}" transform="translate(${{n.x}},${{n.y}})"><circle r="${{radius(n)}}" fill="${{color(n)}}" stroke="${{selected?.id===n.id?'#fff':(n.articulation?'#f8fafc':'#020617')}}" stroke-width="${{selected?.id===n.id?4:(n.articulation?3:1.5)}}" opacity=".94"/><text x="${{radius(n)+5}}" y="4" fill="#dbeafe" font-size="11">${{esc(n.label.length>34?n.label.slice(0,34)+'…':n.label)}}</text></g>`).join('');
 svg.innerHTML = `<defs><filter id="glow"><feGaussianBlur stdDeviation="3" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>${{linkSvg}}${{nodeSvg}}`;
 svg.querySelectorAll('.gnode').forEach(g => {{ g.onclick = () => selectNode(nodeMap.get(g.dataset.id)); g.onmousemove = ev => showTip(ev, nodeMap.get(g.dataset.id)); g.onmouseleave = hideTip; }});
}}
function showTip(ev,n) {{ const t=document.getElementById('tooltip'); t.style.display='block'; t.style.left=ev.clientX+14+'px'; t.style.top=ev.clientY+14+'px'; t.innerHTML=`<b>${{esc(n.label)}}</b><br><span class="muted">${{esc(n.kind)}} · degree ${{n.degree}} · PR ${{n.pagerank}} · between ${{n.betweenness}}</span>`; }}
function hideTip() {{ document.getElementById('tooltip').style.display='none'; }}
function selectNode(n) {{ selected=n; renderDetails(n); renderNodeList(); draw(); }}
function renderDetails(n) {{
 const outs=(outgoing.get(n.id)||[]).map(e=>`<li><code>${{esc(e.relation)}}</code> → ${{esc(label(e.target))}}</li>`).join('');
 const ins=(incoming.get(n.id)||[]).map(e=>`<li>${{esc(label(e.source))}} → <code>${{esc(e.relation)}}</code></li>`).join('');
 document.getElementById('details').innerHTML = `<div class="kv"><b>Label</b><span>${{esc(n.label)}}</span><b>Kind</b><span>${{esc(n.kind)}}</span><b>Risk</b><span>${{n.risk?`<span class="pill" style="background:${{riskColors[n.risk]}}">${{n.risk}}</span>`:'—'}}</span><b>Degree</b><span>${{n.degree}}</span><b>Weighted</b><span>${{n.weighted_degree}}</span><b>PageRank</b><span>${{n.pagerank}}</span><b>Betweenness</b><span>${{n.betweenness}}</span><b>Cut vertex</b><span>${{n.articulation?'yes':'no'}}</span></div><hr style="border-color:#1e293b"><b>Outgoing</b><ul>${{outs||'<li>none</li>'}}</ul><b>Incoming</b><ul>${{ins||'<li>none</li>'}}</ul>`;
}}
function renderNodeList() {{
 const sorted=[...nodes].sort((a,b)=>(b.betweenness-a.betweenness)||(b.pagerank-a.pagerank)||(b.degree-a.degree)).slice(0,22);
 document.getElementById('nodeList').innerHTML=sorted.map(n=>`<div class="nodeItem" data-id="${{esc(n.id)}}"><i class="dot" style="background:${{color(n)}}"></i><div><b>${{esc(n.label.length>40?n.label.slice(0,40)+'…':n.label)}}</b><div class="small muted">${{n.kind}} · degree ${{n.degree}} · PR ${{n.pagerank}}</div></div><code>${{n.betweenness}}</code></div>`).join('');
 document.querySelectorAll('.nodeItem').forEach(el=>el.onclick=()=>selectNode(nodeMap.get(el.dataset.id)));
}}
['search','kindFilter','riskFilter'].forEach(id=>document.getElementById(id).addEventListener('input', draw));
document.getElementById('reheat').onclick=()=>{{ for(const n of nodes){{n.vx+=(Math.random()-.5)*18;n.vy+=(Math.random()-.5)*18;}} }};
renderNodeList(); tick();

// Pathways
function renderPathways() {{
 const lanes = [
   ['Observe',['state:workspace']],
   ['Subsystems',graph.nodes.filter(n=>n.kind==='subsystem').map(n=>n.id)],
   ['Changed files',graph.nodes.filter(n=>n.kind==='file').sort((a,b)=>(riskWeight(b.risk)-riskWeight(a.risk))).slice(0,32).map(n=>n.id)],
   ['Tests',graph.nodes.filter(n=>n.kind==='test').map(n=>n.id)],
   ['Gates',graph.nodes.filter(n=>n.kind==='gate' && n.id!=='gate:commit-readiness').map(n=>n.id)],
   ['Commit',['gate:commit-readiness']]
 ];
 document.getElementById('pathway').innerHTML = lanes.map(([title,ids])=>`<div class="lane"><h3>${{title}}</h3>${{ids.map(id=>{{const n=byId.get(id); return `<div class="pnode" data-id="${{esc(id)}}"><strong>${{esc(n.label.length>48?n.label.slice(0,48)+'…':n.label)}}</strong><span class="small muted">${{esc(n.kind)}} · degree ${{n.degree}}</span></div>`}}).join('')}}</div>`).join('');
 document.querySelectorAll('.pnode').forEach(el=>el.onclick=()=>{{ document.querySelector('[data-view="graph"]').click(); selectNode(nodeMap.get(el.dataset.id)); }});
}}
function riskWeight(r) {{ return {{critical:4, high:3, medium:2, low:1}}[r]||0; }}
renderPathways();

function table(el, rows) {{ document.getElementById(el).innerHTML = `<tr><th>Node</th><th>Kind</th><th>Value</th></tr>` + rows.map(r=>`<tr><td>${{esc(r.label)}}</td><td><code>${{esc(r.kind)}}</code></td><td><b>${{r.value}}</b></td></tr>`).join(''); }}
table('degreeTable', metrics.top_degree); table('pagerankTable', metrics.top_pagerank); table('betweenTable', metrics.top_betweenness);
document.getElementById('articulation').innerHTML = metrics.articulation_points.length ? metrics.articulation_points.map(a=>`<div class="command"><span><b>${{esc(a.label)}}</b><br><span class="muted small">${{esc(a.kind)}} · removing this node disconnects at least one pathway</span></span><code>${{esc(a.id)}}</code></div>`).join('') : '<p class="muted">No articulation points.</p>';

const commands = Object.entries(capsule.test_matrix||{{}});
document.getElementById('commands').innerHTML = commands.map(([cmd,paths])=>`<div class="command"><span><code>${{esc(cmd)}}</code><br><span class="muted small">covers ${{paths.length}} dirty file(s)</span></span><button class="copy">copy</button></div>`).join('');
document.body.addEventListener('click', ev => {{ if(ev.target.classList.contains('copy')) {{ const code=ev.target.parentElement.querySelector('code')?.textContent || ''; navigator.clipboard?.writeText(code); ev.target.textContent='copied'; setTimeout(()=>ev.target.textContent='copy',900); }} }});
selectNode(nodeMap.get('state:workspace'));
</script>
</body>
</html>
"""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate DSCO Agency Graph UI from a capsule JSON.")
    ap.add_argument("--capsule", default=str(DEFAULT_CAPSULE))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args(argv)
    capsule_path = pathlib.Path(args.capsule)
    out_path = pathlib.Path(args.out)
    capsule = json.loads(capsule_path.read_text())
    graph = build_graph(capsule)
    metrics = enrich_metrics(graph)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(render(capsule, graph, metrics))
    print(json.dumps({
        "out": str(out_path),
        "capsule": str(capsule_path),
        "nodes": metrics["node_count"],
        "edges": metrics["edge_count"],
        "density": metrics["density"],
        "articulation_points": len(metrics["articulation_points"]),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
