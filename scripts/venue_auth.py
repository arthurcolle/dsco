"""
Authenticated clients for the three prediction-market venues.

  - Kalshi:     RSA-PSS signed requests (KALSHI-ACCESS-* headers)
  - Polymarket: CLOB L2 (HMAC-SHA256 POLY_* headers) via py_clob_client
  - PredictIt:  no real auth API — session-cookie scraping is the only path,
                so we just expose the public endpoint and let the caller
                add cookies if needed.

Load credentials from /Users/arthurcolle/dsco-cli/.env (or env vars).
"""
from __future__ import annotations

import base64
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import requests
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding


# ─── Env loader ───────────────────────────────────────────────────────────────

ENV_PATH = Path("/Users/arthurcolle/dsco-cli/.env")
_ENV_CACHE: dict[str, str] | None = None


def env(key: str, default: str = "") -> str:
    global _ENV_CACHE
    if _ENV_CACHE is None:
        _ENV_CACHE = {}
        if ENV_PATH.exists():
            for line in ENV_PATH.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                _ENV_CACHE[k.strip()] = v.strip().strip('"').strip("'")
    return os.environ.get(key) or _ENV_CACHE.get(key, default)


# ─── Kalshi ──────────────────────────────────────────────────────────────────

class Kalshi:
    BASE = "https://api.elections.kalshi.com/trade-api/v2"

    def __init__(self) -> None:
        self.key_id = env("KALSHI_API_KEY")
        key_path_str = env("KALSHI_RSA_PRIVATE_KEY_PATH")
        if not self.key_id or not key_path_str:
            raise RuntimeError("KALSHI_API_KEY and KALSHI_RSA_PRIVATE_KEY_PATH required")
        key_path = (Path("/Users/arthurcolle/dsco-cli") / key_path_str
                    if not Path(key_path_str).is_absolute() else Path(key_path_str))
        self._pkey = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
        self.s = requests.Session()

    def _sign(self, method: str, path: str) -> dict[str, str]:
        ts = str(int(time.time() * 1000))
        msg = (ts + method.upper() + path).encode()
        sig = self._pkey.sign(
            msg,
            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH),
            hashes.SHA256(),
        )
        return {
            "KALSHI-ACCESS-KEY": self.key_id,
            "KALSHI-ACCESS-TIMESTAMP": ts,
            "KALSHI-ACCESS-SIGNATURE": base64.b64encode(sig).decode(),
            "Accept": "application/json",
        }

    def get(self, path: str, **params) -> dict:
        # Signature path is just the route, no query string
        headers = self._sign("GET", path)
        r = self.s.get(self.BASE + path, params=params, headers=headers, timeout=20)
        r.raise_for_status()
        return r.json()

    def balance(self) -> dict:
        return self.get("/portfolio/balance")

    def positions(self) -> dict:
        return self.get("/portfolio/positions", limit=200)

    def fills(self, limit: int = 50) -> dict:
        return self.get("/portfolio/fills", limit=limit)

    def orders(self, status: str = "resting") -> dict:
        return self.get("/portfolio/orders", status=status, limit=200)

    def orderbook(self, ticker: str, depth: int = 30) -> dict:
        return self.get(f"/markets/{ticker}/orderbook", depth=depth)


# ─── Polymarket ──────────────────────────────────────────────────────────────

