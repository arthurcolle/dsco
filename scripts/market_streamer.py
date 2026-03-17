#!/usr/bin/env python3
"""
market_streamer.py — Websocket delta streamer for Kalshi + Polymarket.

Connects to both exchanges' websocket feeds, stores every trade, ticker update,
and orderbook delta into SQLite for predictive modeling.

Usage:
    python3 scripts/market_streamer.py [--kalshi] [--poly] [--all-markets]

Env vars required:
    KALSHI_API_KEY, KALSHI_RSA_PRIVATE_KEY_PATH
    POLYMARKET_API_KEY, POLYMARKET_API_SECRET, POLYMARKET_PASSPHRASE (optional, for user channel)
"""

import asyncio
import json
import os
import signal
import sqlite3
import sys
import time
import base64
import hashlib
import threading
from datetime import datetime, timezone
from pathlib import Path

try:
    import websockets
except ImportError:
    print("pip install websockets")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)

# ── Config ──────────────────────────────────────────────────────────────────

DB_PATH = os.environ.get("DSCO_MARKET_DB", str(Path(__file__).parent.parent / ".workspace" / "market_deltas.db"))
KALSHI_WS = "wss://api.elections.kalshi.com/trade-api/ws/v2"
KALSHI_REST = "https://api.elections.kalshi.com/trade-api/v2"
POLY_WS_MARKET = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
POLY_WS_LIVE = "wss://ws-live-data.polymarket.com"

PING_INTERVAL = 9  # seconds, both exchanges expect ~10s

shutdown_event = asyncio.Event()

# ── SQLite ──────────────────────────────────────────────────────────────────

def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    c.execute("""CREATE TABLE IF NOT EXISTS kalshi_trades (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts_unix INTEGER NOT NULL,
        ts_iso TEXT,
        trade_id TEXT,
        market_ticker TEXT NOT NULL,
        yes_price REAL,
        no_price REAL,
        count REAL,
        taker_side TEXT,
        inserted_at TEXT DEFAULT (datetime('now'))
    )""")

    c.execute("""CREATE TABLE IF NOT EXISTS kalshi_tickers (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts_unix INTEGER NOT NULL,
        ts_iso TEXT,
        market_ticker TEXT NOT NULL,
        price REAL,
        yes_bid REAL,
        yes_ask REAL,
        volume REAL,
        open_interest REAL,
        dollar_volume INTEGER,
        dollar_open_interest INTEGER,
        inserted_at TEXT DEFAULT (datetime('now'))
    )""")

    c.execute("""CREATE TABLE IF NOT EXISTS kalshi_orderbook_deltas (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts_iso TEXT,
        market_ticker TEXT NOT NULL,
        price REAL,
        delta REAL,
        side TEXT,
        seq INTEGER,
        inserted_at TEXT DEFAULT (datetime('now'))
    )""")

    c.execute("""CREATE TABLE IF NOT EXISTS kalshi_orderbook_snapshots (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        market_ticker TEXT NOT NULL,
        side TEXT NOT NULL,
        price REAL NOT NULL,
        size REAL NOT NULL,
        seq INTEGER,
        inserted_at TEXT DEFAULT (datetime('now'))
    )""")

    c.execute("""CREATE TABLE IF NOT EXISTS poly_trades (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts_unix INTEGER,
        asset_id TEXT,
        market TEXT,
        side TEXT,
        price REAL,
        size REAL,
        raw_json TEXT,
        inserted_at TEXT DEFAULT (datetime('now'))
    )""")

    c.execute("""CREATE TABLE IF NOT EXISTS poly_book_updates (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts_unix INTEGER,
        asset_id TEXT,
        market TEXT,
        msg_type TEXT,
        raw_json TEXT,
        inserted_at TEXT DEFAULT (datetime('now'))
    )""")

    # Indexes for time-series queries
    c.execute("CREATE INDEX IF NOT EXISTS idx_kt_ts ON kalshi_trades(ts_unix)")
    c.execute("CREATE INDEX IF NOT EXISTS idx_kt_ticker ON kalshi_trades(market_ticker)")
    c.execute("CREATE INDEX IF NOT EXISTS idx_kti_ts ON kalshi_tickers(ts_unix)")
    c.execute("CREATE INDEX IF NOT EXISTS idx_kti_ticker ON kalshi_tickers(market_ticker)")
    c.execute("CREATE INDEX IF NOT EXISTS idx_kod_ticker ON kalshi_orderbook_deltas(market_ticker)")
    c.execute("CREATE INDEX IF NOT EXISTS idx_pt_ts ON poly_trades(ts_unix)")
    c.execute("CREATE INDEX IF NOT EXISTS idx_pt_asset ON poly_trades(asset_id)")

    conn.commit()
    return conn


