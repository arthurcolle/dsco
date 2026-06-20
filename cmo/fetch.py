#!/usr/bin/env python3
"""Resolve EDGAR accessions to their primary documents and download them.

Each filing is a directory on EDGAR; index.json lists its documents. We pick the
documents worth parsing (ABS-EE asset XML, 424B prospectus HTML) and cache them
locally so the parsers run offline and idempotently.
"""
import os, re, sqlite3
import requests

UA = "dsco-research arthurcolle@gmail.com"
DOCROOT = "data/edgar/docs"
_session = None


def session():
    global _session
    if _session is None:
        _session = requests.Session()
        _session.headers.update({"User-Agent": UA})
    return _session


def _accnd(acc):
    return acc.replace("-", "")


def list_docs(cik, acc):
    """Return [(name, type, size)] for a filing."""
    u = f"https://www.sec.gov/Archives/edgar/data/{cik}/{_accnd(acc)}/index.json"
    j = session().get(u, timeout=60).json()
    return [(it["name"], it.get("type", ""), it.get("size", "")) for it in j["directory"]["item"]]


def doc_url(cik, acc, name):
    return f"https://www.sec.gov/Archives/edgar/data/{cik}/{_accnd(acc)}/{name}"


def fetch_doc(cik, acc, name, cache=DOCROOT):
    """Download one document to cache; return local path. Streams large files."""
    d = os.path.join(cache, str(cik), _accnd(acc))
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, name)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    with session().get(doc_url(cik, acc, name), stream=True, timeout=300) as r:
        r.raise_for_status()
        with open(path, "wb") as f:
            for chunk in r.iter_content(chunk_size=1 << 20):
                f.write(chunk)
    return path


def pick(docs, *exts, contains=None):
    """First doc name whose lowercase name ends with any ext (and contains substr)."""
    for name, _t, _s in docs:
        n = name.lower()
        if contains and contains not in n:
            continue
        if any(n.endswith(e) for e in exts):
            return name
    return None


def absee_xml(cik, acc):
    """Return local path to the ABS-EE asset-data XML (ex102), downloading it."""
    docs = list_docs(cik, acc)
    name = pick(docs, ".xml", contains="ex102") or pick(docs, ".xml", contains="assetdata")
    if not name:
        # fall back to the largest .xml (asset tape dwarfs the others)
        xmls = [(n, int(s) if str(s).isdigit() else 0) for n, _t, s in docs if n.lower().endswith(".xml")]
        name = max(xmls, key=lambda x: x[1])[0] if xmls else None
    return fetch_doc(cik, acc, name) if name else None


def prospectus_html(cik, acc):
    """Return local path to the 424B prospectus HTML (the largest .htm)."""
    docs = list_docs(cik, acc)
    htms = [(n, int(s) if str(s).isdigit() else 0)
            for n, _t, s in docs if n.lower().endswith((".htm", ".html"))
            and "index" not in n.lower()]
    if not htms:
        return None
    name = max(htms, key=lambda x: x[1])[0]
    return fetch_doc(cik, acc, name)


_HDR_CACHE = {}


def issuing_entity(cik, acc):
    """The issuing-entity conformed name (carries the deal series), from the SEC
    submission header. The full-index name is often the depositor; this is the
    actual trust, e.g. 'World Omni Auto Receivables Trust 2022-A'."""
    key = (cik, acc)
    if key in _HDR_CACHE:
        return _HDR_CACHE[key]
    u = f"https://www.sec.gov/Archives/edgar/data/{cik}/{_accnd(acc)}/{acc}-index-headers.html"
    try:
        txt = re.sub(r"<[^>]+>", " ", session().get(u, timeout=60).text)
    except Exception:
        _HDR_CACHE[key] = None
        return None
    # A filing carries several FILER blocks (depositor LLC, issuing trust, ...).
    # The issuing entity is the one bearing the deal series (e.g. '2025-P4',
    # '2022-A'), usually a later block. Prefer a series-bearing name; among those
    # prefer one that says TRUST; otherwise fall back to the first conformed name.
    names = [n.strip() for n in
             re.findall(r"COMPANY CONFORMED NAME:\s*([^\n\r]+)", txt)]
    if not names:
        _HDR_CACHE[key] = None
        return None
    series = re.compile(r"\b\d{4}-[A-Z0-9]+\b")
    serial = [n for n in names if series.search(n)]
    pool = serial or names
    trust = [n for n in pool if "TRUST" in n.upper()]
    name = (trust or pool)[0]
    _HDR_CACHE[key] = name
    return name


def deal_key(name):
    """Normalize an issuing-entity name to a stable join key shared across the
    collateral (ABS-EE) and bond (424B5) filings of the same deal."""
    if not name:
        return None
    k = name.upper()
    k = re.sub(r"[.,]", "", k)
    k = re.sub(r"\s+", " ", k).strip()
    return k


def ensure_db(db_path):
    conn = sqlite3.connect(db_path)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "deal_schema.sql")) as f:
        conn.executescript(f.read())
    return conn
