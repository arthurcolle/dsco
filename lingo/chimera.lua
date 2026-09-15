-- Per-script preferences, plan observations and explicit guarded execution.
return function(l)
  local states=setmetatable({}, {__mode="k"})
  local plans=setmetatable({}, {__mode="k"})
  local methods={}
  local policy_schema=l.decode([[{"type":"object","properties":{"success_weight":{"type":"number","minimum":0,"maximum":1000000},"quality_weight":{"type":"number","minimum":0,"maximum":1000000},"benchmark_weight":{"type":"number","minimum":0,"maximum":1000000},"preference_weight":{"type":"number","minimum":0,"maximum":1000000},"cost_weight":{"type":"number","minimum":0,"maximum":1000000},"latency_weight":{"type":"number","minimum":0,"maximum":1000000},"failure_weight":{"type":"number","minimum":0,"maximum":1000000},"uncertainty_weight":{"type":"number","minimum":0,"maximum":1000000},"quality_lcb_z":{"type":"number","minimum":0,"maximum":1000000},"missing_metadata_penalty":{"type":"number","minimum":0,"maximum":1000000},"max_cost_usd":{"type":"number","minimum":0,"maximum":1000000},"max_latency_s":{"type":"number","minimum":0,"maximum":1000000},"max_failure_probability":{"type":"number","minimum":0,"maximum":1},"quality_floor":{"type":"number","minimum":0,"maximum":1},"min_context":{"type":"integer","minimum":0,"maximum":10000000},"required_modalities":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"required_output_modalities":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"required_parameters":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"allowed_models":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"denied_models":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"allowed_providers":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"denied_providers":{"type":"array","maxItems":25,"items":{"type":"string","minLength":1,"maxLength":200}},"allow_additional_output_modalities":{"type":"boolean"},"zdr_required":{"type":"boolean"},"require_known_price":{"type":"boolean"},"include_expired":{"type":"boolean"},"allow_batch":{"type":"boolean"},"allow_router_models":{"type":"boolean"},"required_region":{"type":"string","minLength":1,"maxLength":64}},"additionalProperties":false}]])
  local request_schema=l.decode([[{"type":"object","properties":{"task":{"type":"string","minLength":1,"maxLength":16384},"max_output_tokens":{"type":"integer","minimum":1,"maximum":4096},"strategy":{"type":"string","enum":["auto","direct","cascade","parallel"]},"max_calls":{"type":"integer","minimum":1,"maximum":3},"max_expected_cost_usd":{"type":"number","minimum":0,"maximum":0.2},"max_worst_case_cost_usd":{"type":"number","minimum":0,"maximum":0.25},"latency_preference":{"type":"string","enum":["low","balanced","quality"]},"max_expected_latency_s":{"type":"number","minimum":0,"maximum":1000000},"max_worst_case_latency_s":{"type":"number","minimum":0,"maximum":1000000},"allow_cascade":{"type":"boolean"},"allow_parallel":{"type":"boolean"}},"required":["task"],"additionalProperties":false}]])
  local defaults=l.decode([[{"success_weight":0.05,"quality_weight":0.0,"benchmark_weight":0.75,"preference_weight":0.01,"cost_weight":25.0,"latency_weight":0.0005,"failure_weight":0.05,"uncertainty_weight":0.05,"quality_lcb_z":1.0,"missing_metadata_penalty":0.05,"min_context":0,"required_modalities":["text"],"required_output_modalities":["text"],"allow_additional_output_modalities":false,"required_parameters":[],"allowed_models":[],"denied_models":[],"allowed_providers":[],"denied_providers":[],"zdr_required":false,"require_known_price":true,"include_expired":false,"allow_batch":false,"allow_router_models":false}]])
  local request_defaults=l.decode([[{"max_output_tokens":256,"strategy":"auto","max_calls":3,"max_expected_cost_usd":0.2,"max_worst_case_cost_usd":0.25,"latency_preference":"quality","allow_cascade":true,"allow_parallel":true}]])

  local function fail(message) error("lingo.chimera: "..message,0) end
  local function need(condition,message) if not condition then fail(message) end end
  local function copy(value) return l.decode(l.encode(value)) end
  local function validate(value,schema,label)
    need(type(value)=="table",label.." must be an argument record")
    for key,v in pairs(value) do
      local spec=type(key)=="string" and schema.properties[key]
      need(spec~=nil,label.." has unsupported field "..tostring(key))
      if spec.type=="number" or spec.type=="integer" then
        need(type(v)=="number" and v==v and v>=spec.minimum and v<=spec.maximum
          and (spec.type~="integer" or v%1==0),label.."."..key.." is outside its numeric bounds")
      elseif spec.type=="string" then
        need(type(v)=="string" and not v:find("%z"),label.."."..key.." must be a string")
        if spec.enum then
          local found=false;for _,item in ipairs(spec.enum) do if item==v then found=true end end
          need(found,label.."."..key.." has unsupported value")
        else need(#v>=spec.minLength and #v<=spec.maxLength,label.."."..key.." exceeds string bounds") end
      elseif spec.type=="boolean" then need(type(v)=="boolean",label.."."..key.." must be boolean")
      elseif spec.type=="array" then
        need(type(v)=="table" and #v<=spec.maxItems,label.."."..key.." must be a bounded array")
        local count=0
        for index,item in pairs(v) do
          count=count+1
          need(type(index)=="number" and index%1==0 and index>=1 and index<=#v,
            label.."."..key.." must be a dense array")
          need(type(item)=="string" and #item>=1 and #item<=200 and not item:find("%z"),
            label.."."..key.." requires bounded strings")
        end
        need(count==#v,label.."."..key.." must be a dense array")
      end
    end
    for _,key in ipairs(schema.required or {}) do need(value[key]~=nil,label.." requires "..key) end
  end
  local function normalize(value,schema,base,label)
    validate(value,schema,label)
    local result=copy(base)
    for key,item in pairs(value) do
      result[key]=copy(item)
      if schema.properties[key].type=="array" then result[key]=l.array(result[key]) end
    end
    return result
  end
  function methods:inspect(...)
    need(select("#",...)==0,"inspect takes no arguments")
    need(states[self]~=nil,"expected Chimera preferences")
    return copy(states[self])
  end
  function methods:plan(request,...)
    need(select("#",...)==0,"plan takes one argument record")
    need(states[self]~=nil,"expected Chimera preferences")
    local normalized=normalize(request,request_schema,request_defaults,"plan")
    local receipt=l.call("chimera_route",{action="plan",policy=copy(states[self]),request=normalized})
    need(type(receipt)=="table" and type(receipt.ok)=="boolean" and type(receipt.result)=="string","invalid DSCO receipt")
    if not receipt.ok then fail("chimera_route failed: "..receipt.result) end
    local ok,result=l.try(function() return l.decode(receipt.result) end)
    need(ok and type(result)=="table" and result.ok==true,"invalid Chimera JSON receipt")
    need(result.profile=="chimera_plan" and result.executed==false and result.scope=="request",
      "Chimera result is not a request-scoped decision")
    need(type(result.decision)=="table" and type(result.policy)=="table" and type(result.request)=="table",
      "incomplete Chimera decision receipt")
    -- Keep the existing JSON-compatible observation fields. Execution uses a
    -- private copy, so editing a displayed receipt never changes what runs.
    plans[result]={policy=copy(result.policy),request=copy(result.request),
      contract=result.decision.execution_contract and copy(result.decision.execution_contract),attempted=false}
    return setmetatable(result,{
      __index=function(_,key)
        if key~="execute" then return nil end
        return function(self,...)
          need(select("#",...)==0,"plan execute takes no arguments; select a new plan to change inputs")
          local state=plans[self]
          need(state~=nil,"expected the original Chimera plan")
          need(type(state.contract)=="table","plan is not eligible for guarded execution; use direct, max_calls=1, max_output_tokens<=512 and cost caps<=0.01")
          need(not state.attempted,"this plan has already attempted execution; inspect its receipt instead of retrying")
          local raw=l.call("chimera_execute",{action="execute",policy=copy(state.policy),
            request=copy(state.request),expected_plan=copy(state.contract)})
          state.attempted=true
          need(type(raw)=="table" and type(raw.ok)=="boolean" and type(raw.result)=="string","invalid DSCO execution receipt")
          if not raw.ok then fail("chimera_execute failed: "..raw.result) end
          local valid,completed=l.try(function() return l.decode(raw.result) end)
          need(valid and type(completed)=="table" and completed.ok==true and
            completed.profile=="chimera_completion" and completed.executed==true and
            type(completed.completion)=="table","invalid Chimera completion receipt")
          return completed
        end
      end,
      __metatable="Lingo Chimera plan observation",
    })
  end
  local function preferences(options,...)
    need(select("#",...)==0,"preferences takes one argument record")
    if options==nil then options={} end
    local policy=normalize(options,policy_schema,defaults,"preferences")
    local ref=newproxy(true);local mt=getmetatable(ref)
    mt.__index=function(_,key)
      if methods[key] then return methods[key] end
      fail("preferences do not implement "..tostring(key).."; available methods: inspect, plan")
    end
    mt.__newindex=function() fail("Chimera preferences are immutable") end
    mt.__metatable="Lingo Chimera preferences"
    mt.__tostring=function() return "ChimeraPreferences(request-scoped)" end
    states[ref]=policy
    return ref
  end
  local api={version="0.2",preferences=preferences}
  return setmetatable({}, {
    __index=function(_,key) if api[key]~=nil then return api[key] end fail("unknown Chimera API "..tostring(key)) end,
    __newindex=function() fail("Chimera API is read-only") end,
    __metatable="Lingo Chimera API",
  })
end
