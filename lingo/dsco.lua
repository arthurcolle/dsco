-- Scriptable handles for the host's existing governed tool registry.
return function(lingo)
  local call, decode = lingo.call, lingo.decode
  local handles = setmetatable({}, {__mode="k"})
  local methods = {}
  local function need(ok, message)
    if not ok then error("lingo.dsco: " .. message, 0) end
  end
  local function name(value)
    need(type(value)=="string" and #value>0 and #value<=128
      and not value:find("%z"), "tool name must be 1..128 bytes without NUL")
    return value
  end
  local function record(value, allowed, label)
    need(type(value)=="table", label .. " must be a record")
    for key in pairs(value) do
      need(type(key)=="string" and allowed[key], label .. " has unknown field " .. tostring(key))
    end
  end
  local function invoke(tool, inputs, ...)
    need(select("#", ...)==0, "unexpected call arguments")
    name(tool)
    if inputs==nil then inputs={} end
    need(type(inputs)=="table", "tool inputs must be a record")
    return call(tool, inputs)
  end
  local function run(tool, inputs, ...)
    local receipt=invoke(tool, inputs, ...)
    need(receipt.ok, tool .. " failed: " .. receipt.result)
    return receipt.result, receipt
  end
  local function json(tool, inputs, ...)
    local text, receipt=run(tool, inputs, ...)
    -- JSON decoding is explicit. The target tool still owns the meaning of
    -- its payload; an application-level status is never silently rewritten.
    return decode(text), receipt
  end
  local function handle_name(self)
    local tool=handles[self]
    need(tool~=nil, "expected a tool handle")
    return tool
  end
  function methods:call(inputs, ...) return invoke(handle_name(self), inputs, ...) end
  function methods:run(inputs, ...) return run(handle_name(self), inputs, ...) end
  function methods:json(inputs, ...) return json(handle_name(self), inputs, ...) end
  local function tool(tool_name, ...)
    need(select("#", ...)==0, "tool takes one name")
    name(tool_name)
    local handle=newproxy(true)
    local mt=getmetatable(handle)
    mt.__index=function(_, key)
      if key=="name" then return tool_name end
      need(methods[key]~=nil, "unknown tool handle member " .. tostring(key))
      return methods[key]
    end
    mt.__newindex=function() error("lingo.dsco: tool handle is immutable",0) end
    mt.__metatable="Lingo DSCO tool"
    mt.__tostring=function() return "Tool(" .. tool_name .. ")" end
    handles[handle]=tool_name
    return handle
  end
  local function discover(options, ...)
    need(select("#", ...)==0, "discover takes one record")
    if options==nil then options={} end
    record(options,{query=true,limit=true,offset=true},"discover")
    local query=options.query
    local limit=options.limit==nil and 8 or options.limit
    local offset=options.offset==nil and 0 or options.offset
    need(query==nil or (type(query)=="string" and #query>0 and #query<=512
      and not query:find("%z")),"query must be 1..512 bytes without NUL")
    need(type(limit)=="number" and limit%1==0 and limit>=1 and limit<=16,
      "limit must be an integer in 1..16")
    need(type(offset)=="number" and offset%1==0 and offset>=0 and offset<=1000000,
      "offset must be an integer in 0..1000000")
    return json("discover_tools",{query=query,limit=limit,offset=offset})
  end
  local api={version="0.1",tool=tool,call=invoke,run=run,json=json,discover=discover}
  return setmetatable({}, {
    __index=function(_,key)
      need(api[key]~=nil,"unknown service " .. tostring(key))
      return api[key]
    end,
    __newindex=function() error("lingo.dsco: API is read-only",0) end,
    __metatable="Lingo DSCO API",
  })
end
