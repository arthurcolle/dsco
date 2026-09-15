-- Source-bound stored-world images in the existing GraphSub persistence path.
-- Publication creates a new node. Addresses verify bytes; they grant no write
-- authority and promise neither server immutability nor retention.
return function(core)
  local addresses=setmetatable({}, {__mode="k"})
  local fields={connection=true,shard_id=true,node_id=true,sha256=true,world_id=true}
  local function need(ok,message) if not ok then error("lingo.workspace: "..message,0) end end
  local function copy(v)
    if type(v)~="table" then return v end
    local out={};for k,x in pairs(v) do out[k]=copy(x) end;return out
  end
  local function record(v,allowed,label,required)
    need(type(v)=="table",label.." must be a record")
    for k in pairs(v) do need(type(k)=="string" and allowed[k],label.." has unsupported field "..tostring(k)) end
    if required then for k in pairs(allowed) do need(v[k]~=nil,label.." is missing "..k) end end
  end
  local function text(v,n,label)
    need(type(v)=="string" and #v>0 and #v<=n and not v:find("[%z\1-\31\127]"),label.." must be bounded text")
  end
  local function scope(v)
    need(v.connection=="default","only the host-configured default connection is available")
    need(type(v.shard_id)=="number" and v.shard_id%1==0 and v.shard_id>=0 and v.shard_id<=2147483647,"invalid shard_id")
  end
  local function validate(v)
    record(v,fields,"artifact address",true);scope(v)
    text(v.world_id,128,"world_id");text(v.node_id,256,"node_id")
    need(not v.node_id:find("[^A-Za-z0-9_-]"),"invalid opaque node_id")
    need(type(v.sha256)=="string" and #v.sha256==64 and not v.sha256:find("[^a-f0-9]"),"invalid artifact SHA256")
    return copy(v)
  end
  local function immutable(v)
    local p=newproxy(true);addresses[p]=validate(v)
    local mt=getmetatable(p)
    mt.__index=function(self,key)
      if key=="inspect" then return function(a,...) need(select("#",...)==0,"inspect takes no arguments");need(addresses[a],"expected artifact address");return copy(addresses[a]) end end
      if fields[key] then return addresses[self][key] end
      error("lingo.workspace: unsupported artifact address field "..tostring(key),0)
    end
    mt.__newindex=function() error("lingo.workspace: artifact address is immutable",0) end
    mt.__metatable="Lingo world artifact address"
    mt.__tostring=function() return "WorldArtifact("..v.world_id..", "..v.sha256..")" end
    return p
  end
  local function request(input)
    local receipt=core.call("graphsub_world",input)
    need(type(receipt)=="table" and type(receipt.ok)=="boolean" and type(receipt.result)=="string","invalid DSCO tool receipt")
    need(receipt.ok,"graphsub_world failed: "..receipt.result)
    local ok,response=core.try(function() return core.decode(receipt.result) end)
    need(ok and type(response)=="table" and response.ok==true,"invalid GraphSub artifact outcome")
    need(response.profile=="world_artifact" and response.action==input.action and response.consistency=="content_verified"
      and response.persistence=="server_reported","invalid GraphSub artifact provenance")
    validate(response.artifact)
    need(response.artifact.connection==input.connection and response.artifact.shard_id==input.shard_id,"artifact scope differs")
    need(type(response.snapshot)=="string" and #response.snapshot>0 and #response.snapshot<=49152,"invalid snapshot text")
    return response
  end
  local function publish(world,options,...)
    need(select("#",...)==0,"publish takes world and optional connection options")
    if options==nil then options={} end
    record(options,{connection=true,shard_id=true},"publish options")
    local s={connection=options.connection==nil and "default" or options.connection,
      shard_id=options.shard_id==nil and 0 or options.shard_id};scope(s)
    -- snapshot() enforces base scope and exports stored values only. The
    -- native layer independently checks shape/limit before any network call.
    local snapshot=world:snapshot();local encoded=core.encode(snapshot)
    need(#encoded<=49152,"world snapshot exceeds 49152 UTF-8 bytes")
    local response=request{action="publish",connection=s.connection,shard_id=s.shard_id,snapshot=encoded}
    need(response.artifact.world_id==snapshot.world_id and response.snapshot==encoded,"publication readback differs")
    return immutable(response.artifact),response
  end
  local function open(address,world,...)
    need(select("#",...)==0,"open takes artifact address and an empty declared world")
    local a=validate(addresses[address] or address)
    local current=world:snapshot()
    need(current.world_id==a.world_id,"artifact belongs to a different world identity")
    need(#current.objects==0,"open requires an empty declared world")
    local input=copy(a);input.action="read"
    local response=request(input)
    for k in pairs(fields) do need(response.artifact[k]==a[k],"returned artifact address differs") end
    local ok,snapshot=core.try(function() return core.decode(response.snapshot) end)
    need(ok and type(snapshot)=="table" and snapshot.world_id==a.world_id,"invalid stored-world identity")
    world:load(snapshot) -- atomic admission validates exact source/runtime and references
    return world,response
  end
  local api={version="0.1",publish=publish,open=open,address=function(value,...) need(select("#",...)==0,"address takes one record");return immutable(value) end}
  return setmetatable({}, {__index=api,__newindex=function() error("lingo.workspace: API is immutable",0) end,__metatable="Lingo workspace API"})
end
