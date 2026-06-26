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

`make install` also installs `tool_embeddings.bin` to `PREFIX/share/dsco/`.
At runtime `dsco` searches, in order:
- `DSCO_TOOL_EMBEDDINGS_FILE`
- repo-local paths such as `include/tool_embeddings.bin`
- paths relative to the executable, including `../share/dsco/tool_embeddings.bin`
- `~/.dsco/tool_embeddings.bin`

## Runtime Modes

- Bare invocation: `./dsco` prints help and exits nonzero
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

Use env vars for stable defaults, local development, CI, and emergency
diagnostics. Prefer command-line flags for one-off runs, and prefer
`./dsco --setup`, `./dsco login`, or `dsco_setup_set_key()` paths for durable
credential storage. `docs/CONSTANTS_ENV_INDEX.md` is the generated exhaustive
source index; this section gives the operational rules for the variables and
families that are meant to be set by users or scripts.

General rules:

- Secrets: do not commit them, print them, or persist them in shell history. API
  keys should live in the process environment only for one run, or in dsco's
  setup/sealed-store path for regular use.
- Booleans: typed helpers accept `1`, `true`, `yes`, `on` and `0`, `false`,
  `no`, `off`. Older call sites may only test whether the variable is present,
  so unset is safest when disabling.
- Numbers: typed integer/size helpers clamp to the valid range. Invalid values
  silently fall back to defaults, so debugging should include the exact value.
- Internal runtime vars: do not persist `DSCO_SUPERVISED`,
  `DSCO_RESUME_AFTER_CRASH`, `DSCO_MEM_PRESSURE`, `DSCO_SUBAGENT`,
  `DSCO_SWARM_DEPTH`, `DSCO_PARENT_INSTANCE_ID`, `DSCO_PROJECT_ID`, or
  `DSCO_WORKER`. Parent processes set these for children.

### Resolution and Precedence

Environment behavior is intentionally layered. When a run behaves unexpectedly,
debug in this order:

1. Explicit CLI flags win over defaults and loaded profile values. Examples:
   `-m`, `-e`, `--provider`, `--local`, `--profile`, `-k`, and `--setup-*`.
2. The live process environment wins over saved setup files. `setup.c` loads
   values only when that key is not already present in the environment.
3. `DSCO_ENV_FILE` changes which saved setup file is loaded. Use it for
   reproducible profiles rather than editing global shell startup files.
4. Provider credential aliases collapse to canonical provider credentials.
   Prefer canonical names for durable config: `FUGU_API_KEY`,
   `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`.
5. The sealed store can intern known secrets after startup. A variable may no
   longer be visible in `env`, while provider resolution can still see it.
6. Built-in defaults apply only after CLI, live env, setup env, sealed-store
   lookups, and provider/model routing have had a chance to resolve.

Routing-specific precedence:

- Provider pins (`--provider`, native `-e`, or `DSCO_EXEC` set to a native
  provider) beat generic executor defaults.
- Model pins (`-m`, `DSCO_MODEL`) constrain routing to that model family unless
  fallback logic is explicitly allowed.
- A provider-shaped `-k` key can select that provider for the run and publish the
  key under its canonical env var in-process.
- `DSCO_EXEC=claude` and `DSCO_EXEC=codex` mean "use the external CLI".
  `DSCO_EXEC=fugu`, `DSCO_EXEC=sakana`, and other native names mean "use dsco's
  native provider transport".

### Persistence Policy

| Scope | Use for | Avoid |
|---|---|---|
| One command: `VAR=value ./dsco ...` | Repros, smoke tests, temporary providers, staging endpoints. | Long-lived secrets that should survive shell restarts. |
| Current shell: `export VAR=value` | Short development sessions. | Shared terminals, pair sessions, and high-value secrets. |
| `./dsco --setup` / setup env file | Stable local provider defaults and noninteractive startup. | Internal runtime vars and crash/supervisor state. |
| `DSCO_ENV_FILE=/path/profile.env` | Per-project or CI profiles. | Machine-global defaults that should apply everywhere. |
| CI secret manager | Build/test/provider smoke credentials. | Debug variables that change supervisor semantics unless the test requires them. |
| Shell rc files | Non-secret UI preferences and stable local paths. | API keys, OAuth tokens, trading keys, and restart-policy overrides. |

