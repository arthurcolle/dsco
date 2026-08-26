#!/usr/bin/env python3
"""Paired benchmark: greedy vs best-of-N local entropy policies."""
import argparse, importlib.util, json, re, statistics, sys, time
from pathlib import Path
spec=importlib.util.spec_from_file_location("ep",Path(__file__).with_name("entropix_proxy.py")); ep=importlib.util.module_from_spec(spec); spec.loader.exec_module(ep)
CASES=[
("add-carry","Compute 88888888 + 11111112. Reply only with the decimal integer.","100000000"),
("multiply-a","Compute 12345 * 678. Reply only with the decimal integer.","8369910"),
("multiply-b","Compute 9876 * 5432. Reply only with the decimal integer.","53646432"),
("mixed-a","Compute (999 * 37) - 100. Reply only with the decimal integer.","36863"),
("mixed-b","Compute (1234 * 56) - 789. Reply only with the decimal integer.","68315"),
("mod-a","Compute the remainder when 123456789 is divided by 97. Reply only with the decimal integer.","39"),
("mod-b","Compute the remainder when 1000000007 is divided by 97. Reply only with the decimal integer.","41"),
("gcd","Compute the greatest common divisor of 12348 and 5670. Reply only with the decimal integer.","126"),
("lcm","Compute the least common multiple of 144 and 360. Reply only with the decimal integer.","720"),
("power","Compute 23^5. Reply only with the decimal integer.","6436343"),
("logic","All bloops are razzies. No razzies are lazzies. Can any bloop be a lazzy? Reply only yes or no.","no"),
("probability","A fair coin is tossed 4 times. How many outcomes contain exactly 2 heads? Reply only with the decimal integer.","6"),
("algebra","If 3x + 7 = 52, what is x? Reply only with the decimal integer.","15"),
("sequence","What is the next integer: 2, 6, 12, 20, 30, ? Reply only with the decimal integer.","42"),
("fact","Who wrote The Brothers Karamazov? Reply only with the author's full name.","fyodor dostoevsky"),
]
def norm(s): return re.sub(r"[^a-z0-9]+"," ",s.lower()).strip()
def correct(s,g): return norm(s)==norm(g)
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--model',default='qwen35-27b-dense'); ap.add_argument('-n',type=int,default=5); ap.add_argument('--temperature',type=float,default=.9); ap.add_argument('--seed',type=int,default=73000); ap.add_argument('--output'); a=ap.parse_args()
 rows=[]
 for ci,(name,prompt,gold) in enumerate(CASES):
  msgs=[{'role':'user','content':prompt}]
  greedy=ep.complete('http://127.0.0.1:1234/v1/chat/completions',a.model,msgs,a.seed+ci,0,1,96,20)
  pool=[]
  for j in range(a.n): pool.append(ep.complete('http://127.0.0.1:1234/v1/chat/completions',a.model,msgs,a.seed+1000+ci*a.n+j,a.temperature,.95,96,20))
  picks={p:ep.select(pool,p)[0] for p in ('likelihood','low_entropy','consensus')}
  row={'case':name,'gold':gold,'greedy':greedy['text'],'greedy_ok':correct(greedy['text'],gold),'oracle':any(correct(x['text'],gold) for x in pool),'pool':[x['text'] for x in pool]}
  for p,x in picks.items(): row[p]=x['text']; row[p+'_ok']=correct(x['text'],gold)
  rows.append(row); print(json.dumps(row,ensure_ascii=False),flush=True)
 summary={k:sum(r[k+'_ok'] for r in rows) for k in ('greedy','likelihood','low_entropy','consensus')}; summary['oracle_pool']=sum(r['oracle'] for r in rows); summary['total']=len(rows)
 # transitions relative to greedy
 for p in ('likelihood','low_entropy','consensus'):
  summary[p+'_fixes']=sum((not r['greedy_ok']) and r[p+'_ok'] for r in rows); summary[p+'_regressions']=sum(r['greedy_ok'] and not r[p+'_ok'] for r in rows)
 out={'model':a.model,'n':a.n,'temperature':a.temperature,'summary':summary,'rows':rows}; print('FINAL '+json.dumps(out,ensure_ascii=False))
 if a.output: Path(a.output).write_text(json.dumps(out,ensure_ascii=False,indent=2))
 return 0
if __name__=='__main__': sys.exit(main())
