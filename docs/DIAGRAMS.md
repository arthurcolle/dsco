# Architecture Diagrams

## 1. Interactive Turn Sequence

```mermaid
sequenceDiagram
  participant U as User
  participant A as agent.c
  participant P as provider.c/llm.c
  participant L as LLM API
  participant T as tools.c

  U->>A: Prompt
  A->>P: Build request from conversation
  P->>L: Streaming API call
  L-->>P: content_block deltas
  P-->>A: text/tool_use/thinking callbacks
  A->>T: Execute tool_use blocks
  T-->>A: tool_result
  A->>P: Next turn request with tool_result
  P->>L: Continue stream
  L-->>A: end_turn
  A-->>U: Final rendered answer
```

## 2. Component Topology

```mermaid
flowchart LR
  CLI[main.c + agent.c] --> LLM[llm.c]
  CLI --> Provider[provider.c]
  CLI --> Renderer[md.c + tui.c]
  CLI --> Tools[tools.c]
  Tools --> Integrations[integrations.c]
  Tools --> MCP[mcp.c]
  Tools --> Plugins[plugin.c]
  CLI --> Baseline[baseline.c]
  CLI --> Setup[setup.c]
  CLI --> Semantic[semantic.c]
  CLI --> AST[ast.c]
  CLI --> Eval[eval.c]
  CLI --> Pipeline[pipeline.c]
  CLI --> Crypto[crypto.c]
  CLI --> Swarm[swarm.c]
  Swarm --> IPC[ipc.c]
```

## 3. Swarm + IPC Lifecycle

```mermaid
sequenceDiagram
  participant R as Root dsco
  participant S as swarm.c
  participant C as Child dsco
  participant I as ipc.c (SQLite)

  R->>S: spawn child task
  S->>C: exec dsco with DSCO_SUBAGENT=1
  C->>I: ipc_register + heartbeat
  R->>I: ipc_task_submit
  C->>I: ipc_task_claim
  C->>C: run prompt/tool loop
  C->>I: ipc_task_complete(result)
  R->>I: collect status/output
```

## 4. Tool Dispatch Pipeline

```mermaid
flowchart TD
  A[tool name + json input] --> B[tool_map lookup]
  B --> C[input schema validation]
  C --> D[start watchdog]
  D --> E[execute built-in or external callback]
  E --> F[capture output + success/failure]
  F --> G[stop watchdog]
  G --> H[record metrics/cache]
  H --> I[return tool_result]
```

## 5. Baseline Timeline Data Flow

```mermaid
flowchart LR
  CLI[agent/main] -->|baseline_log| DB[(baseline sqlite)]
  CLI -->|trace_span_begin/end| DB
  Browser -->|GET /| Server[baseline_serve_http]
  Browser -->|GET /events.json| Server
  Server --> DB
  Server --> WWW[www/freight.html and static assets]
```

## 6. Markdown Streaming Renderer State

```mermaid
stateDiagram-v2
  [*] --> Normal
  Normal --> CodeBlock: fence start
  CodeBlock --> Normal: fence end
  Normal --> LatexBlock: $$ start
  LatexBlock --> Normal: $$ end
  Normal --> HtmlBlock: html block start
  HtmlBlock --> Normal: html close
  Normal --> Table: table row start
  Table --> Normal: non-table line
```

## 7. Local-First Hosted Control Plane

`dsco` should treat hosted `distributed.systems` services as optional
accelerators. Local execution, policy, secrets, and audit state remain
authoritative; hosted planes provide shared tool discovery, durable remote work,
fleet health, and advisory routing data.

```mermaid
flowchart LR
  subgraph Local["Local dsco edge"]
    Agent["agent.c / main.c"]
    Policy["global + project policy"]
    LocalTools["built-ins / MCP / plugins"]
    Stores["Baseline / Chronicle / IPC / control DB"]
    Secrets["env / sealed store / Keychain"]
  end

  subgraph Hosted["distributed.systems"]
    Tools["tools.distributed.systems"]
    Control["future control plane"]
    Graph["future graph/memory plane"]
    Artifacts["future artifact plane"]
  end

  Agent --> Policy
  Policy --> LocalTools
  Agent --> Stores
  Agent --> Secrets
  LocalTools -->|optional bearer-token calls| Tools
  Agent -->|optional signed heartbeat| Control
  Stores -->|optional redacted summaries| Artifacts
  Stores -->|optional aggregate priors| Graph
  Control -->|advisory desired-state hints| Agent
  Tools -->|catalog / execute / batch / recommend| LocalTools
```

See [`LOCAL_CONTROL_PLANE.md`](LOCAL_CONTROL_PLANE.md) for the detailed
hybrid-control-plane flows, sync policy, offline behavior, and service split.