Recommended profile pattern:

```bash
DSCO_ENV_FILE="$PWD/.dsco.env.dev" ./dsco --setup-report
DSCO_ENV_FILE="$PWD/.dsco.env.dev" ./dsco -e fugu 'smoke test'
DSCO_ENV_FILE="$PWD/.dsco.env.ci" DSCO_NO_AUTO_INTERACTIVE=1 ./dsco --version
```

### Risk Tiers

| Tier | Examples | Policy |
|---|---|---|
| Secret | `*_API_KEY`, `*_TOKEN`, `*_SECRET`, `DSCO_NET_AUTH_KEY`, `DSCO_MESH_SECRET`, trading credentials. | Store in setup/sealed store or a secret manager. Do not log. Redact from incident reports. |
| Routing default | `DSCO_EXEC`, `DSCO_MODEL`, `DSCO_PROFILE`, `DSCO_CHEAP`. | Safe to persist only when intentional. Include in bug reports because these change behavior. |
| Runtime internal | `DSCO_SUPERVISED`, `DSCO_RESUME_AFTER_CRASH`, `DSCO_MEM_PRESSURE`, `DSCO_SUBAGENT`, `DSCO_SWARM_DEPTH`. | Never persist. Clear before manual repros. |
| Diagnostics | `DSCO_DEBUG_AUTH`, `DSCO_DEBUG_REQUEST`, `DSCO_TRACE`, `DSCO_PERF`, `DSCO_TEST_CRASH`. | Use narrowly. Remove after the repro. Treat generated logs as potentially sensitive. |
| Resource policy | `DSCO_SUPERVISE_*`, `DSCO_MAX_TOKENS`, `DSCO_TOOL_DEFAULT_TIMEOUT`, swarm limits. | Tune per host or test suite. Do not globally raise limits without load testing. |
| UI/local preference | `DSCO_GLYPH`, `DSCO_NO_CLEAR`, `DSCO_HYPERLINKS`, local host overrides. | Safe to persist if non-secret and machine-specific. |

### Advanced Debug Commands

Use these commands to make env state explicit without dumping secrets:

```bash
./dsco --setup-report
DSCO_DEBUG_AUTH=1 ./dsco -m fugu 'ping'
env -u DSCO_EXEC -u DSCO_MODEL ./dsco --version
DSCO_ENV_FILE=/tmp/dsco-empty.env ./dsco --setup-report
```

For request-construction bugs:

```bash
DSCO_DEBUG_REQUEST=1 ./dsco --local 'echo the exact prompt'
ls -l ~/.dsco/debug/
```

Do not attach `~/.dsco/debug/*` to issues without reviewing it first; request
debug files can contain prompts, documents, tool results, and provider payloads.

### Provider, Model, and Routing

| Env var | Guideline |
|---|---|
| `DSCO_EXEC` | Default executor/provider. Use `claude`, `codex`, `auto`, `smart`, `fugu`, `sakana`, or another native provider name. Only persist a provider value when the matching credential is available; otherwise bare `./dsco` will fail with a configuration error. |
| `DSCO_MODEL` | Default model override. Persist only when you want every run to use that model family. Clear it when switching providers if routing looks surprising. |
| `DSCO_PROFILE` | Startup profile: `full`, `lite`, or `worker`. Use `lite` for fast local utility invocations. `worker` is normally set by dsco for child processes. |
| `DSCO_CHEAP` | Enables cheap mode when truthy: minimal core tools plus dynamic discovery. Use for low-cost smoke tests and quick prompts. |
| `DSCO_MAX_TOKENS` | Max output token reserve. Raise for long-form generation; lower for tight budget runs. |
| `DSCO_MAX_AGENT_TURNS` | Checkpoint cadence for long agent loops, not the primary stop condition. Lower it for more frequent progress surfacing. |
| `DSCO_HARD_TURN_CEILING` | Emergency runaway backstop. Leave at default outside stress testing. |
| `DSCO_EFFORT` | Reasoning effort default. Common values are `low`, `medium`, `high`, `max`, or provider-specific equivalents. |
| `DSCO_TEMPERATURE`, `DSCO_TOP_P`, `DSCO_TOP_K` | Sampling controls. Prefer unset for provider defaults; set only for repeatable experiments or creative generation. |
| `DSCO_THINKING_BUDGET` | Provider reasoning-token budget. Use only with providers/models that support it. |
| `DSCO_TOOL_CHOICE` | Default tool-choice policy. Use sparingly; interactive `/force` is safer for ad hoc control. |
| `DSCO_SYSTEM_PROMPT` | Overrides the system prompt. Use for isolated experiments; prefer `~/.dsco/system_prompt.txt` for durable local customization. |

