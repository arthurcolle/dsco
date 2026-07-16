#!/usr/bin/env python3
"""
facet_live.py — per-turn governed facet retrieval, scoring, and row highlighting.

Pipeline per chat turn:
  0. lexical gate      Governor.SENSITIVE_LEXICON + prohibited_patterns (hard block, audited)
  1. recall            embed turn -> cosine top-K over the facet index
  2. rerank (optional) Jina reranker over top-K candidate texts
  3. governance        S4/boundary -> suppressed; consent/sensitivity/threshold gates
  4. evidence          per-facet Bayesian-style accumulation with half-life decay;
                       a facet ACTIVATES only after clearing confidence + evidence gates
  5. arrangement       MMR diversification (embedding space) + per-subdomain cap

Row states: ● active   ○ candidate   ✖ suppressed (reason shown, audited)
Rows whose evidence changed THIS turn are highlighted (bold + ▲).

  python3 facet_live.py --index facet_index --turn "..." [--json] [--rerank]
  python3 facet_live.py --index facet_index --interactive
"""
import argparse, json, re, sys, time
from pathlib import Path

import numpy as np
import requests

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
from prompt_analyzer import Governor  # noqa: E402
from facet_index import embed_local, jina_key  # noqa: E402

RERANK_URL = "https://api.jina.ai/v1/rerank"
EMBED_URL = "https://api.jina.ai/v1/embeddings"

SENS_COLOR = {"S0_OPERATIONAL": "\033[2;32m", "S1_STANDARD": "\033[0m",
              "S2_PRIVATE": "\033[33m", "S3_RESTRICTED": "\033[31m"}
BOUNDARY_COLOR = "\033[35;7m"
RESET, BOLD, DIM = "\033[0m", "\033[1m", "\033[2m"


