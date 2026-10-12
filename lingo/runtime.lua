-- Lingo 0.2 reference semantics. Loaded privately by the DSCO LuaJIT host.
-- The returned API exposes objects, values, scenarios and explicit effects.
return function(host)
  local L = {version = "0.2", null = host.null}
  local states, refs = setmetatable({}, {__mode="k"}), setmetatable({}, {__mode="k"})
  local methods = {}
  local active_world, evaluation_depth, scenario_depth = nil, 0, 0
  local scenario_world, world_sequence, call_sequence = nil, 0, 0
  local function fail(message)
    if host.observe then
      local s=states[active_world or scenario_world]
      host.observe("validation_failed",{message=message,world_id=s and s.id or L.null,
        evaluation_depth=evaluation_depth,scenario_depth=scenario_depth})
    end
    error("lingo: " .. message, 0)
  end
  local function need(condition, message) if not condition then fail(message) end end
  local function keys(t)
    local k = {}; for key in pairs(t) do k[#k+1] = key end
    table.sort(k, function(a,b) return tostring(a)<tostring(b) end); return k
  end
  local function copy(v, seen)
    if type(v) ~= "table" then return v end
    seen = seen or {}; need(not seen[v], "cyclic data is not a value")
    seen[v] = true; local out = {}
    for k,x in pairs(v) do
      need(type(k)=="string" or type(k)=="number", "data keys must be strings or array indices")
      out[k] = copy(x, seen)
    end
    seen[v] = nil; if host.is_array(v) then host.array(out) end; return out
  end
  local function canonical(v, seen)
    if v == L.null then return "null" end
    local kind = type(v)
    if refs[v] then return "ref:" .. refs[v].id end
    if kind == "string" then return host.encode(v) end
    if kind == "boolean" then return v and "true" or "false" end
    if kind == "number" then
      need(v==v and v~=math.huge and v~=-math.huge, "non-finite number")
      return string.format("%.17g", v == 0 and 0 or v)
    end
    if kind == "nil" then return "nil" end
    need(kind=="table", "unsupported value: "..kind)
    seen=seen or {}; need(not seen[v], "cyclic data"); seen[v]=true
    local parts={}
    for _,k in ipairs(keys(v)) do
      parts[#parts+1]=canonical(k,seen)..":"..canonical(v[k],seen)
    end
    seen[v]=nil; local array=host.is_array(v) or #v>0
    return (array and "[" or "{")..table.concat(parts,",")..(array and "]" or "}")
  end
  local function validate(t,v,w,path)
    path=path or "value"
    if type(t)=="table" then
      if t.kind=="optional" then if v==L.null then return end; return validate(t.of,v,w,path) end
      if t.kind=="ref" then
        local r=refs[v]; need(r and r.world==w and r.class==t.class, path.." requires reference to "..t.class); return
      end
      if t.kind=="list" then
        need(type(v)=="table",path.." requires a list")
        local n=0; for k in pairs(v) do need(type(k)=="number" and k>=1 and k%1==0,path.." requires dense 1-based indices"); n=n+1 end
        need(n==#v,path.." has a hole"); for i=1,n do validate(t.of,v[i],w,path.."["..i.."]") end; host.array(v); return
      end
      if t.kind=="record" then
        need(type(v)=="table",path.." requires a record")
        for k in pairs(v) do need(t.fields[k]~=nil,path.." has unknown field "..tostring(k)) end
        for k,vt in pairs(t.fields) do validate(vt,v[k],w,path.."."..k) end; return
      end
      fail("unknown type descriptor")
    end
    if t=="json" then need(v~=nil,path.." requires JSON; use lingo.null for null"); canonical(v); host.encode(v); return end
    if t=="integer" then
      need(type(v)=="number" and v%1==0 and math.abs(v)<=9007199254740991,path.." requires an exact integer"); return
    end
    need(t=="number" or t=="string" or t=="boolean", "unknown type "..tostring(t))
    need(type(v)==t,path.." requires "..t)
    if t=="number" then need(v==v and v~=math.huge and v~=-math.huge,path.." must be finite") end
  end
  L.types = {
    number="number", integer="integer", string="string", boolean="boolean", json="json",
    ref=function(class) return {kind="ref",class=class} end,
    list=function(of) return {kind="list",of=of} end,
    optional=function(of) return {kind="optional",of=of} end,
    record=function(fields) return {kind="record",fields=copy(fields)} end,
  }
  function L.stored(t, default)
    return {kind="stored",type=t,default=copy(default),has_default=default~=nil}
  end
  function L.derived(t, run, options)
    need(type(run)=="function", "derived value requires a function")
    options=options or {}; need(options.cache==nil or options.cache=="memo" or options.cache=="none","cache must be memo or none")
    return {kind="derived",type=t,run=run,params=copy(options.params or {}),cache=options.cache or "memo"}
  end
  local function state(w)
    local s=states[w]; need(s,"expected a Lingo world")
    need(not active_world or active_world==w,"cross-world reads inside a calculation require an explicit imported value")
    return s
  end
  local function record(w,obj)
    local r=refs[obj]; need(r and r.world==w,"reference belongs to a different world"); return r
  end
  local function definition(s,r,field)
    local d=s.classes[r.class].fields[field]; need(d,"unknown value "..r.id.."."..tostring(field)); return d
  end
  local function argcheck(d,args,w)
    if args==nil then args={} end
    need(type(args)=="table" and not host.is_array(args),"arguments must be a record")
    local params=d.params or {}
    for k in pairs(args) do need(params[k],"unknown value argument "..tostring(k)) end
    for k,t in pairs(params) do validate(t,args[k],w,"argument "..k) end
    return copy(args)
  end
  local function nodekey(id,field,args) return "value\0"..id.."\0"..field.."\0"..canonical(args or {}) end
  local function node(ctx,id,field,args,kind)
    local k=kind and (kind.."\0"..field) or nodekey(id,field,args); local n=ctx.nodes[k]
    if not n then
      n={key=k,object=id,field=field,args=copy(args or {}),context=ctx,valid=false,deps={},parents={},evaluations=0,hits=0,invalidations=0}
      ctx.nodes[k]=n
    end
    return n
  end
  -- Audit payloads contain detached values and explicit reference identities,
  -- never private object handles. The inspector ring remains a compact cache.
  local function observed(v)
    if v==L.null or v==nil then return L.null end
    local r=refs[v]
    if r then return {world_id=states[r.world].id,class=r.class,object=r.id} end
    if type(v)~="table" then return v end
    local out={}
    for k,x in pairs(v) do out[k]=observed(x) end
    if host.is_array(v) then host.array(out) end
    return out
  end
  local function address(n)
    return {object=n.object,field=n.field,args=observed(n.args),context=n.context.name}
  end
  local function emit(s,kind,n,detail,extra)
    local e={kind=kind,world_id=s and s.id or L.null,
      scenario=s and s.current.name or L.null,object=n and n.object or L.null,
      field=n and n.field or L.null,detail=detail or ""}
    if s then s.sequence=s.sequence+1; e.seq=s.sequence end
    if host.observe then
      local payload=copy(e)
      if n then
        payload.address=address(n)
        payload.evaluation_id=n.evaluation_id or L.null
        payload.parent_evaluation_id=n.parent_evaluation_id or L.null
      end
      if extra then for k,v in pairs(extra) do payload[k]=observed(v) end end
      -- Native audit failure is terminal; never swallow it or fall back to
      -- the lossy ring as if the event had been delivered.
      host.observe(kind,payload)
    end
    if s then
      s.events[(s.sequence-1)%4096+1]=e
    end
  end
  local function disconnect(s,n)
    for child in pairs(n.deps) do
      child.parents[n]=nil
      emit(s,"dependency_removed",n,nil,{dependency=address(child)})
    end
    n.deps={}
  end
  local function invalidate(s,n,why,seen)
    seen=seen or {}; if seen[n] then return end; seen[n]=true
    n.valid=false; n.invalidations=n.invalidations+1; n.reason=why
    emit(s,"invalidated",n,why,{invalidations=n.invalidations})
    for parent in pairs(n.parents) do invalidate(s,parent,why,seen) end
  end
  local function dependency(s,n)
    local parent=s.stack[#s.stack]
    if parent then
      parent.reads[n]=true
      emit(s,"dependency_read",parent,nil,{dependency=address(n)})
    end
  end
  local function overlay(ctx,k)
    local c=ctx
    while c do if c.overrides[k]~=nil then return true,c.overrides[k] end; c=c.parent end
    return false
  end
  function L.world(options)
    need(evaluation_depth==0,"cannot create a world during a value calculation")
    if options==nil then options={} end
    need(type(options)=="table","world options must be a record")
    for k in pairs(options) do need(k=="id","unknown world option "..tostring(k)) end
    world_sequence=world_sequence+1
    local id=options.id==nil and ("local:"..world_sequence) or options.id
    need(type(id)=="string" and #id>=1 and #id<=128 and not id:find("[%z\1-\31\127]"),"world ID must be 1..128 bytes without control characters")
    local w=newproxy(true); local mt=getmetatable(w)
    mt.__index=methods; mt.__newindex=function() fail("world state is private") end
    mt.__metatable="Lingo world"
    local base={name="base",nodes={},overrides={}}
    states[w]={id=id,classes={},interfaces={},objects={},base=base,current=base,stack={},events={},sequence=0,scenario_seq=0,evaluation_seq=0}
    emit(states[w],"world_created")
    return w
  end
  function methods:interface(name, fields)
    local s=state(self); need(evaluation_depth==0 and scenario_depth==0,"declarations require base script scope")
    need(type(name)=="string" and #name>0 and not name:find("\0",1,true) and not s.interfaces[name],"interface name must be new")
    s.interfaces[name]=copy(fields)
    emit(s,"interface_defined",nil,name)
  end
  function methods:define(name,fields,options)
    local s=state(self); need(evaluation_depth==0 and scenario_depth==0,"declarations require base script scope")
    need(type(name)=="string" and #name>0 and not name:find("\0",1,true) and not s.classes[name],"class name must be new")
    need(type(fields)=="table","class fields required"); options=options or {}
    need(options.immutable==nil or type(options.immutable)=="boolean","immutable must be boolean")
    need(options.version==nil or (type(options.version)=="string" and #options.version>0 and #options.version<=128),"class version must be 1..128 bytes")
    local out={}
    for k,d in pairs(fields) do
      need(type(k)=="string" and #k>0 and not k:find("\0",1,true),"invalid value name")
      need(type(d)=="table" and (d.kind=="stored" or d.kind=="derived"),"use stored or derived to define "..k)
      out[k]=copy(d)
      if d.kind=="derived" then out[k].source=host.function_source(d.run) end
    end
    for _,interface in ipairs(options.implements or {}) do
      local contract=s.interfaces[interface]; need(contract,"unknown interface "..interface)
      for field,t in pairs(contract) do need(out[field] and canonical(out[field].type)==canonical(t),"interface mismatch: "..interface.."."..field) end
    end
    s.classes[name]={fields=out,version=options.version or "1",implements=copy(options.implements or {}),
      immutable=options.immutable or false,source=host.caller_source()}
    emit(s,"class_defined",nil,name,{source=s.classes[name].source,version=s.classes[name].version})
  end
  local function make_ref(w,class,id,data,revision)
    local obj=newproxy(true); local r={world=w,class=class,id=id,values=data,revision=revision}
    refs[obj]=r
    local mt=getmetatable(obj)
    mt.__index=function(_,field) return w:get(obj,field) end
    mt.__newindex=function() fail("use world:set or scenario:override; reference fields cannot be assigned") end
    mt.__tostring=function() return class.."("..id..")" end; mt.__metatable="Lingo reference"
    return obj
  end
  function methods:new(class,id,values)
    local s=state(self); need(evaluation_depth==0 and scenario_depth==0,"object creation requires base script scope")
    local c=s.classes[class]; need(c,"unknown class "..tostring(class))
    need(type(id)=="string" and #id>0 and not id:find("\0",1,true) and not s.objects[id],"object identity must be new and nonempty")
    values=values or {}; local data={}
    for k in pairs(values) do need(c.fields[k] and c.fields[k].kind=="stored","unknown or derived initial field "..tostring(k)) end
    for k,d in pairs(c.fields) do
      if d.kind=="stored" then
        local v=values[k]; if v==nil and d.has_default then v=d.default end
        validate(d.type,v,self,id.."."..k); data[k]=copy(v)
      end
    end
    local obj=make_ref(self,class,id,data,1); s.objects[id]=obj
    local catalog=s.base.nodes["catalog\0"..class]
    if catalog then invalidate(s,catalog,"object created: "..id) end
    local presence=s.base.nodes["identity\0"..id]
    if presence then invalidate(s,presence,"object created: "..id) end
    emit(s,"object_created",nil,id,{class=class,object_id=id,revision=1,values=data}); return obj
  end
  function methods:ref(id)
    local s=state(self); need(type(id)=="string","object ID must be a string")
    local n=node(s.current,"@identity",id,{},"identity"); dependency(s,n); n.valid=true
    emit(s,"identity_read",n,nil,{present=s.objects[id]~=nil})
    need(s.objects[id],"unknown object "..id); return s.objects[id]
  end
  function methods:objects(class)
    local s=state(self); need(s.classes[class],"unknown class")
    local n=node(s.current,"@objects",class,{},"catalog"); dependency(s,n); n.valid=true
    local result=host.array{}
    for _,id in ipairs(keys(s.objects)) do local obj=s.objects[id]; if refs[obj].class==class then result[#result+1]=obj end end
    emit(s,"collection_read",n,nil,{members=result})
    return result
  end
  function methods:get(obj,field,args)
    local s=state(self); local r=record(self,obj); local d=definition(s,r,field)
    args=argcheck(d,args,self); local ctx=s.current; local n=node(ctx,r.id,field,args)
    dependency(s,n)
    emit(s,"value_read",n,nil,{class=r.class,revision=r.revision,value_kind=d.kind})
    if n.running then emit(s,"calculation_cycle",n,"dependency cycle") end
    need(not n.running,"dependency cycle at "..r.id.."."..field)
    local replaced,v=overlay(ctx,n.key)
    if replaced then
      disconnect(s,n); n.valid=true; n.value=copy(v); n.overridden=true
      emit(s,"override_read",n,nil,{value=v}); return copy(v)
    end
    n.overridden=false
    if d.kind=="stored" then
      n.valid=true; n.value=copy(r.values[field])
      emit(s,"stored_read",n,nil,{value=n.value,revision=r.revision}); return copy(n.value)
    end
    if n.valid and d.cache=="memo" then
      n.hits=n.hits+1
      emit(s,"cache_hit",n,nil,{value=n.value,cache_hits=n.hits}); return copy(n.value)
    end
    n.running=true; n.reads={}; n.evaluations=n.evaluations+1
    s.evaluation_seq=(s.evaluation_seq or 0)+1; n.evaluation_id=s.evaluation_seq
    local parent=s.stack[#s.stack]; n.parent_evaluation_id=parent and parent.evaluation_id or nil
    emit(s,"calculation_started",n,nil,{source=d.source,evaluations=n.evaluations})
    s.stack[#s.stack+1]=n; local previous=active_world; active_world=self; evaluation_depth=evaluation_depth+1
    local ok,result=pcall(d.run,obj,self,copy(args))
    evaluation_depth=evaluation_depth-1; active_world=previous; s.stack[#s.stack]=nil; n.running=false
    host.check_budget()
    if ok then ok,result=pcall(function() validate(d.type,result,self,r.id.."."..field); return result end) end
    disconnect(s,n)
    for child in pairs(n.reads) do
      n.deps[child]=true; child.parents[n]=true
      emit(s,"dependency_added",n,nil,{dependency=address(child)})
    end
    n.reads=nil
    if not ok then n.valid=false; n.error=tostring(result); emit(s,"calculation_failed",n,n.error); error(result,0) end
    n.value=copy(result); n.valid=true; n.error=nil; n.reason=nil; emit(s,"calculated",n,nil,{value=n.value})
    return copy(result)
  end
  function methods:set(obj,field,value)
    local s=state(self); need(evaluation_depth==0,"a value calculation cannot mutate another value")
    need(scenario_depth==0,"use override inside a scenario; base mutation is forbidden there")
    local r=record(self,obj); local d=definition(s,r,field); need(d.kind=="stored","only stored fields can be set")
    need(not s.classes[r.class].immutable,"immutable objects require a new observation identity")
    validate(d.type,value,self,r.id.."."..field)
    if canonical(r.values[field])==canonical(value) then
      emit(s,"set_unchanged",{object=r.id,field=field,args={},context=s.base},nil,{revision=r.revision}); return false
    end
    need(r.revision<9007199254740991,"object revision exhausted; create a new world version explicitly")
    local old=r.values[field]; r.values[field]=copy(value); r.revision=r.revision+1
    local n=node(s.base,r.id,field,{}); invalidate(s,n,"changed "..r.id.."."..field)
    emit(s,"set",n,nil,{previous=old,value=value,revision=r.revision}); return true
  end
  function methods:override(obj,field,value,args)
    local s=state(self); need(evaluation_depth==0,"cannot override during a calculation")
    need(s.current~=s.base,"overrides require a scenario")
    local r=record(self,obj); local d=definition(s,r,field); args=argcheck(d,args,self)
    validate(d.type,value,self,r.id.."."..field)
    local n=node(s.current,r.id,field,args); s.current.overrides[n.key]=copy(value)
    disconnect(s,n); invalidate(s,n,"override "..r.id.."."..field); emit(s,"overridden",n,nil,{value=value})
  end
  function methods:restore(obj,field,args)
    local s=state(self); need(evaluation_depth==0 and s.current~=s.base,"restore requires scenario script scope")
    local r=record(self,obj); args=argcheck(definition(s,r,field),args,self)
    local n=node(s.current,r.id,field,args); s.current.overrides[n.key]=nil
    invalidate(s,n,"restored "..r.id.."."..field); emit(s,"restored",n)
  end
  function methods:scenario(name,run)
    local s=state(self); need(evaluation_depth==0,"scenarios cannot be opened during value calculation")
    need(type(name)=="string" and type(run)=="function","scenario requires name and callback")
    local parent=s.current; local previous_scenario_world=scenario_world; s.scenario_seq=s.scenario_seq+1
    s.current={name=name.."#"..s.scenario_seq,parent=parent,nodes={},overrides={}}
    scenario_world=self; scenario_depth=scenario_depth+1
    emit(s,"scenario_enter",nil,nil,{parent_context=parent.name})
    local ok,result=pcall(run,self)
    emit(s,"scenario_exit",nil,ok and "success" or "error",{parent_context=parent.name})
    scenario_depth=scenario_depth-1; s.current=parent; scenario_world=previous_scenario_world; host.check_budget()
    if not ok then error(result,0) end; return result
  end
  function methods:why(obj,field,args)
    need(evaluation_depth==0,"inspection is available outside value calculations")
    local s=state(self); local r=record(self,obj); args=argcheck(definition(s,r,field),args,self)
    local n=s.current.nodes[nodekey(r.id,field,args)]
    if not n then return {object=r.id,field=field,state="unread",scenario=s.current.name} end
    local deps=host.array{};for child in pairs(n.deps) do deps[#deps+1]={object=child.object,field=child.field,args=copy(child.args)} end
    table.sort(deps,function(a,b)return a.object..a.field..canonical(a.args)<b.object..b.field..canonical(b.args) end)
    return {object=r.id,field=field,scenario=s.current.name,state=n.valid and "valid" or "invalid",overridden=n.overridden or false,
      dependencies=deps,evaluations=n.evaluations,cache_hits=n.hits,invalidations=n.invalidations,reason=n.reason or L.null,error=n.error or L.null}
  end
  function methods:describe(obj)
    need(evaluation_depth==0,"inspection is available outside value calculations")
    local s=state(self); local r=record(self,obj); local c=s.classes[r.class]; local fields=host.array{}
    for _,name in ipairs(keys(c.fields)) do local d=c.fields[name]; fields[#fields+1]={name=name,kind=d.kind,type=copy(d.type),params=copy(d.params or {}),cache=d.cache or "stored"} end
    return {id=r.id,class=r.class,class_version=c.version,revision=r.revision,immutable=c.immutable,fields=fields}
  end
  function methods:events(after,limit)
    need(evaluation_depth==0,"inspection is available outside value calculations")
    local s=state(self);after=after or 0;limit=limit or 100
    need(type(after)=="number" and after>=0 and after%1==0,"invalid event cursor")
    need(type(limit)=="number" and limit>=1 and limit<=100 and limit%1==0,"limit must be 1..100")
    local out=host.array{};local cursor=after
    local first=math.max(1,s.sequence-4095)
    for seq=math.max(first,after+1),math.min(s.sequence,math.max(first,after+1)+limit-1) do
      local e=s.events[(seq-1)%4096+1];out[#out+1]=copy(e);cursor=e.seq
    end
    return {events=out,next_cursor=cursor,truncated=after<first-1}
  end
  function L.call(name,args)
    call_sequence=call_sequence+1
    local s=states[active_world or scenario_world]
    local attempt={call_sequence=call_sequence,tool=type(name)=="string" and name or L.null,
      evaluation_depth=evaluation_depth,scenario_depth=scenario_depth}
    emit(s,"tool_call_attempt",nil,nil,attempt)
    if evaluation_depth~=0 or scenario_depth~=0 then
      emit(s,"tool_call_denied",nil,evaluation_depth~=0 and "calculated value" or "scenario",attempt)
    end
    need(evaluation_depth==0,"DSCO interactions cannot execute inside a calculated value")
    need(scenario_depth==0,"DSCO interactions cannot execute inside a scenario")
    if type(name)~="string" or type(args or {})~="table" then
      emit(s,"tool_call_denied",nil,"invalid call arguments",attempt)
    end
    need(type(name)=="string" and type(args or {})=="table","call requires tool name and argument record")
    local receipt=host.call(name,args or {})
    attempt.ok=receipt.ok;attempt.host_sequence=receipt.sequence
    emit(s,"tool_call_result",nil,nil,attempt)
    return receipt
  end
  function L.try(run)
    need(type(run)=="function","try requires a function")
    local ok,result=pcall(run);host.check_budget();return ok,result
  end
  L.encode=host.encode;L.decode=host.decode;L.array=host.array
  local private={L=L,host=host,states=states,refs=refs,methods=methods,need=need,copy=copy,
    canonical=canonical,validate=validate,state=state,record=record,definition=definition,
    argcheck=argcheck,keys=keys,make_ref=make_ref,emit=emit,
    base_scope=function(operation)
      need(evaluation_depth==0 and scenario_depth==0,operation.." requires base script scope")
    end}
  host.world_io(private)
  host.view(private)
  local function readonly(t)
    return setmetatable({}, {__index=t,__newindex=function() fail("API namespace is read-only") end,__metatable="Lingo API"})
  end
  L.types=readonly(L.types)
  return readonly(L)
end
