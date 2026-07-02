#!/usr/bin/env python3
"""Generate an SVG/HTML diagram suite from dsco object instrumentation profile dirs."""
from __future__ import annotations

import csv, html, json, math, os, re, sys
from collections import Counter, defaultdict
from pathlib import Path

PALETTE = ["#3b82f6", "#10b981", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4", "#84cc16", "#f97316", "#ec4899", "#64748b"]

def read_tsv(path: Path):
    if not path.exists(): return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))

def clean_sym(s: str) -> str:
    s = s.strip()
    s = re.sub(r" \(in .*", "", s)
    s = re.sub(r" \+ \d+$", "", s)
    return s or "unknown"

def short(s: str, n=72) -> str:
    s = clean_sym(s)
    return s if len(s) <= n else s[: n-1] + "…"

def svg_bar(title, rows, value_label, path: Path, width=1280, row_h=28, left=360):
    rows = rows[:25]
    maxv = max([v for _, v in rows] or [1])
    height = 64 + row_h * len(rows)
    chart_w = width - left - 150
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="Menlo,Consolas,monospace" font-size="12">',
           '<rect width="100%" height="100%" fill="#0b1020"/>',
           f'<text x="20" y="28" fill="#e5e7eb" font-size="18" font-weight="700">{html.escape(title)}</text>']
    for i, (name, val) in enumerate(rows):
        y = 55 + i * row_h
        w = (val / maxv) * chart_w if maxv else 0
        color = PALETTE[i % len(PALETTE)]
        out.append(f'<text x="20" y="{y+16}" fill="#cbd5e1">{html.escape(short(name, 48))}</text>')
        out.append(f'<rect x="{left}" y="{y}" width="{w:.1f}" height="20" rx="3" fill="{color}"/>')
        out.append(f'<text x="{left + w + 8:.1f}" y="{y+15}" fill="#e5e7eb">{val:,} {html.escape(value_label)}</text>')
    out.append('</svg>')
    path.write_text("\n".join(out) + "\n")

def svg_stacked(title, series, labels, path: Path, width=1280, height=560):
    # series: workload -> {label: value}
    workloads = list(series)
    left, right, top, bottom = 170, 40, 58, 96
    chart_w, chart_h = width-left-right, height-top-bottom
    max_total = max([sum(series[w].values()) for w in workloads] or [1])
    bar_h = max(24, min(46, chart_h / max(len(workloads),1) * .65))
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="Menlo,Consolas,monospace" font-size="12">',
           '<rect width="100%" height="100%" fill="#0b1020"/>',
           f'<text x="20" y="30" fill="#e5e7eb" font-size="18" font-weight="700">{html.escape(title)}</text>']
    for i,w in enumerate(workloads):
        y = top + i * (chart_h / max(len(workloads),1))
        out.append(f'<text x="20" y="{y+bar_h/2+4:.1f}" fill="#cbd5e1">{html.escape(w)}</text>')
        x = left
        total = sum(series[w].values())
        for j,lbl in enumerate(labels):
            val = series[w].get(lbl,0)
            bw = (val / max_total) * chart_w if max_total else 0
            if bw > 0:
                out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bw:.1f}" height="{bar_h:.1f}" fill="{PALETTE[j%len(PALETTE)]}"/>')
            x += bw
        out.append(f'<text x="{left + (total/max_total)*chart_w + 8:.1f}" y="{y+bar_h/2+4:.1f}" fill="#e5e7eb">{total:,}</text>')
    lx, ly = left, height-54
    for j,lbl in enumerate(labels[:10]):
        out.append(f'<rect x="{lx}" y="{ly}" width="14" height="14" fill="{PALETTE[j%len(PALETTE)]}"/><text x="{lx+20}" y="{ly+12}" fill="#cbd5e1">{html.escape(lbl)}</text>')
        lx += 115 + len(lbl)*6
        if lx > width-240: lx, ly = left, ly+22
    out.append('</svg>')
    path.write_text("\n".join(out)+"\n")

def svg_heatmap(title, matrix, row_names, col_names, path: Path, width=1280):
    cell_w, cell_h = 135, 34
    left, top = 360, 70
    height = top + len(row_names)*cell_h + 50
    maxv = max([matrix.get((r,c),0) for r in row_names for c in col_names] or [1])
    out=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="Menlo,Consolas,monospace" font-size="11">','<rect width="100%" height="100%" fill="#0b1020"/>',f'<text x="20" y="30" fill="#e5e7eb" font-size="18" font-weight="700">{html.escape(title)}</text>']
    for j,c in enumerate(col_names): out.append(f'<text x="{left+j*cell_w+4}" y="58" fill="#cbd5e1">{html.escape(c[:16])}</text>')
    for i,r in enumerate(row_names):
        y=top+i*cell_h
        out.append(f'<text x="20" y="{y+22}" fill="#cbd5e1">{html.escape(short(r,46))}</text>')
        for j,c in enumerate(col_names):
            v=matrix.get((r,c),0); alpha=0.12+0.88*(v/maxv if maxv else 0)
            out.append(f'<rect x="{left+j*cell_w}" y="{y}" width="{cell_w-3}" height="{cell_h-3}" fill="#38bdf8" fill-opacity="{alpha:.3f}"/>')
            if v: out.append(f'<text x="{left+j*cell_w+6}" y="{y+20}" fill="#f8fafc">{v:,}</text>')
    out.append('</svg>')
    path.write_text("\n".join(out)+"\n")

