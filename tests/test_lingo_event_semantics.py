#!/usr/bin/env python3
"""Exercise the real Lua runtime's audit boundary with an injected observer.

No DSCO tools or services execute. Native IPC/durability tests exercise the
production host separately; this fixture can reject, inspect and mutate every
detached event payload, which the public Lingo API intentionally cannot do.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
local factory=assert(loadfile(arg[1]))()
local checks=0
local function check(name,fn) fn(); checks=checks+1; print("PASS "..name) end
local function make(observe)
  local null=newproxy(true)
  local arrays=setmetatable({}, {__mode="k"})
  local events={}
  local calls=0
  local function json(v,seen)
    if v==null then return "null" end
    if type(v)=="string" then return string.format("%q",v) end
    if type(v)=="boolean" then return tostring(v) end
    if type(v)=="number" then assert(v==v and math.abs(v)~=math.huge);return tostring(v) end
    assert(type(v)=="table","private/non-JSON value escaped: "..type(v))
    assert(getmetatable(v)==nil,"private proxy escaped")
    seen=seen or {};assert(not seen[v],"cyclic event");seen[v]=true
    local parts={}
    for k,x in pairs(v) do
      assert(type(k)=="string" or (arrays[v] and type(k)=="number"))
      parts[#parts+1]=tostring(k)..":"..json(x,seen)
    end
    seen[v]=nil;table.sort(parts);return "{"..table.concat(parts,",").."}"
  end
  local host={null=null,array=function(v)arrays[v]=true;return v end,
    is_array=function(v)return arrays[v] or false end,encode=json,
    check_budget=function()end,world_io=function()end,view=function()end,
    caller_source=function()return {sha256="fixture-source"}end,
    function_source=function()return {sha256="fixture-function"}end,
    call=function(name,args)calls=calls+1;return {ok=true,tool=name,result="{}",sequence=calls}end}
  if observe~=false then
    host.observe=function(kind,payload)
      json(payload)
      events[#events+1]={kind=kind,payload=payload}
      if type(observe)=="function" then observe(kind,payload) end
    end
  end
  return factory(host),events,function()return calls end,host
end
local function count(events,kind)
  local n=0;for _,event in ipairs(events)do if event.kind==kind then n=n+1 end end;return n
end
local function latest(events,kind)
  for i=#events,1,-1 do if events[i].kind==kind then return events[i].payload end end
  error("missing event "..kind)
end
local l,events,calls=make()
local w=l.world{id="audit-world"}
w:define("Node",{a=l.stored("integer",1),b=l.stored("integer",10),choose=l.stored("boolean",true),
  selected=l.derived("integer",function(s)return s.choose and s.a or s.b end),
  twice=l.derived("integer",function(s)return s.selected*2 end),
  scaled=l.derived("integer",function(s,ctx,args)return s.a*args.factor end,{params={factor="integer"}}),
  fragile=l.derived("integer",function(s)assert(s.a>=0,"negative");return s.a end)})
local x=w:new("Node","x")
check("start/read/dependency/complete order and cache reuse",function()
  assert(x.twice==2)
  local completed=latest(events,"calculated")
  assert(completed.world_id=="audit-world" and completed.address.object=="x")
  assert(completed.address.field=="twice" and completed.value==2)
  assert(count(events,"calculation_started")==2)
  assert(count(events,"dependency_read")==3 and count(events,"dependency_added")==3)
  local started={}
  for _,e in ipairs(events)do
    if e.kind=="calculation_started" then started[e.payload.evaluation_id]=true end
    if e.kind=="calculated" then assert(started[e.payload.evaluation_id]) end
  end
  assert(x.twice==2 and count(events,"calculation_started")==2)
  assert(latest(events,"cache_hit").address.field=="twice")
end)
check("invalidation is lazy and branch changes remove old edges",function()
  local before=count(events,"calculation_started")
  w:set(x,"a",3)
  assert(count(events,"calculation_started")==before and w:why(x,"twice").state=="invalid")
  assert(count(events,"invalidated")==3)
  assert(x.twice==6)
  w:set(x,"choose",false);assert(x.twice==20)
  assert(count(events,"dependency_removed")>=3)
  local deps=w:why(x,"selected").dependencies
  assert(#deps==2 and deps[1].field=="b" and deps[2].field=="choose")
end)
check("parameter identities and detached result values",function()
  assert(w:get(x,"scaled",{factor=2})==6)
  local result=latest(events,"calculated")
  assert(result.address.args.factor==2)
  result.address.args.factor=99
  assert(w:get(x,"scaled",{factor=2})==6)
  assert(latest(events,"cache_hit").address.args.factor==2)
end)
check("scenario entry/exit preserves context and blocks host effects",function()
  local before=calls()
  w:scenario("trial",function(s)
    s:override(x,"selected",12);assert(x.twice==24)
    local ok,err=l.try(function()return l.call("cwd",{})end)
    assert(not ok and err:find("inside a scenario",1,true))
    local denied=latest(events,"tool_call_denied")
    assert(denied.world_id=="audit-world" and denied.scenario=="trial#1")
    s:restore(x,"selected");assert(x.twice==20)
  end)
  assert(calls()==before and x.twice==20)
  assert(latest(events,"scenario_exit").parent_context=="base")
  assert(latest(events,"scenario_exit").detail=="success")
end)
check("calculation denial and error outcome have complete events",function()
  w:define("Bad",{v=l.derived("integer",function()l.call("cwd",{});return 0 end)})
  local bad=w:new("Bad","bad")
  local before=calls();local ok=l.try(function()return bad.v end)
  assert(not ok and calls()==before)
  assert(latest(events,"tool_call_denied").evaluation_depth==1)
  assert(latest(events,"calculation_failed").address.object=="bad")
  w:set(x,"a",-1);assert(not l.try(function()return x.fragile end))
  assert(latest(events,"calculation_failed").detail:find("negative",1,true))
end)
check("reference payloads contain explicit identities and no handles",function()
  w:define("Product",{requires=l.stored(l.types.list(l.types.ref("Node")))})
  w:new("Product","product",{requires={x}})
  local data=latest(events,"object_created").values.requires[1]
  assert(data.world_id=="audit-world" and data.class=="Node" and data.object=="x")
  assert(data~=x)
end)
check("equal write does not create an unread cache node",function()
  local fresh=w:new("Node","fresh")
  assert(w:why(fresh,"a").state=="unread")
  assert(not w:set(fresh,"a",1))
  assert(w:why(fresh,"a").state=="unread")
end)
check("stream exceeds ring capacity without losing an observed event",function()
  local previous=#events
  for i=1,2300 do assert(x.b==10) end
  assert(#events-previous==4600)
  local page=w:events(0,100)
  assert(page.truncated and #page.events==100)
  local n=0;local after=0;local last=0
  while true do
    local p=w:events(after,100)
    if #p.events==0 then break end
    for _,e in ipairs(p.events)do assert(e.seq>last);last=e.seq;n=n+1 end
    after=p.next_cursor
  end
  assert(n==4096)
  local previous_seq=0
  for _,e in ipairs(events)do
    if e.payload.seq then assert(e.payload.seq==previous_seq+1);previous_seq=e.payload.seq end
  end
end)
check("observer payload mutation cannot mutate values or inspector events",function()
  local observed_l,observed_events=make(function(kind,payload)
    payload.detail="tampered"
    if payload.values then payload.values.a=777 end
  end)
  local world=observed_l.world{id="detached"}
  world:define("X",{a=observed_l.stored("integer",4)})
  local obj=world:new("X","x")
  assert(obj.a==4)
  for _,e in ipairs(world:events(0,100).events)do assert(e.detail~="tampered") end
end)
check("observer failure propagates rather than silently using the ring",function()
  local broken=make(function()error("audit unavailable")end)
  local ok,err=pcall(function()broken.world{id="fail"}end)
  assert(not ok and tostring(err):find("audit unavailable",1,true))
end)
check("standalone hosts without an observer remain supported",function()
  local plain=make(false)
  local world=plain.world{id="plain"};world:define("X",{v=plain.stored("integer",3)})
  assert(world:new("X","x").v==3 and #world:events(0,100).events>0)
end)
print("PASS "..checks.." Lingo observer groups")
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--luajit", default=shutil.which("luajit"))
    args = parser.parse_args()
    if not args.luajit:
        parser.error("LuaJIT is required to test the actual runtime source")
    run = subprocess.run([args.luajit, "-", str(ROOT / "lingo/runtime.lua")],
                         input=FIXTURE, text=True, capture_output=True, timeout=20)
    print(run.stdout, end="")
    if run.returncode:
        raise SystemExit(run.stderr or run.returncode)


if __name__ == "__main__":
    main()