# ── Kalshi Auth ─────────────────────────────────────────────────────────────

def kalshi_sign(method, path):
    """Generate Kalshi RSA-PSS auth headers."""
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding

    api_key = os.environ["KALSHI_API_KEY"]
    key_path = os.environ.get("KALSHI_RSA_PRIVATE_KEY_PATH", "./keys/kalshi.pem")

    ts = str(int(time.time() * 1000))
    with open(key_path, "rb") as f:
        pk = serialization.load_pem_private_key(f.read(), password=None)

    msg = (ts + method + path).encode()
    sig = base64.b64encode(
        pk.sign(msg, padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                  salt_length=padding.PSS.MAX_LENGTH), hashes.SHA256())
    ).decode()

    return {
        "KALSHI-ACCESS-KEY": api_key,
        "KALSHI-ACCESS-SIGNATURE": sig,
        "KALSHI-ACCESS-TIMESTAMP": ts,
    }


def kalshi_get_active_tickers():
    """Fetch all active market tickers from REST API."""
    headers = kalshi_sign("GET", "/trade-api/v2/markets")
    tickers = []
    cursor = None
    while True:
        url = f"{KALSHI_REST}/markets?status=open&limit=200"
        if cursor:
            url += f"&cursor={cursor}"
        r = requests.get(url, headers=headers)
        data = r.json()
        markets = data.get("markets", [])
        for m in markets:
            tk = m.get("ticker", "")
            if tk:
                tickers.append(tk)
        cursor = data.get("cursor")
        if not cursor or not markets:
            break
    return tickers


# ── Kalshi Websocket ────────────────────────────────────────────────────────

