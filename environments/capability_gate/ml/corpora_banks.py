"""Load the vendored corpora snapshots (built by build_corpora.py) and expose enriched,
deterministic banks for the capability-gate generator.

Pure + offline: reads only ml/corpora/<source>/<snapshot>.json (no network, no wall-clock).
Everything is sorted so unions are stable. If a snapshot is absent, that source degrades to
empty and the generator falls back to its hand-authored banks (see gen_v2.py wire-in).

Exposes:
  AVAILABLE          — True if at least one snapshot loaded
  TOOL_KINDS         — {capability_kind: [mined tool binaries/names]} (LOLBAS+GTFOBins+tldr+MCP)
  CLOUD_DENY/CLOUD_OK— real AWS+GCP IAM actions, verb-classified (write/privesc vs read)
  MCP_TOOL_NAMES     — real MCP tool identifiers (naming-convention diversity)
  PKG_OK             — top real PyPI package names
  enrich_tools(base) — union base TOOLS with TOOL_KINDS (per kind), sorted
"""
import hashlib
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
COR = os.path.join(HERE, "corpora")


def _load(rel):
    p = os.path.join(COR, rel)
    if not os.path.exists(p):
        return None
    try:
        with open(p) as f:
            return json.load(f)
    except Exception:
        return None


# Per-kind cap on mined tool binaries. The generator's holdout axis is the tool binary; with
# ~12k rows a family touches each of its kind's tools only a few times, so an unbounded pool
# (LOLBAS+GTFOBins put 400+ binaries in some kinds) makes most tools appear ~2x. That starves
# the tool-name shortcut probe into a small-sample split artifact (each rare tool's single
# allow lands in train, its single deny in test), pushing its AUC away from 0.5 even though
# every tool is exactly label-balanced. Capping to an evenly-sampled subset keeps hundreds of
# distinct binaries (still >>79) while giving each enough rows for the probe to read ~0.5.
MAX_PER_KIND = int(os.environ.get("CORPORA_MAX_PER_KIND", "40"))


def _even(names, k):
    """Deterministic evenly-spaced subset of a sorted list (representative, not alphabetical)."""
    names = sorted(names)
    if len(names) <= k:
        return names
    step = len(names) / k
    return [names[int(i * step)] for i in range(k)]


def _merge_kinds(dst, src_kinds):
    if not src_kinds:
        return
    for k, names in src_kinds.items():
        dst.setdefault(k, set()).update(names)


# ── mined tool-binary banks, merged per capability kind ──
_kinds = {}
_lolbas = _load("lolbas/lolbas.json")
_gtfo = _load("gtfobins/gtfobins.json")
_tldr = _load("tldr/binaries.json")
_mcp = _load("mcp/tool_names.json")
if _lolbas:
    _merge_kinds(_kinds, _lolbas.get("kinds"))
if _gtfo:
    _merge_kinds(_kinds, _gtfo.get("kinds"))
if _tldr:
    _merge_kinds(_kinds, _tldr.get("kinds"))
if _mcp:
    _merge_kinds(_kinds, _mcp.get("kinds"))
TOOL_KINDS = {k: _even(v, MAX_PER_KIND) for k, v in _kinds.items()}

# ── cloud IAM allow/deny (biggest lever): AWS + GCP verb-classified actions ──
_aws = _load("aws_iam/actions.json") or {}
_gcp = _load("gcp_iam/permissions.json") or {}
CLOUD_DENY = sorted(set(_aws.get("deny", [])) | set(_gcp.get("deny", [])))
CLOUD_OK = sorted(set(_aws.get("allow", [])) | set(_gcp.get("allow", [])))

# ── MCP tool identifiers + PyPI benign package names ──
MCP_TOOL_NAMES = sorted((_mcp or {}).get("tool_names", []))
_pypi = _load("pypi/pkg_ok.json") or {}
PKG_OK = sorted(_pypi.get("packages", []))

# ── REAL public-API surface (botocore/apis.guru/google/telegram/apple/meta/salesforce): tens of
# thousands of real operation names, verb-classified by build_api_surface.py. We bucket them into
# the generator's capability KINDS so every family draws real tool names (each used for BOTH labels
# by gen_v2's family logic, preserving the balance invariant). Cloud IAM ops feed CLOUD_OK/DENY. ──
# Separate, larger cap than the security-binary banks (which stay at MAX_PER_KIND=40): the API
# surface is label-balanced per-tool by construction, so a bigger pool grows tool diversity into the
# thousands while the tool-name shortcut probe stays ~chance. Env-overridable.
MAX_API_PER_KIND = int(os.environ.get("CORPORA_MAX_API_PER_KIND", "300"))
MAX_CLOUD_ACTIONS = int(os.environ.get("CORPORA_MAX_CLOUD", "1500"))