### Provider Credentials and Auth

| Env var | Guideline |
|---|---|
| `ANTHROPIC_API_KEY`, `CLAUDE_API_KEY` | Direct Anthropic credentials. Claude Code OAuth is preferred when available; avoid setting both unless debugging auth precedence. |
| `DSCO_CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_OAUTH_TOKEN` | Claude Code OAuth access token override. Treat as a secret. Prefer discovery from Claude Code's own auth store instead of manually exporting. |
| `DSCO_CLAUDE_CODE_CREDENTIALS_FILE`, `CLAUDE_CONFIG_DIR` | Override Claude Code credential discovery paths. Use in tests, sandboxes, and nonstandard installs. |
| `DSCO_CLAUDE_CODE_KEYCHAIN_SERVICE` | Override the macOS keychain service used for Claude Code OAuth lookup. Use only when testing alternate keychain entries. |
| `DSCO_CLAUDE_CODE_VERSION`, `CLAUDE_CODE_VERSION`, `DSCO_CLAUDE_CODE_ENTRYPOINT`, `CLAUDE_CODE_ENTRYPOINT` | Claude Code compatibility overrides. Leave unset unless reproducing a version-specific adapter issue. |
| `DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY` | Truthy disables Claude Code OAuth discovery. Use to force direct `ANTHROPIC_API_KEY` behavior. |
| `OPENAI_API_KEY`, `OPENAI_KEY`, `CHATGPT_API_KEY` | OpenAI API key aliases. Use `OPENAI_API_KEY` as canonical. |
| `DSCO_CHATGPT_OAUTH_TOKEN`, `CHATGPT_OAUTH_TOKEN`, `DSCO_CHATGPT_ACCOUNT_ID` | ChatGPT/Codex subscription auth overrides. Treat as secrets; prefer `dsco login`/Codex auth discovery. |
| `DSCO_DISABLE_CODEX_OAUTH_DISCOVERY`, `DSCO_DISABLE_CHATGPT_NATIVE` | Truthy disables subscription/native ChatGPT routes. Use to force direct OpenAI API-key routing. |
| `OPENROUTER_API_KEY` | OpenRouter fallback/routing credential. Use for namespaced `org/model` IDs and cross-provider fallbacks. |
| `FUGU_API_KEY` | Canonical Sakana/Fugu credential. Required for `DSCO_EXEC=fugu`, `DSCO_EXEC=sakana`, `-e fugu`, or `--provider sakana`. |
| `SAKANA_API_KEY`, `FISH_API_KEY`, `SAKANA_TOKEN` | Accepted aliases for `FUGU_API_KEY`. Prefer migrating durable config to `FUGU_API_KEY`. |
| `FUGU_BASE_URL`, `FUGU_API_BASE`, `SAKANA_API_BASE`, `SAKANA_BASE_URL` | Sakana endpoint override. Leave unset for production; set only for staging, proxy, or compatibility tests. |
| Other `*_API_KEY`, `*_TOKEN`, `*_SECRET` | Setup can auto-detect many provider/tool credentials. Use canonical provider names where known; generic names work for custom providers when paired with a base URL. |
| `<LOCAL_SERVER>_HOST` | Local model host override, for example `LMSTUDIO_HOST=http://127.0.0.1:1234`. Supported server names include `OLLAMA`, `LMSTUDIO`, `MLX`, `VLLM`, `LLAMACPP`, `JAN`, `GPT4ALL`, `KOBOLDCPP`, `TEXTGEN`, `TGI`, and `SGLANG`. |
| `DSCO_DEBUG_AUTH` | Prints provider/model/auth-mode diagnostics without revealing keys. Good first step for routing bugs. |
| `DSCO_DEBUG_REQUEST` | Saves request JSON for provider debugging. Treat output as sensitive because prompts and tool data may be included. |