async def kalshi_stream(db_conn, subscribe_all=False):
    """Connect to Kalshi WS, subscribe to trades + tickers + orderbook on active markets."""
    auth_headers = kalshi_sign("GET", "/trade-api/ws/v2")

    print(f"[kalshi] Fetching active markets...")
    tickers = kalshi_get_active_tickers()
    print(f"[kalshi] Found {len(tickers)} active markets")

    # Batch into groups of 50 for subscription
    BATCH = 50
    ticker_batches = [tickers[i:i+BATCH] for i in range(0, len(tickers), BATCH)]

    while not shutdown_event.is_set():
        try:
            async with websockets.connect(
                KALSHI_WS,
                additional_headers=auth_headers,
                ping_interval=PING_INTERVAL,
                ping_timeout=30,
                max_size=10 * 1024 * 1024,
            ) as ws:
                print(f"[kalshi] Connected to websocket")

                cmd_id = 1
                # Subscribe in batches
                for batch in ticker_batches:
                    for channel in ["trade", "ticker", "orderbook_delta"]:
                        msg = {
                            "id": cmd_id,
                            "cmd": "subscribe",
                            "params": {
                                "channels": [channel],
                                "market_tickers": batch,
                            }
                        }
                        await ws.send(json.dumps(msg))
                        cmd_id += 1

                print(f"[kalshi] Subscribed to {len(tickers)} markets x 3 channels")

                # Stats
                trade_count = 0
                ticker_count = 0
                delta_count = 0
                last_stats = time.time()

                async for raw in ws:
                    if shutdown_event.is_set():
                        break

                    try:
                        data = json.loads(raw)
                    except json.JSONDecodeError:
                        continue

                    msg_type = data.get("type", "")
                    msg = data.get("msg", {})
                    seq = data.get("seq", 0)

                    if msg_type == "trade":
                        db_conn.execute(
                            "INSERT INTO kalshi_trades (ts_unix, ts_iso, trade_id, market_ticker, yes_price, no_price, count, taker_side) VALUES (?,?,?,?,?,?,?,?)",
                            (
                                msg.get("ts", int(time.time())),
                                msg.get("time", ""),
                                msg.get("trade_id", ""),
                                msg.get("market_ticker", ""),
                                float(msg.get("yes_price_dollars", 0)),
                                float(msg.get("no_price_dollars", 0)),
                                float(msg.get("count_fp", 0)),
                                msg.get("taker_side", ""),
                            )
                        )
                        trade_count += 1

                    elif msg_type == "ticker":
                        db_conn.execute(
                            "INSERT INTO kalshi_tickers (ts_unix, ts_iso, market_ticker, price, yes_bid, yes_ask, volume, open_interest, dollar_volume, dollar_open_interest) VALUES (?,?,?,?,?,?,?,?,?,?)",
                            (
                                msg.get("ts", int(time.time())),
                                msg.get("time", ""),
                                msg.get("market_ticker", ""),
                                float(msg.get("price_dollars", 0)),
                                float(msg.get("yes_bid_dollars", 0)),
                                float(msg.get("yes_ask_dollars", 0)),
                                float(msg.get("volume_fp", 0)),
                                float(msg.get("open_interest_fp", 0)),
                                msg.get("dollar_volume", 0),
                                msg.get("dollar_open_interest", 0),
                            )
                        )
                        ticker_count += 1

                    elif msg_type == "orderbook_delta":
                        db_conn.execute(
                            "INSERT INTO kalshi_orderbook_deltas (ts_iso, market_ticker, price, delta, side, seq) VALUES (?,?,?,?,?,?)",
                            (
                                msg.get("ts", ""),
                                msg.get("market_ticker", ""),
                                float(msg.get("price_dollars", 0)),
                                float(msg.get("delta_fp", 0)),
                                msg.get("side", ""),
                                seq,
                            )
                        )
                        delta_count += 1

                    elif msg_type == "orderbook_snapshot":
                        ticker = msg.get("market_ticker", "")
                        for side_key, side_name in [("yes_dollars_fp", "yes"), ("no_dollars_fp", "no")]:
                            for level in msg.get(side_key, []):
                                if len(level) >= 2:
                                    db_conn.execute(
                                        "INSERT INTO kalshi_orderbook_snapshots (market_ticker, side, price, size, seq) VALUES (?,?,?,?,?)",
                                        (ticker, side_name, float(level[0]), float(level[1]), seq)
                                    )

                    # Commit + stats every 5 seconds
                    now = time.time()
                    if now - last_stats > 5:
                        db_conn.commit()
                        if trade_count + ticker_count + delta_count > 0:
                            print(f"[kalshi] trades={trade_count} tickers={ticker_count} deltas={delta_count}")
                        last_stats = now

                db_conn.commit()

        except (websockets.exceptions.ConnectionClosed, ConnectionError, OSError) as e:
            print(f"[kalshi] Connection lost: {e}. Reconnecting in 5s...")
            await asyncio.sleep(5)
        except Exception as e:
            print(f"[kalshi] Error: {e}. Reconnecting in 10s...")
            await asyncio.sleep(10)


# ── Polymarket Websocket ────────────────────────────────────────────────────

def poly_get_active_tokens(limit=200):
    """Get active market token IDs from Polymarket."""
    r = requests.get(
        f"https://gamma-api.polymarket.com/markets?active=true&closed=false&order=volume24hr&ascending=false&limit={limit}"
    )
    markets = r.json()
    tokens = []
    for m in markets:
        cid = m.get("conditionId", "")
        # tokens are in clobTokenIds field (comma-separated or JSON)
        token_str = m.get("clobTokenIds", "")
        question = m.get("question", "")[:60]
        if token_str:
            try:
                token_ids = json.loads(token_str)
            except (json.JSONDecodeError, TypeError):
                token_ids = [t.strip() for t in str(token_str).split(",") if t.strip()]
            for tid in token_ids:
                tokens.append({"token_id": tid, "question": question, "condition_id": cid})
    return tokens


