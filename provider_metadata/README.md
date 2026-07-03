# Provider Machinery Metadata

DSCO's durable catalog of provider/client mechanics so we don't fall behind provider machinery: prompt caching, retries, streaming, wire APIs, tool calling, usage accounting, auth headers, context caching, reasoning effort, endpoint-family coverage, cost-management levers, and SDK quirks.

This is a behavior catalog, distinct from the model catalog.

## Layout

```
provider_metadata/
  SCHEMA.md                 # record schema + status vocabulary + governance rules
  README.md                 # this file
  providers/<name>.json     # one record per provider (docs-backed, provenance required)
  reports/AUDIT.json        # latest validator output
```

## Commands

```bash
# validate + summary table
scripts/provider_metadata_audit.py

# only gaps + high-risk providers
scripts/provider_metadata_audit.py --gaps

# cross-check metadata claims against src/provider.c implementation signals
scripts/provider_metadata_audit.py --check-impl

# machine-readable
scripts/provider_metadata_audit.py --json

# surface from the binary, including which providers have coverage/cost maps
DSCO_REPO_ROOT="$PWD" dsco config metadata
```

## Enforcement

Two tests bind this catalog to the runtime (tests/test.c):

- `test_provider_metadata_catalog_validates` — runs the audit; fails on validation errors.
- `test_provider_metadata_impl_matches_code` — asserts `provider_model_supports_*` matches the catalog's documented mechanisms (cache_control / prompt_cache_key / prompt_cache_retention / automatic).

If you add a provider mechanism in code, update the metadata record and the binding test, or CI fails.

## Current prompt-cache status (2026-07-02)

| Provider | Status | Mechanism | DSCO |
|---|---|---|---|
| anthropic | explicit | cache_control ephemeral | implemented |
| openai | explicit | automatic + prompt_cache_key + prompt_cache_retention | implemented |
| openrouter | explicit | forwarded cache_control (claude/qwen) | implemented |
| mistral | explicit | prompt_cache_key | implemented |
| cerebras | explicit | prompt_cache_key | implemented |
| xai | automatic | x-grok-conv-id header | implemented |
| groq | automatic | provider-managed prefix cache | policy recognized |
| deepseek | automatic | disk context cache (hit/miss usage) | policy recognized |
| google | resource | cachedContent resource lifecycle | PLANNED (high risk) |
| sakana | unknown | none documented | n/a |
| cohere/perplexity/together/moonshot | unknown | needs doc refresh | high risk |

## Refresh workflow

1. Pull the provider's current prompt-caching / API-reference / streaming / pricing docs.
2. Update `providers/<name>.json`: docs[] (url + retrieved date + confidence), API coverage, cost-management levers, mechanisms, usage fields, streaming, quirks, `last_reviewed`.
3. Run `scripts/provider_metadata_audit.py --check-impl`.
4. If you mark a mechanism `implemented`, add/adjust a request-shape or header test.
5. `make test`.

## OpenAI coverage and cost controls (2026-07-02)

OpenAI is tracked beyond chat transport now. The catalog records Responses, Chat Completions, legacy Completions, Realtime, Batch, Audio, Images, Videos, Embeddings, Moderations, Files, Uploads, Vector Stores, Containers, Evals/Graders, Fine-tuning, Models, Conversations, Assistants beta, Administration, Webhooks, and Workload Identity.

The OpenAI cost map separates raw usage from pricing application. It tracks prompt caching, Batch API routing, Flex processing, `/responses/input_tokens` preflight counting, Realtime usage events, hosted tool metering, media endpoint metering, storage/session meters, and admin usage reconciliation.

## Known gaps (tracked, not hidden)

- OpenAI Responses native transport, Batch JSONL planning, input-token preflight, hosted tool/session/storage ledgering, Realtime usage import, and admin usage reconciliation are metadata-backed but not fully implemented.
- Gemini native `cachedContent` resource lifecycle is not implemented (only OpenAI-compat chat path).
- DeepSeek/Groq automatic cache: usage hit/miss not yet mapped into DSCO `cache_read_tokens` display.
- cohere / perplexity / together / moonshot need a documentation pass before emitting any cache fields.
- OpenRouter per-model cache capability should be ingested from `/models` rather than pattern-matched.