### OpenRouter and Provider Policy

| Env var | Guideline |
|---|---|
| `DSCO_OR_ROUTE` | Forces an OpenRouter route. Use for reproducibility, not as a general default. |
| `DSCO_OR_TRANSFORMS` | Controls OpenRouter transforms. Set only when testing transform behavior. |
| `DSCO_OR_PROVIDER_ORDER`, `DSCO_OR_PROVIDER_ONLY`, `DSCO_OR_PROVIDER_IGNORE` | Provider allow/order/deny controls. Use for incident mitigation or benchmarking. |
| `DSCO_OR_REQUIRE_PARAMS` | Requires provider parameters. Use when a model must honor exact options. |
| `DSCO_OR_ALLOW_FALLBACKS`, `DSCO_OR_FALLBACK_MODELS` | Fallback policy. Disable or pin for deterministic tests; allow for resilience. |
| `DSCO_OR_DATA_COLLECTION`, `DSCO_OR_ZDR` | Privacy/routing preferences. Set explicitly for sensitive workloads. |
| `DSCO_OR_QUANTIZATIONS`, `DSCO_OR_SORT`, `DSCO_OR_MAX_PRICE_INPUT`, `DSCO_OR_MAX_PRICE_OUTPUT` | Cost/performance routing controls. Prefer one-run exports while tuning. |
| `DSCO_OR_REASONING_EFFORT`, `DSCO_OR_CACHE`, `DSCO_OR_DISABLE_TOOLS`, `DSCO_OR_MAX_TOOLS` | OpenRouter request shaping. Use in provider compatibility tests and smoke tests. |
| `DSCO_OR_REFERER`, `DSCO_OR_TITLE` | OpenRouter attribution headers. Set for branded deployments; defaults are fine locally. |
| `DSCO_OR_DEBUG` | OpenRouter-specific debugging. Avoid persistent enablement because logs can be noisy. |

### Supervisor, Restart, and Crash Diagnostics

| Env var | Guideline |
|---|---|
| `DSCO_NO_AUTO_SUPERVISE` | Truthy disables bare interactive auto-supervision. Use when debugging startup/restart behavior. |
| `DSCO_NO_SUPERVISE` | Prevents recursion inside supervised children. Do not persist globally unless intentionally disabling all supervision. |
| `DSCO_SUPERVISE_RESTART` | Restart policy: `transient`, `permanent`, or `temporary`. Bare interactive auto-supervise sets `permanent`; `/quit` switches to transient. |
| `DSCO_SUPERVISE_MAX_RESTARTS` | Rapid restart cap. Default is `20`; valid range is `1..100`. Lower in tests to prove crash-loop behavior quickly. |
| `DSCO_SUPERVISE_WINDOW_S` | Crash-loop counting window in seconds. Default is `120`; valid range is `5..3600`. |
| `DSCO_SUPERVISE_STABLE_S` | Uptime that resets the crash counter. Default is `30`; valid range is `5..3600`. |
| `DSCO_SUPERVISE_BACKOFF_MS` | Initial restart delay. Default is `250`; valid range is `10..60000`. |
| `DSCO_SUPERVISE_BACKOFF_MAX_MS` | Backoff ceiling. Default is `15000`; valid range is `100..300000`. |
| `DSCO_SUPERVISE_MEM_LIMIT_MB` | Explicit child RSS budget. Leave unset for dynamic fleet budgeting; set only for memory stress tests or constrained hosts. |
| `DSCO_SUPERVISE_MEM_SOFT_PCT` | Soft warning percent of budget. Default is `80`; valid range is `10..99`. |
| `DSCO_SUPERVISE_POLL_MS` | RSS sampling interval. Default is `500`; valid range is `20..5000`. Lower values add overhead. |
| `DSCO_SUPERVISE_TERM_GRACE_MS` | SIGTERM-to-SIGKILL grace on memory preemption. Default is `8000`; valid range is `100..60000`. |
| `DSCO_SUPERVISE_METRICS_SECS` | Child RSS JSONL sample cadence. Default is `10`; `0` disables metrics. |
| `DSCO_HOTSWAP_BIN` | Explicit staged binary path for hotswap. Use during upgrade/restart testing. |
| `DSCO_CRASH_DEBUGGER` | Truthy attaches lldb/gdb on crash when available. Supervisor sets it for children. |
| `DSCO_TEST_CRASH` | Fault injection for supervisor tests. Never persist. |
| `DSCO_ALLOW_DEBUGGER`, `DSCO_DEBUG` | Allow debugger/tamper exceptions. Use only in local debugging. |

