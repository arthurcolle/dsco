-- Private world serialization and logical value addresses. No transport here.
return function(core)
  local L,host,refs,methods=core.L,core.host,core.refs,core.methods
  local need,copy,keys=core.need,core.copy,core.keys
  local function record(value,allowed,label)
    need(type(value)=="table" and not host.is_array(value),label.." must be a record")
    for k in pairs(value) do need(type(k)=="string" and allowed[k],label.." has unknown field "..tostring(k)) end
    for k in pairs(allowed) do need(value[k]~=nil,label.." is missing "..k) end
  end
  local function text(value,label,maximum)
    need(type(value)=="string" and #value>0 and #value<=maximum and not value:find("[%z\1-\31\127]"),label.." must be bounded text without control characters")
  end
  local function dense(value,label,maximum)
    need(type(value)=="table",label.." must be an array")
    local n=0
    for k in pairs(value) do need(type(k)=="number" and k>=1 and k%1==0,label.." has non-array keys");n=n+1 end
    need(n==#value and n<=maximum,label.." has holes or exceeds its limit")
    need(n>0 or host.is_array(value),label.." requires an explicit empty array")
  end
  -- Reference encoding is driven by the declared type. JSON payloads remain
  -- ordinary JSON, even if they happen to contain a key called '$ref'.
  local function encode(t,value,world,is_default)
    core.validate(t,value,world)
    if type(t)~="table" then return copy(value) end
    if t.kind=="optional" then return value==L.null and L.null or encode(t.of,value,world,is_default) end
    if t.kind=="ref" then
      need(not is_default,"portable definitions cannot contain reference defaults; supply references when creating objects")
      return refs[value].id
    end
    local out=t.kind=="list" and host.array{} or {}
    if t.kind=="list" then for i,v in ipairs(value) do out[i]=encode(t.of,v,world,is_default) end
    elseif t.kind=="record" then for k,vt in pairs(t.fields) do out[k]=encode(vt,value[k],world,is_default) end end
    return out
  end
  local function decode(t,value,world,objects)
    if type(t)~="table" then return copy(value) end
    if t.kind=="optional" then return value==L.null and L.null or decode(t.of,value,world,objects) end
    if t.kind=="ref" then
      text(value,"stored reference",512)
      local ref=objects[value]
      need(ref and refs[ref].class==t.class,"missing or wrongly typed stored reference "..value)
      return ref
    end
    need(type(value)=="table","stored composite value must be a table")
    local out=t.kind=="list" and host.array{} or {}
    if t.kind=="list" then
      dense(value,"stored list",16384)
      for i,v in ipairs(value) do out[i]=decode(t.of,v,world,objects) end
    elseif t.kind=="record" then
      record(value,t.fields,"stored record")
      for k,vt in pairs(t.fields) do out[k]=decode(vt,value[k],world,objects) end
    else need(false,"unknown stored type descriptor") end
    return out
  end
  local function class_manifest(w,c)
    local fields={}
    for name,d in pairs(c.fields) do
      local f={kind=d.kind,type=copy(d.type),params=copy(d.params or {}),cache=d.cache or "stored"}
      if d.kind=="derived" then f.source=copy(d.source)
      else
        f.has_default=d.has_default
        if d.has_default then f.default=encode(d.type,d.default,w,true) else f.default=L.null end
      end
      fields[name]=f
    end
    return {version=c.version,immutable=c.immutable,implements=copy(c.implements),source=copy(c.source),fields=fields}
  end
  local function manifest(w)
    local s=core.state(w);local classes={}
    for name,c in pairs(s.classes) do
      text(name,"portable class name",1024)
      classes[name]=class_manifest(w,c)
    end
    return {format="lingo.definitions/1",runtime=copy(host.runtime_identity),classes=classes}
  end
  function methods:manifest()
    core.base_scope("definition inspection")
    return manifest(self)
  end
  function methods:snapshot()
    core.base_scope("world snapshot")
    local s=core.state(self);local definitions=manifest(self);local objects=host.array{}
    for _,id in ipairs(keys(s.objects)) do
      text(id,"portable object ID",512)
      local r=refs[s.objects[id]];local c=s.classes[r.class];local values={}
      for name,d in pairs(c.fields) do
        if d.kind=="stored" then values[name]=encode(d.type,r.values[name],self) end
      end
      objects[#objects+1]={id=id,class=r.class,revision=r.revision,values=values}
    end
    return {format="lingo.world/1",world_id=s.id,runtime=definitions.runtime,classes=definitions.classes,objects=objects}
  end
  function methods:load(snapshot)
    core.base_scope("world load")
    local s=core.state(self)
    need(next(s.objects)==nil,"load requires an empty declared world")
    record(snapshot,{format=true,world_id=true,runtime=true,classes=true,objects=true},"snapshot")
    need(snapshot.format=="lingo.world/1","unsupported world snapshot format")
    need(snapshot.world_id==s.id,"snapshot belongs to a different world identity")
    local expected=manifest(self)
    need(core.canonical(snapshot.runtime)==core.canonical(expected.runtime),"snapshot runtime identity differs")
    need(core.canonical(snapshot.classes)==core.canonical(expected.classes),"snapshot definition or source identity differs")
    dense(snapshot.objects,"snapshot objects",4096)
    local staged,records={},{}
    for _,entry in ipairs(snapshot.objects) do
      record(entry,{id=true,class=true,revision=true,values=true},"snapshot object")
      text(entry.id,"portable object ID",512)
      need(type(entry.class)=="string" and s.classes[entry.class],"unknown snapshot class")
      need(not staged[entry.id],"duplicate snapshot object identity")
      need(type(entry.revision)=="number" and entry.revision%1==0 and entry.revision>=1 and entry.revision<=9007199254740991,"invalid object revision")
      staged[entry.id]=core.make_ref(self,entry.class,entry.id,{},entry.revision)
      records[entry.id]=entry
    end
    for id,entry in pairs(records) do
      local r=refs[staged[id]];local allowed={}
      for name,d in pairs(s.classes[entry.class].fields) do if d.kind=="stored" then allowed[name]=true end end
      record(entry.values,allowed,"stored fields")
      for name in pairs(allowed) do
        local t=s.classes[entry.class].fields[name].type
        local value=decode(t,entry.values[name],self,staged)
        core.validate(t,value,self,id.."."..name)
        r.values[name]=value
      end
    end
    -- Nothing became visible before every object, reference and value passed.
    s.objects=staged;s.base.nodes={}
    core.emit(s,"world_loaded",nil,s.id)
    return self
  end
  function methods:address(obj,field,args)
    core.base_scope("value address creation")
    local s=core.state(self);local r=core.record(self,obj);local d=core.definition(s,r,field)
    args=core.argcheck(d,args,self);local encoded={}
    for k,t in pairs(d.params or {}) do encoded[k]=encode(t,args[k],self) end
    return {format="lingo.value/1",world_id=s.id,object_id=r.id,class=r.class,field=field,args=encoded,
      definition_sha256=host.fingerprint(core.canonical(class_manifest(self,s.classes[r.class]))),
      runtime=copy(host.runtime_identity)}
  end
  local function resolve(w,address)
    local s=core.state(w)
    record(address,{format=true,world_id=true,object_id=true,class=true,field=true,args=true,definition_sha256=true,runtime=true},"value address")
    need(address.format=="lingo.value/1" and address.world_id==s.id,"value address belongs to a different world")
    need(core.canonical(address.runtime)==core.canonical(host.runtime_identity),"value address runtime identity differs")
    local obj=w:ref(address.object_id);local r=refs[obj]
    need(address.class==r.class,"value address class differs")
    need(address.definition_sha256==host.fingerprint(core.canonical(class_manifest(w,s.classes[r.class]))),"value address definition identity differs")
    local d=core.definition(s,r,address.field);local args={}
    record(address.args,d.params or {},"value arguments")
    for k,t in pairs(d.params or {}) do args[k]=decode(t,address.args[k],w,s.objects) end
    return obj,address.field,args
  end
  function methods:read(address)
    local obj,field,args=resolve(self,address)
    return self:get(obj,field,args)
  end
  function methods:explain(address)
    local obj,field,args=resolve(self,address)
    return self:why(obj,field,args)
  end
end
