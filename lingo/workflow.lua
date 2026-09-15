-- A serializable work definition executed by Autobot's existing durable owner.
-- Construction/inspection are pure; execute/read use the native capability gate.
return function(l)
  local states=setmetatable({}, {__mode="k"})
  local methods={}
  local function need(ok,message)
    if not ok then error("lingo.workflow: "..message,0) end
  end
  local function copy(v) return l.decode(l.encode(v)) end
  local function default(v,fallback) if v==nil then return fallback end;return v end
  local function record(v,allowed,label)
    need(type(v)=="table",label.." must be a record")
    for k in pairs(v) do need(type(k)=="string" and allowed[k],label.." has unsupported field "..tostring(k)) end
  end
  local function text(v,maximum,label,identifier)
    need(type(v)=="string" and #v>0 and #v<=maximum and not v:find("[%z\1-\31\127]"),label.." must be bounded text")
    if identifier then need(not v:find("[^A-Za-z0-9_.:%-]") and v~="." and v~="..",label.." must be an identifier") end
    return v
  end
  local function array(v,maximum,label)
    need(type(v)=="table" and #v>=1 and #v<=maximum,label.." must be a nonempty bounded array")
    local count=0
    for k in pairs(v) do count=count+1;need(type(k)=="number" and k%1==0 and k>=1 and k<=#v,label.." must be dense") end
    need(count==#v,label.." must be dense")
  end
  local function call(request)
    local receipt=l.call("autobot_workflow",request)
    need(type(receipt)=="table" and type(receipt.ok)=="boolean" and type(receipt.result)=="string","invalid DSCO receipt")
    need(receipt.ok,"Autobot interaction failed: "..receipt.result)
    local value=l.decode(receipt.result)
    need(type(value)=="table" and value.ok==true and value.profile=="lingo.workflow/1" and value.owner=="autobot"
      and value.action==request.action and type(value.receipt)=="table","invalid workflow owner receipt")
    return value.receipt,receipt
  end
  function methods:inspect(...)
    need(select("#",...)==0,"inspect takes no arguments")
    need(states[self]~=nil,"expected a workflow")
    return copy(states[self])
  end
  function methods:execute(options,...)
    need(select("#",...)==0,"execute takes one argument record")
    need(states[self]~=nil,"expected a workflow")
    record(options,{inputs=true,idempotency_key=true,trace_id=true},"execute")
    need(type(options.inputs)=="table" and l.encode(options.inputs):sub(1,1)=="{","inputs must be a JSON object")
    text(options.idempotency_key,128,"idempotency_key",true)
    if options.trace_id~=nil then text(options.trace_id,128,"trace_id",true) end
    return call{action="execute",workflow=copy(states[self]),inputs=copy(options.inputs),
      idempotency_key=options.idempotency_key,trace_id=options.trace_id}
  end
  local function create(options,...)
    need(select("#",...)==0,"create takes one argument record")
    record(options,{id=true,name=true,version=true,steps=true,timeout_seconds=true},"workflow")
    local spec={id=text(options.id,128,"id",true),name=text(options.name,128,"name"),
      version=text(default(options.version,"1"),64,"version",true),timeout_seconds=default(options.timeout_seconds,60),steps=l.array{}}
    need(type(spec.timeout_seconds)=="number" and spec.timeout_seconds%1==0 and spec.timeout_seconds>=1
      and spec.timeout_seconds<=120,"timeout_seconds must be an integer in 1..120")
    array(options.steps,16,"steps")
    local seen={}
    for _,step in ipairs(options.steps) do
      record(step,{id=true,tool=true,mode=true,input_from=true,input_mapping=true},"step")
      local id=text(step.id,64,"step.id",true)
      need(not seen[id],"duplicate step "..id)
      local mode=default(step.mode,"passthrough")
      need(mode=="passthrough" or mode=="map","step mode must be passthrough or map")
      if step.input_from~=nil then
        text(step.input_from,64,"input_from",true)
        need(seen[step.input_from],"input_from must name an earlier step")
      end
      local mapping={}
      if step.input_mapping~=nil then
        need(type(step.input_mapping)=="table" and l.encode(step.input_mapping):sub(1,1)=="{","input_mapping must be a record")
        local count=0
        for target,source in pairs(step.input_mapping) do
          count=count+1;text(target,128,"mapping target");text(source,128,"mapping source")
          need(count<=32,"input_mapping exceeds 32 entries");mapping[target]=source
        end
      end
      spec.steps[#spec.steps+1]={id=id,tool=text(step.tool,200,"tool",true),mode=mode,
        input_from=step.input_from,input_mapping=mapping}
      seen[id]=true
    end
    need(#l.encode(spec)<=32768,"workflow definition exceeds 32 KiB")
    local ref=newproxy(true);local mt=getmetatable(ref)
    states[ref]=copy(spec)
    mt.__index=function(_,k) need(methods[k]~=nil,"unknown workflow method "..tostring(k));return methods[k] end
    mt.__newindex=function() error("lingo.workflow: workflows are immutable",0) end
    mt.__metatable="Lingo workflow";mt.__tostring=function() return "Workflow("..spec.id..")" end
    return ref
  end
  local function read(execution_id,...)
    need(select("#",...)==0,"read takes one execution ID")
    return call{action="read",execution_id=text(execution_id,128,"execution_id",true)}
  end
  local function reconcile(idempotency_key,...)
    need(select("#",...)==0,"reconcile takes one idempotency key")
    return call{action="reconcile",idempotency_key=text(idempotency_key,128,"idempotency_key",true)}
  end
  local api={version="0.1",create=create,read=read,reconcile=reconcile}
  return setmetatable({}, {__index=function(_,k)need(api[k]~=nil,"unknown API "..tostring(k));return api[k] end,
    __newindex=function()error("lingo.workflow: API is read-only",0) end,__metatable="Lingo workflow API"})
end
