#!/usr/bin/env python3
"""Focused closed-schema and RuntimeSpec-binding checks for cloud_build_factory."""
from __future__ import annotations

import base64
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "cloud_build_factory.py"
LEASE_SIGNER = ROOT / "scripts" / "activation_lease_sign.py"
spec = importlib.util.spec_from_file_location("cloud_build_factory", SCRIPT)
assert spec and spec.loader
factory = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = factory
spec.loader.exec_module(factory)
signer_spec = importlib.util.spec_from_file_location("activation_lease_sign", LEASE_SIGNER)
assert signer_spec and signer_spec.loader
lease_signer = importlib.util.module_from_spec(signer_spec)
sys.modules[signer_spec.name] = lease_signer
signer_spec.loader.exec_module(lease_signer)


def request() -> dict:
    runtime_spec = {
        "apiVersion": "distributed.systems/v1alpha1",
        "kind": "RuntimeSpec",
        "metadata": {"name": "tenant-a.runtime-01", "reproducible": True},
        "connections": [
            {"provider": "openai", "ref": "account://openai", "credentialMaterial": "excluded"},
            {"provider": "local", "ref": "local://discovery", "credentialMaterial": "excluded"},
        ],
        "routing": {
            "primary": {"provider": "openai", "model": "openai/codex-default"},
            "fallback": "next-connected", "eligibleProviders": ["openai", "local"],
            "routes": [
                {"provider": "openai", "model": "openai/codex-default"},
                {"provider": "local", "model": "local/auto"},
            ],
        },
        "tools": [
            {"id": "repository", "scope": "runtime-policy"},
            {"id": "web", "scope": "runtime-policy"},
        ],
        "governance": {
            "preset": "balanced", "approvals": ["external_write", "secret_use"], "perRunBudgetUsd": 12,
            "secretIsolation": "account-vault-reference-only", "chronicle": "required",
        },
        "build": {
            "artifact": "signed-runtime-bundle", "isolation": "customer-specific",
            "hardening": ["locked-connection-refs", "policy-embedded", "minimal-tool-surface"],
            "reproducibleFrom": "this-runtime-spec",
        },
        "deployment": {"target": "dsco-cloud", "authority": "signed-runtime-spec"},
        "network": {"allowedHosts": ["api.example.com", "tools.distributed.systems", "tools.example.com"]},
    }
    digest = factory.runtime_spec_digest(runtime_spec)
    runtime_spec["integrity"] = {
        "canonicalization": "JCS-compatible", "digest": f"sha256:{digest}",
        "signature": "issued-after-account-approval", "artifactAttestation": "issued-with-customer-build",
    }
    return {"runtime_spec": runtime_spec, "build_context": {
        "source_revision": "a" * 40, "build_epoch": 1_700_000_000,
        "issuer_public_key_b64": base64.urlsafe_b64encode(b"\x04" + b"p" * 64).rstrip(b"=").decode(),
        "plugin_requirement_b64": "cmVxdWlyZW1lbnQ",
    }, "activation_lease": {
        "schema_version": 1, "lease_id": "lease-a", "subject": "tenant-a", "plan": "pro",
        "issuer": "issuer", "scopes": "cloud", "issued_at": 1_700_000_000,
        "expires_at": 1_800_000_000, "runtime_spec_sha256": digest, "signature": "AA",
    }}


class CloudBuildFactoryTests(unittest.TestCase):
    def test_validate_emits_stable_digest_and_never_echoes_lease(self):
        payload = request()
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "request.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            done = subprocess.run([sys.executable, str(SCRIPT), "--request", str(path), "--validate"],
                                  text=True, capture_output=True, check=True)
        result = json.loads(done.stdout)
        self.assertEqual(result["status"], "validated")
        self.assertEqual(result["runtime_spec_sha256"], payload["activation_lease"]["runtime_spec_sha256"])
        self.assertNotIn("signature", done.stdout)
        self.assertNotIn("subject", done.stdout)

    def test_rejects_unknown_secret_bearing_input(self):
        payload = request()
        payload["build_context"]["api_key"] = "must-not-enter-factory"
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_rejects_unknown_secret_field_inside_live_runtime_spec(self):
        payload = request()
        payload["runtime_spec"]["connections"][0]["token"] = "must-not-enter-runtime-spec"
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_rejects_lease_not_bound_to_immutable_spec(self):
        payload = request()
        payload["activation_lease"]["runtime_spec_sha256"] = "0" * 64
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_rejects_tampered_spec_even_when_lease_digest_is_unchanged(self):
        payload = request()
        payload["runtime_spec"]["network"]["allowedHosts"].append("operator.example")
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_derives_compiled_ceilings_from_live_runtime_spec(self):
        payload = request()
        _, _, _, ceilings, _ = factory.validate_request(payload)
        self.assertEqual(ceilings["allowed_hosts"], "api.example.com,tools.distributed.systems,tools.example.com")
        self.assertEqual(ceilings["allowed_providers"], "openai,local")
        self.assertEqual(ceilings["primary_model"], "openai/codex-default")
        self.assertEqual(ceilings["allowed_models"], "openai/codex-default,local/auto")
        self.assertEqual(ceilings["disable_cross_provider_routing"], "0")
        self.assertEqual(ceilings["session_budget_usd"], "12")
        self.assertIn("WebSearch", ceilings["tool_allowlist"])
        self.assertNotIn("Bash", ceilings["tool_allowlist"])

    def test_rejects_shell_that_has_no_narrow_compiled_execution_policy(self):
        payload = request()
        payload["runtime_spec"]["tools"].append({"id": "shell", "scope": "runtime-policy"})
        with self.assertRaisesRegex(factory.RequestError, "unsupported cloud tool policy"):
            factory.validate_request(payload)

    def test_rejects_write_and_dynamic_plugin_surfaces_without_signed_sandbox_contract(self):
        for tool_id in ("documents", "mcp", "github"):
            with self.subTest(tool_id=tool_id):
                payload = request()
                payload["runtime_spec"]["tools"].append({"id": tool_id, "scope": "runtime-policy"})
                with self.assertRaisesRegex(factory.RequestError, "unsupported cloud tool policy"):
                    factory.validate_request(payload)

    def test_rejects_out_of_order_or_tampered_fallback_routes(self):
        payload = request()
        payload["runtime_spec"]["routing"]["routes"].reverse()
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_rejects_spec_that_would_deny_tool_management(self):
        payload = request()
        payload["runtime_spec"]["network"]["allowedHosts"].remove("tools.distributed.systems")
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_rejects_empty_lease_plan(self):
        payload = request()
        payload["activation_lease"]["plan"] = ""
        with self.assertRaises(factory.RequestError):
            factory.validate_request(payload)

    def test_issuer_signed_lease_payload_includes_runtime_spec_digest(self):
        payload = request()
        lease = payload["activation_lease"]
        signed_payload = lease_signer.payload(lease).decode("utf-8")
        self.assertTrue(signed_payload.endswith(
            f"|64:{lease['runtime_spec_sha256']}"
        ))


if __name__ == "__main__":
    unittest.main()