async def poly_stream(db_conn):
    """Connect to Polymarket market WS, subscribe to all active markets."""
    print(f"[poly] Fetching active markets...")
    tokens = poly_get_active_tokens(300)
    token_ids = [t["token_id"] for t in tokens]
    token_map = {t["token_id"]: t["question"] for t in tokens}
    print(f"[poly] Found {len(token_ids)} active tokens")

    # Batch subscribe in groups of 50
    BATCH = 50

    while not shutdown_event.is_set():
        try:
            async with websockets.connect(
                POLY_WS_MARKET,
                ping_interval=PING_INTERVAL,
                ping_timeout=30,
                max_size=10 * 1024 * 1024,
            ) as ws:
                print(f"[poly] Connected to market websocket")

                # Subscribe in batches
                for i in range(0, len(token_ids), BATCH):
                    batch = token_ids[i:i+BATCH]
                    sub_msg = {
                        "assets_ids": batch,
                        "type": "market",
                    }
                    await ws.send(json.dumps(sub_msg))

                print(f"[poly] Subscribed to {len(token_ids)} tokens")

                msg_count = 0
                trade_count = 0
                last_stats = time.time()

                async for raw in ws:
                    if shutdown_event.is_set():
                        break

                    if raw == "PONG":
                        continue

                    try:
                        data = json.loads(raw)
                    except json.JSONDecodeError:
                        # Handle PING
                        if raw == "PING" or raw == b"PING":
                            await ws.send("PONG")
                        continue

                    # Polymarket sends arrays of events
                    events = data if isinstance(data, list) else [data]

                    for evt in events:
                        evt_type = evt.get("event_type", evt.get("type", ""))
                        asset_id = ""

                        # Extract asset_id from various message formats
                        if "asset_id" in evt:
                            asset_id = evt["asset_id"]
                        elif "market" in evt and isinstance(evt["market"], str):
                            asset_id = evt["market"]

                        market_name = token_map.get(asset_id, "")
                        ts_now = int(time.time())

                        if evt_type in ("last_trade_price", "trade"):
                            price = float(evt.get("price", 0))
                            size = float(evt.get("size", evt.get("amount", 0)))
                            side = evt.get("side", "")
                            db_conn.execute(
                                "INSERT INTO poly_trades (ts_unix, asset_id, market, side, price, size, raw_json) VALUES (?,?,?,?,?,?,?)",
                                (ts_now, asset_id, market_name, side, price, size, json.dumps(evt))
                            )
                            trade_count += 1

                        elif evt_type in ("book", "price_change", "best_bid_ask", "tick_size_change"):
                            db_conn.execute(
                                "INSERT INTO poly_book_updates (ts_unix, asset_id, market, msg_type, raw_json) VALUES (?,?,?,?,?)",
                                (ts_now, asset_id, market_name, evt_type, json.dumps(evt))
                            )

                        msg_count += 1

                    now = time.time()
                    if now - last_stats > 5:
                        db_conn.commit()
                        if msg_count > 0:
                            print(f"[poly] msgs={msg_count} trades={trade_count}")
                        last_stats = now

                db_conn.commit()

        except (websockets.exceptions.ConnectionClosed, ConnectionError, OSError) as e:
            print(f"[poly] Connection lost: {e}. Reconnecting in 5s...")
            await asyncio.sleep(5)
        except Exception as e:
            print(f"[poly] Error: {e}. Reconnecting in 10s...")
            await asyncio.sleep(10)


# ── Polymarket PING keepalive ───────────────────────────────────────────────

