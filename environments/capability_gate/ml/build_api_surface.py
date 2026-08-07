"""Vendor the public-API surface of real tech companies into deterministic, offline tool banks.

Companion to build_corpora.py (same snapshot/manifest/loader conventions). We fetch public API
*metadata* (operation names + HTTP methods — facts, safe to vendor) ONCE from:

  * botocore        — every AWS service's operations (fully offline; the installed package).
  * apis.guru       — the ~2500-spec OpenAPI directory (Stripe/Twilio/Slack/GitHub/Azure/Adyen/…).
  * google_discovery— discovery.googleapis.com method ids (gmail.users.messages.send, …).
  * telegram        — Bot API method list (sendMessage, sendDocument, …).
  * apple           — App Store Connect + SiriKit OpenAPI (via apis.guru mirror).
  * meta_graph      — Facebook/Instagram Graph API edges (curated public reference names).
  * salesforce      — REST SObject CRUD + Bulk/Tooling verbs (curated public reference names).

Each op is normalized to {provider, tool_name, http_method, verb_class} and verb-classified
(READ / WRITE / DESTROY / ADMIN / EGRESS). Snapshots land in ml/corpora/apis/<source>/operations.json
(sorted, deterministic); provenance (url+license+sha256+counts) is merged into ml/corpora/manifest.json.
corpora_banks.py then buckets these into the generator's capability KINDS — so gen_v2 auto-consumes
them with no edit. Generation reads ONLY the local snapshots (offline + deterministic).

Run once (fetch + build):     python build_api_surface.py
Offline re-build from snapshot: python build_api_surface.py --build-only

SAFETY: only operation names + HTTP methods are vendored. Any server URL / host in a spec is
discarded (we never keep a resolvable host); names are public facts. Licenses recorded per source.
"""
import glob
import gzip
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COR = os.path.join(HERE, "corpora")
APIS = os.path.join(COR, "apis")

SOURCES = {
    "botocore": {
        "url": "python-package://botocore/data/*/service-2.json.gz (AWS SDK service models)",
        "license": "Apache-2.0 (botocore / AWS SDK)",
    },
    "apisguru": {
        "url": "https://api.apis.guru/v2/list.json + per-spec swagger/openapi",
        "license": "CC0-1.0 catalog (APIs.guru); individual specs under their own vendor licenses",
    },
    "google_discovery": {
        "url": "https://discovery.googleapis.com/discovery/v1/apis + per-API discovery docs",
        "license": "CC-BY-4.0 (Google API Discovery Service, public metadata)",
    },
    "telegram": {
        "url": "https://core.telegram.org/bots/api",
        "license": "public API reference (Telegram Bot API method names)",
    },
    "apple": {
        "url": "https://api.apis.guru/v2/specs/apple.com/* (App Store Connect + SiriKit OpenAPI)",
        "license": "Apple developer API metadata (public OpenAPI); catalog CC0 via APIs.guru",
    },
    "meta_graph": {
        "url": "https://developers.facebook.com/docs/graph-api/reference (public edge/method names)",
        "license": "public API reference (Meta Graph API node/edge names)",
    },
    "salesforce": {
        "url": "https://developer.salesforce.com/docs/atlas.en-us.api_rest.meta (SObject/Bulk/Tooling)",
        "license": "public API reference (Salesforce REST resource verbs)",
    },
}

# ── cloud providers whose ops route through the CLOUD_OK/CLOUD_DENY IAM banks (see corpora_banks) ──
CLOUD_PROVIDERS = {"aws", "google", "googleapis.com", "azure.com", "azure", "gcp"}

CURL = ["curl", "-sS", "--fail", "--max-time", "25", "--retry", "1"]


# ── verb classification: precedence READ > DESTROY > ADMIN > EGRESS > WRITE > (default WRITE) ──
_READ = re.compile(
    r"^(get|list|describe|head|search|read|download|query|fetch|scan|lookup|watch|export|"
    r"check|test|validate|count|view|aggregated|batchget|preview|estimate|discover|detect|"
    r"select|resolve|poll|receive|retrieve|enumerate|find|show|status|report|inspect)", re.I)
_DESTROY = re.compile(
    r"(delete|remove|terminate|destroy|purge|revoke|deregister|uninstall|drop|erase|wipe|"
    r"expire|clear|discard|dispose|expunge|evict|dedelete|undeploy|teardown)", re.I)
