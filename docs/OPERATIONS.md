# Operations, Config, Storage, and Troubleshooting

## Build and Test

### Build

```bash
make -j8
```

### Run Unit/Integration Test Runner

```bash
make test
```

### Run Hygiene Targets

```bash
make format-check
make lint
make static-analysis
make asan-test
make ubsan-test
make check-version
```

### Install/Uninstall

```bash
make install PREFIX=/usr/local
make uninstall PREFIX=/usr/local
```

## Runtime Modes

- Interactive mode: `./dsco`
- One-shot mode: `./dsco "...prompt..."`
- Setup mode:
  - `./dsco --setup`
  - `./dsco --setup-force`
  - `./dsco --setup-report`
- Timeline server mode:
  - `./dsco --timeline-server --timeline-port 8421`

## Interactive Slash Commands

From `agent.c`, key commands include:

- `/clear`
- `/model [name]`
- `/effort [low|medium|high]`
- `/cost`
- `/context`
- `/compact`
- `/save [name]`
- `/load [name]`
- `/sessions`
- `/setup`
- `/setup report`
- `/force [tool|auto|none|any]`
- `/web [on|off]`
- `/code [on|off]`
- `/budget [amount|off]`
- `/trust [trusted|standard|untrusted]`
- `/status`
- `/tools`
- `/plugins`
- `/plugins validate [manifest] [lock]`
- `/mcp` and `/mcp reload`
- `/provider`
- `/temp [0.0-2.0|off]`
- `/thinking [auto|>=1024]`
- `/fallback model1,model2,...`
- `/metrics`
- `/telemetry`
- `/cache [clear]`
- `/trace [trace_id]`
- `/version`
- `/help`

## Environment Variables

### Core CLI/Model

- `ANTHROPIC_API_KEY`
- `OPENAI_API_KEY`
- `DSCO_MODEL`
- `DSCO_PROFILE`
- `DSCO_ENV_FILE`

### Storage and Telemetry

- `DSCO_BASELINE_DB`
- `DSCO_INSTANCE_ID`
- `DSCO_PARENT_INSTANCE_ID`

### IPC/Swarm

- `DSCO_IPC_DB`
- `DSCO_SUBAGENT`
- `DSCO_SWARM_DEPTH`

### Context/Browser Controls

- `DSCO_CONTEXT_OFFLOAD_BYTES`
- `DSCO_BROWSER_HOST_DB`
- `DSCO_BROWSER_HOST_FLUSH_SEC`
- `DSCO_BROWSER_MAX_PASSES`

### Integrations (commonly used)

- `GITHUB_TOKEN`
- `JINA_API_KEY`
- `FRED_API_KEY`
- `ALPHA_VANTAGE_API_KEY`
- `SUPABASE_URL`
- `SUPABASE_API_KEY`
- `DISCORD_TOKEN`
- `TWILIO_ACCOUNT_SID`
- `TWILIO_FROM_NUMBER`

### Setup-Autodiscovered Keys

`setup.c` includes a broader curated list (OpenRouter, Together, Groq, DeepSeek, Mistral, Cohere, xAI, Cerebras, Perplexity, HF, Slack, Stripe, Mapbox, OpenWeather, etc.) and alias normalization.

## Persistent Paths

- Sessions: `~/.dsco/sessions`
- Plugins: `~/.dsco/plugins`
- Debug requests: `~/.dsco/debug`
- MCP config: `~/.dsco/mcp.json`
- Optional system prompt override: `~/.dsco/system_prompt.txt`
- Baseline DB default: `~/.dsco/baseline.db`

Script-specific caches:

- `~/.dsco/cache`
- `~/.dsco/cache/freight_quant`
- `~/.dsco/cache/hormuz`

## SQLite Schemas

### Baseline DB (`baseline.c`)

- `instances(instance_id, parent_instance_id, pid, model, mode, started_at, ended_at)`
- `events(id, instance_id, ts, ts_epoch, category, title, detail, metadata_json)`
- `trace_spans(span_id, trace_id, parent_span, name, start_epoch, end_epoch, status, metadata_json)`

### IPC DB (`ipc.c`)

`ipc.c` manages schema for:

- agents and heartbeats
- messages
- tasks
- scratch key-value rows

## Timeline Server

`baseline_serve_http()` binds to `127.0.0.1:<port>` and serves:

- `/` timeline HTML (instance/event view)
- `/events.json` machine-readable event feed
- `/health` text liveness
- `/freight` static freight dashboard (`www/freight.html`)
- `/www/*` static assets

## Provider Selection

`provider_detect()` uses model/key heuristics:

- Anthropic model IDs and `sk-ant-` keys -> `anthropic`
- `gpt-`, `o1`, `o3`-style + `sk-` keys -> `openai` family
- Other supported endpoints are treated as OpenAI-compatible base URLs

## Troubleshooting

### `ANTHROPIC_API_KEY not set`

- Export the key or use `--setup` to persist credentials.

### Tools timing out

- Check `/metrics` for timeout counts.
- Increase per-tool timeout in `tools.c` timeout config if needed.

### Timeline not starting

- Verify write permissions for baseline DB parent directory.
- Check whether selected port is already in use.

### MCP tools not appearing

- Verify `~/.dsco/mcp.json` structure.
- Run `/mcp reload` and inspect server status with `/mcp`.

### Provider mismatch

- Confirm `/model` value and corresponding API key availability.
- Use `/provider` to view currently detected provider.

### Large context pressure

- Use `/context` and `/compact`.
- Tune offload threshold with `DSCO_CONTEXT_OFFLOAD_BYTES`.
