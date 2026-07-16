#!/usr/bin/env python3
"""Fetch prices for tracked Nasdaq Symbol Directory instruments in parallel."""
from __future__ import annotations

import argparse, concurrent.futures, csv, json, os, tempfile, time, urllib.error, urllib.parse, urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data" / "nasdaq_symbol_directory"
UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/126 Safari/537.36"
FIELDS = ["symbol", "name", "instrument_type", "price", "currency", "change", "percent_change", "volume", "market_time_utc", "market_state", "status", "source"]


def atomic_csv(path: Path, rows: list[dict]) -> None:
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(fd, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS); w.writeheader(); w.writerows(rows); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)


def get_json(url: str, attempts: int = 3):
    for attempt in range(attempts):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json,text/plain,*/*"})
            with urllib.request.urlopen(req, timeout=30) as r: return json.load(r)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
            if attempt + 1 == attempts: raise
            time.sleep(0.5 * 2**attempt)


def equities() -> list[dict]:
    url = "https://api.nasdaq.com/api/screener/stocks?tableonly=true&limit=20000&offset=0&download=true"
    remote = {r["symbol"]: r for r in get_json(url)["data"]["rows"]}
    with (DATA / "equities.csv").open(newline="", encoding="utf-8") as f: tracked = list(csv.DictReader(f))
    out = []
    for t in tracked:
        r = remote.get(t["symbol"]); raw = (r or {}).get("lastsale", "").replace("$", "").replace(",", "")
        out.append({"symbol": t["symbol"], "name": t["name"], "instrument_type": "equity", "price": raw,
                    "currency": "USD" if raw else "", "change": (r or {}).get("netchange", ""),
                    "percent_change": (r or {}).get("pctchange", "").replace("%", ""), "volume": (r or {}).get("volume", ""),
                    "market_time_utc": "", "market_state": "", "status": "priced" if raw else "unavailable",
                    "source": "nasdaq_api" if raw else ""})
    return out


def yahoo_one(t: dict) -> dict:
    symbol = t["symbol"]
    url = "https://query1.finance.yahoo.com/v8/finance/chart/" + urllib.parse.quote(symbol, safe="") + "?interval=1d&range=5d"
    base = {"symbol": symbol, "name": t["name"], "instrument_type": "bond", "price": "", "currency": "", "change": "", "percent_change": "", "volume": "", "market_time_utc": "", "market_state": "", "status": "unavailable", "source": ""}
    try:
        result = get_json(url, 2)["chart"]["result"][0]; m = result["meta"]
        price = m.get("regularMarketPrice"); previous = m.get("chartPreviousClose") or m.get("previousClose")
        if price is None: return base
        base.update(price=price, currency=m.get("currency", ""), market_state=m.get("marketState", ""), status="priced", source="yahoo_chart")
        if previous: base.update(change=price-previous, percent_change=(price/previous-1)*100)
        if m.get("regularMarketTime"): base["market_time_utc"] = datetime.fromtimestamp(m["regularMarketTime"], timezone.utc).isoformat()
    except Exception as e:
        base["status"] = "unavailable"
    return base


def bonds(workers: int) -> list[dict]:
    with (DATA / "bonds.csv").open(newline="", encoding="utf-8") as f: tracked = list(csv.DictReader(f))
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool: return list(pool.map(yahoo_one, tracked))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--workers", type=int, default=24); args = ap.parse_args()
    DATA.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        ef = pool.submit(equities); bf = pool.submit(bonds, args.workers); erows, brows = ef.result(), bf.result()
    atomic_csv(DATA / "equity_prices.csv", erows); atomic_csv(DATA / "bond_prices.csv", brows)
    summary = {"retrieved_at_utc": datetime.now(timezone.utc).isoformat(), "workers": args.workers,
               "equities": {"tracked": len(erows), "priced": sum(r["status"] == "priced" for r in erows)},
               "bonds": {"tracked": len(brows), "priced": sum(r["status"] == "priced" for r in brows)},
               "notes": "Blank/unavailable means no current quote was returned; it is not a zero price."}
    (DATA / "prices_manifest.json").write_text(json.dumps(summary, indent=2) + "\n"); print(json.dumps(summary, indent=2))

if __name__ == "__main__": main()
