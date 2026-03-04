# Operational Runbooks

## 1. Baseline DB Lock / Timeline Failures

Symptoms:

- timeline server fails to start
- event logging appears stalled
- SQLite busy/locked errors

Steps:

1. Identify DB path:

```bash
./dsco --setup-report | rg baseline -n
```

2. Check active processes using DB:

```bash
lsof | rg baseline.db
```

3. If stale process exists, stop it cleanly.
4. Back up DB before invasive actions.
5. Validate DB integrity:

```bash
sqlite3 ~/.dsco/baseline.db 'PRAGMA integrity_check;'
```

6. Restart timeline server:

```bash
./dsco --timeline-server --timeline-port 8421
```

## 2. MCP Server Down or No Tools Registered

Symptoms:

- `/mcp` shows disconnected servers
- expected MCP tools missing from `/tools`

Steps:

1. Validate config:

```bash
cat ~/.dsco/mcp.json
```

2. Verify each server command is executable.
3. Run `/mcp reload`.
4. Inspect server stderr in terminal logs.
5. Check environment variables required by server.
6. Confirm tool schemas are returned by MCP initialize/list tools flow.

## 3. Provider/API Outage or Stream Errors

Symptoms:

- repeated HTTP errors
- stream aborts mid-response
- provider mismatch

Steps:

1. Check active provider and model:
   - `/provider`
   - `/model`
2. Confirm key env var for provider is set.
3. Retry with alternate model/provider.
4. Check baseline timeline `/events.json` for error events.
5. Inspect debug request payload in `~/.dsco/debug`.
6. If outage persists, switch to fallback chain via `/fallback`.

## 4. Tool Timeout Storm

Symptoms:

- many tool failures with timeout
- degraded throughput in interactive session

Steps:

1. Inspect `/metrics` for timeout-heavy tools.
2. Validate network and dependent external services.
3. Narrow down to one prompt with minimal tool calls.
4. Increase timeout override in `tools.c` for specific tool if justified.
5. Add caching/fallback behavior if endpoint latency is bursty.

## 5. Stuck Sub-Agent / Swarm Incompletion

Symptoms:

- `agent_wait` never finishes
- swarm children remain running but idle

Steps:

1. Use `agent_status` and `agent_output` tools to inspect state.
2. Check IPC DB for heartbeat freshness and task state.
3. Kill individual stuck child (`agent_kill`) and resubmit task.
4. Verify `DSCO_SWARM_DEPTH` and inheritance settings.
5. Add more explicit child task boundaries (avoid open-ended prompts).

## 6. Context Blow-Up / Poor Response Quality

Symptoms:

- high token usage
- degraded outputs due to context pressure

Steps:

1. Run `/context` and `/status`.
2. Run `/compact`.
3. Disable noisy features if needed (`/web off`, `/code off`).
4. Re-run with focused prompt and scoped tool choice.
5. Optionally lower model/context complexity.

## 7. Corrupt Session Save/Load

Symptoms:

- `/load` fails or loads incomplete context

Steps:

1. List sessions: `/sessions`
2. Validate JSON format:

```bash
jq . ~/.dsco/sessions/<name>.json
```

3. Recover with `_autosave` if available.
4. If parser fails, salvage manually by extracting message content blocks.

## Escalation Data to Capture

When escalating incidents, include:

- model/provider in use
- exact command/prompt
- `/status`, `/metrics`, `/telemetry` snapshots
- baseline event excerpt (`/events.json`)
- relevant debug request payload
