#!/usr/bin/env python3
"""Deterministic Matrix embedding/retrieval/rerank smoke test."""
import json, math, subprocess, urllib.request
EMBED="http://127.0.0.1:8187/v1/embeddings"
RERANK_SCRIPT="/Users/agent/dsco/scripts/jina_tiny_rerank.py"
def post(url,p):
 r=urllib.request.urlopen(urllib.request.Request(url,data=json.dumps(p).encode(),headers={"Content-Type":"application/json"}),timeout=120);return json.load(r)
def cosine(a,b):return sum(x*y for x,y in zip(a,b))/(math.sqrt(sum(x*x for x in a))*math.sqrt(sum(y*y for y in b)))
def main():
 docs=["A neural router selects the best language model for each request.","Baltimore will have rain tomorrow.","Banana bread uses ripe bananas."]
 query="How does an LLM router select a model?"
 e=post(EMBED,{"model":"jina-v5-nano-retrieval","input":[query]+docs})["data"]
 q=e[0]["embedding"]; scores=[cosine(q,x["embedding"]) for x in e[1:]]; retrieved=sorted(range(len(docs)),key=lambda i:scores[i],reverse=True)
 rr=json.loads(subprocess.check_output(["python3",RERANK_SCRIPT,"--query",query,"--documents-json",json.dumps(docs)],text=True))["results"]
 out={"embedding_dims":len(q),"retrieval_order":retrieved,"retrieval_scores":scores,"rerank_order":[x["index"] for x in rr],"rerank_scores":[x["relevance_score"] for x in rr]}
 out["ok"]=retrieved[0]==0 and out["rerank_order"][0]==0
 print(json.dumps(out,indent=2));raise SystemExit(0 if out["ok"] else 1)
if __name__=="__main__":main()