def main():
    out = Path(sys.argv[1]) if len(sys.argv)>1 else Path("build/perf-diagram-suite")
    dirs = [Path(p) for p in sys.argv[2:]]
    out.mkdir(parents=True, exist_ok=True)
    workloads=[]; totals=[]; top_func_global=Counter(); line_global=Counter(); edge_global=Counter(); stack_by_workload={}
    for d in dirs:
        if not (d/"summary.json").exists(): continue
        summary=json.loads((d/"summary.json").read_text())
        cmd=" ".join(summary.get("command",[]))
        name=d.name.replace("perf-","")
        workloads.append((name,d,summary,cmd))
        frows=read_tsv(d/"functions.symbolized.tsv")
        lrows=read_tsv(d/"lines.tsv")
        erows=read_tsv(d/"edges.symbolized.tsv")
        for r in frows:
            sym=clean_sym(r.get("symbol","unknown")); val=int(r.get("self_ns") or 0); top_func_global[sym]+=val
        for r in lrows: line_global[r.get("location","unknown")]+=int(r.get("hits") or 0)
        for r in erows: edge_global[clean_sym(r.get("symbol","unknown"))]+=int(r.get("hits") or 0)
        totals.append((name, int(summary.get("functions",0)), int(summary.get("edges",0)), int(summary.get("folded_stacks",0))))
        stack_by_workload[name]=Counter()
        for line in (d/"stacks.symbolized.folded").read_text().splitlines() if (d/"stacks.symbolized.folded").exists() else []:
            try: stack,c=line.rsplit(" ",1)
            except ValueError: continue
            leaf=clean_sym(stack.split(";")[-1]); stack_by_workload[name][leaf]+=int(c)
    svg_bar("Top functions by cumulative self time", top_func_global.most_common(25), "ns", out/"top-functions-self-time.svg")
    svg_bar("Top source locations by sanitizer-coverage edge hits", line_global.most_common(25), "hits", out/"top-lines-hit-count.svg")
    svg_bar("Top object-code coverage edges", edge_global.most_common(25), "hits", out/"top-edges-hit-count.svg")
    series={n:{"functions":f,"edges":e,"stacks":s} for n,f,e,s in totals}
    svg_stacked("Instrumentation volume by workload", series, ["functions","edges","stacks"], out/"workload-instrumentation-volume.svg")
    top_leaf=[k for k,_ in sum((c for c in stack_by_workload.values()), Counter()).most_common(16)]
    matrix={(leaf,w): stack_by_workload[w].get(leaf,0) for w in stack_by_workload for leaf in top_leaf}
    svg_heatmap("Hot stack leaf heatmap by workload", matrix, top_leaf, list(stack_by_workload), out/"hot-stack-leaf-heatmap.svg")
    # index
    cards=[]
    for fn in ["top-functions-self-time.svg","top-lines-hit-count.svg","top-edges-hit-count.svg","workload-instrumentation-volume.svg","hot-stack-leaf-heatmap.svg"]:
        cards.append(f'<h2>{fn}</h2><object data="{fn}" type="image/svg+xml" style="width:100%;border:1px solid #334155"></object>')
    flame_links="".join(f'<li><a href="../{html.escape(str(d))}/flamegraph.svg">{html.escape(n)} flamegraph</a> — <code>{html.escape(cmd)}</code></li>' for n,d,_,cmd in workloads if (d/"flamegraph.svg").exists())
    (out/"index.html").write_text(f'<!doctype html><meta charset="utf-8"><title>dsco perf diagrams</title><body style="background:#020617;color:#e5e7eb;font-family:system-ui"><h1>dsco instrumented performance diagram suite</h1><p>Generated from {len(workloads)} object-code instrumentation runs.</p><h2>Per-run flamegraphs</h2><ul>{flame_links}</ul>{"".join(cards)}</body>\n')
    (out/"summary.json").write_text(json.dumps({"runs":[{"name":n,"dir":str(d),"cmd":cmd,"summary":s} for n,d,s,cmd in workloads],"diagrams":[p.name for p in out.glob("*.svg")]}, indent=2)+"\n")
    print(out)

if __name__ == "__main__": main()