_ADMIN = re.compile(
    r"(policy|policies|\brole\b|roles|permission|accesskey|access[-_]?key|\bgrant\b|authoriz|"
    r"\biam\b|\bacl\b|setiam|addmember|removemember|\bmember\b|binding|principal|entitlement|"
    r"loginprofile|mfadevice|servicelinkedrole|assumerole|passrole|impersonate|privilege)", re.I)
_EGRESS = re.compile(
    r"(send|publish|upload|transfer|charge|payout|payment|\bpay\b|email|mail|webhook|"
    r"\bmessage\b|notify|dispatch|deliver|broadcast|\bsms\b|invite|share|post[-_]?message|"
    r"forward|reply|announce|emit|push|export)", re.I)
_WRITE = re.compile(
    r"^(create|update|put|post|patch|set|add|insert|modify|write|replace|enable|disable|"
    r"start|stop|register|import|copy|move|apply|provision|deploy|attach|associate|batchcreate|"
    r"batchupdate|generate|issue|activate|deactivate|rotate|reset|restore|run|execute|invoke|"
    r"trigger|schedule|cancel|approve|reject|accept|complete|configure|initialize|upsert)", re.I)


def _leaf(tool_name):
    """Discriminating verb token: last dotted/underscore/camel segment's leading word."""
    seg = re.split(r"[.\-_/:]", tool_name)[-1] or tool_name
    return seg


def classify(tool_name, http_method):
    leaf = _leaf(tool_name)
    whole = tool_name
    # READ wins even when an admin noun is present (GetRole is a read, routed to CLOUD_OK).
    if _READ.search(leaf) or (http_method or "").upper() in ("GET", "HEAD"):
        # a GET whose leaf is an egress/destroy verb (rare) still reads
        if not _DESTROY.match(leaf):
            return "READ"
    if _DESTROY.search(leaf) or (http_method or "").upper() == "DELETE":
        return "DESTROY"
    if _ADMIN.search(whole):
        return "ADMIN"
    if _EGRESS.search(leaf):
        return "EGRESS"
    if _WRITE.search(leaf):
        return "WRITE"
    # unknown mutating verb (POST/PUT/PATCH with novel name) -> WRITE (risk-leaning), else READ
    if (http_method or "").upper() in ("POST", "PUT", "PATCH"):
        return "WRITE"
    return "READ"


def _norm_ver(prov):
    return prov.split(":")[0].split(".")[0]


# ── botocore: every AWS service operation (offline) ──
def build_botocore():
    import botocore
    dd = os.path.join(os.path.dirname(botocore.__file__), "data")
    files = sorted(
        p for p in glob.glob(os.path.join(dd, "*", "*", "service-2.json.gz")))
    ops = []
    providers = {}
    for p in files:
        try:
            with gzip.open(p, "rt") as f:
                model = json.load(f)
        except Exception:
            continue
        meta = model.get("metadata", {})
        svc = (meta.get("endpointPrefix") or meta.get("serviceId") or
               os.path.basename(os.path.dirname(os.path.dirname(p))))
        svc = re.sub(r"[^A-Za-z0-9]+", "-", str(svc)).strip("-").lower()
        for opname, spec in sorted((model.get("operations") or {}).items()):
            http = (spec.get("http") or {})
            method = http.get("method", "")
            tool_name = f"aws.{svc}.{opname}"
            ops.append({"provider": "aws", "tool_name": tool_name,
                        "http_method": method, "verb_class": classify(tool_name, method)})
            providers["aws"] = providers.get("aws", 0) + 1
    return _finish("botocore", ops, providers, extra={"n_services": len(files),
                   "botocore_version": botocore.__version__})


# ── apis.guru: fetch the directory, download one spec per provider (+ all for majors), parse ──
MAJORS = {"stripe.com", "twilio.com", "slack.com", "github.com", "digitalocean.com",
          "atlassian.com", "zoom.us", "box.com", "sendgrid.com", "adyen.com", "plaid.com",
          "apple.com", "telegram", "azure.com", "shopify.com", "dropbox.com", "salesforce.com",
          "googleapis.com", "amazonaws.com", "microsoft.com", "gitlab.com", "spotify.com",
          "walmart.com", "ebay.com", "paypal.com", "mastercard.com", "fitbit.com"}
