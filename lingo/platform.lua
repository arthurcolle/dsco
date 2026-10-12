-- Shared platform objects. This module performs no I/O and owns no scheduler.
-- Service readiness and Task eligibility are local, advisory calculations.
return function(core)
  local T, stored, derived = core.types, core.stored, core.derived
  local worlds = setmetatable({}, {__mode="k"})
  local version = "0.1"
  local function need(ok, message)
    if not ok then error("lingo.platform: " .. message, 0) end
  end
  local function record(value, allowed, label)
    need(type(value)=="table", label .. " must be a record")
    for key in pairs(value) do
      need(type(key)=="string" and allowed[key], label .. " has unknown field " .. tostring(key))
    end
  end
  local function text(value, label, maximum)
    need(type(value)=="string" and #value>0 and #value<=maximum
      and not value:find("[%z\1-\31\127]"), label .. " must be nonempty text without ASCII controls")
    return value
  end
  local function no_extra(...)
    need(select("#", ...)==0, "unexpected arguments")
  end
  local function declarations(w)
    local origin = T.record{system="string", operation="string", source_id="string"}
    w:define("Observation", {
      origin=stored(origin), observed_at=stored("string"),
      consistency=stored("string"), payload=stored("json"),
      summary=derived("json", function(self)
        return {origin=self.origin, observed_at=self.observed_at,
          consistency=self.consistency, authority="reported_evidence"}
      end),
    }, {version=version, immutable=true})

    w:define("Service", {
      observation=stored(T.ref("Observation")), enabled=stored("boolean", true),
      availability=stored("string", "unknown"),
      ready=derived("boolean", function(self)
        return self.enabled and self.availability=="ready"
      end),
      summary=derived("json", function(self)
        return {origin=self.observation.origin, observed_at=self.observation.observed_at,
          consistency=self.observation.consistency, availability=self.availability,
          ready=self.ready, advisory=true}
      end),
    }, {version=version})

    w:define("Object", {
      service=stored(T.ref("Service")), remote_id=stored("string"),
      observation=stored(T.ref("Observation")),
      payload=derived("json", function(self) return self.observation.payload end),
    }, {version=version})

    w:define("Artifact", {
      observation=stored(T.ref("Observation")), locator=stored("string"),
      sha256=stored("string", ""),
      summary=derived("json", function(self)
        return {locator=self.locator, sha256=self.sha256,
          origin=self.observation.origin, verification="not_performed_by_language"}
      end),
    }, {version=version})

    w:define("Policy", {
      allow_effects=stored("boolean", false), max_cost_usd=stored("number"),
      max_calls=stored("integer"),
      valid=derived("boolean", function(self)
        return self.max_cost_usd>=0 and self.max_calls>=1
      end),
    }, {version=version})

    w:define("Task", {
      service=stored(T.ref("Service")), policy=stored(T.ref("Policy")),
      operation=stored("string"), inputs=stored("json", {}),
      estimated_cost_usd=stored("number"), expected_calls=stored("integer", 1),
      operation_valid=derived("boolean", function(self)
        local name=self.operation
        return #name>0 and #name<=256 and not name:find("[%z\1-\31\127]")
      end),
      estimates_valid=derived("boolean", function(self)
        return self.estimated_cost_usd>=0 and self.expected_calls>=1
      end),
      within_budget=derived("boolean", function(self)
        return self.policy.valid and self.estimates_valid
          and self.estimated_cost_usd<=self.policy.max_cost_usd
          and self.expected_calls<=self.policy.max_calls
      end),
      readiness=derived("string", function(self)
        if not self.policy.valid then return "invalid_policy" end
        if not self.estimates_valid then return "invalid_estimate" end
        if not self.operation_valid then return "invalid_operation" end
        if not self.service.ready then return "service_unavailable" end
        if not self.policy.allow_effects then return "effects_disabled" end
        if not self.within_budget then return "over_budget" end
        return "eligible"
      end),
      eligible=derived("boolean", function(self) return self.readiness=="eligible" end),
      summary=derived("json", function(self)
        return {operation=self.operation, eligible=self.eligible, readiness=self.readiness,
          advisory=true, grants_authority=false, submits_effects=false,
          estimated_cost_usd=self.estimated_cost_usd, expected_calls=self.expected_calls,
          source=self.service.observation.summary}
      end),
    }, {version=version})

    w:define("Run", {
      task=stored(T.ref("Task")), observation=stored(T.ref("Observation")),
      remote_id=stored("string"), state=stored("string", "unknown"),
      terminal=derived("boolean", function(self)
        return self.state=="succeeded" or self.state=="failed" or self.state=="cancelled"
      end),
      summary=derived("json", function(self)
        return {remote_id=self.remote_id, state=self.state, terminal=self.terminal,
          operation=self.task.operation, source=self.observation.summary}
      end),
    }, {version=version})

    w:define("Model", {
      service=stored(T.ref("Service")), model_id=stored("string"),
      observation=stored(T.ref("Observation")),
      service_ready=derived("boolean", function(self) return self.service.ready end),
    }, {version=version})
    w:define("Agent", {
      service=stored(T.ref("Service")), agent_id=stored("string"),
      observation=stored(T.ref("Observation")),
      service_ready=derived("boolean", function(self) return self.service.ready end),
    }, {version=version})
  end

  local function world(options, ...)
    no_extra(...)
    record(options, {id=true}, "world options")
    text(options.id, "world id", 128)
    local w=core.world{id=options.id}
    declarations(w)
    worlds[w]=true
    return w
  end
  local function observe(w, id, value, ...)
    no_extra(...)
    need(worlds[w], "observation requires a platform world")
    text(id, "observation id", 512)
    record(value, {origin=true, observed_at=true, consistency=true, payload=true}, "observation")
    record(value.origin, {system=true, operation=true, source_id=true}, "observation origin")
    text(value.origin.system, "origin system", 128)
    text(value.origin.operation, "origin operation", 256)
    text(value.origin.source_id, "origin source_id", 512)
    text(value.observed_at, "observed_at", 128)
    need(value.consistency=="live" or value.consistency=="snapshot"
      or value.consistency=="recorded" or value.consistency=="local",
      "consistency must be live, snapshot, recorded or local")
    need(value.payload~=nil, "payload is required; use lingo.null for null")
    return w:new("Observation", id, value)
  end
  local api={version=version, world=world, observe=observe}
  return setmetatable({}, {
    __index=api,
    __newindex=function() need(false, "API namespace is read-only") end,
    __metatable="Lingo platform API",
  })
end
