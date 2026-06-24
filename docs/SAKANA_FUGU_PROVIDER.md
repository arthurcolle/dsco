# Sakana Fugu Provider Integration

Status: core provider integrated.

## Provider

| Field | Value |
|---|---|
| Provider key | `sakana` |
| Aliases | `fugu`, `sakana-ai`, `sakanaai`, `fugu-ultra` |
| Base URL | `https://api.sakana.ai/v1` |
| Auth env | `FUGU_API_KEY` |
| Alias envs | `SAKANA_API_KEY`, `FISH_API_KEY`, `SAKANA_TOKEN` |
| Base override | `FUGU_BASE_URL` (preferred), `FUGU_API_BASE`, `SAKANA_API_BASE`, `SAKANA_BASE_URL` |
| Default model | `fugu` |
| Ultra model | `fugu-ultra` |
| Dated Ultra pin | `fugu-ultra-20260615` |
| Transport | OpenAI-compatible Chat Completions SSE |

## Usage

```bash
export FUGU_API_KEY='...'
export FUGU_BASE_URL='https://api.sakana.ai/v1'

./dsco -e fugu 'Say hello in one sentence.'
# or
DSCO_EXEC=fugu ./dsco 'Say hello in one sentence.'
# ultra / pinned ultra
./dsco -e fugu-ultra 'Solve this hard debugging task.'
./dsco --provider sakana -m fugu-ultra-20260615 'Solve this hard debugging task.'
```

Model-family routing also recognizes:

```text
fugu
fugu-ultra
fugu-ultra-20260615
sakana/fugu
sakana/fugu-ultra
sakana/fugu-ultra-20260615
```

## API map

### Models

| Model ID | dsco alias | Notes |
|---|---|---|
| `fugu` | `fugu`, `DSCO_EXEC=fugu` | Default model. Low-latency everyday route that delegates internally. |
| `fugu-ultra` | `fugu-ultra`, `DSCO_EXEC=fugu-ultra` | Higher-cost quality route over a deeper expert pool. |
| `fugu-ultra-20260615` | `fugu-ultra-20260615` | Dated Ultra pin for reproducibility. |

### Endpoints

| Endpoint | Status in dsco | Notes |
|---|---|---|
| `/v1/chat/completions` | Implemented | Current streaming transport. Uses `messages`, `stream`, `max_completion_tokens`, `reasoning`, tools, and `tool_choice`. |
| `/v1/responses` | API documented, not default transport yet | Recommended by Sakana for generation. Needed for built-in `web_search` and Responses-native multimodal/tool semantics. |
| `/v1/models` | OpenAI-compatible | Can list `fugu`, `fugu-ultra`, and `fugu-ultra-20260615`. |

### Responses API fields

| Field | dsco mapping |
|---|---|
| `model` | `session.model` / `-m` / `DSCO_EXEC=fugu*` |
| `input` | Conversation history when Responses transport is added |
| `instructions` | System/developer prompt when Responses transport is added |
| `metadata` | Pass-through candidate for future provider metadata |
| `stream` | `true` for streaming |
| `max_output_tokens` | `dsco_max_tokens()` |
| `reasoning.effort` | `high` or `xhigh`; `max` maps to `xhigh` |
| `tools` | Function tools; built-in `web_search` is Responses-only |
| `tool_choice` | `auto`, `none`, required/object forms as supported |
| `text.format` | Future structured-output mapping |
| `temperature` | Accepted by provider, ignored |
| `parallel_tool_calls` | Accepted by provider, ignored/server-controlled |
| `previous_response_id` | Not used; dsco sends full conversation history |

### Chat Completions fields

| Field | dsco mapping |
|---|---|
| `model` | `fugu`, `fugu-ultra`, or `fugu-ultra-20260615` |
| `messages` | Current conversation history |
| `metadata` | Pass-through candidate for future provider metadata |
| `stream` | `true` |
| `stream_options` | Only when usage inclusion is explicitly needed |
| `max_completion_tokens` | `dsco_max_tokens()` |
| `max_tokens` | Legacy; dsco avoids it for Fugu |
| `reasoning_effort` / `reasoning` | `reasoning: {"effort": "high"|"xhigh"}` |
| `tools` | OpenAI-compatible function tools |
| `tool_choice` | Current tool-choice policy |
| `response_format` | Future structured-output mapping |
| sampling fields | Accepted but ignored by provider; dsco should avoid setting them unless compatibility requires it |
| `parallel_tool_calls` | Accepted but ignored/server-controlled |

## Pricing encoded in model registry

Subscription-backed Fugu is treated as zero marginal cost for local routing and
budget gates. If PAYG pricing is enabled later, Fugu Ultra usage details must
include both visible and orchestration tokens.

Known PAYG standard tier for `fugu-ultra-20260615`:

| Token type | Price / 1M |
|---|---:|
| Input | $5.00 |
| Output | $30.00 |
| Cached input | $0.50 |

The provider has a higher-price tier for context above 272K. The local registry caps Fugu Ultra aliases at `272000` context until dynamic tiered cost accounting is implemented, avoiding silent under-estimation.

## Fugu-specific behavior

### Reasoning effort

Fugu Chat Completions accepts:

```json
{"reasoning":{"effort":"high"}}
{"reasoning":{"effort":"xhigh"}}
```

DSCO remaps session effort:

| DSCO effort | Fugu effort |
|---|---|
| unset | `high` |
| `low` | `high` |
| `medium` | `high` |
| `high` | `high` |
| `max` | `xhigh` |
| `xhigh` | `xhigh` |

### Orchestration token accounting

Fugu Ultra reports orchestration tokens in detail fields. Unlike OpenAI, these are real billable tokens outside visible input/output counts.

DSCO now parses:

```json
{
  "usage": {
    "input_tokens": 120,
    "output_tokens": 80,
    "input_tokens_details": {
      "cached_tokens": 0,
      "orchestration_input_tokens": 0,
      "orchestration_input_cached_tokens": 0
    },
    "output_tokens_details": {
      "orchestration_output_tokens": 0
    }
  }
}
```

Accounting behavior:

- `input_tokens` or `prompt_tokens` -> `usage.input_tokens`
- `output_tokens` or `completion_tokens` -> `usage.output_tokens`
- `orchestration_input_tokens` -> added to `usage.input_tokens`
- `orchestration_input_cached_tokens` -> added to `usage.cache_read_input_tokens`
- `orchestration_output_tokens` -> added to `usage.output_tokens`

### Built-in tools

Fugu built-in tools such as `web_search` are Responses API tools. They should
not be sent through the current Chat Completions function-tool path until dsco
has a Sakana Responses transport.

## Smoke test

```bash
./scripts/smoke_sakana_fugu.sh
```

Expected:

- exits 0
- prints provider/model output
- no `HTTP 400` reasoning-effort error
- no missing-key error when `FUGU_API_KEY` or `SAKANA_API_KEY` is set

## Files touched

- `src/provider_profiles.c` — provider profile, aliases, caps
- `src/provider.c` — endpoint, auth aliasing, request builder, usage parser, model-family routing
- `include/config.h` — model registry and pricing
- `src/main.c` / `src/agent.c` — provider list/help integration
- `src/sealed_store.c` — allowlisted credential env