async def poly_ping_loop(ws):
    """Send PING every 9 seconds to keep Polymarket connection alive."""
    while not shutdown_event.is_set():
        try:
            await ws.send("PING")
            await asyncio.sleep(PING_INTERVAL)
        except Exception:
            break


# ── 5-minute snapshot aggregator ────────────────────────────────────────────

async def snapshot_aggregator(db_conn):
    """Every 5 minutes, compute and store aggregate snapshots from deltas."""
    while not shutdown_event.is_set():
        await asyncio.sleep(300)  # 5 minutes
        try:
            now = int(time.time())
            window_start = now - 300

            # Kalshi: aggregate trades per market in last 5 min
            rows = db_conn.execute("""
                SELECT market_ticker,
                       COUNT(*) as num_trades,
                       SUM(count) as total_volume,
                       AVG(yes_price) as avg_yes_price,
                       MIN(yes_price) as min_yes_price,
                       MAX(yes_price) as max_yes_price,
                       SUM(CASE WHEN taker_side='yes' THEN count ELSE 0 END) as buy_volume,
                       SUM(CASE WHEN taker_side='no' THEN count ELSE 0 END) as sell_volume
                FROM kalshi_trades
                WHERE ts_unix >= ?
                GROUP BY market_ticker
                HAVING num_trades > 0
                ORDER BY total_volume DESC
            """, (window_start,)).fetchall()

            if rows:
                print(f"\n[snapshot] 5-min aggregate @ {datetime.now(timezone.utc).strftime('%H:%M:%S')} UTC")
                print(f"  {'Market':<45s} trades   vol    avg_px  buy/sell")
                for r in rows[:20]:
                    ticker, n, vol, avg_px, _, _, buy_v, sell_v = r
                    print(f"  {ticker:<45s} {n:>5d}  {vol:>7.0f}  {avg_px:.3f}   {buy_v:.0f}/{sell_v:.0f}")

            # Poly: aggregate trades
            poly_rows = db_conn.execute("""
                SELECT market,
                       COUNT(*) as num_trades,
                       SUM(size) as total_size,
                       AVG(price) as avg_price
                FROM poly_trades
                WHERE ts_unix >= ?
                GROUP BY market
                HAVING num_trades > 0
                ORDER BY total_size DESC
            """, (window_start,)).fetchall()

            if poly_rows:
                print(f"\n  [poly 5-min]")
                for r in poly_rows[:10]:
                    market, n, sz, avg_px = r
                    print(f"  {(market or '?'):<50s} trades={n} size={sz:.1f} avg={avg_px:.3f}")

        except Exception as e:
            print(f"[snapshot] Error: {e}")


# ── Main ────────────────────────────────────────────────────────────────────

async def main():
    db_conn = init_db()
    print(f"[init] Database: {DB_PATH}")

    row = db_conn.execute("SELECT COUNT(*) FROM kalshi_trades").fetchone()
    print(f"[init] Existing kalshi trades: {row[0]}")
    row = db_conn.execute("SELECT COUNT(*) FROM poly_trades").fetchone()
    print(f"[init] Existing poly trades: {row[0]}")

    tasks = []

    # Kalshi
    if os.environ.get("KALSHI_API_KEY"):
        tasks.append(asyncio.create_task(kalshi_stream(db_conn)))
        print("[init] Kalshi stream enabled")
    else:
        print("[init] Kalshi stream DISABLED (no KALSHI_API_KEY)")

    # Polymarket
    tasks.append(asyncio.create_task(poly_stream(db_conn)))
    print("[init] Polymarket stream enabled")

    # Aggregator
    tasks.append(asyncio.create_task(snapshot_aggregator(db_conn)))

    print("[init] Streaming... Ctrl+C to stop.\n")

    def handle_signal():
        print("\n[shutdown] Stopping...")
        shutdown_event.set()

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, handle_signal)

    try:
        await asyncio.gather(*tasks, return_exceptions=True)
    finally:
        db_conn.commit()
        db_conn.close()
        print("[shutdown] Database closed. Done.")


if __name__ == "__main__":
    asyncio.run(main())
