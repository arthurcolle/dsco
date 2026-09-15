#!/usr/bin/env python3
"""Public-source harness radar. Python stdlib only; never invokes an LLM.

Source changes create review candidates, never capability assertions. Downloaded
content is data: no commands, plugins, or instructions from it are executed.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import configparser
import csv
import difflib
import fcntl
import hashlib
import html
import io
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "data/harness-radar/registry.json"
STATE = Path.home() / ".local/state/dsco-harness-radar"
OUTPUT = ROOT / "reports/harness-radar"
LABEL = "com.dsco.harness-radar"
MAX_BYTES = 2_000_000
AXES = {
    "permissions": r"\b(?:sandbox|permission|approval|allowlist|capability gate)\b",
    "recovery": r"\b(?:durable|checkpoint|resume|resumable|recovery|rollback|replay)\b",
    "context": r"\b(?:compact(?:ion)?|context window|prompt cach|token budget|tool search)",
    "interop": r"\b(?:MCP|ACP|A2A|SDK|JSON-RPC|headless|non-interactive)\b",
    "orchestration": r"\b(?:subagents?|sub-agents?|worktrees?|multi-agent|handoff|fan.out)\b",
    "coding_tools": r"\b(?:LSP|DAP|hashline|AST|debugger|diagnostics|patch)\b",
    "learning": r"\b(?:memory|skills?|learning|recall)\b",
    "economics": r"\b(?:cost|budget|pricing|quota|usage|billing|cache hit)\b",
    "operations": r"\b(?:cron|webhook|daemon|schedule|heartbeat|telemetry|tracing)\b",
    "lifecycle": r"\b(?:archived|unmaintained|deprecated|successor|shut down|no longer maintained|no longer actively maintained|final release|wound down|renamed)\b",
}
ROADMAP = {
    "permissions": "03,13: test permission enforcement and OS isolation separately",
    "recovery": "01,02: kill/restart and duplicate-effect acceptance probes",
    "context": "04,07: retained constraints and tokens per verified completion",
    "interop": "05,06: versioned headless/MCP/SDK contract fixtures",
    "orchestration": "02: isolated workers, typed results, cancellation and budgets",
    "coding_tools": "experiment: stale edits, LSP feedback and debugger fixtures",
    "learning": "08,15: trace-to-skill held-out evaluation and invalidation",
    "economics": "09,12: exact provider lane, quota and all-worker receipts",
    "operations": "12,14,16: event delivery, replay and observed completion",
    "lifecycle": "check successor identity and support status before adopting patterns",
}


def utc(ts=None):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() if ts is None else ts))


def atomic(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)


def read_registry(path):
    data = json.loads(Path(path).read_text())
    ids = [p["id"] for p in data["projects"]]
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate project IDs")
    for p in data["projects"]:
        if not re.fullmatch(r"[a-z0-9_.-]+", p["id"]):
            raise ValueError("Invalid project ID")
        for s in p.get("sources", []):
            u = urllib.parse.urlsplit(s["url"])
            if u.scheme != "https" or u.username or u.password:
                raise ValueError("Sources must be public HTTPS URLs without credentials")
    return data


def connect(state):
    state.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(state / "radar.sqlite3")
    db.row_factory = sqlite3.Row
    db.executescript("""
    PRAGMA journal_mode=WAL;
    CREATE TABLE IF NOT EXISTS sources(
      url TEXT PRIMARY KEY, project TEXT, kind TEXT, checked REAL DEFAULT 0,
      success REAL DEFAULT 0, due REAL DEFAULT 0, etag TEXT, modified TEXT,
      sha TEXT, body TEXT, error TEXT, failures INTEGER DEFAULT 0,
      final_url TEXT, http_status INTEGER);
    CREATE TABLE IF NOT EXISTS events(
      id INTEGER PRIMARY KEY, at REAL, project TEXT, url TEXT, kind TEXT,
      before_sha TEXT, after_sha TEXT, axes TEXT, diff TEXT,
      status TEXT DEFAULT 'pending', note TEXT DEFAULT '');
    CREATE UNIQUE INDEX IF NOT EXISTS event_identity
      ON events(url,before_sha,after_sha);
    CREATE TABLE IF NOT EXISTS runs(
      id INTEGER PRIMARY KEY, started REAL, finished REAL, checked INTEGER,
      changed INTEGER, errors INTEGER, bytes INTEGER);
    """)
    return db


class PlainText(HTMLParser):
    def __init__(self):
        super().__init__(); self.parts = []; self.skip = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style", "noscript"):
            self.skip += 1
        elif not self.skip and tag in ("p", "li", "h1", "h2", "h3", "br", "div"):
            self.parts.append("\n")

    def handle_endtag(self, tag):
        if tag in ("script", "style", "noscript"):
            self.skip = max(0, self.skip - 1)

    def handle_data(self, data):
        if not self.skip:
            self.parts.append(data)


def normalize(body, kind):
    if kind == "directory_acp":
        entries = []
        with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as archive:
            for member in archive:
                if member.isfile() and member.name.endswith('/agent.json') and member.size < 300000:
                    obj = json.load(archive.extractfile(member))
                    entries.append({k: obj.get(k, '') for k in ('id', 'name', 'version', 'repository', 'website')})
                    if len(entries) > 1000:
                        raise ValueError('ACP entry limit exceeded')
        if not entries:
            raise ValueError('ACP archive has no agent descriptors')
        return json.dumps(sorted(entries, key=lambda x: x['id']), sort_keys=True, indent=2)
    if kind == "releases":
        root = ET.fromstring(body)
        ns = {"a": "http://www.w3.org/2005/Atom"}
        return json.dumps([{
            k: e.findtext("a:" + k, default="", namespaces=ns)
            for k in ("id", "title", "updated", "content")
        } for e in root.findall("a:entry", ns)], ensure_ascii=False, indent=2)
    if kind == "metadata":
        obj = json.loads(body)
        keys = ("full_name", "html_url", "description", "archived", "disabled",
                "default_branch", "license", "pushed_at", "updated_at", "language")
        return json.dumps({k: obj.get(k) for k in keys}, sort_keys=True, indent=2)
    if kind == "website":
        parser = PlainText(); parser.feed(body)
        return "\n".join(" ".join(x.split()) for x in "".join(parser.parts).splitlines() if x.strip())
    if kind == "directory":
        obj = json.loads(body)
        return json.dumps(sorted({p["github_id"] for p in obj["projects"] if p.get("github_id")}))
    if kind == "directory_md":
        # These are discovery leads, including adjacent tools. No descriptions,
        # rankings, or external instructions are promoted into assertions.
        repos = set()
        for line in body.splitlines():
            if not re.match(r"^(?:[-*] |\||## )", line):
                continue
            match = re.search(r"https://github.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)", line)
            if match and match.group(1).split('/')[0] not in ('apps', 'orgs', 'features', 'sponsors', 'user-attachments'):
                repos.add(match.group(1).removesuffix('.git'))
        return json.dumps(sorted(repos))
    return body.replace("\r\n", "\n").strip() + "\n"


def features(text):
    return sorted(k for k, pattern in AXES.items() if re.search(pattern, text, re.I))


def fetch(source, old, timeout=18):
    headers = {"User-Agent": "dsco-harness-radar/1.0", "Accept": "*/*"}
    if old.get("etag"):
        headers["If-None-Match"] = old["etag"]
    if old.get("modified"):
        headers["If-Modified-Since"] = old["modified"]
    req = urllib.request.Request(source["url"], headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read(MAX_BYTES + 1)
            if len(raw) > MAX_BYTES:
                raise ValueError("response exceeds 2 MB cap")
            body = normalize(raw if source['kind'] == 'directory_acp' else raw.decode("utf-8", errors="replace"), source["kind"])
            return {"status": response.status, "body": body, "bytes": len(raw),
                    "etag": response.headers.get("ETag"),
                    "modified": response.headers.get("Last-Modified"),
                    "final_url": response.url}
    except urllib.error.HTTPError as exc:
        if exc.code == 304:
            exc.close()
            return {"status": 304, "bytes": 0}
        retry = exc.headers.get("Retry-After", "")
        reset = exc.headers.get("X-RateLimit-Reset", "")
        wait = float(retry) if retry.isdigit() else 0
        if reset.isdigit():
            wait = max(wait, float(reset) - time.time())
        result = {"status": exc.code, "error": f"HTTP {exc.code}", "bytes": 0,
                  "retry_seconds": max(wait, 3600 if exc.code in (403, 429) else 0)}
        exc.close()
        return result
    except Exception as exc:
        return {"status": 0, "error": type(exc).__name__ + ": " + str(exc)[:160], "bytes": 0}


def record(db, project, source, old, result, now):
    url, kind = source["url"], source["kind"]
    interval = source["interval_seconds"]
    if result.get("error"):
        failures = old.get("failures", 0) + 1
        backoff = min(86400, 900 * 2 ** min(failures, 7))
        due = now + max(backoff, result.get("retry_seconds", 0))
        db.execute("""UPDATE sources SET checked=?,due=?,error=?,failures=?,http_status=? WHERE url=?""",
                   (now, due, result["error"], failures, result["status"], url))
        return 0
    if result["status"] == 304:
        if not old.get("sha"):
            raise ValueError("304 without a baseline")
        db.execute("UPDATE sources SET checked=?,success=?,due=?,error=NULL,failures=0,http_status=304 WHERE url=?",
                   (now, now, now + interval, url))
        return 0
    body = result["body"]
    sha = hashlib.sha256(body.encode()).hexdigest()
    changed = bool(old.get("sha") and old["sha"] != sha)
    if changed:
        before = old["body"].splitlines()
        after = body.splitlines()
        delta = "\n".join(difflib.unified_diff(before, after, n=2))
        changed_lines = "\n".join(l for l in delta.splitlines() if l[:1] in ("+", "-") and l[:3] not in ("+++", "---"))
        axes = features(changed_lines)
        # License/metadata changes always deserve review even without feature words.
        db.execute("""INSERT OR IGNORE INTO events
            (at,project,url,kind,before_sha,after_sha,axes,diff) VALUES (?,?,?,?,?,?,?,?)""",
            (now, project["id"], url, kind, old["sha"], sha, json.dumps(axes), delta[:80000]))
    db.execute("""UPDATE sources SET checked=?,success=?,due=?,etag=?,modified=?,sha=?,body=?,
                error=NULL,failures=0,final_url=?,http_status=? WHERE url=?""",
               (now, now, now + interval, result.get("etag"), result.get("modified"), sha, body,
                result.get("final_url", url), result["status"], url))
    return int(changed)


def sync(db, registry, args):
    started = time.time(); queued = []
    if args.only:
        unknown = set(args.only.split(',')) - {p['id'] for p in registry['projects']}
        if unknown:
            raise ValueError('Unknown project IDs: ' + ', '.join(sorted(unknown)))
    for project in registry["projects"]:
        for source in project.get("sources", []):
            db.execute("INSERT OR IGNORE INTO sources(url,project,kind) VALUES (?,?,?)",
                       (source["url"], project["id"], source["kind"]))
            old = dict(db.execute("SELECT * FROM sources WHERE url=?", (source["url"],)).fetchone())
            if (args.only and project["id"] not in args.only.split(",")) or (old["due"] > started and not args.force):
                continue
            queued.append((project, source, old))
    db.commit()
    queued.sort(key=lambda x: (x[0].get("priority", 3), x[2]["checked"], x[1]["kind"] == "metadata"))
    selected = []; api = 0
    for item in queued:
        if item[1]["kind"] == "metadata":
            if api >= args.max_api:
                continue
            api += 1
        if len(selected) >= args.max_requests:
            break
        selected.append(item)
    checked = changed = errors = byte_count = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        pending = {pool.submit(fetch, s, o): (p, s, o) for p, s, o in selected}
        for future in concurrent.futures.as_completed(pending):
            project, source, old = pending[future]; result = future.result()
            changed += record(db, project, source, old, result, time.time())
            errors += bool(result.get("error")); checked += 1; byte_count += result["bytes"]
            db.commit()
    db.execute("INSERT INTO runs(started,finished,checked,changed,errors,bytes) VALUES (?,?,?,?,?,?)",
               (started, time.time(), checked, changed, errors, byte_count)); db.commit()
    # Bound bulky history while retaining source hashes, event identity and review decisions.
    db.execute("DELETE FROM runs WHERE started < ?", (started - 90 * 86400,))
    db.execute("UPDATE events SET diff='[Diff expired after 90 days; hashes and review retained]' WHERE at < ? AND length(diff)>80", (started - 90 * 86400,))
    db.commit()
    print(json.dumps({"checked": checked, "changed": changed, "errors": errors,
                      "download_bytes": byte_count, "due_backlog": len(queued) - len(selected), "llm_calls": 0}))


def license_signal(text):
    lower = text.lower()
    if "all rights reserved" in lower and "commercial terms" in lower:
        return "vendor terms; not OSS"
    if "permission is hereby granted, free of charge" in lower and "the software is provided" in lower:
        return "MIT text detected"
    if "apache license" in lower and "version 2.0" in lower:
        return "Apache-2.0 text detected"
    if "gnu affero general public license" in lower:
        return "AGPL text detected"
    if "gnu general public license" in lower:
        return "GPL text detected"
    if "mozilla public license" in lower:
        return "MPL text detected"
    if "redistribution and use in source and binary forms" in lower:
        return "BSD-family text detected"
    return "unclassified; inspect license"


def md(value):
    return str(value or "—").replace("|", "\\|").replace("\n", " ")


def render(db, registry, output):
    output.mkdir(parents=True, exist_ok=True)
    states = {r["url"]: dict(r) for r in db.execute("SELECT * FROM sources")}
    now = time.time(); rows = []; source_rows = []
    for p in registry["projects"]:
        entries = [(s, states.get(s["url"], {})) for s in p.get("sources", [])]
        successful = [r for _, r in entries if r.get("sha")]
        failures = [r for _, r in entries if r.get("error")]
        stale = [r for s, r in entries if not r.get("success") or now - r["success"] > s["interval_seconds"] * 2]
        metadata = next((json.loads(r["body"]) for s, r in entries if s["kind"] == "metadata" and r.get("sha")), {})
        lic = next((license_signal(r["body"]) for s, r in entries if s["kind"] == "license" and r.get("sha")), "unverified")
        spdx = (metadata.get("license") or {}).get("spdx_id")
        if spdx not in (None, "NOASSERTION"):
            lic = spdx + " (GitHub metadata)"
        if p.get("license_override"):
            lic = p["license_override"]
        texts = "\n".join(r.get("body") or "" for s, r in entries if s["kind"] in ("readme", "changelog"))
        row = {"id": p["id"], "name": p["name"], "category": p["category"],
               "url": metadata.get("html_url") or p["url"], "priority": p.get("priority", 3),
               "license_signal": lic, "archived": metadata.get("archived", "unknown"),
               "description": metadata.get("description") or p.get("summary", ""),
               "readme_axes_mentions_only": ", ".join(features(texts)),
               "sources_ok": len(successful), "sources_total": len(entries),
               "source_errors": len(failures), "stale_sources": len(stale),
               "last_success": utc(max(r["success"] for r in successful)) if successful else "never",
               "review_status": p.get("review_status", "discovery; not feature-reviewed"),
               "lifecycle_note": p.get("lifecycle_note", ""),
               "discovered_via": p.get("discovered_via", "configured source")}
        rows.append(row)
        for s, r in entries:
            source_rows.append({"project": p["id"], "kind": s["kind"], "url": s["url"],
                "last_checked": utc(r["checked"]) if r.get("checked") else None,
                "last_success": utc(r["success"]) if r.get("success") else None,
                "sha256": r.get("sha"), "error": r.get("error"), "http_status": r.get("http_status"),
                "final_url": r.get("final_url"), "interval_seconds": s["interval_seconds"]})
    atomic(output / "catalog.json", json.dumps({"generated_at": utc(), "projects": rows}, indent=2))
    atomic(output / "source-status.json", json.dumps(source_rows, indent=2))
    with (output / "catalog.csv.tmp").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0])); writer.writeheader()
        writer.writerows({k: ("'" + v if isinstance(v, str) and v.startswith(('=', '+', '-', '@', '\t', '\r')) else v) for k, v in row.items()} for row in rows)
    (output / "catalog.csv.tmp").replace(output / "catalog.csv")
    lines = ["# Global agent harness and adjacent ecosystem catalog", "", "Generated: " + utc(), "",
             f"{len(rows)} entries. Discovery coverage is broad, not an exhaustive census. Unknown means unverified.",
             "Keyword mentions are search aids, not tested feature support. Category membership is curated; links and license signals are checked independently.", ""]
    for cat in sorted({r["category"] for r in rows}):
        group = sorted([r for r in rows if r["category"] == cat], key=lambda r: r["name"].lower())
        lines += ["## " + cat + f" ({len(group)})", "", "| Project | License signal | Archived | Sources checked | Review |", "|---|---|---|---|---|"]
        for r in group:
            lines.append(f"| [{md(r['name'])}]({r['url']}) | {md(r['license_signal'])} | {md(r['archived'])} | {r['sources_ok']}/{r['sources_total']} | {md(r['review_status'])} |")
        lines.append("")
    atomic(output / "CATALOG.md", "\n".join(lines))
    harness_categories = {'coding-cli-and-workspace', 'personal-agent-runtime', 'general-or-specialist-agent',
                          'agent-runtime-reference', 'commercial-coding-product', 'research-agent',
                          'historical-harness', 'browser-and-computer-agent', 'editor-with-agent', 'assistant-platform', 'app-builder'}
    harness_rows = [r for r in rows if r['category'] in harness_categories]
    atomic(output / 'HARNESS_INDEX.md', '\n'.join([
        '# Harness and agent product index', '', f'{len(harness_rows)} runtime/product candidates from {len(rows)} ecosystem entries, generated {utc()}.',
        'Category assignments for discovered entries are provisional. Reachability does not prove installability or feature support. Archived and unavailable entries are retained for lineage.', '',
        '| Harness / agent product | Type | License signal | Lifecycle | Sources |', '|---|---|---|---|---|'] +
        [f"| [{md(r['name'])}]({r['url']}) | {md(r['category'])} | {md(r['license_signal'])} | {md(r['lifecycle_note']) if r['lifecycle_note'] else ('archived' if r['archived'] is True else 'unreviewed')} | {r['sources_ok']}/{r['sources_total']} |"
         for r in sorted(harness_rows, key=lambda x: x['name'].lower())]) + '\n')
    events = [dict(e) for e in db.execute("SELECT * FROM events ORDER BY at DESC,id DESC LIMIT 300")]
    pending = db.execute("SELECT count(*) FROM events WHERE status='pending'").fetchone()[0]
    digest = ["# Harness radar change review queue", "", f"Generated {utc()}. Pending: {pending}. Latest 300 events shown.", "",
              "Changes are unverified candidates. Baseline observations do not count as new features.", "",
              "| ID | Observed UTC | Project | Change | Feature axes | State |", "|---|---|---|---|---|---|"]
    for e in events:
        digest.append(f"| {e['id']} | {utc(e['at'])} | {md(e['project'])} | [{md(e['kind'])}]({e['url']}) | {md(', '.join(json.loads(e['axes'])))} | {md(e['status'])} |")
    if not events:
        digest += ["", "No changes since baseline yet."]
    digest += ["", "## DSCO triage routes", ""] + [f"- **{k}**: {v}." for k, v in ROADMAP.items()]
    atomic(output / "CHANGES.md", "\n".join(digest) + "\n")
    # A locally openable, searchable page; no server, CDN, tracking or external JS.
    heads = ("Project", "Category", "License signal", "Mentioned axes (unverified)", "Freshness")
    table = "".join("<tr>" + f'<td><a href="{html.escape(r["url"], quote=True)}">{html.escape(r["name"])}</a></td>' +
                    "".join(f"<td>{html.escape(str(v))}</td>" for v in (r["category"], r["license_signal"],
                    r["readme_axes_mentions_only"], f'{r["sources_ok"]}/{r["sources_total"]} checked · {r["source_errors"]} errors · {r["stale_sources"]} stale')) + "</tr>" for r in rows)
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="300"><title>DSCO · Harness radar</title>
<style>body{font:15px/1.5 system-ui;margin:3vw;color:#172b3b;background:#f7f8fa}h1{font-size:38px;margin-bottom:0}a{color:#125c9a}input{font:inherit;padding:12px;width:min(680px,90%);margin:20px 0}table{border-collapse:collapse;width:100%;background:white}th,td{text-align:left;padding:12px;border-bottom:1px solid #dde3e8}th{position:sticky;top:0;background:#e7edf2}p{max-width:1000px}.wrap{overflow:auto}</style>
<h1>Harness radar</h1><p>Local evidence for dsco-cli direction. Public-source polling; no recurring LLM calls.</p>
''' + f'<p>{len(rows)} projects · {pending} pending changes · generated {utc()}</p>' + '''
<p><a href="HARNESS_INDEX.md">Harness index</a> · <a href="REVIEW.md">Comparison</a> · <a href="FEATURES.md">Feature matrix</a> · <a href="CHANGES.md">Change queue</a> · <a href="catalog.csv">CSV</a> · <a href="source-status.json">Source health</a> · <a href="interop-conformance.json">Interop conformance</a> · <a href="LOCAL_INVENTORY.md">Local inventory</a></p>
<p>Broad ecosystem discovery, not an exhaustive census. A keyword mention does not prove support. Errors and stale sources remain visible.</p>
<label for="q">Filter projects, categories, licenses or feature mentions</label><br><input id="q" type="search" placeholder="e.g. coding-cli, MCP, MIT, recovery">
<p id="count"></p><div class="wrap"><table><thead><tr>''' + "".join(f"<th>{h}</th>" for h in heads) + "</tr></thead><tbody>" + table + '''</tbody></table></div>
<script>const q=document.getElementById('q'),rows=[...document.querySelectorAll('tbody tr')];function filter(){let n=0;for(const r of rows){r.hidden=!r.textContent.toLowerCase().includes(q.value.toLowerCase());if(!r.hidden)n++}document.getElementById('count').textContent=n+' matching projects';sessionStorage.setItem('harnessQuery',q.value)}q.value=sessionStorage.getItem('harnessQuery')||'';q.addEventListener('input',filter);filter();</script></html>'''
    atomic(output / "index.html", page)
    current_ids = {p["repo"].lower() for p in registry["projects"] if p.get("repo")}
    candidates = []
    for s in source_rows:
        if s["kind"] in ("directory", "directory_md") and states.get(s["url"], {}).get("body"):
            candidates.extend({"repo": repo, "source": s["url"], "status": "needs classification and primary verification"}
                              for repo in json.loads(states[s["url"]]["body"])
                              if registry.get('repo_aliases', {}).get(repo.lower(), repo).lower() not in current_ids and repo.lower() not in current_ids)
        if s['kind'] == 'directory_acp' and states.get(s['url'], {}).get('body'):
            acp = json.loads(states[s['url']]['body'])
            atomic(output / 'acp-registry.json', json.dumps(acp, indent=2))
            known_urls = {p['url'].rstrip('/').lower() for p in registry['projects']}
            for a in acp:
                url = a.get('repository') or a.get('website')
                match = re.match(r'https://github.com/([^/]+/[^/]+)', url)
                canonical = registry.get('repo_aliases', {}).get(match[1].lower(), match[1]).lower() if match else None
                if url and url.rstrip('/').lower() not in known_urls and (not canonical or canonical not in current_ids):
                    candidates.append({'repo': match[1] if match else None, 'url': url, 'name': a['name'],
                                       'source': s['url'], 'status': 'ACP descriptor discovered; assess adapter versus runtime'})
    # Merge repeated discoveries from several indexes without losing attribution.
    merged = {}
    for c in candidates:
        key = (c.get('repo') or c.get('url', '')).lower()
        if key not in merged:
            merged[key] = {**c, 'sources': [c['source']]}
        elif c['source'] not in merged[key]['sources']:
            merged[key]['sources'].append(c['source'])
    decisions = {k.lower(): v for k, v in registry.get('discovery_decisions', {}).items()}
    excluded = [{**c, 'decision': decisions[k]} for k, c in merged.items() if k in decisions]
    candidates = [c for k, c in merged.items() if k not in decisions]
    atomic(output / 'discovery-exclusions.json', json.dumps(excluded, indent=2))
    atomic(output / "discovery-candidates.json", json.dumps(candidates, indent=2))


def local_inventory(registry, roots, state, output, depth):
    prune = {"Library", ".git", "node_modules", ".venv", "venv", "target", "build", "dist", "__pycache__",
             "vendor", "data", "logs", "sessions", "memory", "Movies", "Music", "Pictures"}
    by_repo = {p["repo"].lower(): p["id"] for p in registry["projects"] if p.get("repo")}
    aliases = registry.get("repo_aliases", {})
    observations = []; errors = []; scanned = 0; seen = set()
    def onerror(exc):
        errors.append(str(exc))
    for base in roots:
        root = Path(base).expanduser().resolve()
        for path, dirs, files in os.walk(root, onerror=onerror):
            p = Path(path); level = len(p.relative_to(root).parts)
            dirs[:] = [d for d in dirs if d not in prune and not d.startswith(".") and not (p/d).is_symlink()]
            if level >= depth:
                dirs[:] = []
            scanned += 1
            if scanned > 100000:
                errors.append("100000-directory cap reached"); break
            cfg = p / ".git/config"
            if not cfg.is_file():
                continue
            parser = configparser.RawConfigParser()
            try:
                parser.read(cfg)
                remotes = [parser.get(sec, "url", fallback="") for sec in parser.sections() if sec.startswith("remote ")]
            except configparser.Error:
                continue
            for url in remotes:
                match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
                if not match:
                    continue
                repo = match.group(1); repo = aliases.get(repo.lower(), repo)
                ident = by_repo.get(repo.lower())
                reconstruction = "nirholas/claude-code" in repo.lower()
                if not ident and not reconstruction:
                    continue
                if str(p) in seen:
                    continue
                seen.add(str(p))
                # Never run checked-out code, fetch, pull, or read credentials.
                rev = subprocess.run(["git", "-C", str(p), "log", "-1", "--format=%H %cI"], capture_output=True, text=True, timeout=5)
                observations.append({"project": ident or "claude-reconstruction", "path": str(p),
                    "repo": repo, "revision_and_date": rev.stdout.strip(),
                    "kind": "third-party reconstruction; not authoritative upstream" if reconstruction else "git checkout"})
    for path in registry.get("local_evidence_paths", []):
        p = Path(path).expanduser()
        if p.exists():
            observations.append({"project": "local-evidence", "path": str(p), "kind": "skill/reference directory; inspect provenance", "repo": "", "revision_and_date": ""})
    installed = []
    for name in ("codex", "claude", "opencode", "omp", "hermes", "pi", "gemini", "aider", "goose", "crush", "kimi", "qwen"):
        exe = shutil.which(name)
        if not exe:
            continue
        try:
            run = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=12)
            installed.append({"name": name, "path": exe, "version": (run.stdout + run.stderr).strip()[:500], "exit_code": run.returncode})
        except subprocess.TimeoutExpired:
            installed.append({"name": name, "path": exe, "version": "version probe timed out"})
    record_ = {"observed_at": utc(), "roots": roots, "max_depth": depth, "directories_scanned": scanned,
               "pruned": sorted(prune), "hidden_directories_skipped": True, "errors": errors,
               "repositories": observations, "installed": installed,
               "limitations": "No symlink traversal, git worktree .git-file resolution, archives, package caches or full disk census. Version output is not a feature test."}
    atomic(state / "local-inventory.json", json.dumps(record_, indent=2))
    atomic(output / "local-inventory.json", json.dumps(record_, indent=2))
    lines = ["# Local harness evidence", "", f"Observed {utc()}; {scanned} directories scanned (depth {depth}).", "",
             "## Installed commands", "", "| Command | Version | Path |", "|---|---|---|"]
    lines += [f"| {md(x['name'])} | {md(x['version'])} | {md(x['path'])} |" for x in installed]
    lines += ["", "## Checkouts and references", "", "| Project | Kind | Path | Revision/date |", "|---|---|---|---|"]
    lines += [f"| {md(x['project'])} | {md(x['kind'])} | {md(x['path'])} | {md(x['revision_and_date'])} |" for x in observations]
    lines += ["", record_["limitations"], "", "Skills and reconstructed sources do not establish current official behavior."]
    atomic(output / "LOCAL_INVENTORY.md", "\n".join(lines) + "\n")
    print(json.dumps({"directories_scanned": scanned, "evidence_locations": len(observations), "installed_commands": len(installed), "errors": len(errors)}))


def launchd(args):
    if sys.platform != "darwin":
        raise SystemExit("launchd commands require macOS; run sync with cron/systemd elsewhere")
    path = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")
    target = f"gui/{os.getuid()}"
    if args.command == "uninstall":
        subprocess.run(["launchctl", "bootout", target + "/" + LABEL], capture_output=True)
        path.unlink(missing_ok=True)
        print("Removed " + str(path)); return
    args.state.mkdir(parents=True, exist_ok=True); path.parent.mkdir(parents=True, exist_ok=True)
    content = {"Label": LABEL, "ProgramArguments": [sys.executable, str(Path(__file__).resolve()),
               "--registry", str(args.registry.resolve()), "--state", str(args.state.resolve()),
               "--output", str(args.output.resolve()), "sync", "--max-requests", "32", "--max-api", "2"],
               "StartInterval": 900, "RunAtLoad": True, "WorkingDirectory": str(ROOT),
               "ProcessType": "Background", "LowPriorityIO": True, "Nice": 10,
               "StandardOutPath": "/dev/null", "StandardErrorPath": "/dev/null"}
    if path.exists() and plistlib.loads(path.read_bytes()) != content:
        raise SystemExit("An existing different radar job exists; uninstall it explicitly before replacing")
    path.write_bytes(plistlib.dumps(content)); path.chmod(0o600)
    status = subprocess.run(["launchctl", "print", target + "/" + LABEL], capture_output=True)
    if status.returncode:
        subprocess.run(["launchctl", "bootstrap", target, str(path)], check=True)
    print("Installed " + str(path))
    subprocess.run(["launchctl", "print", target + "/" + LABEL], check=True)


def run_interop_conformance(output):
    """Run the provider-free installed-harness contract probe after each sync."""
    script = ROOT / "scripts" / "agent_interop_conformance.py"
    candidates = [
        os.environ.get("DSCO_INTEROP_BINARY", ""),
        str(Path.home() / ".local" / "bin" / "dsco"),
        "/opt/homebrew/bin/dsco",
        str(ROOT / "dsco"),
        shutil.which("dsco") or "",
    ]
    binary = next((path for path in candidates if path and os.access(path, os.X_OK)), None)
    target = output / "interop-conformance.json"
    if not binary:
        atomic(target, json.dumps({
            "schema": "dsco.agent.interop.conformance/v1",
            "observed_at": utc(),
            "ok": False,
            "error": "dsco_executable_not_found",
            "real_invocation": False,
        }, indent=2) + "\n")
        return False
    try:
        completed = subprocess.run(
            [sys.executable, str(script), "--binary", binary, "--output", str(target)],
            text=True, capture_output=True, timeout=90,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        atomic(target, json.dumps({
            "schema": "dsco.agent.interop.conformance/v1",
            "observed_at": utc(),
            "ok": False,
            "dsco_binary": binary,
            "error": type(exc).__name__,
            "real_invocation": False,
        }, indent=2) + "\n")
        return False
    if not target.exists():
        atomic(target, json.dumps({
            "schema": "dsco.agent.interop.conformance/v1",
            "observed_at": utc(),
            "ok": False,
            "dsco_binary": binary,
            "error": "probe_failed_without_report",
            "exit_code": completed.returncode,
            "real_invocation": False,
        }, indent=2) + "\n")
    return completed.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=REGISTRY)
    parser.add_argument("--state", type=Path, default=STATE)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    sub = parser.add_subparsers(dest="command", required=True)
    sync_p = sub.add_parser("sync")
    sync_p.add_argument("--max-requests", type=int, default=32)
    sync_p.add_argument("--max-api", type=int, default=2)
    sync_p.add_argument("--workers", type=int, default=4)
    sync_p.add_argument("--force", action="store_true")
    sync_p.add_argument("--only", default="")
    sub.add_parser("render"); sub.add_parser("status")
    sub.add_parser("install"); sub.add_parser("uninstall")
    local = sub.add_parser("inventory")
    local.add_argument("roots", nargs="*", default=[str(Path.home())])
    local.add_argument("--depth", type=int, default=6)
    diff = sub.add_parser("diff"); diff.add_argument("event", type=int)
    review = sub.add_parser("review"); review.add_argument("event", type=int)
    review.add_argument("--status", required=True, choices=["pending", "accepted", "rejected", "deferred"])
    review.add_argument("--note", required=True)
    args = parser.parse_args()
    registry = read_registry(args.registry)
    args.state.mkdir(parents=True, exist_ok=True)
    if args.command in ("install", "uninstall"):
        launchd(args); return
    with (args.state / "lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("Radar already running; skipped overlapping invocation"); return
        db = connect(args.state)
        if args.command == "inventory":
            local_inventory(registry, args.roots, args.state, args.output, args.depth)
        elif args.command == "sync":
            if not 1 <= args.max_requests <= 2000 or not 1 <= args.workers <= 8 or args.max_api < 0:
                raise SystemExit("Invalid request, worker or API budget")
            sync(db, registry, args); render(db, registry, args.output)
            print(json.dumps({"interop_conformance": run_interop_conformance(args.output),
                              "real_model_calls": 0}))
        elif args.command == "render":
            render(db, registry, args.output)
        elif args.command == "status":
            active_urls = {s['url'] for p in registry['projects'] for s in p.get('sources', [])}
            active = [dict(s) for s in db.execute('SELECT * FROM sources') if s['url'] in active_urls]
            print(json.dumps({"state": str(args.state), "projects": len(registry["projects"]),
                "sources": len(active_urls),
                "sources_with_baseline": sum(bool(s['sha']) for s in active),
                "source_errors": sum(bool(s['error']) for s in active),
                "sources_waiting_for_baseline": len(active_urls) - sum(bool(s['sha']) for s in active),
                "due_sources": sum(s['due'] <= time.time() for s in active),
                "pending_changes": db.execute("SELECT count(*) FROM events WHERE status='pending'").fetchone()[0],
                "last_runs": [dict(r) for r in db.execute("SELECT * FROM runs ORDER BY id DESC LIMIT 3")]}, indent=2))
        elif args.command == "diff":
            row = db.execute("SELECT * FROM events WHERE id=?", (args.event,)).fetchone()
            if not row: raise SystemExit("Unknown event")
            print(row["url"] + "\n" + row["diff"])
        elif args.command == "review":
            cursor = db.execute("UPDATE events SET status=?,note=? WHERE id=?", (args.status, args.note, args.event))
            if not cursor.rowcount: raise SystemExit("Unknown event")
            db.commit(); render(db, registry, args.output)
            print("Review saved. Curated feature assertions were not changed.")
        db.close()


if __name__ == "__main__":
    main()