class Polymarket:
    """Wraps py_clob_client. Reads creds from .env."""

    def __init__(self) -> None:
        from py_clob_client.client import ClobClient
        from py_clob_client.clob_types import ApiCreds

        self.host = "https://clob.polymarket.com"
        self.chain_id = 137  # Polygon
        self.address = env("POLYMARKET_ADDRESS")
        self.proxy = env("POLYMARKET_PROXY_ADDRESS") or None
        self.pkey = env("POLYMARKET_PRIVATE_KEY")
        creds = ApiCreds(
            api_key=env("POLYMARKET_API_KEY"),
            api_secret=env("POLYMARKET_API_SECRET"),
            api_passphrase=env("POLYMARKET_PASSPHRASE"),
        )
        # signature_type=2 = email/magic-link proxy account; 1 = email EOA; 0 = browser wallet
        sig_type = int(env("POLYMARKET_SIG_TYPE", "2"))
        kwargs: dict[str, Any] = {
            "host": self.host,
            "key": self.pkey,
            "chain_id": self.chain_id,
            "creds": creds,
            "signature_type": sig_type,
        }
        if self.proxy:
            kwargs["funder"] = self.proxy
        self.client = ClobClient(**kwargs)

    def balance(self) -> dict:
        # USDC balance/allowance check
        from py_clob_client.clob_types import BalanceAllowanceParams, AssetType
        try:
            return self.client.get_balance_allowance(BalanceAllowanceParams(asset_type=AssetType.COLLATERAL))
        except Exception as e:
            return {"error": str(e)}

    def positions(self) -> Any:
        # CLOB doesn't expose positions directly; use Data API
        addr = self.proxy or self.address
        r = requests.get("https://data-api.polymarket.com/positions",
                         params={"user": addr, "limit": 200}, timeout=20)
        return r.json() if r.ok else {"error": r.status_code}

    def orders(self) -> Any:
        try:
            return self.client.get_orders()
        except Exception as e:
            return {"error": str(e)}

    def trades(self, limit: int = 50) -> Any:
        try:
            return self.client.get_trades()
        except Exception as e:
            return {"error": str(e)}

    def orderbook(self, token_id: str) -> dict:
        r = requests.get(f"{self.host}/book", params={"token_id": token_id}, timeout=20)
        return r.json() if r.ok else {"error": r.status_code}


# ─── PredictIt ───────────────────────────────────────────────────────────────

class PredictIt:
    """PredictIt has no first-party auth API. Public endpoint only.

    For account-side data (balance, positions, orders) you need to scrape with
    a logged-in session cookie. Drop a cookie value in PREDICTIT_COOKIE and
    we'll forward it on a request to predictit.org (best-effort).
    """
    PUBLIC = "https://www.predictit.org/api/marketdata/all/"
    USER_ACCOUNT = "https://www.predictit.org/api/User/Wallet/Balance"

    def __init__(self) -> None:
        self.cookie = env("PREDICTIT_COOKIE")
        self.s = requests.Session()
        if self.cookie:
            self.s.headers["Cookie"] = self.cookie

    def markets(self) -> dict:
        return self.s.get(self.PUBLIC, timeout=20).json()

    def balance(self) -> Any:
        if not self.cookie:
            return {"error": "PREDICTIT_COOKIE not set — paste your browser cookie"}
        r = self.s.get(self.USER_ACCOUNT, timeout=20)
        return r.json() if r.ok else {"error": r.status_code, "body": r.text[:200]}


# ─── Smoke test ──────────────────────────────────────────────────────────────

def smoke() -> None:
    print("─── Kalshi ───")
    try:
        k = Kalshi()
        bal = k.balance()
        print(f"  balance (cents): {bal}")
        pos = k.positions()
        n_pos = len(pos.get("market_positions") or pos.get("positions") or [])
        print(f"  positions: {n_pos}")
        rests = k.orders("resting").get("orders", [])
        print(f"  resting orders: {len(rests)}")
    except Exception as e:
        print(f"  ✗ {type(e).__name__}: {e}")

    print("\n─── Polymarket ───")
    try:
        p = Polymarket()
        print(f"  address={p.address[:6]}…{p.address[-4:]} proxy={p.proxy and p.proxy[:6]+'…'+p.proxy[-4:]}")
        print(f"  balance: {p.balance()}")
        pos = p.positions()
        if isinstance(pos, list):
            print(f"  positions: {len(pos)}")
            for x in pos[:5]:
                ticker = x.get('title') or x.get('outcome') or x.get('asset')
                size = x.get('size')
                px = x.get('avgPrice') or x.get('curPrice')
                print(f"    • {ticker} | size={size} px={px}")
        else:
            print(f"  positions: {pos}")
    except Exception as e:
        print(f"  ✗ {type(e).__name__}: {e}")

    print("\n─── PredictIt ───")
    try:
        pi = PredictIt()
        ms = pi.markets()
        print(f"  public markets: {len(ms.get('markets', []))}")
        print(f"  account: {pi.balance()}")
    except Exception as e:
        print(f"  ✗ {type(e).__name__}: {e}")


if __name__ == "__main__":
    smoke()
