#!/usr/bin/env python3
"""Build, run, and symbolize dsco object-code instrumentation profiles."""

from __future__ import annotations

import argparse
import html
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=ROOT, text=True, check=False, **kwargs)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--out-dir")
    p.add_argument("--sample-rate", type=int, default=1)
    p.add_argument("--stream", action="store_true")
    p.add_argument("--keep-going", action="store_true")
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    return p.parse_args()


def normalize_cmd(raw: list[str]) -> list[str]:
    cmd = list(raw)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    return cmd or ["./dsco-instrumented", "--version"]


def read_tsv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open() as f:
        header = f.readline().rstrip("\n").split("\t")
        rows = []
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == len(header):
                rows.append(dict(zip(header, parts)))
        return rows


def symbolize_apple(binary: str, load_address: str, addrs: list[str]) -> dict[str, str]:
    if not addrs or not shutil.which("atos"):
        return {a: a for a in addrs}
    cmd = ["atos", "-o", binary]
    if load_address and load_address != "0x0":
        cmd.extend(["-l", load_address])
    cp = subprocess.run(
        cmd,
        input="\n".join(addrs) + "\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    if cp.returncode != 0:
        return {a: a for a in addrs}
    lines = cp.stdout.splitlines()
    return {a: lines[i] if i < len(lines) and lines[i] else a for i, a in enumerate(addrs)}


def symbolize_addr2line(binary: str, addrs: list[str]) -> dict[str, str]:
    if not addrs or not shutil.which("addr2line"):
        return {a: a for a in addrs}
    cp = subprocess.run(
        ["addr2line", "-Cfpe", binary, *addrs],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    if cp.returncode != 0:
        return {a: a for a in addrs}
    lines = cp.stdout.splitlines()
    return {a: lines[i] if i < len(lines) and lines[i] else a for i, a in enumerate(addrs)}


def symbolize(manifest: dict, addrs: list[str]) -> dict[str, str]:
    unique = sorted(set(addrs))
    binary = str(Path(manifest.get("binary", "")).resolve())
    if platform.system() == "Darwin":
        return symbolize_apple(binary, manifest.get("load_address", "0x0"), unique)
    return symbolize_addr2line(binary, unique)


def frame_name(symbol: str) -> str:
    if " (in " in symbol:
        symbol = symbol.split(" (in ", 1)[0]
    return (symbol.strip() or "unknown").replace(";", ":")


def line_key(symbol: str) -> str:
    matches = re.findall(r"\(([^()]+:\d+)(?::\d+)?\)", symbol)
    return matches[-1] if matches else symbol


def load_manifests(out_dir: Path) -> list[dict]:
    manifests = []
    for path in sorted(out_dir.glob("manifest-*.json")):
        obj = json.loads(path.read_text())
        obj["_pid"] = path.stem.split("-", 1)[1]
        manifests.append(obj)
    return manifests


def collect_edges(out_dir: Path, manifest: dict) -> tuple[list[tuple[int, str, str]], Counter[str]]:
    rows = read_tsv(out_dir / f"edges-{manifest['_pid']}.tsv")
    syms = symbolize(manifest, [r["pc"] for r in rows])
    edges = []
    lines: Counter[str] = Counter()
    for row in rows:
        hits = int(row["hits"])
        pc = row["pc"]
        sym = syms.get(pc, pc)
        edges.append((hits, pc, sym))
        lines[line_key(sym)] += hits
    edges.sort(reverse=True)
    return edges, lines


def collect_functions(out_dir: Path, manifest: dict) -> list[tuple[int, int, int, str, str]]:
    rows = read_tsv(out_dir / f"functions-{manifest['_pid']}.tsv")
    syms = symbolize(manifest, [r["pc"] for r in rows])
    out = []
    for row in rows:
        pc = row["pc"]
        out.append((int(row["total_ns"]), int(row["self_ns"]), int(row["calls"]), pc, syms.get(pc, pc)))
    out.sort(reverse=True)
    return out


def collect_folded(out_dir: Path, manifest: dict) -> Counter[str]:
    path = out_dir / f"stacks-{manifest['_pid']}.folded"
    folded: Counter[str] = Counter()
    if not path.exists():
        return folded
    raw: list[tuple[list[str], int]] = []
    addrs: list[str] = []
    for line in path.read_text().splitlines():
        if not line:
            continue
        stack_s, count_s = line.rsplit(" ", 1)
        stack = stack_s.split(";")
        raw.append((stack, int(count_s)))
        addrs.extend(stack)
    syms = symbolize(manifest, addrs)
    for stack, count in raw:
        folded[";".join(frame_name(syms.get(a, a)) for a in stack)] += count
    return folded


class Node:
    def __init__(self, name: str) -> None:
        self.name = name
        self.value = 0
        self.children: dict[str, Node] = {}


def color(name: str) -> str:
    h = 0
    for ch in name:
        h = (h * 131 + ord(ch)) & 0xFFFFFFFF
    return f"#{180 + (h & 63):02x}{80 + ((h >> 8) & 95):02x}{55 + ((h >> 16) & 79):02x}"


def write_flamegraph(folded: Counter[str], path: Path) -> None:
    root = Node("root")
    for stack, count in folded.items():
        root.value += count
        node = root
        for frame in stack.split(";"):
            node = node.children.setdefault(frame, Node(frame))
            node.value += count
    width = 1400
    frame_h = 18
    pad = 10
    max_depth = 1

    def find_depth(n: Node, d: int) -> None:
        nonlocal max_depth
        max_depth = max(max_depth, d)
        for c in n.children.values():
            find_depth(c, d + 1)

    find_depth(root, 0)
    scale = (width - pad * 2) / max(root.value, 1)
    height = 40 + max_depth * frame_h
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        'font-family="Menlo,Consolas,monospace" font-size="11">',
        '<rect width="100%" height="100%" fill="#fff"/>',
        f'<text x="{pad}" y="16">dsco object-code flamegraph samples={root.value}</text>',
    ]

    def draw(n: Node, x: float, y: float) -> None:
        cur = x
        for child in sorted(n.children.values(), key=lambda c: c.value, reverse=True):
            w = child.value * scale
            if w < 0.3:
                continue
            name = html.escape(child.name)
            svg.append(
                f'<g><title>{name} {child.value}</title>'
                f'<rect x="{cur:.2f}" y="{y:.2f}" width="{w:.2f}" height="{frame_h - 1}" '
                f'fill="{color(child.name)}" stroke="#fff" stroke-width="0.5"/>'
            )
            if w > 48:
                shown = child.name[: max(1, int(w / 7))]
                svg.append(f'<text x="{cur + 3:.2f}" y="{y + 12:.2f}">{html.escape(shown)}</text>')
            svg.append("</g>")
            draw(child, cur, y + frame_h)
            cur += w

    draw(root, pad, 28)
    svg.append("</svg>")
    path.write_text("\n".join(svg) + "\n")


def main() -> int:
    args = parse_args()
    if args.sample_rate <= 0:
        raise SystemExit("--sample-rate must be positive")
    out_dir = Path(args.out_dir).resolve() if args.out_dir else ROOT / "build" / "profiles" / f"run-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}"
    out_dir.mkdir(parents=True, exist_ok=True)

    if not args.no_build:
        cp = run(["make", "profile-instrumented"])
        if cp.returncode != 0:
            return cp.returncode

    cmd = normalize_cmd(args.cmd)
    env = os.environ.copy()
    env["DSCO_INSTRUMENT"] = "1"
    env["DSCO_INSTRUMENT_DIR"] = str(out_dir)
    env["DSCO_INSTRUMENT_STACK_SAMPLE_RATE"] = str(args.sample_rate)
    env.setdefault("DSCO_NO_AUTO_INTERACTIVE", "1")
    if args.stream:
        env["DSCO_INSTRUMENT_STREAM"] = "1"
    cp = run(cmd, env=env)
    if cp.returncode != 0 and not args.keep_going:
        return cp.returncode

    manifests = load_manifests(out_dir)
    if not manifests:
        print(f"no profile manifests under {out_dir}", file=sys.stderr)
        return 1

    all_edges: list[tuple[int, str, str]] = []
    all_functions: list[tuple[int, int, int, str, str]] = []
    all_lines: Counter[str] = Counter()
    all_folded: Counter[str] = Counter()
    for manifest in manifests:
        edges, lines = collect_edges(out_dir, manifest)
        all_edges.extend(edges)
        all_lines.update(lines)
        all_functions.extend(collect_functions(out_dir, manifest))
        all_folded.update(collect_folded(out_dir, manifest))
    all_edges.sort(reverse=True)
    all_functions.sort(reverse=True)

    (out_dir / "edges.symbolized.tsv").write_text(
        "hits\tpc\tsymbol\n" + "".join(f"{h}\t{pc}\t{sym}\n" for h, pc, sym in all_edges)
    )
    (out_dir / "lines.tsv").write_text(
        "hits\tlocation\n" + "".join(f"{h}\t{loc}\n" for loc, h in all_lines.most_common())
    )
    (out_dir / "functions.symbolized.tsv").write_text(
        "total_ns\tself_ns\tcalls\tpc\tsymbol\n"
        + "".join(f"{t}\t{s}\t{c}\t{pc}\t{sym}\n" for t, s, c, pc, sym in all_functions)
    )
    (out_dir / "stacks.symbolized.folded").write_text(
        "".join(f"{stack} {count}\n" for stack, count in all_folded.most_common())
    )
    if all_folded:
        write_flamegraph(all_folded, out_dir / "flamegraph.svg")
    (out_dir / "summary.json").write_text(json.dumps({
        "command": cmd,
        "returncode": cp.returncode,
        "manifests": len(manifests),
        "edges": len(all_edges),
        "functions": len(all_functions),
        "folded_stacks": len(all_folded),
        "top_lines": all_lines.most_common(20),
    }, indent=2, sort_keys=True) + "\n")
    print(out_dir)
    return cp.returncode


if __name__ == "__main__":
    sys.exit(main())
