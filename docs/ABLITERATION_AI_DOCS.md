# Abliteration.ai documentation index

Generated from [`llms.txt`](https://docs.abliteration.ai/llms.txt) at `2026-09-01T04:55:59+00:00`. Indexed 71 unique documents with 71 successful fetches.

This is a metadata/search index, not a mirror. It stores vendor summaries, headings, API paths, search symbols, and SHA-256 digests. Source licensing is not stated in the catalog; use remains limited to internal discovery, integration, citation, and drift detection.

## Provider contract

- Base URL: `https://api.abliteration.ai/v1`
- Credentials: `ABLITERATION_API_KEY` (preferred), `ABLIT_KEY` (compatible)
- Current models: `abliterated-model`, `abliterated-model-large-v2`, `abliterated-model-large`
- Chat: `/v1/chat/completions`; Responses: `/v1/responses`; Anthropic: `/v1/messages`
- Pricing: base model $3/M input and output; large models $5/M; cache reads 10% of input rate

## DSCO integration

The native provider is selected with `--provider abliteration-ai` (aliases: `abliteration`, `ablit`) and accepts either `ABLITERATION_API_KEY` or `ABLIT_KEY`.

DSCO uses Chat Completions by default because it covers the complete interactive agent loop plus provider-only video input. Set `DSCO_ABLITERATION_API=responses` for the native Responses lifecycle or `DSCO_ABLITERATION_API=anthropic` for the provider's Anthropic Messages transport.

### Automatic KV/prompt caching

Abliteration caching is automatic. DSCO sends a stable per-session `prompt_cache_key`, a documented `prompt_cache_retention` (`24h` by default, or `in_memory`), and requests final streaming usage so `prompt_tokens_details.cached_tokens` reaches DSCO's canonical cache accounting. Cache reads use the provider's published 10% input rate.

```sh
export ABLIT_KEY='ak_...'
export DSCO_PROMPT_CACHE_KEY='my-stable-session'
export DSCO_ABLITERATION_CACHE_RETENTION='24h' # or in_memory
dsco --provider abliteration-ai -m abliterated-model-large-v2
```

### Provider-specific request controls

Pass documented Chat Completions options without changing other OpenAI-compatible providers:

```sh
export DSCO_ABLITERATION_PARAMS='{
  "include_reasoning": false,
  "flagged_categories": ["harassment", "hate"],
  "cache_salt": "tenant-a"
}'
```

The provider's OpenAPI schema has `additionalProperties: false`; DSCO therefore forwards only fields accepted by the current Chat schema. Session `/web on` uses `web_search_options` and suppresses function tools for that request because the provider documents those two shapes as mutually exclusive. Optional tuning:

```sh
export DSCO_ABLITERATION_WEB_SEARCH_CONTEXT=high   # low|medium|high
export DSCO_ABLITERATION_WEB_SEARCH_LOCATION=us-east-1
# Anthropic Messages optional JSON arrays:
export DSCO_ABLITERATION_WEB_ALLOWED_DOMAINS='["sec.gov","nvidia.com"]'
export DSCO_ABLITERATION_WEB_BLOCKED_DOMAINS='["reddit.com"]'
```

### Policy Gateway

```sh
export DSCO_ABLITERATION_POLICY_GATEWAY=1
export DSCO_ABLITERATION_POLICY_PROJECT=proj_support_bot
export DSCO_ABLITERATION_POLICY_TARGET=dsco-cli
export DSCO_ABLITERATION_POLICY_USER=arthur
```

This switches the selected surface to its matching `/policy/*` endpoint and sends the three documented `X-Policy-*` attribution headers. It requires a provider Policy Gateway plan.

### Model-aware media

`abliterated-model-large-v2` and `abliterated-model-large` are text-only. If a DSCO turn contains an image or video while a large model is selected, the request is routed to `abliterated-model`, avoiding a guaranteed provider `400`. Images use OpenAI `image_url`; videos use provider-native `video_url` and are available only on Chat Completions.

### Exact token counting

The C provider API exposes `provider_count_tokens("abliteration-ai", key, request_json)`, backed by `POST /v1/messages/count_tokens`.

## Api

- [Use the Anthropic SDK with abliteration.ai](https://docs.abliteration.ai/api/anthropic-compatibility.md) — How to use abliteration.ai with the Anthropic Messages API — base URL, auth token, and what's supported. (`826c65ecf5c4`, 383 words)
- [abliteration.ai API errors](https://docs.abliteration.ai/api/errors.md) — HTTP error codes, error body shapes (OpenAI and Anthropic), and how to handle policy-blocked requests. (`10e90119706a`, 289 words)
- [Introduction](https://docs.abliteration.ai/api/introduction.md) — Overview of abliteration.ai's HTTP APIs — base URL, authentication, and how the OpenAI-compatible and Anthropic-compatible surfaces are exposed. (`c151c466b353`, 199 words)
- [Use any OpenAI SDK with abliteration.ai](https://docs.abliteration.ai/api/openai-compatibility.md) — How to use abliteration.ai with any OpenAI SDK — base URL, authentication, supported features, and caveats. (`425c61377213`, 468 words)
- [Policy endpoints](https://docs.abliteration.ai/api/policy-endpoints.md) — The /policy/* API surface that adds project quotas, policy evaluation, streaming metadata, and audit events. (`65cb9f3ebe12`, 519 words)

## Api Reference

- [Create an OpenAI-compatible chat completion](https://docs.abliteration.ai/api-reference/chat-completions/create-an-openai-compatible-chat-completion.md) — No catalog summary. (`54f2a3612c70`, 1859 words)
- [Count Anthropic Messages input tokens](https://docs.abliteration.ai/api-reference/messages/count-anthropic-messages-input-tokens.md) — No catalog summary. (`21f4577cc888`, 889 words)
- [Create an Anthropic Messages-compatible response](https://docs.abliteration.ai/api-reference/messages/create-an-anthropic-messages-compatible-response.md) — No catalog summary. (`5320cf39db12`, 1128 words)
- [List models available to the API key project](https://docs.abliteration.ai/api-reference/models/list-models-available-to-the-api-key-project.md) — No catalog summary. (`1cb4aa47e179`, 728 words)
- [Create a stateless OpenAI Responses-compatible response](https://docs.abliteration.ai/api-reference/responses/create-a-stateless-openai-responses-compatible-response.md) — No catalog summary. (`4982d0d3b904`, 1276 words)

## Capabilities

- [Count input tokens before sending](https://docs.abliteration.ai/capabilities/count-tokens.md) — Measure the exact input-token cost of a request before sending it, via POST /v1/messages/count_tokens. (`a6702c245ab1`, 227 words)
- [Send images to abliteration.ai](https://docs.abliteration.ai/capabilities/images.md) — Send images alongside text on the OpenAI Chat Completions and Anthropic Messages surfaces. (`9b2cb47dcc41`, 245 words)
- [Request safety filtering](https://docs.abliteration.ai/capabilities/request-safety-filtering.md) — Reject requests whose content matches moderation categories you choose — per request, with no policy setup, via the flagged_categories parameter. (`eabc63cd01d9`, 471 words)
- [Streaming policy metadata](https://docs.abliteration.ai/capabilities/streaming-policy-metadata.md) — On `/policy/*` endpoints, abliteration.ai injects a policy object into every streaming frame so clients can render compliance UI. (`f68a659e295f`, 348 words)
- [Stream tokens from abliteration.ai](https://docs.abliteration.ai/capabilities/streaming.md) — Stream abliteration.ai responses as server-sent events to reduce time-to-first-token. (`cdb130aa2fdf`, 307 words)
- [Thinking and reasoning effort](https://docs.abliteration.ai/capabilities/thinking.md) — All abliteration.ai models reason before answering. Tune depth, disable reasoning where supported, or hide the trace across Chat Completions, Responses, and Anthropic Messages. (`a6f0a48b30d1`, 811 words)
- [Tool calling on abliteration.ai](https://docs.abliteration.ai/capabilities/tool-calling.md) — Function calling on abliteration.ai across all three API surfaces. Pick the format that matches your client. (`6b292d9feb93`, 212 words)
- [Anthropic Messages](https://docs.abliteration.ai/capabilities/tool-calling/anthropic-messages.md) — Tool calling on /v1/messages — input_schema definition, tool_use response blocks, tool_result continuation blocks. (`e306df9b9eea`, 260 words)
- [OpenAI Chat Completions](https://docs.abliteration.ai/capabilities/tool-calling/openai-chat-completions.md) — Tool calling on /v1/chat/completions — define tools, handle the call/execute/continue loop, and stream tool arguments. (`46ffb624e5ea`, 257 words)
- [OpenAI Responses API](https://docs.abliteration.ai/capabilities/tool-calling/openai-responses.md) — Tool calling on /v1/responses — flat function schema, function_call items in output[], function_call_output continuation. (`d643429dd968`, 234 words)
- [Send video to abliteration.ai](https://docs.abliteration.ai/capabilities/video.md) — Send short video clips alongside text on the OpenAI Chat Completions surface, either base64-inlined or as an HTTPS URL. (`decbf06bc428`, 237 words)
- [Web fetch on abliteration.ai](https://docs.abliteration.ai/capabilities/web-fetch.md) — Server-side web fetch on abliteration.ai — let the model pull and read a specific URL via the Anthropic Messages API. (`0c63b0606565`, 201 words)
- [Web search on abliteration.ai](https://docs.abliteration.ai/capabilities/web-search.md) — Enable web search on abliteration.ai across OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages — citations included. (`5a5ba69276f6`, 524 words)
- [Anthropic Messages](https://docs.abliteration.ai/capabilities/web-search/anthropic-messages.md) — Server-side web search on /v1/messages via the tools array, with native allowed_domains and blocked_domains at the tool's top level. (`863d45c29a52`, 362 words)
- [OpenAI Chat Completions](https://docs.abliteration.ai/capabilities/web-search/openai-chat-completions.md) — Server-side web search on /v1/chat/completions via the web_search_options request field. (`dd15365b910e`, 240 words)
- [OpenAI Responses API](https://docs.abliteration.ai/capabilities/web-search/openai-responses.md) — Server-side web search on /v1/responses via the tools array, with OpenAI's native tools_options.web_search.filters.include_domains allow list. (`3f670896463f`, 313 words)

## Getting Started Reference

- [How to authenticate with abliteration.ai](https://docs.abliteration.ai/authentication.md) — How to authenticate abliteration.ai API requests with bearer tokens, scope keys to projects, and rotate keys safely. (`495a45a97b79`, 211 words)
- [Compatibility matrix](https://docs.abliteration.ai/compatibility-matrix.md) — What abliteration.ai supports across OpenAI Chat Completions, Responses, Anthropic Messages, and Count Tokens surfaces. (`e9e7da1b9677`, 316 words)
- [FAQ](https://docs.abliteration.ai/faq.md) — Frequently asked questions about abliteration.ai — compatibility, models, retention, and the Policy Gateway. (`57acee1f9bf1`, 919 words)
- [abliteration.ai documentation](https://docs.abliteration.ai/index.md) — abliteration.ai is an inference API for unrestricted, uncensored models, compatible with the OpenAI and Anthropic SDKs, with a built-in policy gateway. (`c7840abf2b59`, 320 words)
- [abliteration.ai models](https://docs.abliteration.ai/models.md) — abliteration.ai serves three unrestricted reasoning models: abliterated-model (multimodal, 256K context), abliterated-model-large-v2 (text-only, 1M context, GLM-5.3), and abliterated-model-large (text-only, 1M context, GLM-5.2). (`be9e82c1c7a6`, 396 words)
- [Pricing](https://docs.abliteration.ai/pricing.md) — Per-token API pricing for abliteration.ai models. Input and output are billed at a flat per-model rate; cached input (prompt-cache reads) is billed at 10%. (`f6982931d4ae`, 313 words)
- [abliteration.ai quickstart](https://docs.abliteration.ai/quickstart.md) — Get your first abliteration.ai request working in under a minute — grab a key, hit /v1/chat/completions, stream tokens. (`98870e537162`, 293 words)
- [Rate limits](https://docs.abliteration.ai/rate-limits.md) — Request and token rate limits for the abliteration.ai API. Your limits are set by your tier — the higher of your subscription plan and the spend tier you earn from lifetime usage. (`afa71130187d`, 1324 words)
- [What is abliteration?](https://docs.abliteration.ai/what-is-abliteration.md) — Abliteration is a weight-modification technique that removes the refusal direction from an open-weight LLM, producing an unrestricted model that responds to prompts the original would refuse. (`5e6c8901020b`, 634 words)

## Integrations

- [Use CC Switch with abliteration.ai](https://docs.abliteration.ai/integrations/cc-switch.md) — Add abliteration.ai as a provider in CC Switch and point Claude Code or Codex at its Anthropic- and OpenAI-compatible API in one click. (`f3a14d5a9ce9`, 495 words)
- [Use Claude Code with abliteration.ai](https://docs.abliteration.ai/integrations/claude-code.md) — Route Anthropic's Claude Code CLI through abliteration.ai by setting two auth variables and picking how to surface abliterated-model. (`88b1bbc3af7a`, 655 words)
- [Use Claude Cowork with abliteration.ai](https://docs.abliteration.ai/integrations/claude-cowork.md) — Configure Claude Cowork on 3P (Claude Desktop's third-party inference mode) to route through abliteration.ai. (`6d917e8f6312`, 694 words)
- [Use CLIProxyAPI with abliteration.ai](https://docs.abliteration.ai/integrations/cli-proxy-api.md) — Route CLIProxyAPI to abliteration.ai's OpenAI-compatible and Anthropic APIs with a copy-paste config.yaml, then point Claude Code or Codex at the local proxy. (`2bc4915d297d`, 524 words)
- [Cloudflare Workers](https://docs.abliteration.ai/integrations/cloudflare-workers.md) — Call abliteration.ai from any Cloudflare Worker with fetch — no SDK required. (`548971399ee8`, 176 words)
- [Use OpenAI Codex with abliteration.ai](https://docs.abliteration.ai/integrations/codex.md) — Use OpenAI's Codex CLI with abliteration.ai by registering a custom provider in ~/.codex/config.toml. (`d24ea016738d`, 437 words)
- [Use CyberStrike with abliteration.ai](https://docs.abliteration.ai/integrations/cyberstrike.md) — Use the CyberStrike offensive security agent with abliteration.ai — CyberStrike is a fork of OpenCode, so authenticate, pick Abliterated Model Large V2, and choose a reasoning level. (`70309aad7712`, 370 words)
- [Giskard](https://docs.abliteration.ai/integrations/giskard.md) — Red-team your AI agents with Giskard's agent vulnerability scanner using abliteration.ai as the LLM backend. (`1cdd6c3c7810`, 739 words)
- [Use Hermes Agent with abliteration.ai](https://docs.abliteration.ai/integrations/hermes.md) — Use Hermes Agent with abliteration.ai through Hermes' custom OpenAI-compatible provider or a small model-provider plugin. (`804f3808d6ae`, 498 words)
- [LangChain](https://docs.abliteration.ai/integrations/langchain.md) — Drop-in abliteration.ai with LangChain and LangGraph via the standard ChatOpenAI class. (`bc5dee6dd0e6`, 174 words)
- [LlamaIndex](https://docs.abliteration.ai/integrations/llamaindex.md) — Use abliteration.ai as the LLM in LlamaIndex pipelines via the OpenAILike adapter. (`3dd17f9c8db1`, 113 words)
- [Node / TypeScript](https://docs.abliteration.ai/integrations/node.md) — Use the official OpenAI or Anthropic JavaScript SDKs with abliteration.ai in Node, Edge, and Bun runtimes. (`f657d1b4bead`, 222 words)
- [Use OpenClaw with abliteration.ai](https://docs.abliteration.ai/integrations/openclaw.md) — Use OpenClaw (open-source agent framework, 100+ skills) with abliteration.ai. (`44684b999788`, 149 words)
- [Use OpenCode with abliteration.ai](https://docs.abliteration.ai/integrations/opencode.md) — Use the OpenCode terminal agent with abliteration.ai — abliteration.ai is a built-in provider, so authenticate, pick Abliterated Model Large V2, and choose a reasoning level. (`c3b403076293`, 350 words)
- [Promptfoo](https://docs.abliteration.ai/integrations/promptfoo.md) — Evaluate and red-team abliteration.ai models with promptfoo using the built-in abliteration provider. (`397e110069a0`, 795 words)
- [Python](https://docs.abliteration.ai/integrations/python.md) — Use the official OpenAI or Anthropic Python SDKs with abliteration.ai by changing the base URL. (`8b8b8496c712`, 182 words)
- [Vercel AI SDK](https://docs.abliteration.ai/integrations/vercel-ai-sdk.md) — Wire abliteration.ai into the Vercel AI SDK using the OpenAI-compatible provider. (`d0831a4a1dc5`, 158 words)

## Openapi

- [openapi](https://api.abliteration.ai/openapi.json) — No catalog summary. (`1e470a72ae9d`, 4832 words)
- [openapi](https://docs.abliteration.ai/api-reference/openapi.json) — No catalog summary. (`919eb7a392aa`, 319 words)

## Policy Gateway

- [Connectors](https://docs.abliteration.ai/policy-gateway/connectors.md) — Stream Policy Gateway events to your SIEM, log pipeline, or data lake. Thirteen destinations available. (`b488f3ef57f9`, 554 words)
- [Integration](https://docs.abliteration.ai/policy-gateway/integration.md) — Wire Policy Gateway into your request flow and read policy decisions on the client. (`bebac1da1ee8`, 199 words)
- [Onboarding](https://docs.abliteration.ai/policy-gateway/onboarding.md) — Set up your first policy, project, and scoped key on Policy Gateway in six steps. (`58fa47cda2f5`, 378 words)
- [Policy Gateway overview](https://docs.abliteration.ai/policy-gateway/overview.md) — Policy Gateway is abliteration.ai's governance layer — rules, rollout modes, and audit events for every request. (`649e76d4fa4c`, 631 words)
- [Azure Blob Storage](https://docs.abliteration.ai/policy-gateway/policy-logs/azure-blob.md) — Archive Policy Gateway events in an Azure Blob Storage container. (`063742cd4299`, 122 words)
- [Azure Monitor](https://docs.abliteration.ai/policy-gateway/policy-logs/azure-monitor.md) — Send Policy Gateway events to an Azure Log Analytics workspace via the Data Collector API. (`2c243806d3a2`, 199 words)
- [Backblaze B2](https://docs.abliteration.ai/policy-gateway/policy-logs/backblaze-b2.md) — Archive Policy Gateway events in a Backblaze B2 bucket via the S3-compatible API. (`8eb77b0ea992`, 129 words)
- [Cloudflare R2](https://docs.abliteration.ai/policy-gateway/policy-logs/cloudflare-r2.md) — Archive Policy Gateway events in Cloudflare R2 with zero egress fees. (`d6d88aec797f`, 130 words)
- [Datadog Logs](https://docs.abliteration.ai/policy-gateway/policy-logs/datadog.md) — Stream Policy Gateway events to Datadog Logs via the intake API. (`66d10b3d36d1`, 186 words)
- [Elastic / OpenSearch](https://docs.abliteration.ai/policy-gateway/policy-logs/elastic.md) — Index Policy Gateway events into Elasticsearch or Elastic Cloud via the Bulk API. (`5767d04cf69f`, 217 words)
- [Google Cloud Storage](https://docs.abliteration.ai/policy-gateway/policy-logs/gcs.md) — Archive Policy Gateway events in Google Cloud Storage via S3-compatible HMAC keys. (`1a386dfdf85b`, 148 words)
- [HTTP Webhook](https://docs.abliteration.ai/policy-gateway/policy-logs/http.md) — POST Policy Gateway events as NDJSON to any HTTPS endpoint — Slack, PagerDuty, internal services, Zapier. (`e92df7399da4`, 168 words)
- [OpenTelemetry](https://docs.abliteration.ai/policy-gateway/policy-logs/otel.md) — Export Policy Gateway events as OTLP log records over HTTP or gRPC to any OpenTelemetry collector. (`7cc57a740877`, 265 words)
- [S3-Compatible](https://docs.abliteration.ai/policy-gateway/policy-logs/s3-compatible.md) — Archive Policy Gateway events in any S3-protocol storage — MinIO, Wasabi, DigitalOcean Spaces. (`37c08a04ff80`, 175 words)
- [Amazon S3](https://docs.abliteration.ai/policy-gateway/policy-logs/s3.md) — Archive Policy Gateway events in an Amazon S3 bucket as JSON objects for long-term retention. (`b74d1fb71257`, 209 words)
- [Splunk HEC](https://docs.abliteration.ai/policy-gateway/policy-logs/splunk-hec.md) — Stream Policy Gateway events to Splunk via HTTP Event Collector. (`169ace5a98e0`, 179 words)
- [Security](https://docs.abliteration.ai/policy-gateway/security.md) — How Policy Gateway handles your data — retention, encryption, access control. (`e9d4af0c26b2`, 217 words)

## Provenance and refresh

- Canonical catalog digest: `sha256:aebf42d58647907c7c2592a1e2c0acf65bb920b409c7ee320b73d93263d821f2`
- Owner: Abliteration AI
- License/terms version: not stated in `llms.txt`; verify before redistribution
- Refresh command: `python3 scripts/sync_abliteration_docs.py`
- Drift rule: any changed document digest requires integration review for models, auth, endpoints, request shapes, pricing, caching, rate limits, and error semantics.
