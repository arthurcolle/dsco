#!/usr/bin/env python3
"""
facet_index.py — build the semantic facet index (Jina embeddings or local fallback).

Embeds every facet (INCLUDING the 512 S4 boundary facets — they are retrieved on
purpose as semantic tripwires, never activated) and writes:

  <out>/embeddings.f16.npy   (N x dim, float16, row i = facet i)
  <out>/manifest.jsonl       (per-facet governance fields needed at query time)
  <out>/meta.json            (model, dim, mode, count, source sha)

Resumable: each batch is checkpointed to <out>/parts/part_NNNNN.npy; re-running
skips completed parts. Safe to kill and restart (e.g. after a Jina top-up).

  python3 facet_index.py                          # Jina (needs funded JINA_API_KEY)
  python3 facet_index.py --local                  # free hash-embedding fallback
  python3 facet_index.py --model jina-embeddings-v4 --dim 1024 --batch 64
"""
import argparse, hashlib, json, math, os, re, sys, time
from pathlib import Path

import numpy as np
import requests

HERE = Path(__file__).parent
JINA_URL = "https://api.jina.ai/v1/embeddings"

MANIFEST_FIELDS = [
    "facet_id", "display_name", "domain", "subdomain", "topic",
    "sensitivity_class", "proxy_risk_class", "facet_kind",
    "minimum_confidence_for_activation", "minimum_evidence_count",
    "half_life_days", "allowed_uses", "activation_allowed",
    "explanation_template",
]


def load_jsonl(p):
    with open(p) as f:
        return [json.loads(line) for line in f if line.strip()]


def facet_text(fc):
    parts = [fc.get("display_name", ""), fc.get("domain_name", ""),
             fc.get("subdomain_name", ""), fc.get("description", "")]
    return ". ".join(p for p in parts if p)


def jina_key():
    key = os.environ.get("JINA_API_KEY", "")
    if not key:
        env = Path.home() / ".dsco" / "env"
        if env.exists():
            m = re.search(r'^(?:export\s+)?JINA_API_KEY=["\']?([^"\'\n]+)',
                          env.read_text(), re.M)
            if m:
                key = m.group(1)
    return key


def embed_jina(texts, model, dim, task, key, retries=5):
    body = {"model": model, "task": task, "embedding_type": "float", "input": texts}
    if dim:
        body["dimensions"] = dim
    for attempt in range(retries):
        r = requests.post(JINA_URL, json=body, timeout=120,
                          headers={"Authorization": f"Bearer {key}"})
        if r.status_code == 200:
            data = sorted(r.json()["data"], key=lambda d: d["index"])
            return np.array([d["embedding"] for d in data], dtype=np.float32)
        if r.status_code in (429, 500, 502, 503, 524):
            wait = 2 ** attempt
            print(f"  [retry] HTTP {r.status_code}, sleeping {wait}s", file=sys.stderr)
            time.sleep(wait)
            continue
        raise RuntimeError(f"Jina HTTP {r.status_code}: {r.text[:300]}")
    raise RuntimeError("Jina: retries exhausted")


# Local fallback: hashed token+bigram counts. Mirrors the spirit of dsco's
# ctx_build_embedding — free lexical recall, not semantics. The index build
# computes corpus IDF over these dims; queries reuse it (see idf.npy).
LOCAL_DIM = 1024
STOP = set("the a an of for and or to in on with is are was be as at by it its this that "
           "these those from your you can may will has have had about facet governed "
           "profile quality score used because settings interactions related review "
           "available controls preference affinity intent interest strength signal "
           "just got need new any some more".split())


def embed_local_counts(texts, dim=LOCAL_DIM):
    out = np.zeros((len(texts), dim), dtype=np.float32)
    for i, t in enumerate(texts):
        toks = [x for x in re.split(r"[^a-z0-9]+", (t or "").lower())
                if len(x) > 2 and x not in STOP]
        grams = toks + [a + "_" + b for a, b in zip(toks, toks[1:])]
        for g in grams:
            h = int(hashlib.md5(g.encode()).hexdigest()[:8], 16)
            out[i, h % dim] += 1.0 if "_" not in g else 0.5
    return out


def embed_local(texts, dim=LOCAL_DIM, idf=None):
    out = embed_local_counts(texts, dim)
    if idf is not None:
        out *= idf
    norms = np.linalg.norm(out, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    return out / norms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--facets", default=str(HERE / "facet_definitions_base_with_boundaries.jsonl"))
    ap.add_argument("--out", default=str(HERE / "facet_index"))
    ap.add_argument("--model", default="jina-embeddings-v4")
    ap.add_argument("--dim", type=int, default=1024)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--local", action="store_true", help="free hash-embedding fallback (no API)")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    facets = load_jsonl(args.facets)
    if args.limit:
        facets = facets[: args.limit]
    texts = [facet_text(fc) for fc in facets]
    n = len(texts)
    mode = "local-hash-v2" if args.local else args.model

    out = Path(args.out)
    parts_dir = out / "parts"
    parts_dir.mkdir(parents=True, exist_ok=True)

    key = "" if args.local else jina_key()
    if not args.local and not key:
        sys.exit("JINA_API_KEY not found in env or ~/.dsco/env")

    if args.local:
        counts = embed_local_counts(texts)
        df = (counts > 0).sum(axis=0).astype(np.float32)
        idf = np.log((n + 1.0) / (df + 1.0)) + 1.0
        mat = counts * idf
        norms = np.linalg.norm(mat, axis=1, keepdims=True)
        norms[norms == 0] = 1.0
        mat /= norms
        np.save(out / "idf.npy", idf)
    else:
        nbatches = math.ceil(n / args.batch)
        t0 = time.time()
        for b in range(nbatches):
            part = parts_dir / f"part_{b:05d}.npy"
            if part.exists():
                continue
            chunk = texts[b * args.batch : (b + 1) * args.batch]
            vecs = embed_jina(chunk, args.model, args.dim, "retrieval.passage", key)
            np.save(part, vecs.astype(np.float16))
            done = b + 1
            if done % 10 == 0 or done == nbatches:
                rate = done / (time.time() - t0 + 1e-9)
                print(f"  [embed] {done}/{nbatches} batches ({rate:.1f}/s)")
        mat = np.vstack([np.load(parts_dir / f"part_{b:05d}.npy") for b in range(nbatches)])
    assert mat.shape[0] == n, f"index rows {mat.shape[0]} != facets {n}"
    np.save(out / "embeddings.f16.npy", mat.astype(np.float16))

    with open(out / "manifest.jsonl", "w") as f:
        for fc in facets:
            f.write(json.dumps({k: fc.get(k) for k in MANIFEST_FIELDS}) + "\n")

    src_sha = hashlib.sha256(Path(args.facets).read_bytes()).hexdigest()
    (out / "meta.json").write_text(json.dumps({
        "mode": mode, "dim": int(mat.shape[1]), "count": n,
        "source": str(args.facets), "source_sha256": src_sha,
        "built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }, indent=2))
    print(f"  [done] {n} facets x {mat.shape[1]}d ({mode}) -> {out}")


if __name__ == "__main__":
    main()
