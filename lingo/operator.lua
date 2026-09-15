-- Live GraphSub browsing through DSCO's existing governed Lingo bridge.
-- Loaded privately by the host; this is not a snapshot or calculation context.
return function(lingo)
  local call, decode, try = lingo.call, lingo.decode, lingo.try
  local scopes = setmetatable({}, {__mode = "k"})
  local methods = {}

  local function fail(message)
    error("lingo.operator: " .. message, 0)
  end
  local function need(condition, message)
    if not condition then fail(message) end
  end
  local function record(value, allowed, label)
    need(type(value) == "table", label .. " must be an argument record")
    for key in pairs(value) do
      need(type(key) == "string" and allowed[key],
        label .. " has unsupported argument " .. tostring(key))
    end
  end
  local function integer(value, minimum, maximum, label)
    need(type(value) == "number" and value == value and value % 1 == 0
      and value >= minimum and value <= maximum,
      label .. " must be an integer in " .. minimum .. ".." .. maximum)
    return value
  end
  local function no_arguments(count, name)
    need(count == 0, name .. " takes no arguments")
  end
  local function remote_error(response)
    local detail = response.error
    if type(detail) == "table" then
      local code = type(detail.code) == "string" and detail.code or "REMOTE_ERROR"
      local message = type(detail.message) == "string" and detail.message
        or "GraphSub operation failed"
      return code .. ": " .. message
    end
    return "REMOTE_ERROR: GraphSub operation failed"
  end
  local function request(scope, action, parameters)
    local state = scopes[scope]
    need(state ~= nil, "expected a BrowseScope")
    local input = {
      action = action,
      connection = state.connection,
      shard_id = state.shard_id,
    }
    for key, value in pairs(parameters or {}) do input[key] = value end

    -- lingo.call enforces calculation/scenario exclusion, then invokes the
    -- native capability gate with the enclosing tier and same-session taint.
    local receipt = call("graphsub_operator", input)
    need(type(receipt) == "table" and type(receipt.ok) == "boolean"
      and type(receipt.result) == "string", "invalid DSCO tool receipt")
    if not receipt.ok then
      -- Capability denials may be plain text; do not require JSON to diagnose.
      fail("graphsub_operator failed: " .. receipt.result)
    end
    local parsed, response = try(function() return decode(receipt.result) end)
    need(parsed and type(response) == "table", "invalid GraphSub JSON response")
    need(type(response.ok) == "boolean", "GraphSub response has no outcome")
    if not response.ok then fail(remote_error(response)) end
    need(response.profile == "browse" and response.consistency == "live"
      and response.snapshot == false, "GraphSub response is not live browse provenance")
    need(response.connection == state.connection and response.shard_id == state.shard_id
      and response.action == action, "GraphSub response scope/action mismatch")
    need(type(response.observed_at) == "string" and #response.observed_at > 0,
      "GraphSub response has no observation time")
    need(response.data ~= nil, "GraphSub response has no data payload")
    -- Each call decodes new data. Mutating the returned record cannot alter
    -- this scope's private connection/shard or a later response.
    return response
  end

  function methods:status(...)
    no_arguments(select("#", ...), "status")
    return request(self, "status")
  end
  function methods:schema(...)
    no_arguments(select("#", ...), "schema")
    return request(self, "schema")
  end
  function methods:list(options, ...)
    no_arguments(select("#", ...), "list extra arguments")
    if options == nil then options = {} end
    record(options, {limit=true, offset=true}, "list")
    local limit = options.limit == nil and 25 or options.limit
    local offset = options.offset == nil and 0 or options.offset
    return request(self, "list", {
      limit=integer(limit, 1, 100, "limit"),
      offset=integer(offset, 0, 10000000, "offset"),
    })
  end
  function methods:read(id, ...)
    no_arguments(select("#", ...), "read extra arguments")
    need(type(id) == "string" and #id > 0 and #id <= 256
      and not id:find("%z"), "read requires an opaque node ID string of 1..256 bytes")
    -- Preserve the backend's opaque ID verbatim. The native adapter encodes
    -- the path; the backend validates identity/shard. Never coerce to a number.
    return request(self, "read", {node_id=id})
  end

  local function attach(options, ...)
    no_arguments(select("#", ...), "attach extra arguments")
    if options == nil then options = {} end
    record(options, {connection=true, shard_id=true}, "attach")
    local connection = options.connection == nil and "default" or options.connection
    need(connection == "default", "only the host-configured default connection is available")
    local shard = options.shard_id == nil and 0 or options.shard_id
    integer(shard, 0, 2147483647, "shard_id")

    local scope = newproxy(true)
    local mt = getmetatable(scope)
    mt.__index = function(_, key)
      if methods[key] then return methods[key] end
      fail("BrowseScope does not implement " .. tostring(key)
        .. "; available methods: status, schema, list, read; no pinned contexts")
    end
    mt.__newindex = function() fail("BrowseScope is immutable") end
    mt.__metatable = "Lingo live BrowseScope"
    mt.__tostring = function() return "BrowseScope(default, shard=" .. shard .. ", live)" end
    scopes[scope] = {connection=connection, shard_id=shard}
    request(scope, "status") -- Attach proves access through the same gate.
    return scope
  end

  local api = {version="0.1-browse", profile="browse", attach=attach}
  return setmetatable({}, {
    __index=function(_, key)
      if api[key] ~= nil then return api[key] end
      fail("operator service " .. tostring(key) .. " is unavailable in the browse profile")
    end,
    __newindex=function() fail("operator API namespace is read-only") end,
    __metatable="Lingo operator API",
  })
end
