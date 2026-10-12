#!/usr/bin/env python3
"""Verify a release_hardened issuer-signed manifest with its public key."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify a DSCO issuer-signed release manifest.")
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--public-key", required=True, type=pathlib.Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    signature = manifest.pop("issuer_signature", None)
    payload_hash = manifest.pop("signed_payload_sha256", None)
    if not isinstance(signature, dict) or signature.get("algorithm") != "ed25519":
        raise SystemExit("manifest has no supported issuer signature")
    payload = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    if not isinstance(payload_hash, str) or hashlib.sha256(payload).hexdigest() != payload_hash:
        raise SystemExit("signed payload hash mismatch")
    public_key = args.public_key.read_bytes()
    if signature.get("key_sha256") != hashlib.sha256(public_key).hexdigest():
        raise SystemExit("public key fingerprint does not match manifest")
    try:
        raw_signature = base64.b64decode(signature["signature_base64"], validate=True)
    except (KeyError, ValueError) as exc:
        raise SystemExit("invalid manifest signature encoding") from exc
    with tempfile.TemporaryDirectory(prefix="dsco-release-verify-") as td:
        data = pathlib.Path(td) / "manifest.payload"
        sig = pathlib.Path(td) / "manifest.signature"
        data.write_bytes(payload)
        sig.write_bytes(raw_signature)
        result = subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
                                 "-inkey", str(args.public_key), "-in", str(data),
                                 "-sigfile", str(sig)], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit("issuer signature verification failed")
    print("release manifest issuer signature: valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
