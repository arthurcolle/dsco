-- Typed Autobot access over the existing Tool Management client and DSCO gate.
return function(lingo)
  local call, encode, decode, try = lingo.call, lingo.encode, lingo.decode, lingo.try
  local handles = setmetatable({}, {__mode="k"})
  local methods = {}
  local function fail(message) error("lingo.autobot: " .. message, 0) end
  local function need(ok, message) if not ok then fail(message) end end
  local function copy(value) return decode(encode(value)) end
  local function no_extra(count) need(count == 0, "unexpected extra arguments") end
  local function record(value, allowed, label)
    need(type(value) == "table", label .. " must be an argument record")
    for key in pairs(value) do
      need(type(key) == "string" and (not allowed or allowed[key]),
        label .. " has unsupported argument " .. tostring(key))
    end
  end
  local function receipt(value)
    need(type(value) == "table" and type(value.ok) == "boolean"
      and type(value.result) == "string", "invalid DSCO tool receipt")
    return value
  end
  function methods:call(inputs, ...)
    no_extra(select("#", ...))
    local metadata = handles[self]
    need(metadata ~= nil, "expected a discovered Autobot tool")
    if inputs == nil then inputs = {} end
    record(inputs, nil, "tool inputs")
    -- No private transport or execution shortcut: the exact registered leaf
    -- gets the enclosing tier, capabilities and accumulated session taint.
    return receipt(call(metadata.name, inputs))
  end
  local function successful_payload(result)
    if not result.ok then fail(result.result) end
    local valid, data = try(function() return decode(result.result) end)
    need(valid and type(data) == "table", "Autobot execution returned invalid JSON; use :call for its receipt")
    if data.status == "error" or data.success == false
      or (data.error ~= nil and data.error ~= lingo.null) then
      local message = type(data.error) == "string" and data.error or "remote execution failed"
      fail(message)
    end
    -- The existing callback can carry HTTP errors as a textual receipt with
    -- ok=true. Only the service's execution envelope proves application success.
    need(data.status == "success" and type(data.tool_id) == "string" and #data.tool_id > 0
      and type(data.tool_name) == "string" and #data.tool_name > 0 and data.output ~= nil,
      "Autobot did not return a successful execution envelope; use :call for its receipt")
    return data
  end
  function methods:run(inputs, ...)
    no_extra(select("#", ...))
    local result = methods.call(self, inputs)
    successful_payload(result)
    return result.result, result
  end
  function methods:json(inputs, ...)
    no_extra(select("#", ...))
    local result = methods.call(self, inputs)
    return successful_payload(result), result
  end
  function methods:describe(...)
    no_extra(select("#", ...))
    need(handles[self] ~= nil, "expected a discovered Autobot tool")
    return copy(handles[self])
  end
  local function handle(metadata)
    local h = newproxy(true)
    handles[h] = copy(metadata)
    local mt = getmetatable(h)
    mt.__index = function(_, key)
      if methods[key] then return methods[key] end
      local value = handles[h][key]
      if type(value) == "table" then return copy(value) end
      return value
    end
    mt.__newindex = function() fail("discovered tool handles are immutable") end
    mt.__metatable = "Lingo Autobot tool"
    mt.__tostring = function() return "AutobotTool(" .. handles[h].name .. ")" end
    return h
  end
  local function discover(options, ...)
    no_extra(select("#", ...))
    record(options, {source=true, query=true, limit=true}, "discover")
    local source = options.source == nil and "tool_management" or options.source
    local query = options.query
    local limit = options.limit == nil and 8 or options.limit
    need(source == "tool_management", "source must be tool_management")
    need(type(query) == "string" and #query >= 1 and #query <= 512
      and query:find("%S") and not query:find("[%z\1-\31\127]"),
      "query must contain visible text in 1..512 bytes without control characters")
    need(type(limit) == "number" and limit % 1 == 0 and limit >= 1 and limit <= 16,
      "limit must be an integer in 1..16")
    local result = receipt(call("autobot_discover", {source=source, query=query, limit=limit}))
    if not result.ok then fail("discovery failed: " .. result.result) end
    local valid, data = try(function() return decode(result.result) end)
    need(valid and type(data) == "table" and data.source == "tool_management_api"
      and data.query == query and type(data.matches) == "table",
      "invalid Tool Management discovery response")
    local tools = {}
    for _, metadata in ipairs(data.matches) do
      need(type(metadata) == "table" and type(metadata.name) == "string"
        and metadata.name:match("^tm__[A-Za-z0-9_]+$")
        and metadata.source == "tool_management_api"
        and type(metadata.remote_tool_id) == "string" and #metadata.remote_tool_id > 0,
        "invalid discovered tool metadata")
      need(tools[metadata.name] == nil, "ambiguous discovered tool name")
      tools[metadata.name] = handle(metadata)
    end
    return {matches=data.matches, tools=tools, receipt=result}
  end
  local api = {version="0.1", source="tool_management", discover=discover}
  return setmetatable({}, {
    __index=api,
    __newindex=function() fail("Autobot API namespace is read-only") end,
    __metatable="Lingo Autobot API",
  })
end
