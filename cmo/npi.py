#!/usr/bin/env python3
"""NPI healthcare-provider shard: every US provider from the CMS NPPES bulk file.

Streams the main npidata CSV directly out of the dissemination zip (no 11GB
extraction) into SQLite. Individuals only (Entity Type 1) — each row is a real
person keyed by their CMS-issued NPI, with credential, sex, practice location,
and every taxonomy slot (specialty code + state license number) they hold.
State license numbers are the competence/reliability anchor: a code alone is a
claim, a license is a registration.

The NUCC taxonomy table (code -> classification/specialization) is loaded
separately so specialty codes resolve to human-readable specialties.

Run: python3 -m cmo.npi --ingest data/people/nppes.zip [--db data/people/npi.db]
     python3 -m cmo.npi --taxonomy            # fetch NUCC code->name table
"""
import argparse
import csv
import io
import os
import sqlite3
import sys
import zipfile

import requests

SCHEMA = """
CREATE TABLE IF NOT EXISTS npi_person (
    npi          INTEGER PRIMARY KEY,  -- CMS National Provider Identifier
    last_name    TEXT,
    first_name   TEXT,
    middle_name  TEXT,
    credential   TEXT,                 -- e.g. MD, DDS, RN, PA-C
    sex          TEXT,
    city         TEXT,                 -- practice location
    state        TEXT,
    enumeration_date TEXT,
    last_update  TEXT,
    deactivation_date TEXT,
    primary_taxonomy TEXT              -- primary specialty code
);
CREATE INDEX IF NOT EXISTS idx_npi_name  ON npi_person(last_name, first_name);
CREATE INDEX IF NOT EXISTS idx_npi_state ON npi_person(state);
CREATE TABLE IF NOT EXISTS npi_taxonomy (
    npi           INTEGER NOT NULL,
    taxonomy_code TEXT,                -- NUCC specialty code
    license       TEXT,                -- state license number (if registered)
    license_state TEXT,
    is_primary    INTEGER,
    PRIMARY KEY (npi, taxonomy_code)
);
CREATE INDEX IF NOT EXISTS idx_npitax_code ON npi_taxonomy(taxonomy_code);
CREATE TABLE IF NOT EXISTS nucc_code (
    code           TEXT PRIMARY KEY,
    grouping       TEXT,
    classification TEXT,               -- e.g. Internal Medicine
    specialization TEXT                -- e.g. Cardiovascular Disease
);
"""

# main-file column offsets (npidata_pfile, 330 cols)
C_NPI, C_ENTITY = 0, 1
C_LAST, C_FIRST, C_MIDDLE, C_CRED = 5, 6, 7, 10
C_CITY, C_STATE = 30, 31
C_ENUM, C_UPD, C_DEACT = 36, 37, 39
C_SEX = 41
C_TAX0, N_TAX = 47, 15                 # 15 slots x (code, license, state, primary)


def ensure(conn):
    conn.executescript(SCHEMA)
    conn.commit()


def ingest(conn, zip_path, batch=20000):
    z = zipfile.ZipFile(zip_path)
    main = [i.filename for i in z.infolist()
            if i.filename.startswith("npidata_pfile")
            and "fileheader" not in i.filename][0]
    print(f"streaming {main} from {zip_path}", flush=True)
    people, taxes, n_seen, n_kept = [], [], 0, 0
    with z.open(main) as f:
        r = csv.reader(io.TextIOWrapper(f, "utf-8"))
        next(r)                          # header
        for row in r:
            n_seen += 1
            if row[C_ENTITY] != "1":     # individuals only
                continue
            npi = int(row[C_NPI])
            primary = None
            for s in range(N_TAX):
                base = C_TAX0 + 4 * s
                code = row[base]
                if not code or code == "<UNAVAIL>":
                    continue
                pri = 1 if row[base + 3] == "Y" else 0
                if pri and primary is None:
                    primary = code
                taxes.append((npi, code, row[base + 1] or None,
                              row[base + 2] or None, pri))
            people.append((npi, row[C_LAST], row[C_FIRST], row[C_MIDDLE] or None,
                           row[C_CRED] or None, row[C_SEX] or None,
                           row[C_CITY] or None, row[C_STATE] or None,
                           row[C_ENUM] or None, row[C_UPD] or None,
                           row[C_DEACT] or None, primary))
            n_kept += 1
            if len(people) >= batch:
                _flush(conn, people, taxes)
                people, taxes = [], []
                if n_kept % 500000 < batch:
                    print(f"  ...{n_kept} individuals ({n_seen} rows scanned)",
                          flush=True)
    _flush(conn, people, taxes)
    print(f"done: {n_kept} individuals of {n_seen} records", flush=True)
    return n_kept


def _flush(conn, people, taxes):
    conn.executemany(
        """INSERT OR REPLACE INTO npi_person
           (npi,last_name,first_name,middle_name,credential,sex,city,state,
            enumeration_date,last_update,deactivation_date,primary_taxonomy)
           VALUES(?,?,?,?,?,?,?,?,?,?,?,?)""", people)
    conn.executemany(
        """INSERT OR REPLACE INTO npi_taxonomy
           (npi,taxonomy_code,license,license_state,is_primary)
           VALUES(?,?,?,?,?)""", taxes)
    conn.commit()


def load_nucc(conn):
    """NUCC taxonomy csv: code -> grouping/classification/specialization."""
    for ver in ("251", "250", "241", "240"):
        url = (f"https://www.nucc.org/images/stories/CSV/"
               f"nucc_taxonomy_{ver}.csv")
        try:
            r = requests.get(url, timeout=60,
                             headers={"User-Agent": "Mozilla/5.0"})
        except requests.RequestException:
            continue
        if r.status_code != 200 or "," not in r.text[:200]:
            continue
        rows = list(csv.reader(io.StringIO(r.text)))
        hdr = [h.strip().lower() for h in rows[0]]
        ix = {k: hdr.index(k) for k in
              ("code", "grouping", "classification", "specialization")}
        conn.executemany(
            "INSERT OR REPLACE INTO nucc_code VALUES(?,?,?,?)",
            [(x[ix["code"]], x[ix["grouping"]], x[ix["classification"]],
              x[ix["specialization"]]) for x in rows[1:] if x and x[ix["code"]]])
        conn.commit()
        n = conn.execute("SELECT COUNT(*) FROM nucc_code").fetchone()[0]
        print(f"loaded {n} NUCC codes from {url}")
        return n
    print("could not fetch any NUCC taxonomy csv", file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/people/npi.db")
    ap.add_argument("--ingest", metavar="ZIP",
                    help="NPPES dissemination zip to stream-load")
    ap.add_argument("--taxonomy", action="store_true",
                    help="fetch NUCC code->specialty table")
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.db), exist_ok=True)
    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    ensure(conn)
    if args.taxonomy:
        load_nucc(conn)
    if args.ingest:
        ingest(conn, args.ingest)


if __name__ == "__main__":
    main()
