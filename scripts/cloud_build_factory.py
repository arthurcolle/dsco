#!/usr/bin/env python3
"""Deterministic, server-side build factory for one immutable DSCO RuntimeSpec.

The request schema is intentionally closed: customer credentials cannot be
accepted, echoed, or copied into the resulting bundle.  Signing keys are
external file references used only when ``--build`` is explicitly requested.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
IDENT = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}\Z")
HOST = re.compile(r"[A-Za-z0-9.-]{1,253}\Z")


class RequestError(ValueError):
    pass


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def runtime_spec_digest(spec: dict[str, Any]) -> str:
    """Digest the live RuntimeSpec excluding its self-referential integrity block."""
    unsigned = {key: value for key, value in spec.items() if key != "integrity"}
    return hashlib.sha256(canonical_json(unsigned)).hexdigest()


def _closed_object(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        unknown = sorted(set(value) - keys) if isinstance(value, dict) else []
        raise RequestError(f"{label} must contain exactly {sorted(keys)}; unknown={unknown}")
    return value


def _b64url(value: Any, label: str, minimum: int = 1) -> bytes:
    if not isinstance(value, str) or not value or not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise RequestError(f"{label} must be unpadded base64url")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except ValueError as exc:
        raise RequestError(f"{label} is not valid base64url") from exc
    if len(decoded) < minimum:
        raise RequestError(f"{label} is too short")
    return decoded


def _string_list(value: Any, label: str, pattern: re.Pattern[str], maximum: int = 128) -> list[str]:
    if not isinstance(value, list) or not value or len(value) > maximum:
        raise RequestError(f"{label} must be a non-empty list of at most {maximum} values")
    if any(not isinstance(item, str) or not pattern.fullmatch(item) for item in value):
        raise RequestError(f"{label} contains an invalid value")
    if len(set(value)) != len(value):
        raise RequestError(f"{label} must not contain duplicates")
    return value


def _validate_runtime_spec(spec: Any) -> tuple[dict[str, Any], dict[str, str]]:
    spec = _closed_object(spec, {
        "apiVersion", "kind", "metadata", "connections", "routing", "tools",
        "governance", "build", "deployment", "network", "integrity",
    }, "runtime_spec")
    if spec["apiVersion"] != "distributed.systems/v1alpha1" or spec["kind"] != "RuntimeSpec":
        raise RequestError("runtime_spec must be distributed.systems/v1alpha1 RuntimeSpec")
    metadata = _closed_object(spec["metadata"], {"name", "reproducible"}, "runtime_spec.metadata")
    if not isinstance(metadata["name"], str) or not IDENT.fullmatch(metadata["name"]) or metadata["reproducible"] is not True:
        raise RequestError("runtime_spec.metadata must carry a reproducible identifier")
    if not isinstance(spec["connections"], list) or not spec["connections"]:
        raise RequestError("runtime_spec.connections must be non-empty")
    connection_providers: list[str] = []
    for index, connection in enumerate(spec["connections"]):
        connection = _closed_object(connection, {"provider", "ref", "credentialMaterial"}, f"runtime_spec.connections[{index}]")
        provider = connection["provider"]
        if not isinstance(provider, str) or not IDENT.fullmatch(provider):
            raise RequestError("runtime_spec connection provider is invalid")
        expected_ref = "local://discovery" if provider == "local" else f"account://{provider}"
        if connection["ref"] != expected_ref or connection["credentialMaterial"] != "excluded":
            raise RequestError("runtime_spec connections must be non-secret account references")
        connection_providers.append(provider)
    if len(set(connection_providers)) != len(connection_providers):
        raise RequestError("runtime_spec connections must not duplicate a provider")
    routing = _closed_object(spec["routing"], {"primary", "fallback", "eligibleProviders", "routes"}, "runtime_spec.routing")
    primary = _closed_object(routing["primary"], {"provider", "model"}, "runtime_spec.routing.primary")
    if primary["provider"] not in connection_providers or not isinstance(primary["model"], str) or not primary["model"].startswith(f"{primary['provider']}/"):
        raise RequestError("runtime_spec routing primary must select a connected provider and its model")
    if routing["fallback"] not in {"fail-closed", "next-connected"} or routing["eligibleProviders"] != connection_providers:
        raise RequestError("runtime_spec routing must preserve its connected provider order")
    if not isinstance(routing["routes"], list) or len(routing["routes"]) != len(connection_providers):
        raise RequestError("runtime_spec routing must include one ordered route per connected provider")
    route_models: list[str] = []
    for index, route in enumerate(routing["routes"]):
        route = _closed_object(route, {"provider", "model"}, f"runtime_spec.routing.routes[{index}]")
        provider = connection_providers[index]
        if route["provider"] != provider or not isinstance(route["model"], str) or not route["model"].startswith(f"{provider}/"):
            raise RequestError("runtime_spec route/model order must match eligibleProviders")
        route_models.append(route["model"])
    if len(set(route_models)) != len(route_models) or route_models[connection_providers.index(primary["provider"])] != primary["model"]:
        raise RequestError("runtime_spec primary route must match its ordered provider model")
    if not isinstance(spec["tools"], list) or not spec["tools"]:
        raise RequestError("runtime_spec.tools must be non-empty")
    tool_ids: list[str] = []
    for index, tool in enumerate(spec["tools"]):
        tool = _closed_object(tool, {"id", "scope"}, f"runtime_spec.tools[{index}]")
        if tool["scope"] != "runtime-policy" or tool["id"] not in TOOL_CEILINGS:
            raise RequestError("runtime_spec.tools contains an unsupported cloud tool policy identifier")
        tool_ids.append(tool["id"])
    if len(set(tool_ids)) != len(tool_ids):
        raise RequestError("runtime_spec.tools must not contain duplicates")
    governance = _closed_object(spec["governance"], {"preset", "approvals", "perRunBudgetUsd", "secretIsolation", "chronicle"}, "runtime_spec.governance")
    if governance["preset"] not in {"guarded", "balanced", "autonomous"} or not isinstance(governance["approvals"], list) or not all(isinstance(item, str) and IDENT.fullmatch(item) for item in governance["approvals"]):
        raise RequestError("runtime_spec.governance contains invalid policy")
    if not isinstance(governance["perRunBudgetUsd"], (int, float)) or isinstance(governance["perRunBudgetUsd"], bool) or governance["perRunBudgetUsd"] <= 0:
        raise RequestError("runtime_spec.governance.perRunBudgetUsd must be positive")
    if governance["secretIsolation"] != "account-vault-reference-only" or governance["chronicle"] != "required":
        raise RequestError("runtime_spec governance must preserve secret isolation and Chronicle")
    build = _closed_object(spec["build"], {"artifact", "isolation", "hardening", "reproducibleFrom"}, "runtime_spec.build")
    if build["artifact"] not in {"native-binary", "signed-runtime-bundle"} or build["isolation"] != "customer-specific" or build["reproducibleFrom"] != "this-runtime-spec" or not isinstance(build["hardening"], list) or not all(isinstance(item, str) and IDENT.fullmatch(item) for item in build["hardening"]):
        raise RequestError("runtime_spec.build contains invalid customer-specific build policy")
    deployment = _closed_object(spec["deployment"], {"target", "authority"}, "runtime_spec.deployment")
    if deployment["target"] not in {"dsco-cloud", "customer-cloud", "local-edge"}:
        raise RequestError("runtime_spec.deployment target is invalid")
    expected_authority = "local-runtime" if deployment["target"] == "local-edge" else "signed-runtime-spec"
    if deployment["authority"] != expected_authority:
        raise RequestError("runtime_spec.deployment authority does not match target")
    network = _closed_object(spec["network"], {"allowedHosts"}, "runtime_spec.network")
    hosts = _string_list(network["allowedHosts"], "runtime_spec.network.allowedHosts", HOST)
    if any(host.startswith(".") for host in hosts):
        raise RequestError("runtime_spec.network.allowedHosts contains an invalid host")
    if "tools.distributed.systems" not in hosts:
        raise RequestError("runtime_spec.network.allowedHosts must include tools.distributed.systems")
    integrity = _closed_object(spec["integrity"], {"canonicalization", "digest", "signature", "artifactAttestation"}, "runtime_spec.integrity")
    digest = runtime_spec_digest(spec)
    if integrity["canonicalization"] != "JCS-compatible" or integrity["digest"] != f"sha256:{digest}" or integrity["signature"] != "issued-after-account-approval" or integrity["artifactAttestation"] != "issued-with-customer-build":
        raise RequestError("runtime_spec.integrity does not match canonical RuntimeSpec content")
    ceilings = {
        "allowed_hosts": ",".join(sorted(hosts)),
        "allowed_providers": ",".join(connection_providers),
        "allowed_models": ",".join(route_models),
        "allowed_tools": ",".join(tool_ids),
        "tool_allowlist": ",".join(tool for tool_id in tool_ids for tool in TOOL_CEILINGS[tool_id]),
        "primary_provider": primary["provider"],
        "primary_model": primary["model"],
        "disable_cross_provider_routing": "1" if routing["fallback"] == "fail-closed" else "0",
        "session_budget_usd": format(governance["perRunBudgetUsd"], ".15g"),
    }
    return spec, ceilings


def validate_runtime_spec(spec: Any) -> tuple[dict[str, Any], dict[str, str], str]:
    """Validate the public, credential-free RuntimeSpec surface.

    The build request and activation lease deliberately remain private to the
    signer/build worker.  Control-plane callers can use this narrow helper to
    validate a configurator payload without ever accepting a lease, a signing
    key, or customer credentials.
    """
    validated, ceilings = _validate_runtime_spec(spec)
    return validated, ceilings, runtime_spec_digest(validated)


TOOL_CEILINGS = {
    "repository": ("Read", "Glob", "Grep", "LS", "read_file", "page_file", "list_directory", "find_files", "grep_files", "file_info"),
    "web": ("WebFetch", "WebSearch", "http_request", "download_file", "tavily_search", "jina_search", "jina_read"),
}


def validate_request(request: Any) -> tuple[dict[str, Any], dict[str, Any], dict[str, str], str]:
    root = _closed_object(request, {"runtime_spec", "build_context", "activation_lease"}, "request")
    spec, ceilings = _validate_runtime_spec(root["runtime_spec"])
    context = _closed_object(root["build_context"], {"source_revision", "build_epoch", "issuer_public_key_b64", "plugin_requirement_b64"}, "build_context")
    if not isinstance(context["source_revision"], str) or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", context["source_revision"]):
        raise RequestError("build_context.source_revision must be a lowercase Git SHA")
    if not isinstance(context["build_epoch"], int) or context["build_epoch"] < 0:
        raise RequestError("build_context.build_epoch must be a non-negative integer")
    if len(_b64url(context["issuer_public_key_b64"], "build_context.issuer_public_key_b64")) != 65:
        raise RequestError("issuer_public_key_b64 must decode to an uncompressed P-256 public key")
    _b64url(context["plugin_requirement_b64"], "build_context.plugin_requirement_b64")
    digest = runtime_spec_digest(spec)
    lease = _closed_object(root["activation_lease"], {
        "schema_version", "lease_id", "subject", "plan", "issuer", "scopes",
        "issued_at", "expires_at", "runtime_spec_sha256", "signature",
    }, "activation_lease")
    if lease["schema_version"] != 1 or lease["runtime_spec_sha256"] != digest:
        raise RequestError("activation_lease must bind the computed runtime_spec_sha256")
    if not all(isinstance(lease[key], str) and lease[key] for key in ("lease_id", "subject", "plan", "issuer", "scopes", "signature")):
        raise RequestError("activation_lease identity, scopes, and signature are required strings")
    if not isinstance(lease["issued_at"], int) or not isinstance(lease["expires_at"], int) or lease["expires_at"] <= lease["issued_at"]:
        raise RequestError("activation_lease expiry must be after issuance")
    _b64url(lease["signature"], "activation_lease.signature")
    return spec, context, lease, ceilings, digest


def _run(argv: list[str], *, env: dict[str, str] | None = None) -> None:
    completed = subprocess.run(argv, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    if completed.returncode:
        # Do not propagate tool output: external tooling may include sensitive
        # server configuration. The factory result stays machine-readable.
        raise RuntimeError(f"build subprocess failed ({pathlib.Path(argv[0]).name})")


def _require_clean_source(revision: str) -> None:
    actual = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                            capture_output=True, check=True).stdout.strip()
    if not actual.startswith(revision):
        raise RequestError("runtime_spec.source_revision does not match the factory checkout")
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, text=True,
                           capture_output=True, check=True).stdout
    if dirty:
        raise RequestError("factory checkout must be clean for a deterministic build")


def build(spec: dict[str, Any], context: dict[str, Any], lease: dict[str, Any], ceilings: dict[str, str], digest: str, out_root: pathlib.Path,
          signing_key: pathlib.Path, public_key: pathlib.Path) -> dict[str, Any]:
    _require_clean_source(context["source_revision"])
    if not signing_key.is_file() or not public_key.is_file():
        raise RequestError("external release signing key and public key must be regular files")
    out_dir = out_root / f"{spec['metadata']['name']}-{digest[:16]}"
    if out_dir.exists():
        raise RequestError("output bundle already exists; RuntimeSpec bundles are immutable")
    out_dir.mkdir(parents=True)
    (out_dir / "runtime_spec.json").write_bytes(canonical_json(spec) + b"\n")
    os.chmod(out_dir / "runtime_spec.json", 0o644)
    binary = out_dir / "dsco-cloud"
    build_date = dt.datetime.fromtimestamp(context["build_epoch"], tz=dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    env = os.environ.copy()
    env["SOURCE_DATE_EPOCH"] = str(context["build_epoch"])
    _run([
        "make", "--no-print-directory", "cloud-build", f"CLOUD_BUILD_DIR={out_dir / 'obj'}",
        f"CLOUD_TARGET={binary}", f"CLOUD_ISSUER_P256_B64={context['issuer_public_key_b64']}",
        f"CLOUD_PLUGIN_REQUIREMENT_B64={context['plugin_requirement_b64']}",
        f"CLOUD_RUNTIME_SPEC_SHA256={digest}", f"BUILD_DATE={build_date}",
        f"CLOUD_ALLOWED_HOSTS_CSV={ceilings['allowed_hosts']}", f"CLOUD_ALLOWED_PROVIDERS_CSV={ceilings['allowed_providers']}",
        f"CLOUD_ALLOWED_MODELS_CSV={ceilings['allowed_models']}", f"CLOUD_ALLOWED_TOOLS_CSV={ceilings['allowed_tools']}",
        f"CLOUD_TOOL_ALLOWLIST_CSV={ceilings['tool_allowlist']}", f"CLOUD_PRIMARY_PROVIDER={ceilings['primary_provider']}",
        f"CLOUD_PRIMARY_MODEL={ceilings['primary_model']}",
        f"CLOUD_DISABLE_CROSS_PROVIDER_ROUTING={ceilings['disable_cross_provider_routing']}",
        f"CLOUD_SESSION_BUDGET_USD={ceilings['session_budget_usd']}",
    ], env=env)
    with tempfile.TemporaryDirectory(prefix="dsco-factory-lease-") as temporary:
        lease_file = pathlib.Path(temporary) / "lease.json"
        lease_file.write_bytes(canonical_json(lease) + b"\n")
        os.chmod(lease_file, 0o600)
        _run([
            "make", "--no-print-directory", "test-activation-lease-smoke",
            f"LEASE_FILE={lease_file}", f"LEASE_ISSUER_P256_B64={context['issuer_public_key_b64']}",
            f"LEASE_RUNTIME_SPEC_SHA256={digest}",
        ], env=env)
    _run([
        sys.executable, "scripts/release_hardened.py", str(binary), "--out-root", str(out_dir / "release"),
        "--name", "dsco-cloud", "--signing-key", str(signing_key), "--public-key", str(public_key),
        "--runtime-spec-digest", digest,
    ], env=env)
    manifests = sorted((out_dir / "release").glob("*/release_manifest.json"))
    if len(manifests) != 1:
        raise RuntimeError("release manifest was not produced exactly once")
    return {
        "schema_version": SCHEMA_VERSION,
        "status": "built",
        "runtime_spec_sha256": digest,
        "runtime_name": spec["metadata"]["name"],
        "lease_sha256": hashlib.sha256(canonical_json(lease)).hexdigest(),
        "bundle": {
            "path": str(out_dir), "binary": str(binary), "runtime_spec": str(out_dir / "runtime_spec.json"),
            "release_manifest": str(manifests[0]),
        },
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build one immutable cloud RuntimeSpec without persisting customer secrets.")
    parser.add_argument("--request", required=True, type=pathlib.Path, help="closed-schema JSON build request")
    parser.add_argument("--validate", action="store_true", help="validate and emit digest without building")
    parser.add_argument("--build", action="store_true", help="perform clean-checkout build, smoke, and signed release")
    parser.add_argument("--out-root", type=pathlib.Path)
    parser.add_argument("--release-signing-key", type=pathlib.Path)
    parser.add_argument("--release-public-key", type=pathlib.Path)
    args = parser.parse_args(argv)
    if args.validate == args.build:
        raise SystemExit("choose exactly one of --validate or --build")
    try:
        request = json.loads(args.request.read_text(encoding="utf-8"))
        spec, context, lease, ceilings, digest = validate_request(request)
        if args.validate:
            result = {"schema_version": SCHEMA_VERSION, "status": "validated", "runtime_spec_sha256": digest,
                      "runtime_name": spec["metadata"]["name"], "lease_sha256": hashlib.sha256(canonical_json(lease)).hexdigest(),
                      "compiled_ceilings": {"network": ceilings["allowed_hosts"], "providers": ceilings["allowed_providers"], "models": ceilings["allowed_models"], "tools": ceilings["allowed_tools"]}}
        else:
            if not args.out_root or not args.release_signing_key or not args.release_public_key:
                raise RequestError("--build requires --out-root, --release-signing-key, and --release-public-key")
            result = build(spec, context, lease, ceilings, digest, args.out_root, args.release_signing_key, args.release_public_key)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, json.JSONDecodeError, RequestError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(json.dumps({"schema_version": SCHEMA_VERSION, "status": "rejected", "error": str(exc)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
