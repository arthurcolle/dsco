"""One-shot vendoring: parse the raw public corpora downloaded under ml/corpora/<source>/
into normalized, sanitized, verb-classified bank snapshots + a provenance manifest.

Run ONCE (operates offline on already-downloaded raw files):  python build_corpora.py
Emits ml/corpora/<source>/<snapshot>.json (sorted, deterministic) and ml/corpora/manifest.json
(source URL + sha256 of the raw artifact + license + row counts). corpora_banks.py then loads
ONLY the normalized snapshots, so generation stays offline + deterministic.

SAFETY: any live/abuse URL, attacker host, or public sink is rewritten to RFC5737 TEST-NET /
*.example.* by sanitize(); no live malicious URL is vendored into a consumable bank.
"""
import glob
import hashlib
import json
import os
import re

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
COR = os.path.join(HERE, "corpora")

SOURCES = {
    "lolbas": {
        "url": "https://lolbas-project.github.io/api/lolbas.json",
        "raw": "lolbas/raw.json",
        "license": "CC-BY-4.0 (LOLBAS Project)",
    },
    "gtfobins": {
        "url": "https://github.com/GTFOBins/GTFOBins.github.io (tar master)",
        "raw": "gtfobins/repo.tar.gz",
        "license": "CC-BY-NC-SA-4.0 (GTFOBins)",
    },
    "aws_iam": {
        "url": "https://raw.githubusercontent.com/iann0036/iam-dataset/main/aws/map.json",
        "raw": "aws_iam/map.json",
        "license": "MIT (iann0036/iam-dataset)",
    },
    "gcp_iam": {
        "url": "https://raw.githubusercontent.com/darkbitio/gcp-iam-role-permissions/master/gcp_roles_cai.json",
        "raw": "gcp_iam/roles_cai.json",
        "license": "Apache-2.0 (darkbitio/gcp-iam-role-permissions)",
    },
    "tldr": {
        "url": "https://github.com/tldr-pages/tldr (tar main)",
        "raw": "tldr/repo.tar.gz",
        "license": "CC-BY-4.0 (tldr-pages)",
    },
    "pypi": {
        "url": "https://hugovk.github.io/top-pypi-packages/top-pypi-packages.min.json",
        "raw": "pypi/top.json",
        "license": "MIT (hugovk/top-pypi-packages, PyPI public names)",
    },
    "mcp": {
        "url": "capability_gate ML environment tool list + registry.modelcontextprotocol.io",
        "raw": "mcp/env_tools.json",
        "license": "public tool identifiers (MCP servers)",
    },
}

# ── sanitizer: rewrite any live abuse host/URL to RFC5737 TEST-NET + *.example.* ──
_SANITIZE = [
    (re.compile(r"\battacker\.com\b"), "attacker.example.com"),
    (re.compile(r"\btarget\.com\b"), "target.example.com"),
    (re.compile(r"\bexample\.com\b"), "example.com"),
    (re.compile(r"\b10\.10\.1[0-9]\.\d{1,3}\b"), "203.0.113.10"),
    (re.compile(r"\b10\.10\.\d{1,3}\.\d{1,3}\b"), "203.0.113.20"),
    (re.compile(r"\b192\.168\.\d{1,3}\.\d{1,3}\b"), "198.51.100.30"),
    (re.compile(r"\bLHOST\b|\bRHOST\b"), "203.0.113.5"),
    (re.compile(r"\bevil\.com\b"), "evil.example.com"),
]


def sanitize(s):
    if not isinstance(s, str):
        return s
    for pat, repl in _SANITIZE:
        s = pat.sub(repl, s)
    # any remaining bare http(s):// host that is not example/test-net -> example placeholder
    def _host(m):
        host = m.group(2)
        if host.endswith(".example.com") or host.endswith(".example") or host.startswith("203.0.113") \
                or host.startswith("198.51.100") or host.startswith("192.0.2") or host in ("localhost", "127.0.0.1"):
            return m.group(0)
        return f"{m.group(1)}sink.example.com"
    s = re.sub(r"(https?://)([A-Za-z0-9.\-]+)", _host, s)
    return s


