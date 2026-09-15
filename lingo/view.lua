-- Private single-view state machine. The host owns lifetime, capabilities and
-- packet storage; this module only evaluates an existing local Lingo world.
return function(core)
  local L,host=core.L,core.host
  local need,copy,canonical=core.need,core.copy,core.canonical
  local registered=false
  local function record(v,fields,label,required)
    need(type(v)=="table" and not host.is_array(v),label.." must be a record")
    for k in pairs(v) do need(type(k)=="string" and fields[k],label.." has unknown field "..tostring(k)) end
    for k in pairs(required or {}) do need(v[k]~=nil,label.." is missing "..k) end
  end
  local function text(v,label,maximum)
    need(type(v)=="string" and #v>0 and #v<=maximum and not v:find("[%z\1-\31\127]"),label.." must be bounded text without control characters")
  end
  local function array(v,label,maximum,strict_empty)
    need(type(v)=="table",label.." must be an array")
    local n=0
    for k in pairs(v) do need(type(k)=="number" and k>=1 and k%1==0,label.." must have dense array indices");n=n+1 end
    need(n==#v and n<=maximum,label.." has holes or exceeds its limit")
    need(not strict_empty or n>0 or host.is_array(v),label.." requires an explicit empty array")
    return n
  end
  -- References in values and calculation arguments use the same typed ID
  -- representation as world snapshots. They never become executable handles.
  local function encode(t,v,w)
    core.validate(t,v,w)
    if type(t)~="table" then return copy(v) end
    if t.kind=="optional" then return v==L.null and L.null or encode(t.of,v,w) end
    if t.kind=="ref" then return core.refs[v].id end
    local out=t.kind=="list" and host.array{} or {}
    if t.kind=="list" then for i,x in ipairs(v) do out[i]=encode(t.of,x,w) end
    elseif t.kind=="record" then for k,vt in pairs(t.fields) do out[k]=encode(vt,v[k],w) end
    else need(false,"unknown view value type") end
    return out
  end
  local function decode(t,v,w)
    if type(t)~="table" then core.validate(t,v,w);return copy(v) end
    if t.kind=="optional" then return v==L.null and L.null or decode(t.of,v,w) end
    if t.kind=="ref" then
      text(v,"reference object ID",512)
      local r=w:ref(v);core.validate(t,r,w);return r
    end
    local out=t.kind=="list" and host.array{} or {}
    if t.kind=="list" then
      array(v,"typed list",16384,true)
      for i,x in ipairs(v) do out[i]=decode(t.of,x,w) end
    elseif t.kind=="record" then
      record(v,t.fields,"typed record",t.fields)
      for k,vt in pairs(t.fields) do out[k]=decode(vt,v[k],w) end
    else need(false,"unknown view value type") end
    core.validate(t,out,w);return out
  end
  -- Explanation arguments may contain reference handles. All other values
  -- returned by the world inspector are ordinary JSON records and arrays.
  local function inspection_json(v)
    if core.refs[v] then return core.refs[v].id end
    if type(v)~="table" then return v end
    local out=host.is_array(v) and host.array{} or {}
    for k,x in pairs(v) do out[k]=inspection_json(x) end
    return out
  end
  local function constructor(options,...)
    need(select("#",...)==0,"view takes one option record")
    core.base_scope("view construction")
    need(not registered,"only one view may be registered per invocation")
    record(options,{title=true,world=true,target=true,controls=true,initialize=true},"view options",
      {title=true,world=true,target=true})
    text(options.title,"view title",256)
    need(options.initialize==nil or type(options.initialize)=="function","view initialize must be a function")
    local w=options.world;core.state(w)
    local title=options.title
    record(options.target,{object=true,field=true,args=true},"view target",{object=true,field=true})
    text(options.target.object,"target object",512);text(options.target.field,"target field",256)
    local declarations=options.controls
    if declarations==nil then declarations=host.array{} end
    array(declarations,"view controls",16)
    local declared=host.array{}
    for i,c in ipairs(declarations) do
      record(c,{object=true,field=true,label=true,choices=true},"view control",{object=true,field=true})
      text(c.object,"control object",512);text(c.field,"control field",256)
      local label=c.label==nil and c.field or c.label;text(label,"control label",128)
      declared[i]={object=c.object,field=c.field,label=label}
      if c.choices~=nil then
        need(array(c.choices,"control choices",32)>0,"control choices must not be empty")
        declared[i].choices=copy(c.choices)
      end
    end
    local packet=host.view_restore
    if packet~=nil then
      record(packet,{format=true,world=true,title=true,target=true,controls=true,overrides=true},"saved view",
        {format=true,world=true,title=true,target=true,controls=true,overrides=true})
      need(packet.format=="lingo.view/1","unsupported saved view format")
      need(packet.title==title,"saved view title differs from declaration")
      array(packet.controls,"saved view controls",16,true)
      need(canonical(packet.controls)==canonical(declared),"saved view controls differ from declaration")
      array(packet.overrides,"saved view overrides",16,true)
      -- load is itself atomic and insists on an empty, declared world with
      -- the exact source/runtime manifest. The initializer is never replayed.
      w:load(packet.world)
    elseif options.initialize then
      options.initialize(w)
    end
    local function field(object,name)
      local ref=w:ref(object)
      return ref,core.definition(core.state(w),core.record(w,ref),name)
    end
    local function bind(selection)
      record(selection,{object=true,field=true,args=true},"selection",{object=true,field=true})
      text(selection.object,"selected object",512);text(selection.field,"selected field",256)
      local ref,d=field(selection.object,selection.field)
      local supplied=selection.args
      if supplied==nil then supplied={} end
      record(supplied,d.params or {},"selection arguments",d.params or {})
      local args={}
      for k,t in pairs(d.params or {}) do args[k]=decode(t,supplied[k],w) end
      return w:address(ref,selection.field,args)
    end
    local target=bind(options.target)
    local controls,seen=host.array{},{}
    for i,c in ipairs(declared) do
      local ref,d=field(c.object,c.field)
      need(d.kind=="stored","view controls must name stored fields")
      need(not core.state(w).classes[core.record(w,ref).class].immutable,"view controls cannot modify immutable objects")
      local key=c.object.."\0"..c.field
      need(not seen[key],"duplicate view control");seen[key]=true
      controls[i]={spec=c,ref=ref,definition=d}
      if c.choices then
        local distinct={}
        for _,choice in ipairs(c.choices) do
          local value=decode(d.type,choice,w);local normalized=encode(d.type,value,w)
          need(canonical(choice)==canonical(normalized),"control choices must use canonical typed JSON")
          local hash=canonical(normalized);need(not distinct[hash],"duplicate control choice");distinct[hash]=true
        end
      end
    end
    local function checked_value(index,value)
      need(type(index)=="number" and index%1==0 and index>=1 and index<=#controls,"invalid view control index")
      need(value~=nil,"control value is required; use lingo.null for null")
      local c=controls[index];local typed=decode(c.definition.type,value,w)
      local encoded=encode(c.definition.type,typed,w)
      if c.spec.choices then
        local found=false
        for _,choice in ipairs(c.spec.choices) do if canonical(encoded)==canonical(choice) then found=true;break end end
        need(found,"control value is not an allowed choice")
      end
      return encoded
    end
    local overrides={}
    if packet then
      -- explain validates the complete bound address without introducing an
      -- implicit source rebind. A selected target may differ from the initial one.
      w:explain(packet.target);target=copy(packet.target)
      for _,override in ipairs(packet.overrides) do
        record(override,{index=true,value=true},"saved override",{index=true,value=true})
        local value=checked_value(override.index,override.value)
        need(overrides[override.index]==nil,"duplicate saved control override")
        overrides[override.index]=value
      end
    end
    local function within(values,run)
      if next(values)==nil then return run() end
      return w:scenario("view",function()
        for i,c in ipairs(controls) do
          if values[i]~=nil then w:override(c.ref,c.spec.field,decode(c.definition.type,values[i],w)) end
        end
        return run()
      end)
    end
    local function control_records(values)
      local base={}
      for i,c in ipairs(controls) do base[i]=encode(c.definition.type,w:get(c.ref,c.spec.field),w) end
      return within(values,function()
        local result=host.array{}
        for i,c in ipairs(controls) do
          local row={index=i,object=c.spec.object,field=c.spec.field,label=c.spec.label,
            type=copy(c.definition.type),base=base[i],current=encode(c.definition.type,w:get(c.ref,c.spec.field),w),
            overridden=values[i]~=nil}
          if c.spec.choices then row.choices=copy(c.spec.choices) end
          result[i]=row
        end
        return result
      end)
    end
    local function render(values,address)
      local ref,d=field(address.object_id,address.field)
      local result=within(values,function()
        local value=w:read(address)
        return {format="lingo.view.result/1",title=title,context=next(values)==nil and "base" or "scenario",
          target=copy(address),value=encode(d.type,value,w),explanation=inspection_json(w:explain(address)),
          selected=w:describe(ref)}
      end)
      result.controls=control_records(values)
      if next(values)~=nil then
        local ok,value=L.try(function()return encode(d.type,w:read(address),w)end)
        if ok then result.baseline=value else result.baseline_error=tostring(value):sub(1,512) end
      end
      host.encode(result) -- validate complete serialization before retained state changes
      return result
    end
    local function dispatch(request)
      core.base_scope("view operation")
      record(request,{action=true,control=true,value=true,object=true,field=true,args=true},"view request",{action=true})
      local action=request.action
      local allowed={read={action=true},why={action=true},controls={action=true},reset={action=true},snapshot={action=true},
        set={action=true,control=true,value=true},select={action=true,object=true,field=true,args=true},inspect={action=true,object=true}}
      need(type(action)=="string" and allowed[action],"unsupported view action")
      record(request,allowed[action],"view "..action)
      if action=="set" then
        local candidate=copy(overrides);candidate[request.control]=checked_value(request.control,request.value)
        local result=render(candidate,target);overrides=candidate;return result
      elseif action=="reset" then
        local result=render({},target);overrides={};return result
      elseif action=="select" then
        local address=bind{object=request.object,field=request.field,args=request.args}
        local result=render(overrides,address);target=address;return result
      elseif action=="snapshot" then
        local saved=host.array{}
        for i=1,#controls do if overrides[i]~=nil then saved[#saved+1]={index=i,value=copy(overrides[i])} end end
        local result={format="lingo.view/1",world=w:snapshot(),title=title,target=copy(target),
          controls=copy(declared),overrides=saved}
        host.encode(result);return result
      elseif action=="inspect" then
        local object=request.object==nil and target.object_id or request.object;text(object,"inspection object",512)
        return w:describe(w:ref(object))
      elseif action=="controls" then return control_records(overrides)
      elseif action=="why" then return render(overrides,target).explanation
      end
      return render(overrides,target)
    end
    local initial=dispatch{action="read"}
    need(type(host.register_view)=="function","view host is unavailable")
    host.register_view(dispatch);registered=true
    host.view_restore=nil -- release the consumed packet; retained state is private
    return initial
  end
  L.view=constructor
end
