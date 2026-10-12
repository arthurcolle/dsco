# Complete provider and model lanes

A model name is not a lane identity. The full inventory preserves provider,
product, principal profile, transport, billing mode, auth mode, model, upstream,
endpoint tag, quantization, effort, endpoint URL, service tier, and catalog variant.
OpenAI API and ChatGPT Codex remain different products/providers even for the same
model. Claude API/Max and Sakana PAYG/subscription likewise remain separate.

Generate the inventory from saved first-party, account and upstream catalogs:

```sh
python3 scripts/unroll_provider_lanes.py \
  --discovery reports/frontier-policy-20260905 \
  --output reports/frontier-policy-20260905/full-unroll
```

The output contains `inventory.json`, line-oriented `lanes.jsonl`, and complete
64-entry `shards/`. There is no catalog-size, quality, price, credential, or modality
filter. All source records, raw prices, unavailable/hidden models, and conflicting
endpoint variants remain represented. Duplicate runtime identities receive separate
catalog-variant IDs; this does not promise that the runtime can select that variant.
Unknown effort support stays `auto`; explicitly listed effort/service tiers expand.
Unsupported native effort values remain visible with `native_effort_supported=false`.

First-party discovery is available through `scripts/discover_provider_inventory.py`.
It derives all native provider profiles from disk and probes public catalogs even
without credentials. The account catalog remains separate: an API catalog is not
proof of subscription model access. Missing account/provider catalogs are reported
as gaps, and configured model defaults are explicitly labeled non-exhaustive.
Inventory snapshots do not automatically replace global routing or launch workers.

## Qualification and opt-in selection

The full inventory is larger than the executable, qualified set. Probe bounded
candidate batches with `scripts/run_provider_qualification.py`, keeping each route's
provider/model/effort/upstream identity and cost receipts. A strict whole-response
validator determines success. Generation metadata can independently confirm the
OpenRouter host; a request pin alone is labeled accordingly.

`swarm` action `provider_fabric` accepts `selection: "cost_frontier"` plus an explicit
`frontier_policy` object. `scripts/compile_frontier_policy.py` compiles the initial
integer-JSON workload policy after independently checking its receipts. It requires
fresh exact-route quotes, known used rates and limits, strict workload-specific
validation, and a latency ceiling. Expiration precedes a known pricing-band change.
Missing request fees are unknown in execution policies, even where older price-only
scenario reports assumed zero. Keep those reports separate from admission evidence.

The native selector handles at most 64 candidate lanes per bounded policy and emits
an exclusion audit. It spawns one selected lane, preserves supported endpoint/auth
identity, and disables inference fallback. It rejects coverage/society conflicts,
stale evidence, unknown cost components, and auth products it cannot pin. In
particular generic Anthropic fabric spawning currently prefers OAuth, so explicit
API/Max policies are excluded rather than falsely advertised as pinned. The full
inventory still contains both products.

A model's `done` status is not validation. The caller must check the actual result.
Model-call budgets are stop conditions between responses, not exact invoice caps;
reserve conservatively for wrapped prompts, output and retries. Never refund an
unknown interrupted attempt as zero. Catalog expansion and concurrency are independent.