def load_index(index_dir):
    d = Path(index_dir)
    meta = json.loads((d / "meta.json").read_text())
    idf = d / "idf.npy"
    meta["idf"] = np.load(idf) if idf.exists() else None
    mat = np.load(d / "embeddings.f16.npy").astype(np.float32)
    norms = np.linalg.norm(mat, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    mat /= norms
    manifest = [json.loads(l) for l in (d / "manifest.jsonl").read_text().splitlines() if l]
    assert len(manifest) == mat.shape[0]
    return meta, mat, manifest


def embed_turn(text, meta):
    if meta["mode"].startswith("local-hash"):
        v = embed_local([text], meta["dim"], idf=meta.get("idf"))[0]
    else:
        key = jina_key()
        r = requests.post(EMBED_URL, timeout=30, headers={"Authorization": f"Bearer {key}"},
                          json={"model": meta["mode"], "task": "retrieval.query",
                                "dimensions": meta["dim"], "embedding_type": "float",
                                "input": [text]})
        r.raise_for_status()
        v = np.array(r.json()["data"][0]["embedding"], dtype=np.float32)
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


def rerank_jina(query, docs, model="jina-reranker-v3"):
    key = jina_key()
    r = requests.post(RERANK_URL, timeout=60, headers={"Authorization": f"Bearer {key}"},
                      json={"model": model, "query": query, "documents": docs,
                            "top_n": len(docs)})
    r.raise_for_status()
    scores = [0.0] * len(docs)
    for res in r.json()["results"]:
        scores[res["index"]] = float(res["relevance_score"])
    return scores


def mmr(order, sims, vecs, lam=0.85, k=20):
    """Greedy maximal-marginal-relevance over candidate row indices `order`."""
    chosen = []
    cand = list(order)
    while cand and len(chosen) < k:
        if not chosen:
            chosen.append(cand.pop(0))
            continue
        cvecs = vecs[chosen]
        best, best_score = None, -1e9
        for i in cand:
            red = float(np.max(cvecs @ vecs[i]))
            score = lam * sims[i] - (1 - lam) * red
            if score > best_score:
                best, best_score = i, score
        chosen.append(best)
        cand.remove(best)
    return chosen


class Session:
    """Evidence accumulation across turns. confidence via noisy-OR of per-turn
    evidence strength; freshness decays by each facet's half_life_days."""

    def __init__(self, path):
        self.path = Path(path)
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {}

    def update(self, facet_id, strength, half_life_days, now=None):
        now = now or time.time()
        st = self.state.get(facet_id, {"confidence": 0.0, "evidence_count": 0,
                                       "last_ts": now})
        if half_life_days:
            dt_days = (now - st["last_ts"]) / 86400.0
            st["confidence"] *= 2.0 ** (-dt_days / half_life_days)
        st["confidence"] = 1.0 - (1.0 - st["confidence"]) * (1.0 - 0.45 * strength)
        st["evidence_count"] += 1
        st["last_ts"] = now
        st["changed"] = True
        self.state[facet_id] = st
        return st

    def save(self):
        for st in self.state.values():
            st.pop("changed", None)
        self.path.write_text(json.dumps(self.state))


def score_turn(text, meta, mat, manifest, gov, session, privacy,
               k_recall=200, k_show=14, use_rerank=False, subdomain_cap=3):
    audit = []
    tokens = [t for t in re.split(r"[^a-z0-9]+", text.lower()) if t]

    # 0. lexical gate — hard block, audited, before any retrieval
    redflags = gov.prompt_redflags(tokens)
    for rf in redflags:
        audit.append({"event": "s4_lexical_block", **rf})

    # 1. recall
    q = embed_turn(text, meta)
    sims = mat @ q
    order = np.argsort(-sims)[:k_recall]

    # 2. optional dense rerank over the recall set
    rr = None
    if use_rerank:
        docs = [manifest[i]["display_name"] + ". " +
                (manifest[i].get("explanation_template") or "") for i in order]
        rr = rerank_jina(text, docs)

    # 3+4. governance + evidence
    rows, suppressed, boundary_hit = [], [], False
    for rank, i in enumerate(order):
        fc = manifest[i]
        match = float(rr[rank]) if rr else float(sims[i])
        if fc.get("facet_kind") == "negative_ontology_boundary_facet":
            if rank < 25:
                boundary_hit = True
                suppressed.append({"row": int(i), "facet": fc, "match": match,
                                   "reason": "S4_boundary_tripwire"})
                audit.append({"event": "s4_semantic_tripwire",
                              "facet_id": fc["facet_id"], "match": round(match, 4)})
            continue
        rows.append((int(i), fc, match))

    # conservative neighborhood rule: any S4 signal this turn (lexical or
    # semantic) freezes S2+ evidence entirely for the turn
    sensitive_turn = bool(redflags) or boundary_hit

    out = []
    for i, fc, match in rows[: k_recall]:
        sens = fc.get("sensitivity_class", "S1_STANDARD")
        if sensitive_turn and sens in ("S2_PRIVATE", "S3_RESTRICTED"):
            suppressed.append({"row": i, "facet": fc, "match": match,
                               "reason": "sensitive_turn_freeze"})
            continue
        ok, why = gov.facet_allowed(fc, 1.0, privacy)  # structural gates only here
        if not ok and why != "ok" and not why.startswith("below_confidence"):
            suppressed.append({"row": i, "facet": fc, "match": match, "reason": why})
            continue
        # calibrate raw cosine -> evidence strength (0.2 = noise floor, 0.8 = strong)
        strength = max(0.0, min(1.0, (match - 0.2) / 0.6))
        st = session.update(fc["facet_id"], strength, fc.get("half_life_days"))
        min_c = fc.get("minimum_confidence_for_activation") or 0.55
        min_e = fc.get("minimum_evidence_count") or 1
        active = st["confidence"] >= min_c and st["evidence_count"] >= min_e
        out.append({"row": i, "facet": fc, "match": match,
                    "confidence": st["confidence"], "evidence": st["evidence_count"],
                    "state": "active" if active else "candidate", "changed": True})
        if active:
            audit.append({"event": "activation", "facet_id": fc["facet_id"],
                          "confidence": round(st["confidence"], 3),
                          "explanation": fc.get("explanation_template")})

    # 5. arrangement: MMR + per-subdomain cap
    idx_by_row = {r["row"]: r for r in out}
    cand_rows = [r["row"] for r in out]
    arranged = mmr(cand_rows, sims, mat, k=k_show * 3)
    seen_sub, final = {}, []
    for row in arranged:
        r = idx_by_row[row]
        sub = f'{r["facet"]["domain"]}.{r["facet"]["subdomain"]}'
        if seen_sub.get(sub, 0) >= subdomain_cap:
            continue
        seen_sub[sub] = seen_sub.get(sub, 0) + 1
        final.append(r)
        if len(final) >= k_show:
            break

    return {"turn": text, "redflags": redflags, "sensitive_turn": sensitive_turn,
            "rows": final, "suppressed": suppressed[:8], "audit": audit}


def bar(x, width=10):
    full = int(round(max(0.0, min(1.0, x)) * width))
    return "█" * full + "░" * (width - full)


def render(result):
    lines = []
    if result["sensitive_turn"]:
        terms = ", ".join(rf["term"] for rf in result["redflags"]) or "semantic tripwire"
        lines.append(f"{BOUNDARY_COLOR} SENSITIVE TURN — S2+ evidence frozen ({terms}) {RESET}")
    hdr = f'{DIM}{"st":2} {"facet":52} {"sens":4} {"match":>6} {"conf":>16} ev{RESET}'
    lines.append(hdr)
    for r in result["rows"]:
        fc, sens = r["facet"], r["facet"].get("sensitivity_class", "S1_STANDARD")
        color = SENS_COLOR.get(sens, "")
        mark = "●" if r["state"] == "active" else "○"
        hl = BOLD if r.get("changed") else ""
        chg = "▲" if r.get("changed") else " "
        lines.append(f'{hl}{color}{mark} {chg}{fc["facet_id"][:51]:52}'
                     f'{sens[:2]:4}{r["match"]:6.3f}  {bar(r["confidence"])} '
                     f'{r["confidence"]:4.2f} {r["evidence"]:2d}{RESET}')
    for s in result["suppressed"]:
        fc = s["facet"]
        lines.append(f'{BOUNDARY_COLOR}✖{RESET} {DIM} {fc["facet_id"][:51]:52}'
                     f'{"S4" if "boundary" in s["reason"] else "  ":4}'
                     f'{s["match"]:6.3f}  suppressed: {s["reason"]}{RESET}')
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", default=str(HERE / "facet_index"))
    ap.add_argument("--turn")
    ap.add_argument("--interactive", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--rerank", action="store_true", help="Jina reranker on top-K")
    ap.add_argument("--session", default="")
    ap.add_argument("--reset", action="store_true", help="clear session state first")
    ap.add_argument("--privacy", default='{"ads_personalization_allowed":false,'
                    '"partner_data_allowed":false,"precise_location_allowed":false}')
    args = ap.parse_args()

    meta, mat, manifest = load_index(args.index)
    prohibited = json.loads((HERE / "prohibited_patterns.json").read_text())
    gov = Governor(prohibited, [])
    privacy = json.loads(args.privacy)
    spath = args.session or str(Path(args.index) / ".session_state.json")
    if args.reset and Path(spath).exists():
        Path(spath).unlink()
    session = Session(spath)

    def one(text):
        res = score_turn(text, meta, mat, manifest, gov, session, privacy,
                         use_rerank=args.rerank)
        session.save()
        print(json.dumps(res, default=str) if args.json else render(res))

    if args.interactive:
        print(f'[facet_live] index={meta["mode"]} n={meta["count"]} — one turn per line, ^D to end')
        for line in sys.stdin:
            line = line.strip()
            if line:
                one(line)
    elif args.turn:
        one(args.turn)
    else:
        ap.error("need --turn or --interactive")


if __name__ == "__main__":
    main()