PER_MAJOR = {"azure.com": 20, "googleapis.com": 20}


def _fetch(url, dest, timeout=25):
    try:
        r = subprocess.run(CURL[:2] + ["--max-time", str(timeout), "-o", dest, url],
                           capture_output=True, timeout=timeout + 8)
        return r.returncode == 0 and os.path.exists(dest) and os.path.getsize(dest) > 0
    except Exception:
        return False


def fetch_apisguru():
    os.makedirs(os.path.join(APIS, "apisguru", "specs"), exist_ok=True)
    listp = os.path.join(APIS, "apisguru", "list.json")
    if not _fetch("https://api.apis.guru/v2/list.json", listp, 40):
        print("SKIP apisguru fetch: list.json unavailable")
        return
    d = json.load(open(listp))
    # choose spec urls: one (first version) per root provider, more for majors
    by_root = {}
    for key, entry in d.items():
        root = key.split(":")[0]
        vers = entry.get("versions", {})
        if not vers:
            continue
        vk = sorted(vers)[-1]  # latest version, deterministic
        url = vers[vk].get("swaggerUrl") or vers[vk].get("swaggerYamlUrl")
        if url:
            by_root.setdefault(root, []).append((key, url))
    tasks = []
    for root in sorted(by_root):
        cap = PER_MAJOR.get(root, 8 if root in MAJORS else 1)
        for key, url in sorted(by_root[root])[:cap]:
            safe = re.sub(r"[^A-Za-z0-9]+", "_", key)
            tasks.append((url, os.path.join(APIS, "apisguru", "specs", safe + ".json")))
    # parallel download (skip ones already present for idempotent resume)
    todo = [(u, dst) for u, dst in tasks if not os.path.exists(dst)]
    print(f"apisguru: {len(tasks)} specs targeted ({len(todo)} to download)")
    _parallel_download(todo)


def _parallel_download(todo, workers=16):
    if not todo:
        return
    import concurrent.futures as cf
    ok = 0
    with cf.ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(_fetch, u, dst): dst for u, dst in todo}
        for fut in cf.as_completed(futs):
            if fut.result():
                ok += 1
    print(f"  downloaded {ok}/{len(todo)}")


def _spec_ops(spec, provider):
    """Extract (tool_name, method) from an OpenAPI/Swagger doc; server hosts are discarded."""
    out = []
    paths = spec.get("paths") or {}
    methods = ("get", "put", "post", "delete", "patch", "head", "options", "trace")
    for path, item in paths.items():
        if not isinstance(item, dict):
            continue
        for m in methods:
            op = item.get(m)
            if not isinstance(op, dict):
                continue
            opid = op.get("operationId")
            if not opid:
                tags = op.get("tags") or []
                tag = re.sub(r"[^A-Za-z0-9]+", "", (tags[0] if tags else ""))
                leaf = re.sub(r"[{}]", "", path).strip("/").split("/")[-1] or "root"
                leaf = re.sub(r"[^A-Za-z0-9]+", "_", leaf)
                opid = f"{m}_{tag}_{leaf}" if tag else f"{m}_{leaf}"
            opid = re.sub(r"\s+", "", str(opid))[:80]
            out.append((opid, m.upper()))
    return out


def build_apisguru():
    specdir = os.path.join(APIS, "apisguru", "specs")
    files = sorted(glob.glob(os.path.join(specdir, "*.json")))
    ops = []
    providers = {}
    for p in files:
        if os.path.getsize(p) > 6_000_000:  # bound parse cost; huge specs skipped, logged in counts
            continue
        try:
            spec = json.load(open(p))
        except Exception:
            continue
        key = os.path.basename(p)[:-5]
        root = key.split("_")[0] + ("." + key.split("_")[1] if len(key.split("_")) > 1 else "")
        # provider label = registered root domain, e.g. stripe.com
        prov = re.match(r"([a-z0-9]+(?:_[a-z0-9]+)?)", key)
        provider = key.split("_")[0]
        # prefer the info.x-providerName / host root if present
        info = spec.get("info", {}) if isinstance(spec, dict) else {}
        pn = info.get("x-providerName")
        if isinstance(pn, str) and pn:
            provider = pn
        provider = re.sub(r"[^A-Za-z0-9.]+", "", provider).lower() or "unknown"
        pslug = provider.split(".")[0]
        for opid, method in _spec_ops(spec if isinstance(spec, dict) else {}, provider):
            tool_name = f"{pslug}.{opid}"
            ops.append({"provider": provider, "tool_name": tool_name,
                        "http_method": method, "verb_class": classify(tool_name, method)})
            providers[provider] = providers.get(provider, 0) + 1
    return _finish("apisguru", ops, providers, extra={"n_specs": len(files)})


