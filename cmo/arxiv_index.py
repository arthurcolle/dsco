#!/usr/bin/env python3
"""Index ALL arXiv publications and their authors — the researcher shard.

Harvests the complete arXiv metadata corpus (~2.5M papers) from the official
OAI-PMH endpoint (export.arxiv.org/oai2, metadataPrefix=arXiv) into a local
SQLite shard. Every paper keeps its real arXiv id; every author row is exactly
what arXiv publishes (keyname/forenames/affiliation per paper) — author rows
are per-paper attributions, joinable across the people graph by name key.

Resumable two ways: the live OAI resumptionToken (same run) and the last
datestamp window (across runs, via ax_progress). Polite: honors 503
Retry-After and spaces requests ~3s as arXiv asks.

Run: python3 -m cmo.arxiv_index [--db data/people/arxiv.db] [--from YYYY-MM-DD]
"""
import argparse
import os
import re
import sqlite3
import time
import xml.etree.ElementTree as ET

import requests

OAI = "https://export.arxiv.org/oai2"
NS = {"oai": "http://www.openarchives.org/OAI/2.0/",
      "ax": "http://arxiv.org/OAI/arXiv/"}
UA = "dsco-research arthurcolle@gmail.com"

SCHEMA = """
CREATE TABLE IF NOT EXISTS ax_paper (
    id          TEXT PRIMARY KEY,      -- arXiv id, e.g. 2403.01234 / hep-th/9901001
    title       TEXT,
    abstract    TEXT,
    categories  TEXT,                  -- space-separated arXiv categories
    created     TEXT,                  -- first submission date
    updated     TEXT,
    doi         TEXT,
    journal_ref TEXT
);
CREATE TABLE IF NOT EXISTS ax_paper_author (
    paper_id    TEXT NOT NULL,
    pos         INTEGER NOT NULL,      -- author order on the paper
    keyname     TEXT,                  -- surname as arXiv publishes it
    forenames   TEXT,
    affiliation TEXT,
    PRIMARY KEY (paper_id, pos)
);
CREATE INDEX IF NOT EXISTS idx_ax_author_name
    ON ax_paper_author(keyname, forenames);
CREATE TABLE IF NOT EXISTS ax_progress (
    kind  TEXT PRIMARY KEY,
    value TEXT,
    ts    TEXT DEFAULT (datetime('now'))
);
"""


def ensure(conn):
    conn.executescript(SCHEMA)
    conn.commit()


def _fetch(params, tries=8):
    for i in range(tries):
        try:
            r = requests.get(OAI, params=params, timeout=120,
                             headers={"User-Agent": UA})
        except requests.RequestException:
            time.sleep(5 * (i + 1))
            continue
        if r.status_code == 200:
            # OAI XML is UTF-8; don't let requests guess latin-1 from headers
            return r.content.decode("utf-8", "replace")
        if r.status_code == 503:           # arXiv flow control
            m = re.search(r"\d+", r.headers.get("Retry-After", ""))
            time.sleep(min(int(m.group()) if m else 20, 300))
            continue
        time.sleep(10 * (i + 1))
    raise RuntimeError(f"OAI fetch kept failing: {params}")


def _text(el, tag):
    x = el.find(f"ax:{tag}", NS)
    return x.text.strip() if x is not None and x.text else None


def _parse(xml_text):
    """-> (records, resumptionToken, cursor, total)"""
    root = ET.fromstring(xml_text)
    out = []
    lr = root.find("oai:ListRecords", NS)
    if lr is None:
        return out, None, None, None
    for rec in lr.findall("oai:record", NS):
        meta = rec.find("oai:metadata/ax:arXiv", NS)
        if meta is None:                  # deleted record header
            continue
        authors = []
        for i, a in enumerate(meta.findall("ax:authors/ax:author", NS)):
            authors.append((i, _text(a, "keyname"), _text(a, "forenames"),
                            _text(a, "affiliation")))
        out.append({"id": _text(meta, "id"),
                    "title": _text(meta, "title"),
                    "abstract": _text(meta, "abstract"),
                    "categories": _text(meta, "categories"),
                    "created": _text(meta, "created"),
                    "updated": _text(meta, "updated"),
                    "doi": _text(meta, "doi"),
                    "journal_ref": _text(meta, "journal-ref"),
                    "authors": authors})
    tok = lr.find("oai:resumptionToken", NS)
    if tok is not None:
        return (out, (tok.text or "").strip() or None,
                tok.get("cursor"), tok.get("completeListSize"))
    return out, None, None, None


def _store(conn, recs):
    for p in recs:
        conn.execute(
            """INSERT OR REPLACE INTO ax_paper
               (id,title,abstract,categories,created,updated,doi,journal_ref)
               VALUES(?,?,?,?,?,?,?,?)""",
            (p["id"], p["title"], p["abstract"], p["categories"],
             p["created"], p["updated"], p["doi"], p["journal_ref"]))
        conn.execute("DELETE FROM ax_paper_author WHERE paper_id=?", (p["id"],))
        for pos, key, fore, aff in p["authors"]:
            conn.execute(
                """INSERT OR REPLACE INTO ax_paper_author
                   (paper_id,pos,keyname,forenames,affiliation)
                   VALUES(?,?,?,?,?)""", (p["id"], pos, key, fore, aff))


def harvest(conn, frm=None):
    row = conn.execute(
        "SELECT value FROM ax_progress WHERE kind='last_datestamp'").fetchone()
    if frm is None and row:
        frm = row[0]
    params = {"verb": "ListRecords", "metadataPrefix": "arXiv"}
    if frm:
        params["from"] = frm
    print(f"harvesting arXiv OAI from={frm or 'beginning'}", flush=True)
    total = 0
    last_stamp = frm
    while True:
        text = _fetch(params)
        recs, tok, cursor, size = _parse(text)
        if recs:
            _store(conn, recs)
            last_stamp = max((p["updated"] or p["created"] or "")
                             for p in recs) or last_stamp
            conn.execute(
                """INSERT INTO ax_progress(kind,value,ts)
                   VALUES('last_datestamp',?,datetime('now'))
                   ON CONFLICT(kind) DO UPDATE SET value=excluded.value,
                     ts=excluded.ts""", (last_stamp,))
            conn.commit()
            total += len(recs)
        pages = total // 1300
        if pages % 10 == 0:
            print(f"  ...{cursor or pages}/{size or '?'} "
                  f"(+{len(recs)}, total {total})", flush=True)
        if not tok:
            break
        params = {"verb": "ListRecords", "resumptionToken": tok}
        time.sleep(3)
    print(f"done: {total} records this run", flush=True)
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/people/arxiv.db")
    ap.add_argument("--from", dest="frm",
                    help="harvest records updated since YYYY-MM-DD")
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.db), exist_ok=True)
    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    ensure(conn)
    harvest(conn, args.frm)


if __name__ == "__main__":
    main()
