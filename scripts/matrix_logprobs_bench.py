#!/usr/bin/env python3
"""Deep compatibility/calibration benchmark for Matrix logprob-capable lanes."""
import argparse, json, math, statistics, time, urllib.request, urllib.error
LANES=[
 ("managed-4b","http://127.0.0.1:1234/v1/chat/completions","qwen3-4b"),
 ("managed-27b","http://127.0.0.1:1234/v1/chat/completions","qwen35-27b-dense"),
 ("dense-27b","http://127.0.0.1:8190/v1/chat/completions","local"),
 ("spec-27b-2b","http://127.0.0.1:8191/v1/chat/completions","local")]
TASKS=[
 ("forced","Reply with exactly: Paris","Paris"),
 ("arithmetic","Return only the integer result of 37*19+8.","711"),
 ("binary","All ravens are birds. No birds are mammals. Can a raven be a mammal? Answer only yes or no.","no"),
 ("choice","Which is prime? Answer only A, B, C, or D. A:21 B:27 C:29 D:33","C"),
 ("uncertain","Answer only yes or no: Is the 10,000,001st decimal digit of pi 7?",""),
 ("json","Return only compact JSON: the name is Ada and age is 36.",'{"name":"Ada","age":36}')]

def call(url,payload):
 req=urllib.request.Request(url,data=json.dumps(payload).encode(),headers={"Content-Type":"application/json"}); t=time.perf_counter()
 try:
  with urllib.request.urlopen(req,timeout=240) as r:return json.load(r),time.perf_counter()-t,None
 except urllib.error.HTTPError as e:return None,time.perf_counter()-t,"HTTP %d %s"%(e.code,e.read().decode())
 except Exception as e:return None,time.perf_counter()-t,repr(e)
def lpstats(lp):
 content=(lp or {}).get("content") or []
 chosen=[]; ent=[]; margins=[]; coverage=[]
 for tok in content:
  if isinstance(tok.get("logprob"),(int,float)):chosen.append(tok["logprob"])
  tops=tok.get("top_logprobs") or []
  ps=[math.exp(x["logprob"]) for x in tops if isinstance(x.get("logprob"),(int,float))]
  if ps:
   ent.append(-sum(p*math.log(max(p,1e-300)) for p in ps)); coverage.append(sum(ps))
  if len(tops)>1:margins.append(tops[0]["logprob"]-tops[1]["logprob"])
 return {"tokens_with_logprobs":len(content),"mean_chosen_logprob":statistics.fmean(chosen) if chosen else None,"min_chosen_logprob":min(chosen) if chosen else None,"mean_top5_entropy_nats":statistics.fmean(ent) if ent else None,"mean_top1_top2_margin":statistics.fmean(margins) if margins else None,"mean_top5_mass":statistics.fmean(coverage) if coverage else None,"tokens":[x.get("token") for x in content]}
def norm(s):return "".join(str(s).lower().split())
def main():
 ap=argparse.ArgumentParser();ap.add_argument("--output",default="matrix-logprobs-deep.jsonl");ap.add_argument("--repeats",type=int,default=2);a=ap.parse_args(); rows=[]
 variants=[("top1",1),("top5",5),("top20",20)]
 for rep in range(a.repeats):
  for tid,prompt,expect in TASKS:
   for lname,url,model in LANES:
    for variant,k in variants:
     payload={"model":model,"messages":[{"role":"user","content":prompt}],"temperature":0,"max_tokens":192,"reasoning_effort":"none","logprobs":True,"top_logprobs":k,"seed":42}
     body,secs,err=call(url,payload); row={"repeat":rep,"task":tid,"lane":lname,"variant":variant,"seconds":round(secs,6),"error":err}
     if body:
      c=(body.get("choices") or [{}])[0]; text=(c.get("message") or {}).get("content") or ""; lp=c.get("logprobs")
      row.update({"text":text,"exact":bool(expect and norm(text)==norm(expect)),"finish_reason":c.get("finish_reason"),"usage":body.get("usage"),"logprobs_present":lp is not None,"lp":lpstats(lp)})
     rows.append(row); print(json.dumps(row,ensure_ascii=False),flush=True)
 with open(a.output,"w") as f:
  for x in rows:f.write(json.dumps(x,ensure_ascii=False)+"\n")
 # Compact aggregate plus exact dense/spec token-distribution comparison.
 agg={}
 for lane,_,_ in LANES:
  xs=[x for x in rows if x["lane"]==lane]; ys=[x for x in xs if not x["error"]]
  agg[lane]={"n":len(xs),"api_success":len(ys)/len(xs),"logprobs_rate":sum(bool(x.get("logprobs_present")) for x in ys)/max(1,len(ys)),"exact_rate":sum(bool(x.get("exact")) for x in ys)/max(1,len(ys)),"mean_seconds":statistics.fmean(x["seconds"] for x in ys) if ys else None}
 pairs=[]
 for rep in range(a.repeats):
  for tid,_,_ in TASKS:
   for variant,_ in variants:
    d=next((x for x in rows if x["repeat"]==rep and x["task"]==tid and x["variant"]==variant and x["lane"]=="dense-27b"),None); s=next((x for x in rows if x["repeat"]==rep and x["task"]==tid and x["variant"]==variant and x["lane"]=="spec-27b-2b"),None)
    if d and s and d.get("lp") and s.get("lp"):
     dt=d["lp"]["tokens"];st=s["lp"]["tokens"];pairs.append({"task":tid,"variant":variant,"token_sequence_equal":dt==st,"common_prefix":next((i for i,(a,b) in enumerate(zip(dt,st)) if a!=b),min(len(dt),len(st))),"dense_seconds":d["seconds"],"spec_seconds":s["seconds"]})
 summary={"aggregate":agg,"dense_spec_pairs":pairs,"dense_spec_equal_rate":sum(x["token_sequence_equal"] for x in pairs)/max(1,len(pairs))}
 with open(a.output+".summary.json","w") as f:json.dump(summary,f,indent=2)
 print(json.dumps(summary,indent=2))
if __name__=="__main__":main()