# ── google discovery: list APIs, fetch a broad capped sample of discovery docs, extract method ids ──
def fetch_google_discovery(cap_apis=220):
    base = os.path.join(APIS, "google_discovery")
    os.makedirs(os.path.join(base, "docs"), exist_ok=True)
    listp = os.path.join(base, "list.json")
    if not _fetch("https://discovery.googleapis.com/discovery/v1/apis", listp, 40):
        print("SKIP google_discovery fetch: apis list unavailable")
        return
    d = json.load(open(listp))
    items = d.get("items", [])
    # prefer preferred versions, dedupe by (name), broad sample
    seen = {}
    for it in items:
        if it.get("preferred") or it["name"] not in seen:
            seen[it["name"]] = it.get("discoveryRestUrl")
    todo = []
    for name in sorted(seen)[:cap_apis]:
        url = seen[name]
        if not url:
            continue
        dst = os.path.join(base, "docs", re.sub(r"[^A-Za-z0-9]+", "_", name) + ".json")
        if not os.path.exists(dst):
            todo.append((url, dst))
    print(f"google_discovery: {len(seen)} apis, sampling {min(cap_apis, len(seen))} ({len(todo)} to download)")
    _parallel_download(todo)


def _walk_google_methods(node, out):
    if not isinstance(node, dict):
        return
    for _, m in sorted((node.get("methods") or {}).items()):
        mid = m.get("id")
        http = m.get("httpMethod", "")
        if mid:
            out.append((mid, http))
    for _, sub in sorted((node.get("resources") or {}).items()):
        _walk_google_methods(sub, out)


def build_google_discovery():
    base = os.path.join(APIS, "google_discovery")
    files = sorted(glob.glob(os.path.join(base, "docs", "*.json")))
    ops = []
    providers = {}
    for p in files:
        try:
            doc = json.load(open(p))
        except Exception:
            continue
        found = []
        _walk_google_methods(doc, found)
        for mid, http in found:
            tool_name = mid  # already namespaced, e.g. compute.instances.delete
            ops.append({"provider": "google", "tool_name": tool_name,
                        "http_method": http, "verb_class": classify(tool_name, http)})
            providers["google"] = providers.get("google", 0) + 1
    return _finish("google_discovery", ops, providers, extra={"n_docs": len(files)})


# ── telegram bot API: scrape method names from the reference HTML ──
def fetch_telegram():
    base = os.path.join(APIS, "telegram")
    os.makedirs(base, exist_ok=True)
    dst = os.path.join(base, "botapi.html")
    if not _fetch("https://core.telegram.org/bots/api", dst, 40):
        print("SKIP telegram fetch: reference unavailable")


def build_telegram():
    p = os.path.join(APIS, "telegram", "botapi.html")
    if not os.path.exists(p):
        return _finish("telegram", [], {})
    html = open(p, encoding="utf-8", errors="ignore").read()
    # method anchors are lowerCamelCase h4 names; types are UpperCamelCase. Keep methods.
    names = set(re.findall(r'<a class="anchor"[^>]*name="([a-z][a-zA-Z0-9]+)"', html))
    if not names:  # fallback: any <h4>lowerCamel</h4>
        names = set(re.findall(r"<h4>([a-z][a-zA-Z0-9]+)</h4>", html))
    ops = []
    for n in sorted(names):
        # telegram methods are POSTable RPC calls; classify by verb
        tool_name = f"telegram.{n}"
        method = "POST"
        ops.append({"provider": "telegram", "tool_name": tool_name,
                    "http_method": method, "verb_class": classify(tool_name, method)})
    return _finish("telegram", ops, {"telegram": len(ops)})