### Storage, Setup, Trace, and Output Control

| Env var | Guideline |
|---|---|
| `DSCO_ENV_FILE` | Override the setup env file. Use for profiles, tests, and temporary sandboxes. |
| `DSCO_BASELINE_DB` | Override sqlite baseline path. Use for isolated test runs or shared timeline storage. |
| `DSCO_SESSION_PATH` | Override session-memory storage path. Use in tests; keep default for normal use. |
| `DSCO_COST_HISTORY` | Override learned-cost history file. Use when benchmarking separately from personal history. |
| `DSCO_INSTANCE_ID` | Baseline instance id set by dsco. Do not persist manually. |
| `DSCO_TRACE`, `DSCO_TRACE_STDERR` | Enable trace logging. Use for repros; disable for normal interactive sessions. |
| `DSCO_PERF` | Enables startup/performance timing. Use for profiling. |
| `DSCO_OUTPUT_GUARD` | Enables/disables output loop guard. Leave enabled unless debugging guard behavior. |
| `DSCO_OUTPUT_REPEAT_LIMIT`, `DSCO_OUTPUT_REPEAT_MIN_BYTES`, `DSCO_OUTPUT_MOTIF_MIN_BYTES`, `DSCO_OUTPUT_MOTIF_SKIP_PATHLIKE`, `DSCO_OUTPUT_MAX_BYTES` | Output guard thresholds. Tune only for specific false positives/false negatives. |
| `DSCO_SECURE_STORE_AUTH_UI` | Allows macOS auth UI for secure-store unlock. Useful when a foreground prompt is acceptable. |
| `DSCO_SECURE_STORE_TIMEOUT_MS` | Secure-store startup timeout. Raise when keychain/Secure Enclave is slow. |
| `DSCO_SECURE_STORE_NO_PROMPT` | Suppresses secure-store prompts. Use in CI or noninteractive contexts. |
| `DSCO_REQUIRE_SECURE_ENCLAVE`, `DSCO_TRY_SECURE_ENCLAVE`, `DSCO_SE_PERSISTENT`, `DSCO_ALLOW_MACHINE_UUID_STORE` | Secure-store backend policy. Change only with security review. |

### Networking, IPC, Swarm, and Mesh

