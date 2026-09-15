#!/usr/bin/env python3
"""Continuous terminal host: every price observation is executed by Lingo.
Quit with q/Ctrl-C. No trading, credentials, or automatically executed config.
"""
import collections, datetime, json, os, pathlib, select, subprocess, sys, time
root=pathlib.Path(__file__).resolve().parents[2]
status=root/'.workspace/bitcoin-live/status.json'
status.parent.mkdir(parents=True,exist_ok=True)
prices=collections.deque(maxlen=300)
seq=0
last=None
print('\033[?25l',end='',flush=True)
try:
 while True:
  started=time.monotonic(); error=None
  try:
   p=subprocess.run([str(root/'dsco'),'lingo','run',str(root/'examples/lingo/bitcoin-quote.lingo')],capture_output=True,text=True,timeout=12)
   if p.returncode: raise RuntimeError(p.stderr[-180:] or p.stdout[-180:])
   doc=json.loads(p.stdout); quote=doc['value']
   seq+=1; last=quote; prices.append((time.monotonic(),quote['price']))
  except Exception as e: error=str(e)[:180]
  now=datetime.datetime.now(datetime.timezone.utc)
  age=(now-datetime.datetime.fromisoformat(last['observed_at'])).total_seconds() if last else None
  record={'pid':os.getpid(),'sequence':seq,'checked_at':now.isoformat(),'quote':last,'error':error,'age_seconds':age,'interval_seconds':2,'prediction_markets':'unavailable: public endpoint returned HTTP 403','order_book':'unavailable: public endpoint returned HTTP 403'}
  temp=status.with_suffix('.tmp'); temp.write_text(json.dumps(record)+'\n'); os.replace(temp,status)
  lines=['\033[1;36m₿  BITCOIN / LIVE LINGO\033[0m','',('\033[1;32m$'+format(last['price'],',.2f')+' USD\033[0m') if last else 'Connecting...', '', 'Coinbase spot · actual observed price', 'Fetched: '+(last['observed_at'] if last else 'not yet'), f'Observation #{seq} · polling every 2s + request latency']
  if len(prices)>1:
   delta=(prices[-1][1]/prices[0][1]-1)*100
   lines+=['',f'Observed-window move: {delta:+.4f}% over {prices[-1][0]-prices[0][0]:.0f}s',f'Observed low / high: ${min(p[1] for p in prices):,.2f} / ${max(p[1] for p in prices):,.2f}']
  lines+=['','FACTORS / DATA HEALTH','Prediction markets: unavailable (HTTP 403)','Order-book / volume feed: unavailable (HTTP 403)','No synthetic price, inferred probability, or causal claim.','',('STALE / FETCH FAILED: '+error) if error else f'Feed OK · observation age {age:.1f}s','Polling, not exchange-tick streaming. q + Enter or Ctrl-C stops.']
  print('\033[H\033[2J'+'\n'.join(lines),flush=True)
  delay=max(.1,2-(time.monotonic()-started))
  if select.select([sys.stdin],[],[],delay)[0]:
   if sys.stdin.readline().strip().lower()=='q': break
except KeyboardInterrupt: pass
finally:
 print('\033[?25h\nTicker stopped.',flush=True)
 if status.exists():
  state=json.loads(status.read_text()); state['stopped']=True; status.write_text(json.dumps(state)+'\n')