# ── apple: reuse App Store Connect + SiriKit OpenAPI specs from the apis.guru mirror ──
def fetch_apple():
    base = os.path.join(APIS, "apple")
    os.makedirs(base, exist_ok=True)
    urls = {
        "app-store-connect":
            "https://api.apis.guru/v2/specs/apple.com/app-store-connect/1.4.1/openapi.json",
        "sirikit-cloud-media":
            "https://api.apis.guru/v2/specs/apple.com/sirikit-cloud-media/1.0.2/openapi.json",
    }
    for name, url in urls.items():
        dst = os.path.join(base, name + ".json")
        if not os.path.exists(dst):
            _fetch(url, dst, 40)


def build_apple():
    base = os.path.join(APIS, "apple")
    files = sorted(glob.glob(os.path.join(base, "*.json")))
    ops = []
    providers = {}
    for p in files:
        try:
            spec = json.load(open(p))
        except Exception:
            continue
        for opid, method in _spec_ops(spec, "apple"):
            tool_name = f"apple.{opid}"
            ops.append({"provider": "apple", "tool_name": tool_name,
                        "http_method": method, "verb_class": classify(tool_name, method)})
            providers["apple"] = providers.get("apple", 0) + 1
    return _finish("apple", ops, providers, extra={"n_specs": len(files)})


# ── meta graph API: curated public edge/method names (no OpenAPI; names are public reference facts) ──
META_NODES = ["me", "user", "page", "post", "photo", "video", "album", "event", "group",
              "comment", "feed", "message", "conversation", "adaccount", "campaign", "adset",
              "ad", "insights", "business", "catalog", "product", "instagram_account", "media",
              "story", "live_video", "reel", "audience", "pixel", "lead", "webhook", "permission"]
META_EDGE_OPS = [  # (edge, http, verb-ish name)
    ("feed", "POST", "publish"), ("feed", "GET", "read"), ("messages", "POST", "send"),
    ("photos", "POST", "upload"), ("videos", "POST", "upload"), ("comments", "POST", "create"),
    ("comments", "GET", "list"), ("likes", "POST", "create"), ("likes", "DELETE", "delete"),
    ("insights", "GET", "read"), ("campaigns", "POST", "create"), ("campaigns", "DELETE", "delete"),
    ("adsets", "POST", "create"), ("ads", "POST", "create"), ("ads", "DELETE", "delete"),
    ("subscribed_apps", "POST", "subscribe"), ("subscribed_apps", "DELETE", "unsubscribe"),
    ("media", "POST", "publish"), ("media_publish", "POST", "publish"), ("leadgen", "GET", "read"),
    ("permissions", "DELETE", "revoke"), ("roles", "POST", "grant"), ("blocked", "POST", "block"),
    ("notifications", "POST", "send"), ("conversations", "GET", "list"),
]


def build_meta_graph():
    ops = []
    for node in META_NODES:
        for edge, http, verb in META_EDGE_OPS:
            tool_name = f"facebook.{node}.{edge}.{verb}"
            ops.append({"provider": "meta", "tool_name": tool_name,
                        "http_method": http, "verb_class": classify(f"{verb}.{edge}", http)})
    # dedupe deterministically
    return _finish("meta_graph", ops, {"meta": len(ops)})


# ── salesforce: REST SObject CRUD + Bulk/Tooling verbs (public reference resource patterns) ──
SF_OBJECTS = ["Account", "Contact", "Lead", "Opportunity", "Case", "Campaign", "User", "Task",
              "Event", "Product2", "Pricebook2", "Order", "Contract", "Asset", "Quote", "Solution",
              "Report", "Dashboard", "Group", "PermissionSet", "Profile", "Attachment",
              "ContentDocument", "EmailMessage", "Note", "Territory", "Entitlement"]
SF_VERBS = [("create", "POST"), ("update", "PATCH"), ("delete", "DELETE"), ("describe", "GET"),
            ("get", "GET"), ("upsert", "PATCH"), ("query", "GET"), ("search", "GET")]
SF_APIS = [  # (api, op, http)
    ("sobjects", "create", "POST"), ("sobjects", "update", "PATCH"), ("sobjects", "delete", "DELETE"),
    ("bulk", "createJob", "POST"), ("bulk", "getJobInfo", "GET"), ("bulk", "abortJob", "DELETE"),
    ("bulk", "uploadBatch", "POST"), ("tooling", "executeAnonymous", "POST"),
    ("tooling", "runTests", "POST"), ("tooling", "query", "GET"), ("composite", "batch", "POST"),
    ("composite", "tree", "POST"), ("query", "queryAll", "GET"), ("search", "parameterizedSearch", "GET"),
    ("chatter", "postFeedItem", "POST"), ("chatter", "getFeed", "GET"), ("limits", "getLimits", "GET"),
    ("apexrest", "invoke", "POST"), ("metadata", "deploy", "POST"), ("metadata", "retrieve", "GET"),
]


