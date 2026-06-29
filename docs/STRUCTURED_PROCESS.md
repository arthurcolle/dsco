# Structured Process Runtime

`dsco` routes long-running work through a typed process contract instead of
unbounded agent loops. The contract is designed for OpenAI Structured Outputs:

```sh
./dsco --structured-schema-json
./dsco --structured-plan-json "profile the agentic CLI"
./dsco --structured-plan-tree "profile the agentic CLI"
```

The schema root is an object, every field is required, and every object uses
`additionalProperties:false`. Model calls should use this as `text.format` when
the model is classifying or decomposing a user request. Tool execution should
still use function/tool calling; the structured process JSON only describes the
bounded work graph.

The local fallback planner reserves 20% of work for smart-model promotion gates
and 80% for deterministic background atoms. Atoms carry lane, dependencies,
timeouts, max attempts, and promotion triggers so the runtime can schedule
small units without `while(1)` control flow.
