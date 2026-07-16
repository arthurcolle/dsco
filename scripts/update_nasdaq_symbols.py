#!/usr/bin/env python3
"""Download and normalize Nasdaq Trader Symbol Directory equities and bonds."""
from __future__ import annotations

import csv
import hashlib
import json
import os
import tempfile
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

BASE_URL = "https://www.nasdaqtrader.com/dynamic/SymDir"
FILES = ("nasdaqlisted.txt", "otherlisted.txt", "bondslist.txt")
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data" / "nasdaq_symbol_directory"
RAW = OUT / "raw"
USER_AGENT = "dsco-cli nasdaq-symbol-tracker/1.0"


def download(name: str) -> tuple[Path, str]:
    RAW.mkdir(parents=True, exist_ok=True)
    url = f"{BASE_URL}/{name}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as response:
        data = response.read()
    if not data or b"File Creation Time:" not in data:
        raise RuntimeError(f"invalid Symbol Directory response for {name}")
    fd, temporary = tempfile.mkstemp(dir=RAW, prefix=f".{name}.")
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, RAW / name)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return RAW / name, hashlib.sha256(data).hexdigest()


def rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle, delimiter="|")
        return [dict(row) for row in reader if row and not (next(iter(row.values()), "").startswith("File Creation Time:"))]


def write_csv(path: Path, records: list[dict[str, str]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)


def main() -> None:
    hashes = {}
    paths = {}
    for name in FILES:
        paths[name], hashes[name] = download(name)

    equities = []
    for row in rows(paths["nasdaqlisted.txt"]):
        equities.append({
            "symbol": row["Symbol"], "name": row["Security Name"],
            "exchange": "Q", "exchange_name": "Nasdaq",
            "etf": row["ETF"], "test_issue": row["Test Issue"],
            "financial_status": row["Financial Status"],
        })
    exchange_names = {"A": "NYSE American", "N": "NYSE", "P": "NYSE Arca", "Z": "Cboe BZX", "V": "IEX"}
    for row in rows(paths["otherlisted.txt"]):
        equities.append({
            "symbol": row["ACT Symbol"], "name": row["Security Name"],
            "exchange": row["Exchange"], "exchange_name": exchange_names.get(row["Exchange"], row["Exchange"]),
            "etf": row["ETF"], "test_issue": row["Test Issue"], "financial_status": "",
        })
    bonds = [{"symbol": r["Symbol"], "name": r["Security Name"], "financial_status": r["Financial Status"]}
             for r in rows(paths["bondslist.txt"])]
    equities.sort(key=lambda r: (r["symbol"], r["exchange"]))
    bonds.sort(key=lambda r: r["symbol"])
    write_csv(OUT / "equities.csv", equities, ["symbol", "name", "exchange", "exchange_name", "etf", "test_issue", "financial_status"])
    write_csv(OUT / "bonds.csv", bonds, ["symbol", "name", "financial_status"])
    manifest = {
        "source": BASE_URL, "retrieved_at_utc": datetime.now(timezone.utc).isoformat(),
        "counts": {"equities": len(equities), "bonds": len(bonds)}, "sha256": hashes,
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