def build_salesforce():
    ops = []
    for obj in SF_OBJECTS:
        for verb, http in SF_VERBS:
            tool_name = f"salesforce.sobjects.{obj}.{verb}"
            ops.append({"provider": "salesforce", "tool_name": tool_name,
                        "http_method": http, "verb_class": classify(f"{verb}.{obj}", http)})
    for api, op, http in SF_APIS:
        tool_name = f"salesforce.{api}.{op}"
        ops.append({"provider": "salesforce", "tool_name": tool_name,
                    "http_method": http, "verb_class": classify(op, http)})
    return _finish("salesforce", ops, {"salesforce": len(ops)})


# ── snapshot writer + provenance ──
def _finish(source, ops, providers, extra=None):
    # dedupe by tool_name (deterministic), sort
    seen = {}
    for o in ops:
        seen[o["tool_name"]] = o
    ops = [seen[k] for k in sorted(seen)]
    by_class = {}
    for o in ops:
        by_class[o["verb_class"]] = by_class.get(o["verb_class"], 0) + 1
    snap = {"source": source, "count": len(ops),
            "providers": {k: providers[k] for k in sorted(providers)},
            "verb_class": {k: by_class[k] for k in sorted(by_class)},
            "ops": ops}
    if extra:
        snap["extra"] = extra
    p = os.path.join(APIS, source, "operations.json")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        json.dump(snap, f, indent=0, sort_keys=True)
    return snap


def _sha_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


BUILDERS = {
    "botocore": build_botocore, "apisguru": build_apisguru,
    "google_discovery": build_google_discovery, "telegram": build_telegram,
    "apple": build_apple, "meta_graph": build_meta_graph, "salesforce": build_salesforce,
}
FETCHERS = {
    "apisguru": fetch_apisguru, "google_discovery": fetch_google_discovery,
    "telegram": fetch_telegram, "apple": fetch_apple,
}


def build_only(manifest=None):
    """Parse local snapshots (offline) -> per-source operations.json + merge provenance into manifest."""
    results = {}
    for name, fn in BUILDERS.items():
        try:
            snap = fn()
        except Exception as e:
            print(f"SKIP {name}: {e}")
            results[name] = {"status": f"error: {e}"}
            continue
        opsp = os.path.join(APIS, name, "operations.json")
        meta = SOURCES[name]
        prov = {"url": meta["url"], "license": meta["license"],
                "snapshot": os.path.relpath(opsp, COR),
                "snapshot_sha256": _sha_of(opsp), "operations": snap["count"],
                "providers": len(snap["providers"]), "verb_class": snap["verb_class"],
                "status": "ok"}
        if snap.get("extra"):
            prov.update({k: v for k, v in snap["extra"].items()})
        results[name] = prov
        top = sorted(snap["providers"].items(), key=lambda kv: -kv[1])[:3]
        print(f"OK   {name:17} ops={snap['count']:<7} providers={len(snap['providers']):<4} "
              f"classes={snap['verb_class']} top={top}")
    if manifest is not None:
        manifest.setdefault("apis", {}).update(results)
    return results


def fetch_all():
    os.makedirs(APIS, exist_ok=True)
    for name, fn in FETCHERS.items():
        try:
            fn()
        except Exception as e:
            print(f"SKIP fetch {name}: {e}")


def main():
    build_only_mode = "--build-only" in sys.argv
    if not build_only_mode:
        fetch_all()
    # merge into the shared manifest (preserve build_corpora's sources)
    mp = os.path.join(COR, "manifest.json")
    manifest = {}
    if os.path.exists(mp):
        try:
            manifest = json.load(open(mp))
        except Exception:
            manifest = {}
    build_only(manifest)
    with open(mp, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    total = sum(v.get("operations", 0) for v in manifest.get("apis", {}).values()
                if isinstance(v, dict))
    print(f"\napi surface: {total} operations across "
          f"{len(manifest.get('apis', {}))} sources; manifest -> corpora/manifest.json")


if __name__ == "__main__":
    main()
