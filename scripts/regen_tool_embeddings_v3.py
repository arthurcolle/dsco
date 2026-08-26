#!/usr/bin/env python3
"""Regenerate data/tool_embeddings.bin with jina-embeddings-v3.

Reads the tool names (in index order) from include/tool_embeddings.h, batch-
embeds them with v3 using task=retrieval.passage (centroids are documents;
runtime queries use retrieval.query — correct asymmetric retrieval), and writes
the bin in the format the C loader expects:

    uint32 count, uint32 dim   (little-endian header)
    count * dim  float32       (row-major, index-aligned to TOOL_EMB_NAMES)
"""
import json
import os
import re
import struct
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "tool_embeddings.h")
BIN = os.path.join(ROOT, "data", "tool_embeddings.bin")
DIM = 1024
MODEL = "jina-embeddings-v3"
BATCH = 100

KEY = os.environ.get("JINA_API_KEY", "")
if not KEY:
    for line in open(os.path.expanduser("~/.dsco/env"), encoding="utf-8"):
        if line.startswith("JINA_API_KEY="):
            KEY = line.split("=", 1)[1].strip()
            break
assert KEY, "JINA_API_KEY not found"

# Parse the names array in order.
text = open(HEADER, encoding="utf-8").read()
block = text.split("TOOL_EMB_NAMES[] = {", 1)[1].split("};", 1)[0]
names = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
names = [n.encode().decode("unicode_escape") for n in names]
print(f"parsed {len(names)} tool names")


def embed_batch(texts):
    body = json.dumps({
        "model": MODEL,
        "task": "retrieval.passage",
        "dimensions": DIM,
        "embedding_type": "float",
        "normalized": True,
        "input": texts,
    }).encode()
    req = urllib.request.Request(
        "https://api.jina.ai/v1/embeddings",
        data=body,
        headers={
            "Authorization": f"Bearer {KEY}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = json.load(r)
            rows = sorted(data["data"], key=lambda d: d.get("index", 0))
            return [row["embedding"] for row in rows]
        except Exception as exc:  # noqa: BLE001
            print(f"  batch attempt {attempt+1} failed: {exc}")
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError("batch embed failed after retries")


vectors = []
t0 = time.time()
for i in range(0, len(names), BATCH):
    chunk = names[i:i + BATCH]
    embs = embed_batch(chunk)
    assert len(embs) == len(chunk), f"got {len(embs)} for {len(chunk)}"
    for e in embs:
        assert len(e) == DIM, f"dim {len(e)} != {DIM}"
        vectors.append(e)
    print(f"  embedded {len(vectors)}/{len(names)}  ({time.time()-t0:.1f}s)")

assert len(vectors) == len(names)
tmp = BIN + ".tmp"
with open(tmp, "wb") as f:
    f.write(struct.pack("<II", len(vectors), DIM))
    for vec in vectors:
        f.write(struct.pack(f"<{DIM}f", *vec))
os.replace(tmp, BIN)
size = os.path.getsize(BIN)
print(f"wrote {BIN}: {len(vectors)} x {DIM}  ({size} bytes) in {time.time()-t0:.1f}s")
assert size == 8 + len(vectors) * DIM * 4, "size mismatch"