| Env var | Guideline |
|---|---|
| `DSCO_HTTP_PORT` | Local HTTP/TLS API port. Use to avoid collisions; default starts at the compiled default and probes nearby ports. |
| `DSCO_MESH_PORT`, `DSCO_DHT_PORT`, `DSCO_DHT_SWARM`, `DSCO_DHT_BOOTSTRAP`, `DSCO_PEERS` | Mesh/DHT discovery controls. Use only when intentionally joining or testing a mesh. |
| `DSCO_NODE_ID`, `DSCO_BEACON_URL`, `DSCO_BEACON_SECS`, `DSCO_MESH_SECRET`, `DSCO_NET_AUTH_KEY` | Node identity/beacon/auth settings. Treat auth values as secrets. |
| `DSCO_IPC_DB` | SQLite IPC path. Parent processes propagate it to workers. Set manually only to join a known IPC bus. |
| `DSCO_REDIS_HOST`, `DSCO_REDIS_PORT`, `DSCO_REDIS_PASSWORD` | Redis IPC backend settings. Treat password as a secret. |
| `DSCO_SUBAGENT`, `DSCO_SWARM_DEPTH`, `DSCO_PARENT_INSTANCE_ID` | Worker metadata. Internal; do not persist. |
| `DSCO_SWARM_MAX_CHILDREN`, `DSCO_SWARM_MAX_GROUPS`, `DSCO_SWARM_MAX_DEPTH` | Swarm guardrails. Lower for resource-constrained machines; raise only after load testing. |
| `DSCO_SWARM_HAIKU`, `DSCO_SWARM_SONNET`, `DSCO_SWARM_OPUS`, `DSCO_WORKER_MODEL` | Worker model overrides. Use for topology experiments and benchmarking. |
| `DSCO_SUBSIDIZED_EXECUTORS` | Executor subsidy hint for swarm routing. Use only when cost accounting policy requires it. |
| `GRAPHSUB_HOST`, `GRAPHSUB_TENANT_ID`, `GRAPHSUB_API_KEY` | GraphSub client settings. Treat API key as a secret. |

### Tools, Browser, MCP, and Integrations

| Env var | Guideline |
|---|---|
| `DSCO_TOOL_EMBEDDINGS_FILE` | Override tool embedding file path. Use for custom installs or testing regenerated embeddings. |
| `DSCO_TOOL_DEFAULT_TIMEOUT`, `DSCO_TOOL_GRACE_PERIOD_S` | Tool timeout controls. Lower in CI; raise for known long-running tools. |
| `DSCO_CONTEXT_OFFLOAD_BYTES` | Context offload threshold. Tune for large documents or memory pressure. |
| `DSCO_BROWSER_HOST_DB`, `DSCO_BROWSER_HOST_FLUSH_SEC`, `DSCO_BROWSER_MAX_PASSES` | Browser host cache/control settings. Defaults are suitable for normal browsing. |
| `DSCO_MCP_TIMEOUT_MS`, `DSCO_MCP_SYNC`, `DSCO_MCP_IMPORT_CLAUDE_DESKTOP` | MCP timing/import behavior. Use longer timeouts for slow MCP servers; import only from trusted local config. |
| `DSCO_SANDBOX_FORCE_NO_DOCKER` | Forces non-Docker sandbox fallback. Use on hosts without Docker or to reproduce fallback behavior. |
| `DSCO_QUORUM_GATE` | Tool quorum gate. Use for experiments with tool-loading policy. |
| `JINA_API_KEY`, `TAVILY_API_KEY`, `BRAVE_API_KEY` | Web/search integrations. Set only for tools that need those providers. |
| `GITHUB_TOKEN` | GitHub integration token. Scope narrowly; avoid broad personal tokens in shared shells. |
| `SUPABASE_URL`, `SUPABASE_API_KEY`, `DISCORD_TOKEN`, `TWILIO_ACCOUNT_SID`, `TWILIO_FROM_NUMBER` | Integration credentials/settings. Treat secrets as deployment-specific, not global developer defaults. |
| `KALSHI_API_KEY`, `KALSHI_RSA_PRIVATE_KEY_PATH`, `POLYMARKET_*`, `POLY_BUILDER_*` | Trading/prediction-market credentials. Use dedicated low-privilege accounts and never enable in unrelated development shells. |
| `TOOLS_API_URL`, `TOOL_MANAGEMENT_API_URL`, `TOOLS_API_TOKEN`, `AUTH_TOKEN`, `TOOLS_API_TIMEOUT`, `TOOLS_API_RETRIES`, `TOOLS_API_DEBUG` | External tool-management API settings. Treat tokens as secrets; enable debug only for repros. |

### UI and Terminal Behavior