def _kebab(v):
    return re.sub(r"(?<!^)(?=[A-Z])", "-", v).lower().replace("_", "-")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _write(rel, obj):
    p = os.path.join(COR, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        json.dump(obj, f, indent=0, sort_keys=True)
    return rel


def _tok(binary):
    """Shell token for a binary name (single token, lowercased, path stripped)."""
    b = os.path.basename(str(binary)).strip()
    return b


# ── LOLBAS: (binary, category, command) ; category -> capability kind ──
LOLBAS_KIND = {"Download": "http_get", "Upload": "http_post", "Copy": "http_post", "ADS": "http_post"}


def build_lolbas():
    d = json.load(open(os.path.join(COR, "lolbas/raw.json")))
    rows, kinds = [], {}
    for e in d:
        binary = _tok(e.get("Name", ""))
        for c in e.get("Commands", []):
            cat = c.get("Category", "")
            kind = LOLBAS_KIND.get(cat, "remote")  # Execute/AWL/UAC/Dump/... -> remote (exec)
            rows.append({"binary": binary, "category": cat,
                         "command": sanitize(c.get("Command", ""))})
            kinds.setdefault(kind, set()).add(binary)
    snap = {"rows": sorted(rows, key=lambda r: (r["binary"], r["category"], r["command"])),
            "kinds": {k: sorted(v) for k, v in kinds.items()}}
    _write("lolbas/lolbas.json", snap)
    return len(rows), {k: len(v) for k, v in kinds.items()}


# ── GTFOBins: (binary, function, command) ; function -> capability kind ──
GTFO_KIND = {"download": "http_get", "upload": "http_post", "file-read": "http_post",
             "file-write": "http_post", "shell": "remote", "command": "remote",
             "reverse-shell": "remote", "bind-shell": "remote"}


def _ensure_extracted(tarball, marker):
    """Extract a vendored tarball next to itself if its tree is absent (offline, idempotent)."""
    if os.path.isdir(os.path.join(COR, marker)):
        return
    import tarfile
    with tarfile.open(os.path.join(COR, tarball)) as t:
        t.extractall(os.path.join(COR, os.path.dirname(tarball)))


def build_gtfobins():
    _ensure_extracted("gtfobins/repo.tar.gz", "gtfobins/GTFOBins.github.io-master")
    rows, kinds = [], {}
    base = os.path.join(COR, "gtfobins/GTFOBins.github.io-master/_gtfobins")
    for p in sorted(glob.glob(os.path.join(base, "*"))):
        if os.path.isdir(p):
            continue
        try:
            y = yaml.safe_load(open(p))
        except Exception:
            continue
        if not y or "functions" not in y:
            continue
        binary = _tok(os.path.basename(p))
        for fn, entries in (y["functions"] or {}).items():
            kind = GTFO_KIND.get(fn)
            for e in (entries or []):
                code = sanitize((e or {}).get("code", "")) if isinstance(e, dict) else ""
                rows.append({"binary": binary, "function": fn, "command": code})
            if kind:
                kinds.setdefault(kind, set()).add(binary)
    snap = {"rows": sorted(rows, key=lambda r: (r["binary"], r["function"], r["command"])),
            "kinds": {k: sorted(v) for k, v in kinds.items()}}
    _write("gtfobins/gtfobins.json", snap)
    return len(rows), {k: len(v) for k, v in kinds.items()}


# ── AWS IAM: service:Verb -> read (allow) / write+privesc (deny), rendered CLI-ish ──
READ_PREFIX = ("Get", "List", "Describe", "Search", "Query", "Head", "Lookup", "Scan",
               "View", "Select", "Preview", "Estimate", "Check", "Test", "Validate",
               "Simulate", "Detect", "Discover", "Retrieve", "Read", "Count")
WRITE_PREFIX = ("Create", "Delete", "Put", "Attach", "Detach", "Authorize", "Update",
                "Modify", "Remove", "Add", "Set", "Enable", "Disable", "Grant", "Revoke",
                "Associate", "Disassociate", "Terminate", "Reboot", "Stop", "Start",
                "Deploy", "Publish", "Invoke", "Assume", "Pass", "Write", "Replace",
                "Reset", "Restore", "Import", "Deactivate", "Activate", "Register",
                "Deregister", "Accept", "Reject", "Send", "Run", "Execute", "Rotate",
                "Provision", "Purchase", "Release", "Apply", "Change", "Copy", "Move")


def _aws_verb(action):
    svc, _, verb = action.partition(":")
    return svc, verb


def _sample(sorted_list, cap):
    if len(sorted_list) <= cap:
        return sorted_list
    step = len(sorted_list) / cap
    return [sorted_list[int(i * step)] for i in range(cap)]


def build_aws_iam():
    d = json.load(open(os.path.join(COR, "aws_iam/map.json")))
    acts = set()
    for v in d["sdk_method_iam_mappings"].values():
        for e in v:
            a = e.get("action") if isinstance(e, dict) else None
            if a and ":" in a:
                acts.add(a)
    allow, deny = set(), set()
    for a in acts:
        svc, verb = _aws_verb(a)
        if not verb:
            continue
        cli = f"{svc} {_kebab(verb)}"
        # privesc / policy writes are unambiguously deny
        privesc = (svc == "iam" and verb.startswith(("Create", "Attach", "Put", "Add", "Update",
                   "Delete", "Remove", "Detach"))) or verb.endswith("Policy") or "AccessKey" in verb \
            or "LoginProfile" in verb
        if verb.startswith(READ_PREFIX) and not privesc:
            allow.add(cli)
        elif verb.startswith(WRITE_PREFIX) or privesc:
            deny.add(cli)
    allow = _sample(sorted(allow), 500)
    deny = _sample(sorted(deny), 700)
    _write("aws_iam/actions.json", {"provider": "aws", "allow": allow, "deny": deny,
                                    "n_actions": len(acts)})
    return len(acts), {"allow": len(allow), "deny": len(deny)}


# ── GCP IAM: service.resource.verb -> read (allow) / write (deny), gcloud-ish ──
GCP_READ = {"get", "list", "getIamPolicy", "check", "aggregatedList", "search", "watch",
            "view", "report", "export", "queryGrantableRoles", "generateAccessToken",
            "resolve", "lookup", "validate", "test", "read", "count", "getPublicKey"}
GCP_WRITE = {"create", "delete", "update", "set", "setIamPolicy", "use", "attach", "bind",
             "actAs", "impersonate", "patch", "add", "remove", "start", "stop", "reset",
             "restart", "deploy", "publish", "run", "execute", "write", "import", "restore",
             "enable", "disable", "grant", "revoke", "cancel", "move", "undelete"}


def build_gcp_iam():
    perms = set()
    with open(os.path.join(COR, "gcp_iam/roles_cai.json")) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except Exception:
                continue
            for p in r.get("includedPermissions", []):
                perms.add(p)
    allow, deny = set(), set()
    for p in perms:
        parts = p.split(".")
        if len(parts) < 2:
            continue
        verb = parts[-1]
        body = " ".join(parts[:-1])
        cli = f"{body} {_kebab(verb)}"
        if verb in GCP_READ:
            allow.add(cli)
        elif verb in GCP_WRITE or verb.startswith(("set", "create", "delete", "update")):
            deny.add(cli)
    allow = _sample(sorted(allow), 300)
    deny = _sample(sorted(deny), 300)
    _write("gcp_iam/permissions.json", {"provider": "gcp", "allow": allow, "deny": deny,
                                        "n_permissions": len(perms)})
    return len(perms), {"allow": len(allow), "deny": len(deny)}


# ── tldr: benign invocation binaries, keyword-classified into capability kinds ──
TLDR_KIND = {
    "http_get": {"curl", "wget", "http", "https", "xh", "aria2c", "lynx", "w3m", "httpie",
                 "curlie", "fetch", "wget2", "youtube-dl", "yt-dlp", "httpx"},
    "http_post": {"http", "curl", "wget"},
    "remote": {"ssh", "telnet", "mosh", "sshpass", "autossh", "dbclient", "rsh", "slogin",
               "eternalterminal", "et"},
    "copy": {"scp", "rsync", "sftp", "rclone", "gsutil", "s3cmd", "lftp", "ncftp"},
    "pkg": {"pip", "pip3", "pipx", "uv", "npm", "pnpm", "yarn", "gem", "cargo", "go", "apt",
            "apt-get", "brew", "conda", "poetry", "composer", "gradle", "mvn", "nuget",
            "dotnet", "apk", "dnf", "yum", "pacman", "zypper", "snap", "flatpak", "bundle",
            "cpan", "cpanm", "opam", "nix-env", "guix", "port", "choco", "scoop", "winget"},
    "db": {"mysql", "psql", "sqlite3", "mongosh", "mongo", "redis-cli", "cqlsh", "cockroach",
           "influx", "clickhouse-client", "duckdb", "mariadb", "sqlplus", "isql", "usql",
           "pgcli", "mycli", "litecli"},
    "cloud": {"aws", "gcloud", "az", "oci", "doctl", "ibmcloud", "aliyun", "linode-cli",
              "flyctl", "heroku", "gsutil", "bq", "eksctl", "terraform", "pulumi"},
    "container": {"docker", "podman", "nerdctl", "ctr", "lima", "singularity", "apptainer",
                  "buildah", "crictl", "kubectl", "helm", "skopeo", "img", "kaniko"},
    "notify": {"mail", "mailx", "sendmail", "mutt", "msmtp", "notify-send", "slack", "curl"},
    "delete": {"rm", "shred", "srm", "unlink", "rmdir", "wipe", "trash", "gio"},
}


def build_tldr():
    _ensure_extracted("tldr/repo.tar.gz", "tldr/tldr-main")
    pages = set()
    for p in glob.glob(os.path.join(COR, "tldr/tldr-main/pages/*/*.md")):
        pages.add(os.path.splitext(os.path.basename(p))[0])
    kinds = {}
    for kind, names in TLDR_KIND.items():
        present = sorted(names & pages)
        if present:
            kinds[kind] = present
    _write("tldr/binaries.json", {"n_pages": len(pages), "kinds": kinds})
    return len(pages), {k: len(v) for k, v in kinds.items()}


# ── PyPI top packages -> PKG_OK ──
def build_pypi():
    d = json.load(open(os.path.join(COR, "pypi/top.json")))
    rows = d["rows"] if isinstance(d, dict) else d
    names = [r["project"] for r in rows[:800] if re.match(r"^[A-Za-z0-9._-]+$", r["project"])]
    names = sorted(set(names))
    _write("pypi/pkg_ok.json", {"packages": names})
    return len(rows), {"pkg_ok": len(names)}


# ── MCP tool names (env harvest + registry servers) -> naming-diverse tool identifiers ──
def build_mcp():
    env = json.load(open(os.path.join(COR, "mcp/env_tools.json")))["tools"]
    reg_path = os.path.join(COR, "mcp/registry_servers.json")
    reg = json.load(open(reg_path))["names"] if os.path.exists(reg_path) else []
    names = sorted(set(env))
    # classify by the leaf verb into both-label capability kinds
    kinds = {"notify": set(), "db": set(), "http_get": set(), "remote": set(), "http_post": set(),
             "delete": set()}
    for n in names:
        leaf = n.split("__")[-1].lower()
        if any(k in leaf for k in ("send", "publish", "notify", "email", "draft", "forward", "reply")):
            kinds["notify"].add(n)
        elif any(k in leaf for k in ("sql", "query", "run_sql", "database", "table", "migration")):
            kinds["db"].add(n)
        elif any(k in leaf for k in ("delete", "expunge", "discard", "remove", "cancel", "vacuum")):
            kinds["delete"].add(n)
        elif any(k in leaf for k in ("run", "exec", "execute", "batch_execute")):
            kinds["remote"].add(n)
        elif any(k in leaf for k in ("upload", "export", "put", "attach", "copy")):
            kinds["http_post"].add(n)
        else:  # fetch/get/list/search/read/download/... default to http_get
            kinds["http_get"].add(n)
    snap = {"tool_names": names, "registry_servers": sorted(set(reg)),
            "kinds": {k: sorted(v) for k, v in kinds.items()}}
    _write("mcp/tool_names.json", snap)
    return len(names), {k: len(v) for k, v in kinds.items()}


def main():
    manifest = {"generated_by": "build_corpora.py", "sources": {}}
    builders = {"lolbas": build_lolbas, "gtfobins": build_gtfobins, "aws_iam": build_aws_iam,
                "gcp_iam": build_gcp_iam, "tldr": build_tldr, "pypi": build_pypi, "mcp": build_mcp}
    for name, fn in builders.items():
        meta = SOURCES[name]
        raw = os.path.join(COR, meta["raw"])
        if not os.path.exists(raw):
            print(f"SKIP {name}: raw artifact missing ({meta['raw']})")
            manifest["sources"][name] = {**meta, "status": "skipped-missing-raw"}
            continue
        try:
            n_rows, kinds = fn()
        except Exception as e:
            print(f"SKIP {name}: {e}")
            manifest["sources"][name] = {**meta, "status": f"error: {e}"}
            continue
        manifest["sources"][name] = {
            "url": meta["url"], "license": meta["license"], "raw": meta["raw"],
            "raw_sha256": sha256(raw), "rows": n_rows, "kinds": kinds, "status": "ok",
        }
        print(f"OK   {name:9} rows={n_rows:<7} kinds={kinds}")
    # fold the vendored public-API surface (build_api_surface.py) into the same manifest, parsing
    # its local snapshots offline. Keeps `python build_corpora.py` a single, idempotent pipeline
    # that preserves the `apis` provenance instead of clobbering it. Network fetch is a separate
    # step (`python build_api_surface.py`); here we only re-derive from already-vendored snapshots.
    try:
        import build_api_surface
        build_api_surface.build_only(manifest)
    except Exception as e:
        print(f"SKIP api_surface: {e}")
    with open(os.path.join(COR, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    print(f"\nwrote {os.path.join('corpora', 'manifest.json')}")


if __name__ == "__main__":
    main()
