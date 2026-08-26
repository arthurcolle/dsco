# Adaptive local inference

The adaptive endpoint keeps the existing Qwen3.5 2B and Qwythos 9B models
resident and routes each OpenAI-compatible chat request to the appropriate
lane. This replaces slow cross-model speculative decoding with a cascade:

- `fast`: Qwen3.5 2B, reasoning off, two continuous-batching slots.
- `smart`: Qwythos 9B Q4_K_M, bounded reasoning on, 16K context.
- `auto`: a scored policy that keeps short/simple prompts fast—even when DSCO
  attaches its tool catalog—and selects smart for code, reasoning, high-stakes
  work, multimodal inputs, forced tool calls, tool-result continuations,
  structured output, long requests, and combined complexity signals.

Both models together occupy about 8.2 GB, less than the former 9.9 GB Q8
target alone. On the local M4 Max, the 2B lane measured 109.48 decode tokens/s
and 921.59 prefill tokens/s. The 9B lane remains available for harder work.

On the actual DSCO system/tool envelope, the fast lane processed 13,788 prompt
tokens at 1,520.85 tokens/s and completed the model request in 9.12 seconds.
The smart lane processed the comparable 13,863-token envelope at 376.57
tokens/s and completed in 39.51 seconds. Automatic routing therefore cut this
simple request's model time by 77% while retaining 9B for difficult work.

The 2B model was also tested as a conventional speculative draft. For the
measured prompt it slowed end-to-end throughput from 65.88 to 30.56 tokens/s,
with 44 of 73 draft tokens accepted. The adaptive cascade uses both models
without paying that cross-model draft overhead on every generated token.

## Start

```sh
scripts/local-smart.sh start
```

Use `http://127.0.0.1:8080/v1` as the OpenAI-compatible base URL:

```sh
LLAMACPP_API_BASE=http://127.0.0.1:8080/v1 dsco -m llamacpp:auto "Hello"
```

Inspect or stop it with:

```sh
scripts/local-smart.sh status
scripts/local-smart.sh test
scripts/local-smart.sh route "compare these two architectures"
scripts/local-smart.sh stop
```

## Routing controls

Set the request's `model` to `auto`, `fast`, or `smart`. For an explicit
transport-level override, send `X-DSCO-Lane: fast` or `X-DSCO-Lane: smart`.
The response reports the decision in `X-DSCO-Local-Lane`,
`X-DSCO-Route-Score`, and `X-DSCO-Route-Reason`. Scores of 4 or greater select
the smart lane. Explicit model/header choices remain authoritative.

`POST /v1/route` applies the same policy without running inference:

```json
{"lane":"smart","score":4,"reason":"reasoning-task","reasoning_budget":48,"forced":false}
```

The router respects explicit `chat_template_kwargs.enable_thinking`,
`cache_prompt`, and `reasoning_budget_tokens` values. Otherwise it enables
prompt caching, disables thinking on the fast lane, and assigns the smart lane
an adaptive 48/64/96-token reasoning budget based on complexity and the output
limit. Streaming SSE responses pass through unchanged. Automatic decisions can
fail over to the healthy lane if their preferred backend is unavailable;
explicit lane choices never silently change.

Environment controls include:

| Variable | Default | Purpose |
|---|---:|---|
| `DSCO_SMART_PORT` | `8080` | Public router port |
| `DSCO_SMART_FAST_PORT` | `8081` | Private 2B backend port |
| `DSCO_SMART_MODEL_PORT` | `8082` | Private 9B backend port |
| `DSCO_SMART_THRESHOLD` | `1800` | Last-user-message threshold for smart routing |
| `DSCO_SMART_FAST_SLOTS` | `2` | Concurrent fast-lane slots |
| `DSCO_SMART_FAST_CTX` | `8192` | Fast KV tokens budgeted per slot |
| `DSCO_SMART_CTX` | `16384` | Smart-lane context tokens |
| `DSCO_SMART_REASONING_BUDGET` | `64` | Default smart reasoning budget |

The C router binds only to loopback, uses the repository's vendored yyjson
parser, forwards both buffered and streaming chat completions, and exposes
combined health counters and model endpoints. It logs only lane, score, reason,
failover state, request byte count, and upstream status—not prompt contents.
