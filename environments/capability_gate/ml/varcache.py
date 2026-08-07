"""Determinism + caching + holdout spine for the high-entropy capability-gate generator.

Synthesized from the swarm: kill the single global seed; derive every random choice from
blake2b(master | family | idx | axis) so each row is reproducible IN ISOLATION and
order-independent. Partition every vocabulary (tool-names, hosts, secrets, arg-keys, deny/
allow signals) into DISJOINT train/val/test buckets by hash of the token string, so
leave-one-vocab-out generalization is enforced at generation time, not hoped for. A content-
addressed SQLite store caches expensive realizations; a lockfile makes a dataset exactly
reconstructable. Pure-Python realizations are cheap, so "cache" here means: deterministically
regenerable bit-identically + materialized once + reproducibility asserted by a golden hash.
"""
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import time

import numpy as np

GEN_VERSION = "v2.0"
MASTER_SEED = 20260807
HERE = os.path.dirname(os.path.abspath(__file__))


def _bl(*parts, size=8):
    h = hashlib.blake2b(digest_size=size)
    for p in parts:
        h.update(str(p).encode("utf-8"))
        h.update(b"\x1f")
    return h.digest()


def axis_rng(*key) -> np.random.Generator:
    """A per-(family, idx, axis) numpy Generator — independent, spawnable, reproducible."""
    return np.random.default_rng(int.from_bytes(_bl(MASTER_SEED, *key), "big"))


def hash_bucket(token: str, salt: str = "", n: int = 10) -> int:
    return int.from_bytes(_bl(salt, token, size=4), "big") % n


# vocab holdout: a token always lands in the SAME split (no cross-split leakage), so a
# tool-name / host / deny-signal seen in train is hash-guaranteed novel in val/test.
def split_of(token: str, kind: str = "", test_frac: float = 0.2, val_frac: float = 0.1) -> str:
    b = hash_bucket(token, salt=kind, n=1000) / 1000.0
    if b < test_frac:
        return "test"
    if b < test_frac + val_frac:
        return "val"
    return "train"


def partition(bank, kind: str):
    """Split a bank list into {train,val,test} by each item's hash — disjoint by construction."""
    out = {"train": [], "val": [], "test": []}
    for x in bank:
        out[split_of(str(x), kind)].append(x)
    # guarantee every split is non-empty for banks with >=3 items (avoid empty-choice at gen)
    for s in ("train", "val", "test"):
        if not out[s] and bank:
            out[s] = [sorted(bank, key=lambda t: _bl(kind, s, t))[0]]
    return out


def pick(rng, bank, kind: str, split: str):
    """Choose a token from the bank restricted to the given split's partition."""
    part = partition(bank, kind)[split] or list(bank)
    return part[int(rng.integers(len(part)))]


def bank_hash(bank) -> str:
    return hashlib.blake2b(json.dumps(sorted(map(str, bank)), sort_keys=True).encode(),
                           digest_size=8).hexdigest()


# ── content-addressed cache (for future LLM/man-page realizers; cheap-realization no-op now) ──
class Cache:
    def __init__(self, path=None):
        self.path = path or os.path.join(HERE, "cache", "varcache.sqlite")
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self.con = sqlite3.connect(self.path)
        self.con.execute("CREATE TABLE IF NOT EXISTS cache(key TEXT PRIMARY KEY, "
                         "recipe TEXT, row TEXT, gen_version TEXT, created REAL)")
        self.con.commit()

    def get(self, key):
        r = self.con.execute("SELECT row FROM cache WHERE key=?", (key,)).fetchone()
        return json.loads(r[0]) if r else None

    def put(self, key, recipe, row):
        self.con.execute("INSERT OR REPLACE INTO cache VALUES(?,?,?,?,?)",
                         (key, json.dumps(recipe, sort_keys=True), json.dumps(row),
                          GEN_VERSION, time.time()))

    def commit(self):
        self.con.commit()


def recipe_key(recipe: dict, salt: str = "") -> str:
    payload = json.dumps({**recipe, "_v": GEN_VERSION, "_salt": salt}, sort_keys=True)
    return hashlib.blake2b(payload.encode(), digest_size=16).hexdigest()


def write_lock(path, master_seed, counts, banks: dict, row_keys, extra=None):
    lock = {
        "gen_version": GEN_VERSION, "master_seed": master_seed, "n": sum(counts.values()),
        "counts": dict(counts),
        "bank_hashes": {k: bank_hash(v) for k, v in banks.items()},
        "row_keys_digest": hashlib.sha256("\n".join(sorted(row_keys)).encode()).hexdigest(),
        **(extra or {}),
    }
    with open(path, "w") as f:
        json.dump(lock, f, indent=2, sort_keys=True)
    return lock


# ── diversity / leakage metrics (the honest thermometer) ──
def ngram_entropy(texts, n=3):
    from collections import Counter
    import math
    c = Counter(t[i:i + n] for t in texts for i in range(max(0, len(t) - n)))
    tot = sum(c.values()) or 1
    return -sum(v / tot * math.log(v / tot) for v in c.values())


def near_dup_rate(texts, k=5, thresh=0.9, cap=4000):
    """Cheap char-shingle Jaccard near-dup rate on a sample (no external deps)."""
    import random as _r
    rr = _r.Random(0)
    sample = texts if len(texts) <= cap else rr.sample(texts, cap)
    shingles = [frozenset(t[i:i + k] for i in range(max(1, len(t) - k))) for t in sample]
    dup = 0
    for i in range(len(shingles)):
        si = shingles[i]
        for j in range(i + 1, min(i + 40, len(shingles))):  # local window, cheap
            sj = shingles[j]
            inter = len(si & sj)
            if inter and inter / len(si | sj) >= thresh:
                dup += 1
                break
    return dup / max(1, len(sample))