| Env var | Guideline |
|---|---|
| `DSCO_NO_AUTO_INTERACTIVE` | Truthy makes bare `dsco` fail instead of entering interactive mode. Good for scripts. |
| `DSCO_NO_CLEAR` | Disables initial terminal clear. Use when embedding or logging. |
| `DSCO_NO_PANE` | Disables the interactive side pane. Use for plain logs or terminals with layout issues. |
| `DSCO_TUI_DSR` | Opts into terminal cursor-position DSR queries (`ESC[6n`). Leave unset unless debugging prompt placement on a known-good terminal. |
| `DSCO_SPLASH` | Splash control. Disable in scripts and screenshots. |
| `DSCO_GLYPH` | Glyph tier override, such as `ascii`, `unicode`, or `full`. Use for terminal compatibility. |
| `DSCO_NERD_FONT`, `NERD_FONT` | Enables Nerd Font UI assumptions. Set only when the terminal font supports it. |
| `DSCO_HYPERLINKS` | Terminal hyperlink override. Set to force-enable/disable clickable links. |
| `NO_COLOR`, `TERM`, `COLORTERM`, `COLORFGBG`, `TERM_BACKGROUND` | Standard terminal variables consumed for rendering. Prefer leaving these to the terminal. |

### Failure-Mode Playbooks

#### Provider is pinned but credentials are missing

Symptom:

```text
error: credentials not set for provider 'sakana'
```

Checks:

```bash
./dsco --setup-report
DSCO_DEBUG_AUTH=1 ./dsco -e sakana 'ping'
env | sort | grep -E '^(DSCO_EXEC|DSCO_MODEL|FUGU|SAKANA|FISH)='
```

Fixes:

- For Fugu/Sakana, set `FUGU_API_KEY` or clear the provider default.
- Use `env -u DSCO_EXEC -u DSCO_MODEL ./dsco ...` to prove whether the profile
  default is the cause.
- Keep `DSCO_EXEC=sakana` only in profiles that also provide a Sakana credential.

The supervisor treats this as a configuration exit, not a crash, so it should
not restart-loop. If it does, verify the binary contains `DSCO_EXIT_CONFIG`
handling and that an older hotswap binary is not being launched.

#### Routing went to the wrong provider

Use `DSCO_DEBUG_AUTH=1` first. Then inspect, in order:

1. CLI flags: `-m`, `-e`, `--provider`, `--local`, `-k`.
2. `DSCO_EXEC` and `DSCO_MODEL` from the live environment.
3. Values loaded from `DSCO_ENV_FILE` or the default setup env file.
4. Provider-specific key aliases that may select a provider implicitly.
5. OpenRouter policy vars such as `DSCO_OR_PROVIDER_ONLY` or
   `DSCO_OR_PROVIDER_IGNORE`.

For a clean comparison:

```bash
env -u DSCO_EXEC -u DSCO_MODEL -u DSCO_OR_PROVIDER_ONLY -u DSCO_OR_PROVIDER_IGNORE \
  DSCO_DEBUG_AUTH=1 ./dsco 'one sentence smoke test'
```

#### Secure store or keychain blocks startup

Use these only for the immediate repro:

```bash
DSCO_SECURE_STORE_AUTH_UI=1 ./dsco --setup-report
DSCO_SECURE_STORE_TIMEOUT_MS=60000 ./dsco --setup-report
DSCO_SECURE_STORE_NO_PROMPT=1 ./dsco --version
```

Do not solve keychain failures by globally disabling prompts unless the process
is intentionally noninteractive. CI should use explicit `DSCO_ENV_FILE` values
or secret-manager injection instead.

#### Supervisor behavior is hard to reason about

Use temporary tuning, not persistent shell defaults:

```bash
DSCO_SUPERVISE_MAX_RESTARTS=1 \
DSCO_SUPERVISE_BACKOFF_MS=10 \
DSCO_SUPERVISE_BACKOFF_MAX_MS=20 \
./dsco supervise -i
```

For startup debugging, disable auto-supervision but keep the normal process:

```bash
DSCO_NO_AUTO_SUPERVISE=1 ./dsco
```

For child process debugging, avoid setting `DSCO_NO_SUPERVISE` globally. That
variable is part of the supervisor-child contract.

#### CI should fail, not become interactive

Recommended CI baseline:

```bash
DSCO_NO_AUTO_INTERACTIVE=1 \
DSCO_NO_AUTO_SUPERVISE=1 \
DSCO_SECURE_STORE_NO_PROMPT=1 \
DSCO_ENV_FILE="$PWD/.ci/dsco.env" \
./dsco --version
```

