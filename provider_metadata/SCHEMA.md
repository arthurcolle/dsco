# DSCO Provider Machinery Metadata Schema

Purpose: keep DSCO ahead of provider/client mechanics: prompt caching, retries, streaming, wire APIs, tool calling, usage accounting, auth, beta headers, context caching, rate limits, and client SDK quirks.

This is not a model catalog. It is a provider-behavior catalog.

## Record shape

Each provider record is JSON under `provider_metadata/providers/<provider>.json`.

```json
{
  "provider": "openai",
  "display_name": "OpenAI",
  "last_reviewed": "2026-06-24",
  "confidence": 0.9,
  "docs": [
    {"title":"Prompt caching", "url":"https://...", "retrieved":"2026-06-24", "confidence":0.9}
  ],
  "wire_apis": [
    {
      "name": "responses",
      "base_url": "https://api.openai.com/v1",
      "endpoint": "/responses",
      "status": "supported|preferred|legacy|unknown",
      "streaming": {"supported": true, "format": "sse|jsonl|websocket|unknown"},
      "tools": {"supported": true, "parallel_tool_calls": true, "strict_schema": true},
      "modalities": {"input": ["text"], "output": ["text"]}
    }
  ],
  "auth": {
    "scheme": "bearer|api-key-header|oauth|aws-sigv4|unknown",
    "env_keys": ["OPENAI_API_KEY"],
    "headers": [{"name":"Authorization", "value":"Bearer $OPENAI_API_KEY"}],
    "client_side_notes": []
  },
  "prompt_caching": {
    "status": "explicit|automatic|resource|unsupported|unknown",
    "mechanisms": [
      {
        "name": "prompt_cache_key",
        "kind": "request_field|header|content_block|resource_lifecycle|automatic",
        "field": "prompt_cache_key",
        "scope": "request|message|content_block|tool|system|resource",
        "models": ["gpt-5*"],
        "default_policy": "enabled|disabled|auto",
        "dsco_support": "implemented|planned|not_applicable|blocked",
        "usage_fields": ["usage.input_tokens_details.cached_tokens"]
      }
    ],
    "minimum_cacheable_tokens": null,
    "ttl": "provider_default|5m|1h|24h|unknown",
    "invalidates_on": ["model_change", "prefix_change"],
    "notes": []
  },
  "usage_accounting": {
    "input_tokens": ["usage.prompt_tokens", "usage.input_tokens"],
    "output_tokens": ["usage.completion_tokens", "usage.output_tokens"],
    "cached_tokens": ["usage.prompt_tokens_details.cached_tokens"],
    "reasoning_tokens": ["usage.completion_tokens_details.reasoning_tokens"],
    "provider_specific": []
  },
  "reasoning": {
    "supported": true,
    "field": "reasoning.effort",
    "efforts": ["low", "medium", "high"],
    "aliases": {"max":"high"},
    "unsupported_values_rejected": true
  },
  "streaming": {
    "idle_timeout_ms_recommended": 120000,
    "request_max_retries": 2,
    "stream_max_retries": 2,
    "idempotent_retries": true,
    "known_failure_modes": []
  },
  "client_side_mechanics": {
    "sdk_compatibility": ["openai-python"],
    "codex_config": {},
    "headers_required": [],
    "headers_optional": [],
    "unsupported_fields": [],
    "quirks": []
  },
  "dsco": {
    "provider_profile": "openai",
    "implemented_features": ["prompt_cache_key"],
    "missing_features": [],
    "tests": ["test_prompt_cache_openai_request_shape"],
    "risk": "low|medium|high"
  }
}
```

## Status vocabulary

- `implemented`: DSCO emits the correct provider-specific request/header/resource behavior.
- `automatic`: provider handles caching without DSCO request fields; DSCO should parse usage and expose metadata.
- `planned`: documented provider feature exists, DSCO has not implemented it yet.
- `unknown`: no reliable documentation retrieved yet.
- `blocked`: requires new transport/lifecycle architecture.

## Governance rules

1. Do not mark `implemented` without either request-shape tests or live gated integration tests.
2. Do not emit undocumented fields to providers.
3. Automatic provider caching still needs metadata so DSCO can parse usage/cost and display cache effectiveness.
4. Resource-lifecycle caching (Gemini) must be represented separately from request-field caching.
5. Every metadata fact needs docs/provenance and a review date.
