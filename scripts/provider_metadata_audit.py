#!/usr/bin/env python3
"""Validate and audit DSCO provider machinery metadata.

Usage:
  scripts/provider_metadata_audit.py                 # validate + summary table
  scripts/provider_metadata_audit.py --json          # machine-readable report
  scripts/provider_metadata_audit.py --gaps          # only show gaps/risks
  scripts/provider_metadata_audit.py --check-impl    # cross-check against src/provider.c

Exit code is non-zero if any record fails structural validation.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROV_DIR = os.path.join(ROOT, "provider_metadata", "providers")
PROVIDER_C = os.path.join(ROOT, "src", "provider.c")
REPORT = os.path.join(ROOT, "provider_metadata", "reports", "AUDIT.json")

REQUIRED_TOP = [
    "provider", "display_name", "last_reviewed", "confidence", "docs",
    "wire_apis", "auth", "prompt_caching", "usage_accounting", "reasoning",
    "streaming", "client_side_mechanics", "dsco",
]
CACHE_STATUS = {"explicit", "automatic", "resource", "unsupported", "unknown"}
SUPPORT = {"implemented", "automatic", "planned", "not_applicable", "blocked", "unknown"}


def load():
    recs = {}
    if not os.path.isdir(PROV_DIR):
        return recs
    for fn in sorted(os.listdir(PROV_DIR)):
        if not fn.endswith(".json"):
            continue
        path = os.path.join(PROV_DIR, fn)
        try:
            recs[fn[:-5]] = json.load(open(path))
        except Exception as e:  # noqa: BLE001
            recs[fn[:-5]] = {"__error__": str(e)}
    return recs


def validate(name, r):
    errs = []
    if "__error__" in r:
        return [f"json parse error: {r['__error__']}"]
    for k in REQUIRED_TOP:
        if k not in r:
            errs.append(f"missing field: {k}")
    if r.get("provider") != name:
        errs.append(f"provider id '{r.get('provider')}' != filename '{name}'")
    if not isinstance(r.get("docs"), list) or not r.get("docs"):
        errs.append("docs must be a non-empty list (provenance required)")
    pc = r.get("prompt_caching", {})
    if pc.get("status") not in CACHE_STATUS:
        errs.append(f"prompt_caching.status invalid: {pc.get('status')}")
    for m in pc.get("mechanisms", []):
        if m.get("dsco_support") not in SUPPORT:
            errs.append(f"mechanism {m.get('name')} dsco_support invalid: {m.get('dsco_support')}")
    if not re.match(r"^\d{4}-\d{2}-\d{2}$", str(r.get("last_reviewed", ""))):
        errs.append("last_reviewed must be YYYY-MM-DD")
    try:
        c = float(r.get("confidence"))
        if not (0.0 <= c <= 1.0):
            errs.append("confidence out of range")
    except Exception:  # noqa: BLE001
        errs.append("confidence not numeric")
    # implemented features should have at least one test
    d = r.get("dsco", {})
    impl = d.get("implemented_features", [])
    if impl and not d.get("tests"):
        errs.append("implemented_features present but no tests listed")
    return errs


def impl_signals():
    """Heuristic: which cache mechanisms appear wired in src/provider.c."""
    sig = {}
    try:
        src = open(PROVIDER_C).read()
    except OSError:
        return sig
    sig["prompt_cache_key"] = "prompt_cache_key" in src
    sig["prompt_cache_retention"] = "prompt_cache_retention" in src
    sig["cache_control"] = "cache_control" in src
    sig["x-grok-conv-id"] = "x-grok-conv-id" in src
    sig["cached_tokens_parse"] = "cached_tokens" in src
    return sig


def main():
    args = set(sys.argv[1:])
    recs = load()
    sig = impl_signals()
    report = {"providers": {}, "summary": {}, "impl_signals": sig}
    total_err = 0
    rows = []
    for name in sorted(recs):
        r = recs[name]
        errs = validate(name, r)
        total_err += len(errs)
        pc = r.get("prompt_caching", {}) if "__error__" not in r else {}
        mech = ",".join(m.get("name", "?") for m in pc.get("mechanisms", [])) or "-"
        impl = ",".join(r.get("dsco", {}).get("implemented_features", [])) if "__error__" not in r else "-"
        missing = r.get("dsco", {}).get("missing_features", []) if "__error__" not in r else []
        risk = r.get("dsco", {}).get("risk", "?") if "__error__" not in r else "error"
        report["providers"][name] = {
            "valid": not errs,
            "errors": errs,
            "cache_status": pc.get("status", "?"),
            "mechanisms": mech,
            "implemented": impl or "-",
            "missing": missing,
            "risk": risk,
            "confidence": r.get("confidence"),
            "last_reviewed": r.get("last_reviewed"),
        }
        rows.append((name, pc.get("status", "?"), risk, len(errs), len(missing), mech))

    report["summary"] = {
        "providers": len(recs),
        "errors": total_err,
        "high_risk": [n for n, v in report["providers"].items() if v["risk"] == "high"],
        "needs_review": [n for n, v in report["providers"].items() if (v.get("confidence") or 0) < 0.6],
    }

    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    json.dump(report, open(REPORT, "w"), indent=2, sort_keys=True)

    if "--json" in args:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if total_err == 0 else 1

    if "--check-impl" in args:
        print("impl signals in src/provider.c:")
        for k, v in sig.items():
            print(f"  {k:28} {'yes' if v else 'NO'}")
        print()

    show_gaps = "--gaps" in args
    print(f"{'provider':14} {'cache':10} {'risk':6} {'err':>3} {'miss':>4}  mechanisms")
    print("-" * 80)
    for name, status, risk, ne, nm, mech in rows:
        if show_gaps and risk != "high" and ne == 0 and nm == 0:
            continue
        flag = "FAIL" if ne else ""
        print(f"{name:14} {status:10} {risk:6} {ne:>3} {nm:>4}  {mech} {flag}")
    print("-" * 80)
    s = report["summary"]
    print(f"providers={s['providers']} errors={s['errors']} high_risk={s['high_risk']} needs_review={s['needs_review']}")
    print(f"report -> {os.path.relpath(REPORT, ROOT)}")
    return 0 if total_err == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