Provider smoke jobs should pin both route and credential source:

```bash
DSCO_NO_AUTO_INTERACTIVE=1 \
DSCO_NO_AUTO_SUPERVISE=1 \
FUGU_API_KEY="$FUGU_API_KEY" \
./dsco -e fugu -m fugu 'Reply with OK.'
```

### Anti-Patterns

Avoid these patterns unless the test explicitly requires them:

- Persisting `DSCO_SUPERVISE_RESTART=permanent` in shell rc files.
- Setting `DSCO_EXEC` to a paid provider without also configuring that provider's
  credential.
- Exporting both a provider API key and a subscription OAuth override while
  debugging unrelated routing behavior.
- Leaving `DSCO_DEBUG_REQUEST=1` enabled across normal work.
- Putting trading credentials in a general development shell.
- Copying generated `DSCO_SUPERVISED` or `DSCO_RESUME_AFTER_CRASH` values from a
  child process into a repro command.
- Editing `docs/CONSTANTS_ENV_INDEX.md` by hand. Regenerate it from the script.

### Change-Control Checklist

When adding or changing an env var:

1. Decide whether it is user-facing, diagnostic, internal, or secret.
2. Prefer typed helpers from `env_config.c` for booleans, integers, sizes, and
   doubles.
3. Define default, min, max, and invalid-value behavior explicitly.
4. Document whether the var may be persisted.
5. If it is secret-like, add alias normalization and sealed-store handling where
   appropriate.
6. Add it to setup/reporting only if a user should manage it directly.
7. Add a failure-mode test or smoke command if it affects startup, routing, or
   supervision.
8. Regenerate generated env indexes instead of hand-editing them.

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

For Claude models, provider selection prefers Anthropic when either of these are available:

- `ANTHROPIC_API_KEY`
- Claude Code OAuth credentials discovered from env, Claude Code Keychain, or Claude Code credentials files

If both Anthropic auth modes are available, `dsco` prefers Claude Code OAuth over an ambient `ANTHROPIC_API_KEY`. Use `-k` to force a specific key for a request, or set `DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY=1` to force API-key-only Anthropic behavior. Only when Anthropic credentials are unavailable does routing fall back to OpenRouter.

### Claude Code Subscription Mode

When an Anthropic request uses Claude Code subscription credentials, `dsco` does all of the following automatically:

- Sends `Authorization: Bearer <Claude Code access token>` instead of `x-api-key`
- Adds the Claude Code OAuth beta header
- Inserts the `x-anthropic-billing-header` system block as the first system entry
- Computes the billing header from the first user message and Claude Code version

Discovery order:

1. `DSCO_CLAUDE_CODE_OAUTH_TOKEN`
2. `CLAUDE_CODE_OAUTH_TOKEN`
3. macOS Keychain entry for Claude Code credentials
4. `DSCO_CLAUDE_CODE_CREDENTIALS_FILE`
5. `CLAUDE_CONFIG_DIR/.credentials.json`
6. `~/.claude/.credentials.json`

For debugging provider/auth selection:

```bash
DSCO_DEBUG_AUTH=1 ./dsco -m claude-sonnet-4-6 "say hi"
```

Expected Anthropic subscription path:

```text
[auth] provider=anthropic model=claude-sonnet-4-6 auth=claude-code-oauth
```

## Troubleshooting

### Anthropic credentials missing

- Export `ANTHROPIC_API_KEY`, or sign into Claude Code and let `dsco` discover the Claude Code OAuth token.
- Use `DSCO_DEBUG_AUTH=1` to confirm whether requests are using `anthropic` or falling back to another provider.
- If Claude Code discovery should be disabled for testing, set `DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY=1`.

### Claude models unexpectedly use OpenRouter

- Unset `OPENROUTER_API_KEY` while testing Anthropic routing.
- Run with `DSCO_DEBUG_AUTH=1` and confirm:
  - `provider=anthropic`
  - `auth=claude-code-oauth` or `auth=anthropic-api-key`
- If you see `provider=openrouter`, Anthropic credentials were not available for that process.

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
