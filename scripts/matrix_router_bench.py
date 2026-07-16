#!/usr/bin/env python3
"""Counterfactual benchmark for DSCO's Matrix model-router lanes."""
import argparse, json, math, re, time, urllib.request, urllib.error

LANES = [
    ("managed-4b", "http://127.0.0.1:1234/v1/chat/completions", "qwen3-4b"),
    ("managed-27b", "http://127.0.0.1:1234/v1/chat/completions", "qwen35-27b-dense"),
    ("dense-27b", "http://127.0.0.1:8190/v1/chat/completions", "local"),
    ("spec-27b-2b", "http://127.0.0.1:8191/v1/chat/completions", "local"),
]
TASKS = [
    {"id":"arith","prompt":"Return only the integer result of 37*19+8.","expect":"711"},
    {"id":"extract","prompt":"Return only compact JSON with keys name and age from: Alice Chen is 34 years old.","json":{"name":"Alice Chen","age":34}},
    {"id":"logic","prompt":"All ravens are birds. No birds are mammals. Can any raven be a mammal? Answer only yes or no.","expect":"no"},
    {"id":"code","prompt":"Return only the output of this Python expression: [x*x for x in range(6) if x%2]","expect":"[1, 9, 25]"},
    {"id":"format","prompt":"Sort these words alphabetically and return only a comma-separated list: pear apple fig banana","expect":"apple, banana, fig, pear"},
    {"id":"debug","prompt":"A C loop is `for (size_t i=n-1; i>=0; --i)`. In one short sentence identify the bug.","contains":["unsigned","underflow"]},
]

def request(url, payload, timeout=180):
    data=json.dumps(payload).encode()
    req=urllib.request.Request(url,data=data,headers={"Content-Type":"application/json"})
    t=time.perf_counter()
    with urllib.request.urlopen(req,timeout=timeout) as r: body=json.load(r)
    return body,time.perf_counter()-t

def norm(s): return re.sub(r"\s+"," ",str(s).strip().lower())
def score(task,text):
    n=norm(text)
    if "json" in task:
        try: return float(json.loads(text)==task["json"])
        except Exception: return 0.0
    if "contains" in task: return sum(x in n for x in task["contains"])/len(task["contains"])
    return float(norm(task["expect"])==n)

def run(lane,task,max_tokens):
    name,url,model=lane
    payload={"model":model,"messages":[{"role":"user","content":task["prompt"]}],"temperature":0,"max_tokens":max_tokens,"stream":False,"reasoning_effort":"none"}
    try:
        body,secs=request(url,payload)
        choice=(body.get("choices") or [{}])[0]; msg=choice.get("message") or {}; text=msg.get("content") or ""
        usage=body.get("usage") or {}; timings=body.get("timings") or {}
        return {"task":task["id"],"lane":name,"ok":True,"seconds":round(secs,6),"score":score(task,text),"text":text,"finish_reason":choice.get("finish_reason"),"usage":usage,"timings":timings}
    except Exception as e:
        return {"task":task["id"],"lane":name,"ok":False,"seconds":None,"score":0.0,"error":repr(e)}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--repeats",type=int,default=2); ap.add_argument("--max-tokens",type=int,default=256); ap.add_argument("--output",default="matrix-router-bench.jsonl"); a=ap.parse_args()
    rows=[]
    for rep in range(a.repeats):
        for task in TASKS:
            for lane in LANES:
                row=run(lane,task,a.max_tokens); row["repeat"]=rep; rows.append(row)
                print(json.dumps(row,ensure_ascii=False),flush=True)
    with open(a.output,"w") as f:
        for row in rows:f.write(json.dumps(row,ensure_ascii=False)+"\n")
    summary={}
    for name,_,_ in LANES:
        xs=[x for x in rows if x["lane"]==name]
        summary[name]={"n":len(xs),"success_rate":sum(x["ok"] for x in xs)/len(xs),"mean_score":sum(x["score"] for x in xs)/len(xs),"mean_seconds":sum(x["seconds"] for x in xs if x["seconds"] is not None)/max(1,sum(x["seconds"] is not None for x in xs))}
    with open(a.output+".summary.json","w") as f:json.dump(summary,f,indent=2)
    print(json.dumps({"summary":summary},indent=2))
if __name__=="__main__":main()