_API = os.path.join(COR, "apis")
_api_ops = []
if os.path.isdir(_API):
    for _src in sorted(os.listdir(_API)):
        _snap = _load(os.path.join("apis", _src, "operations.json"))
        if _snap and _snap.get("ops"):
            _api_ops.extend(_snap["ops"])


def _bucket(tag, name, choices):
    """Deterministic, content-addressed bucket pick (no set-iteration / wall-clock nondeterminism)."""
    h = int(hashlib.sha256(f"{tag}:{name}".encode()).hexdigest(), 16)
    return choices[h % len(choices)]


def _is_cloud(prov):
    prov = (prov or "").lower()
    return prov in ("aws", "google", "gcp") or "azure" in prov or prov.startswith("microsoft")


_DB_HINT = re.compile(r"(table|database|schema|collection|index|record|\brow\b|dataset|document|"
                      r"bucket|blob|object|query|\bsql\b|datastore|keyspace)", re.I)
_PAY_HINT = re.compile(r"(payment|payout|\bpay\b|charge|refund|invoice|checkout|billing|"
                       r"subscription|transaction|transfer|wallet|payee|disburse|settlement)", re.I)

_api_kinds = {}
_capi_ok, _capi_deny = set(), set()
for _o in _api_ops:
    _name = _o.get("tool_name")
    _vc = _o.get("verb_class")
    _prov = _o.get("provider", "")
    if not _name:
        continue
    if _vc == "ADMIN":                                   # privesc/policy/role -> deny cloud bank
        _capi_deny.add(_name)
    elif _vc == "READ":
        if _is_cloud(_prov):                             # read-only cloud call -> allow cloud bank
            _capi_ok.add(_name)
        else:
            _api_kinds.setdefault(_bucket("r", _name, ("http_get", "db")), set()).add(_name)
    elif _vc == "DESTROY":
        _api_kinds.setdefault("db" if _DB_HINT.search(_name) else "delete", set()).add(_name)
    elif _vc == "EGRESS":
        if _PAY_HINT.search(_name):
            _api_kinds.setdefault("payment", set()).add(_name)
        else:
            _api_kinds.setdefault(_bucket("e", _name, ("notify", "http_post")), set()).add(_name)
    else:                                                # WRITE / MUTATE
        if _PAY_HINT.search(_name):
            _api_kinds.setdefault(_bucket("wp", _name, ("payment", "http_post")), set()).add(_name)
        else:
            _api_kinds.setdefault(_bucket("w", _name, ("http_post", "notify")), set()).add(_name)

# fold API tool names into TOOL_KINDS (post the security-binary cap; own larger cap), sorted.
# MAX_API_PER_KIND<=0 disables the API-surface merge (falls back to security banks only).
if MAX_API_PER_KIND > 0:
    for _k, _names in _api_kinds.items():
        _capped = _even(sorted(_names), MAX_API_PER_KIND)
        TOOL_KINDS[_k] = sorted(set(TOOL_KINDS.get(_k, [])) | set(_capped))
    # fold cloud IAM API actions into the allow/deny banks (even-sampled cap, deterministic).
    CLOUD_OK = sorted(set(CLOUD_OK) | set(_even(sorted(_capi_ok), MAX_CLOUD_ACTIONS)))
    CLOUD_DENY = sorted(set(CLOUD_DENY) | set(_even(sorted(_capi_deny), MAX_CLOUD_ACTIONS)))

API_OPS_TOTAL = len(_api_ops)
API_TOOL_NAMES = sorted({o["tool_name"] for o in _api_ops if o.get("tool_name")})

AVAILABLE = bool(TOOL_KINDS or CLOUD_DENY or CLOUD_OK or MCP_TOOL_NAMES or PKG_OK)


def enrich_tools(base):
    """Return a new TOOLS dict = per-kind union(hand-authored, mined), sorted (deterministic)."""
    out = {}
    for k in sorted(set(base) | set(TOOL_KINDS)):
        out[k] = sorted(set(base.get(k, [])) | set(TOOL_KINDS.get(k, [])))
    return out


def summary():
    return {"available": AVAILABLE,
            "tool_kinds": {k: len(v) for k, v in TOOL_KINDS.items()},
            "cloud_deny": len(CLOUD_DENY), "cloud_ok": len(CLOUD_OK),
            "mcp_tool_names": len(MCP_TOOL_NAMES), "pkg_ok": len(PKG_OK),
            "api_ops_total": API_OPS_TOTAL, "api_tool_names": len(API_TOOL_NAMES)}


if __name__ == "__main__":
    print(json.dumps(summary(), indent=2))
