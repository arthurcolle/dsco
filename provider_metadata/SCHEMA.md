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
  "api_coverage": {
    "review_scope": "Provider public API reference endpoint groups reviewed YYYY-MM-DD",
    "endpoint_groups": [
      {
        "name": "responses",
        "endpoints": ["/responses", "/responses/input_tokens"],
        "status": "covered_preferred|covered_supported|covered_metadata|covered_planned|covered_legacy",
        "dsco_status": "implemented|partial|planned|metadata_only|legacy_metadata_only",
        "cost_hooks": ["token_usage", "prompt_caching", "batch_api"],
        "notes": "Short docs-backed coverage note."
      }
    ],
    "hosted_tools": [
      {"name": "web_search", "status": "tracked_cost", "cost_hooks": ["per_call_price", "model_tokens"]}
    ]
  },
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
  "cost_management": {
    "status": "docs_backed_cost_map|unknown",
    "pricing_basis": [
      "Store raw usage quantities first; apply pricing tables separately."
    ],
    "levers": [
      {
        "name": "batch_api",
        "dsco_support": "implemented|available_via_env|partial|planned|metadata_only",
        "policy": "When to use this lever.",
        "savings": "discount_or_budget_effect",
        "tracking_fields": ["batch_id", "request_count"]
      }
    ]
  },
  "reasoning": {
    "supported": true,
    "field": "reasoning.effort",
    "efforts": ["none", "minimal", "low", "medium", "high", "xhigh"],
    "aliases": {"max":"xhigh"},
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
6. `api_coverage` is for endpoint-family coverage and known lifecycle surfaces; `wire_apis` is still the request transport map DSCO may emit directly.
7. `cost_management` should describe raw usage meters, routing levers, and reconciliation hooks. Keep live prices in pricing/model catalogs; this schema should avoid hard-coding volatile price tables unless the provider has no separate catalog.
