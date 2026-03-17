# dsco Architecture & Call Flow

## High-Level Data Flow

```
┌─────────────────────────────────────────────────────────────┐
│                    User Input (CLI/REPL)                    │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
        ┌────────────────────────────┐
        │    main() — CLI Dispatch   │
        │  (parse args, mode select) │
        └────────┬──────────────────┘
                 │
        ┌────────▼─────────┬──────────────────┐
        │                  │                  │
   ┌────▼────┐      ┌─────▼──┐        ┌─────▼────┐
   │ Oneshot │      │  REPL  │        │ Topology │
   │  Mode   │      │ Mode   │        │   List   │
   └────┬────┘      └─────┬──┘        └────┬─────┘
        │                 │                │
        ▼                 ▼                ▼
   ┌─────────────────────────────────────────┐
   │    run_oneshot_topology()               │
   │  or  interactive_repl()                 │
   └────────────┬────────────────────────────┘
                │
                ▼
        ┌──────────────────────────┐
        │  llm_stream_request()    │
        │  (Invoke Claude API)     │
        └──────────┬───────────────┘
                   │
        ┌──────────┴──────────┐
        │                     │
   ┌────▼──────┐     ┌───────▼──────┐
   │ Text      │     │ Tool Call    │
   │ Streaming │     │ Detection    │
   │ (callback)│     │              │
   └────┬──────┘     └───────┬──────┘
        │                    │
        │              ┌─────▼──────────┐
        │              │ tool_dispatch()│
        │              │  (match name)  │
        │              └─────┬──────────┘
        │                    │
        │         ┌──────────▼──────────┐
        │         │                     │
        │    ┌────▼────┐          ┌────▼────┐
        │    │ Spawn   │          │ Execute │
        │    │ Swarm?  │          │ Local   │
        │    │ (IPC)   │          │ Tool    │
        │    └─────────┘          └────┬────┘
        │                              │
        └──────────────┬───────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │  Response to LLM     │
            │  (tool result)       │
            └──────────┬───────────┘
                       │
                    Loop...
                    (until stop)
```

## Key Function Call Chains

### 1. Oneshot Invocation
```
main()
  ├─> run_oneshot_topology()
  │    ├─> llm_stream_request()
  │    │    ├─> llm_provider_detect()
  │    │    ├─> curl_easy_perform() [HTTP POST]
  │    │    └─> parse_sse_response()
  │    │         ├─> oneshot_text_cb()
  │    │         └─> oneshot_tool_cb()
  │    │              ├─> tool_find()
  │    │              ├─> tool_execute()
  │    │              │    └─> [individual tool function]
  │    │              └─> llm_append_tool_result()
  │    └─> print_result()
  │
  └─> exit()
```

### 2. Interactive REPL Mode
```
main()
  ├─> interactive_repl()
  │    └─> loop: ▼
  │         ├─> read_user_input()
  │         ├─> llm_stream_request() [with conversation history]
  │         │    ├─> llm_parse_response()
  │         │    └─> tool callbacks
  │         ├─> accumulate_context()
  │         └─> render_output()
  │    until (exit_signal || EOF)
  │
  └─> cleanup()
```

### 3. Agent Spawning (Swarm)
```
tool_spawn_agent()
  ├─> parse_json_input()
  ├─> fork()
  │    │
  │    ├─ PARENT:
  │    │  ├─> store_child_pid()
  │    │  ├─> await_agent_ready()
  │    │  └─> return agent_id
  │    │
  │    └─ CHILD:
  │       ├─> initialize_ipc_channel()
  │       ├─> spawn_new_dsco_instance()
  │       │    └─> execvp("./dsco", ["--agent-id", id, ...])
  │       │         └─> (new dsco process takes over)
  │       └─> [parent code path continues]
  │
  └─> ipc_register_child()
```

### 4. Tool Dispatch Chain
```
tool_dispatch(name, input)
  ├─> tool_find(name)
  │    └─> lookup in tool_registry[]
  │
  ├─> validate input against schema
  │
  ├─> execute tool function:
  │    ├─ File tools: file_read(), file_write(), etc.
  │    ├─ Network: ping(), dns_lookup(), etc.
  │    ├─ Git: git_status(), git_commit(), etc.
  │    ├─ Crypto: sha256(), hmac(), uuid(), etc.
  │    ├─ Shell: bash_execute(), run_command(), etc.
  │    ├─ LLM: huggingface(), av_quote(), etc.
  │    └─ [+180 more tools]
  │
  └─> return result (or error)
```

## IPC & Multi-Agent Communication

```
Agent A                          Agent B
───────────────────────────────────────────

send_message()                   (listening on channel)
  └─> ipc_post_msg()
       └─> write to named_pipe
                                 ├─> ipc_recv()
                                 └─> parse_msg()

              Shared Scratchpad (shared memory or tmpfs)
              ┌────────────────┐
              │ task_queue     │
              │ state_dict     │
              │ agent_registry │
              └────────────────┘
                    ▲      ▲
                    │      │
              (read/write both agents)
```

## Swarm Hierarchy (Depth ≤ 4)

```
┌─────────────────────────────────────────┐
│ Parent Agent (depth=0)                  │
│                                         │
│  ┌──────────────────────────────────┐  │
│  │ spawn_agent("task1", params)     │  │
│  │ spawn_agent("task2", params)     │  │
│  │ create_swarm("name", [task1, 2]) │  │
│  └──────┬───────────────────────────┘  │
└─────────┼──────────────────────────────┘
          │
    ┌─────┴─────┐
    │           │
┌───▼──┐    ┌──▼───┐
│Child1│    │Child2│  (depth=1)
│depth1│    │depth1│
└───┬──┘    └──┬───┘
    │          │
    │    ┌─────▼──────┐
    │    │ Grandchild │  (depth=2)
    │    │ can spawn  │
    │    │ more...    │
    │    └────────────┘
    │
    └─> max depth = 4
```

