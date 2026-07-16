#!/usr/bin/env python3
"""Entropix-inspired best-of-N sampler for local OpenAI-compatible models.

This is an executable sampler *against currently served models*: it requests N
independent continuations with logprobs, measures token entropy/varentropy and
sequence likelihood, then selects a completion using an explicit policy.
Unlike shadow telemetry, the policy changes the completion returned to callers.
It cannot alter an individual continuation token-by-token without a decoder hook.
"""
import argparse, json, math, random, statistics, sys, time, urllib.request

def complete(url, model, messages, seed, temperature, top_p, max_tokens, topk):
    body={"model":model,"messages":messages,"stream":False,"seed":seed,
          "temperature":temperature,"top_p":top_p,"max_tokens":max_tokens,
          "logprobs":True,"top_logprobs":topk}
    if model.startswith("qwen35-"): body["reasoning_effort"]="none"
    req=urllib.request.Request(url,data=json.dumps(body).encode(),headers={"Content-Type":"application/json"})
    t=time.perf_counter()
    with urllib.request.urlopen(req,timeout=300) as r: payload=json.load(r)
    c=payload["choices"][0]; entries=(c.get("logprobs") or {}).get("content",[])
    per=[]
    for e in entries:
        alts=e.get("top_logprobs") or []
        ps=[math.exp(a["logprob"]) for a in alts]
        mass=sum(ps)
        if mass:
            q=[p/mass for p in ps]
            surprisal=[-math.log(max(p,1e-300)) for p in q]
            h=sum(p*s for p,s in zip(q,surprisal))
            v=sum(p*(s-h)**2 for p,s in zip(q,surprisal))
        else: h=v=0.0
        per.append({"token":e.get("token"),"logprob":e.get("logprob"),"entropy":h,"varentropy":v,"topk_mass":mass})
    lps=[x["logprob"] for x in per if x["logprob"] is not None]
    return {"seed":seed,"text":c.get("message",{}).get("content","") or "",
            "finish_reason":c.get("finish_reason"),"seconds":time.perf_counter()-t,
            "mean_logprob":statistics.fmean(lps) if lps else -1e9,
            "joint_logprob":sum(lps),"mean_entropy":statistics.fmean([x["entropy"] for x in per]) if per else 1e9,
            "max_entropy":max((x["entropy"] for x in per),default=1e9),
            "mean_varentropy":statistics.fmean([x["varentropy"] for x in per]) if per else 1e9,
            "tokens":per,"usage":payload.get("usage",{})}

def select(candidates, policy):
    # Likelihood is length-normalized. Entropy policy rewards decisiveness but
    # retains likelihood so pathological low-entropy junk does not dominate.
    if policy=="likelihood": key=lambda x:x["mean_logprob"]
    elif policy=="low_entropy": key=lambda x:x["mean_logprob"]-0.25*x["mean_entropy"]-0.10*x["mean_varentropy"]
    elif policy=="consensus":
        counts={}
        for x in candidates: counts[x["text"].strip()]=counts.get(x["text"].strip(),0)+1
        key=lambda x:(counts[x["text"].strip()],x["mean_logprob"])
    else: raise ValueError(policy)
    return max(candidates,key=key), key

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--model",required=True); ap.add_argument("--prompt")
    ap.add_argument("--messages-json"); ap.add_argument("--url",default="http://127.0.0.1:1234/v1/chat/completions")
    ap.add_argument("-n",type=int,default=5); ap.add_argument("--temperature",type=float,default=.8)
    ap.add_argument("--top-p",type=float,default=.95); ap.add_argument("--top-logprobs",type=int,default=20)
    ap.add_argument("--max-tokens",type=int,default=256); ap.add_argument("--seed",type=int,default=20260712)
    ap.add_argument("--policy",choices=["likelihood","low_entropy","consensus"],default="low_entropy")
    ap.add_argument("--details",action="store_true"); a=ap.parse_args()
    if a.messages_json: messages=json.loads(a.messages_json)
    elif a.prompt: messages=[{"role":"user","content":a.prompt}]
    else: ap.error("--prompt or --messages-json required")
    cs=[]
    for i in range(a.n):
        try: cs.append(complete(a.url,a.model,messages,a.seed+i,a.temperature,a.top_p,a.max_tokens,a.top_logprobs))
        except Exception as e: print(json.dumps({"seed":a.seed+i,"error":str(e)}),file=sys.stderr)
    if not cs: return 1
    winner,key=select(cs,a.policy)
    result={"model":a.model,"policy":a.policy,"selected":winner["text"],"selected_seed":winner["seed"],
            "candidates":[{"seed":x["seed"],"text":x["text"],"score":key(x),"mean_logprob":x["mean_logprob"],
            "mean_entropy":x["mean_entropy"],"max_entropy":x["max_entropy"],"mean_varentropy":x["mean_varentropy"]} for x in cs]}
    if a.details: result["selected_details"]=winner
    print(json.dumps(result,ensure_ascii=False,indent=2)); return 0
if __name__=="__main__": sys.exit(main())
