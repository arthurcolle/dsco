#!/usr/bin/env python3
"""Build/query an offline, provenance-preserving lab -> model -> access-path catalog."""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "data/model_labs.json"
DEFAULT_OUTPUT = ROOT / "reports/model-labs/current"


def read(path):
    return json.loads(Path(path).read_text())


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=path.name + ".", delete=False) as handle:
        temp = Path(handle.name)
        try:
            handle.write(json.dumps(value, indent=2) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
            temp.replace(path)
        finally:
            temp.unlink(missing_ok=True)


class Attribution:
    def __init__(self, registry):
        self.labs = {r["id"]: r for r in registry["labs"]}
        self.aliases = {}
        self.families = []
        for lab in registry["labs"]:
            for alias in set([lab["id"], *lab["aliases"]]):
                alias = alias.casefold()
                if alias in self.aliases and self.aliases[alias] != lab["id"]:
                    raise ValueError("Duplicate lab alias: " + alias)
                self.aliases[alias] = lab["id"]
            self.families.extend((f.casefold(), lab["id"]) for f in lab.get("families", []))
        self.families.sort(key=lambda r: len(r[0]), reverse=True)

    def identify(self, model, provider):
        name = model.casefold()
        if name.startswith("nim/"):
            lab, basis, namespace = self.identify(model[4:], provider)
            return lab, "nim_packaging/" + basis, namespace
        namespace = name.split("/", 1)[0].lstrip("~") if "/" in name else None
        if namespace in self.aliases:
            return self.aliases[namespace], "catalog_namespace", namespace
        # Never reinterpret an unknown explicit publisher as a known base-model
        # creator. A fine-tune of Llama is not automatically published by Meta.
        if namespace is not None:
            return None, "unresolved_namespace", namespace
        for prefix, lab in self.families:
            if name.startswith(prefix) and (prefix not in ("o1", "o3", "o4", "k3") or
                                           name == prefix or name.startswith(prefix + "-")):
                return lab, "curated_model_family", None
        # Only dedicated first-party products can supply this fallback. NVIDIA,
        # Groq, Ramp, Vercel, Together and other multi-lab hosts cannot.
        dedicated = {
            "anthropic": "anthropic", "openai": "openai", "openai-codex": "openai",
            "kimi-code": "moonshot", "moonshot": "moonshot", "sakana": "sakana",
            "zai": "zai", "xai": "xai", "xai-grok": "xai", "mistral": "mistral",
            "cohere": "cohere", "deepseek": "deepseek", "alibaba": "alibaba",
            "alibaba-coding-plan": "alibaba", "minimax": "minimax", "minimax-cn": "minimax",
            "perplexity": "perplexity", "google": "google", "abliteration-ai": "abliteration-ai",
        }
        lab = dedicated.get(provider)
        return lab, "dedicated_first_party_product" if lab else "unresolved_model", None


def profile_transports():
    # Reuse the existing native-registry reader, not a second provider list.
    from discover_provider_inventory import profiles
    return {p["provider"]: p for p in profiles()}


def build(base, registry_path=REGISTRY, auth_path=None):
    base = Path(base)
    attribution = Attribution(read(registry_path))
    inventory = read(base / "inventory.json")
    original = inventory["lanes"]
    gateways = read(base / "gateways/inventory.json")["lanes"]
    gateway_meta = {
        provider: {r["id"]: r for r in read(base / ("gateways/" + provider + "-models.json"))["data"]}
        for provider in ("ramp-router", "vercel")
    }
    summary = read(base / "summary.json")
    provider_status = {r["provider"]: r for r in read(base / "provider-inventory/inventory.json")}
    auth_source = Path(auth_path) if auth_path else base / "auth-lanes.json"
    auth = read(auth_source)["lanes"]
    profiles = profile_transports()
    manifest_paths = [Path(registry_path), base / "inventory.json", base / "gateways/inventory.json", auth_source,
                      base / "summary.json", base / "provider-inventory/inventory.json",
                      base / "gateways/ramp-router-models.json", base / "gateways/vercel-models.json",
                      ROOT / "src/provider_profiles.c"]
    routes = []
    unresolved = collections.Counter()
    for row in [*original, *gateways]:
        provider, model = row["provider"], row["model"]
        meta = gateway_meta[provider][model] if provider in gateway_meta else row.get("source_model_metadata", {})
        if not isinstance(meta, dict):
            meta = {}
        lab, basis, namespace = attribution.identify(model, provider)
        if lab is None and provider == "ramp-router":
            # These are model aliases, not the serving host's owned_by field.
            matched = [attribution.identify(alias, provider) for alias in meta.get("router", {}).get("aliases", [])]
            known = {match[0] for match in matched if match[0]}
            if len(known) == 1:
                lab = known.pop()
                basis = "ramp_explicit_model_alias"
        if lab is None:
            unresolved[namespace or (provider + ":" + model)] += 1
        kind = attribution.labs[lab]["kind"] if lab else "unresolved"
        router = meta.get("router", {})
        declared = meta.get("architecture", {})
        modality = meta.get("type") or row.get("type")
        if not modality and declared:
            output = declared.get("output_modalities", [])
            modality = "language" if "text" in output else ",".join(output) or None
        auth_matches = [a for a in auth if a["provider"] == provider and
                        (not row.get("product") or a.get("product") == row.get("product")) and
                        (not row.get("billing") or a.get("billing") == row.get("billing"))]
        credential = any(a.get("ready") for a in auth_matches) if auth_path else row.get("auth_ready", row.get("credential_present", False))
        profile = profiles.get(provider)
        native = bool(profile and profile.get("transport") not in ("PROVIDER_TRANSPORT_NONE", "PROVIDER_TRANSPORT_EXTERNAL_PROCESS"))
        # Source presence is not evidence that the installed executable contains
        # the transport. A fresh runtime auth inventory supplies that evidence.
        runtime_seen = any(a["provider"] == provider for a in auth) if auth_path else None
        blockers = []
        if not credential:
            blockers.append("credential_or_native_auth_unavailable")
        if not native:
            blockers.append("native_transport_unavailable")
        if auth_path and not runtime_seen:
            blockers.append("provider_absent_from_checked_binary")
        if provider == "openrouter" and summary.get("openrouter_account_key_status") != 200:
            blockers.append("account_key_check_http_" + str(summary.get("openrouter_account_key_status")))
        if provider == "zai" and row.get("billing") == "included":
            blockers.append("coding_plan_expired_in_prior_live_test")
        if meta.get("visibility") in ("hidden", "hide"):
            blockers.append("hidden_client_model")
        if kind in ("router", "composite_service", "unresolved"):
            blockers.append("not_an_attributed_individual_model")
        if row.get("catalog_variant"):
            blockers.append("conflicting_catalog_variant_requires_review")
        if row.get("native_effort_supported") is False:
            blockers.append("native_effort_not_validated")
        parameters = row.get("supported_parameters") or meta.get("supported_parameters") or []
        supports_tools = row.get("tools_advertised")
        if supports_tools is None and parameters:
            supports_tools = "tools" in parameters
        current = {
            "lane_id": row["lane_id"], "lab_id": lab, "lab_kind": kind,
            "attribution_basis": basis, "publisher_namespace": namespace,
            "provider": provider, "product": row.get("product", provider),
            "model": model, "model_type": modality or "unclassified",
            "model_creator_independence_verified": False,
            "co_developers": attribution.labs[lab].get("members", []) if lab else [],
            "lineage_notes": attribution.labs[lab].get("lineage_notes") if lab else None,
            "hosting_provider": row.get("upstream") or row.get("hosting_provider") or None,
            "endpoint_tag": row.get("endpoint_tag") or None,
            "endpoint": row.get("endpoint") or ("https://api.router.com/v1/responses" if provider == "ramp-router" else "https://ai-gateway.vercel.sh/v1/chat/completions" if provider == "vercel" else None),
            "billing": row.get("billing", "prepaid_credits" if provider == "ramp-router" else "metered"),
            "auth_mode": row.get("auth_mode", "api_key"),
            "principal_profile": row.get("principal_profile", "default"),
            "effort": row.get("effort", "auto"), "service_tier": row.get("service_tier", "default"),
            "quantization": row.get("quantization") or None, "catalog_variant": row.get("catalog_variant") or None,
            "dynamic_alias": model.startswith("~") or model.endswith("-latest"),
            "credential_locally_ready": bool(credential), "native_profile_in_source": native,
            "provider_seen_in_checked_binary": runtime_seen,
            "inference_verified": bool(row.get("inference_verified", False)),
            "catalog_status": provider_status.get(provider, {}).get("status") or row.get("catalog_http_status"),
            "catalog_scope": row.get("catalog_scope"),
            "context_tokens": row.get("context_tokens") or meta.get("context_window") or meta.get("context_length"),
            "tools_advertised": supports_tools,
            "reasoning_options": row.get("reasoning_metadata") or meta.get("reasoning_options") or router.get("capabilities", {}).get("reasoning"),
            "source": row.get("source"), "launch_blockers": blockers,
            "qualification_candidate": not blockers,
            "general_agent_worker_candidate": not blockers and modality == "language" and supports_tools is True,
        }
        routes.append(current)
    ids = [r["lane_id"] for r in routes]
    if len(set(ids)) != len(ids):
        raise ValueError("Input lane identity collision; do not silently drop a route")
    labs = []
    for lab_id, definition in attribution.labs.items():
        owned = [r for r in routes if r["lab_id"] == lab_id]
        if not owned:
            continue
        labs.append({
            **definition, "route_records": len(owned),
            "distinct_provider_model_ids": len({(r["provider"], r["model"]) for r in owned}),
            "access_paths": sorted({r["provider"] for r in owned}),
            "qualification_candidate_paths": sorted({r["provider"] for r in owned if r["qualification_candidate"]}),
            "modalities": sorted({r["model_type"] for r in owned}),
            "model_examples": list(dict.fromkeys(r["model"] for r in owned))[:5],
        })
    labs.sort(key=lambda r: (r["kind"] != "model_lab", r["name"].casefold()))
    return {
        "schema": "dsco.model_lab_catalog.v1", "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_inventory": str(base.resolve()), "attribution_scope": read(registry_path)["scope"],
        "summary": {
            "source_lane_records": len(routes), "mapped_lane_records": sum(r["lab_id"] is not None for r in routes),
            "unresolved_lane_records": sum(unresolved.values()), "identified_publishers": len(labs),
            "kinds": dict(collections.Counter(l["kind"] for l in labs)),
            "access_paths": len({r["provider"] for r in routes}),
            "qualification_candidates": sum(r["qualification_candidate"] for r in routes),
            "advertised_language_tool_candidates": sum(r["general_agent_worker_candidate"] for r in routes),
            "concurrent_worker_capacity": None,
        },
        "labs": labs, "routes": routes,
        "unresolved": [{"namespace_or_model": k, "route_records": v} for k, v in sorted(unresolved.items())],
        "source_manifest": [{"path": str(p.resolve()), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in manifest_paths],
        "limitations": [
            "All publishers means all publishers in these observed catalogs, not all labs worldwide.",
            "Family and namespace normalization does not establish independent weights, corporate parentage, training provenance, or quality.",
            "A qualification candidate has usable local route metadata; it is not a passed inference or concurrency test.",
            "Native source profiles, current executable support, account authentication, remaining credits and model entitlement are distinct evidence.",
            "Catalog counts include aliases, historical models, nonlanguage modalities and distinct billing/transport configurations.",
        ],
    }


def markdown(catalog):
    s = catalog["summary"]
    lines = ["# DSCO model labs and access paths", "", "Observed: " + catalog["generated_at"], "",
             "[Implementation, live verification and next steps](../../../docs/MODEL_LABS_AND_ACCESS_PATHS.md)", "",
             f"**{s['identified_publishers']} identified creators/publishers; {s['kinds'].get('model_lab', 0)} model labs.** "
             f"{s['source_lane_records']} preserved route records across {s['access_paths']} access paths; "
             f"{s['unresolved_lane_records']} records still need attribution.", "",
             "Scope: all publishers observed in the saved native, OpenRouter, Hugging Face, Ramp and Vercel catalogs. "
             "This is not a worldwide company census or a count of simultaneous workers.", "",
             "## Lab → model → access path", "",
             "A lab creates or publishes a model. A host runs it. A gateway connects the account. "
             "For example, Moonshot → Kimi → Fireworks → Ramp is one account path, not four independent labs.", "",
             "| Creator / publisher | Kind | Route records | Access paths | Paths with local qualification candidates |",
             "|---|---|---:|---|---|"]
    for lab in catalog["labs"]:
        lines.append(f"| {lab['name']} | {lab['kind']} | {lab['route_records']} | {', '.join(lab['access_paths'])} | {', '.join(lab['qualification_candidate_paths']) or 'None verified locally'} |")
    lines.extend(["", "## Practical expansion order", "",
        "1. Use existing paid subscriptions for their own lab models; keep each billing product explicit.",
        "2. Qualify Ramp's public Responses transport using its account catalog and existing prepaid credits. A successful model listing is not a tool-loop test.",
        "3. Restore OpenRouter account authentication and locate the Vercel gateway credential. They broaden access without establishing new independent lab capability.",
        "4. Qualify underrepresented language-model labs through configured direct API accounts; then test image, video, speech, embedding and evaluation specialists in their matching workloads.",
        "5. Measure cross-provider tool correctness, latency, cost and shared throttling before increasing the governed farm's concurrency.", "",
        "## Query and refresh", "", "```sh",
        "python3 scripts/model_lab_catalog.py --lab moonshot --provider ramp-router",
        "python3 scripts/model_lab_catalog.py --lab prism-ml --json",
        f"python3 scripts/model_lab_catalog.py --inventory {catalog['source_inventory']} --build", "```", "",
        "`catalog.json` and `routes.jsonl` are machine-readable. Exact model IDs and billing identities are preserved. "
        "Fleet Mesh imports this same catalog as metadata; it does not turn all catalog rows into scheduled workers.", "",
        "## Unresolved attribution", ""])
    lines.extend(f"- `{x['namespace_or_model']}`: {x['route_records']} route records" for x in catalog["unresolved"])
    lines.extend(["", "## Evidence limits", "", *["- " + s for s in catalog["limitations"]], ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, default=ROOT / "reports/lane-census-20260918")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--auth", type=Path, help="Fresh dsco auth lanes JSON for the checked binary")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--lab")
    parser.add_argument("--provider")
    parser.add_argument("--candidates", action="store_true", help="Filter local qualification candidates; does not execute them")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.build:
        catalog = build(args.inventory, auth_path=args.auth)
        atomic_json(args.output / "catalog.json", catalog)
        (args.output / "README.md").write_text(markdown(catalog))
        with (args.output / "routes.jsonl").open("w") as stream:
            for row in catalog["routes"]:
                stream.write(json.dumps(row) + "\n")
    else:
        catalog = read(args.output / "catalog.json")
    if args.lab or args.provider or args.candidates:
        aliases = Attribution(read(REGISTRY)).aliases
        lab = aliases.get(args.lab.casefold(), args.lab) if args.lab else None
        rows = [r for r in catalog["routes"] if (not lab or r["lab_id"] == lab) and
                (not args.provider or r["provider"] == args.provider) and
                (not args.candidates or r["qualification_candidate"])]
        if args.json:
            print(json.dumps({"count": len(rows), "routes": rows}, indent=2))
        else:
            for row in rows:
                state = ",".join(row["launch_blockers"]) or "candidate; inference unverified"
                print(f"{row['lab_id'] or '?':20} {row['provider']:18} {row['model']:60} {state}")
    else:
        print(json.dumps(catalog["summary"], indent=2))


if __name__ == "__main__":
    main()
