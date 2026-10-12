# Provider source evidence for swimlanes

`reports/model-labs/capability-sources/latest.json` is the read-only capability **evidence overlay** for every route in `reports/model-labs/current/routes.jsonl`. The installed daily `model_lab_watch.py` run refreshes this overlay using the **same GET-only provider discovery**; it does not spend inference credits or change routing, permissions, models, or prices. The native DSCO account-scoped Codex model cache is also used when recent (the public Codex `/models` GET may return HTTP 401 without implying account failure). Catalog-only IDs not yet represented by routes are listed separately as **unregistered candidates**, never executable lanes.

```sh
python3 scripts/swimlane_sources.py --provider openai-codex --model gpt-6-sol
python3 scripts/swimlane_sources.py --provider openrouter --model anthropic/claude-fable-5
python3 scripts/swimlane_sources.py --lane-id YOUR_LANE_ID
python3 scripts/swimlane_sources.py --refresh  # optional GET-only on-demand refresh
```

An observation retains provider feed status, observation time, SHA-256 of its raw catalog, a SHA-256 and durable projection of structured model declarations, and per-lane exact-match status. Default context, larger catalog maximum, output limit, tool advertisement, and `supports_experimental_context` are **separate** fields. The raw vendor prose is not copied into the overlay. Neither a maximum nor a `catalog_ok` listing proves account entitlement or actual usable request length. Failed or >24-hour-old feeds stay unknown; a failed catalog GET is **not** model retirement. A provider/model declaration does not certify a specific host, quantization, or endpoint: such routes explicitly read `provider_model_not_endpoint_verified` and have no verified context. `verified_context_tokens` is null everywhere until a separately governed route-specific test writes trustworthy evidence; this pipeline never changes executable model limits.

The saved 3,990-route inventory is a *baseline*, not a live census of every new product/credential. New exact provider/model IDs are shown under `unregistered_catalog_models` without invented lane IDs or launch authority. Next step for routing promotion is a bounded, dated probe tied to the exact endpoint, authentication/billing lane, modality, tool behavior, and failure recovery; it must not be inferred from catalog metadata alone.
