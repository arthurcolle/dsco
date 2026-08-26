#!/usr/bin/env python3
"""Issue a DSCO activation lease with an offline P-256 issuer key.

The private key path is supplied at invocation time and is never copied into
the repository or output. The emitted signature is DER-encoded ECDSA,
base64url-wrapped for Security.framework's X9.62 verifier.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import subprocess
import tempfile
import re


def payload(lease: dict) -> bytes:
    fields = ("lease_id", "subject", "plan", "issuer", "scopes")
    try:
        values = [str(lease[name]) for name in fields]
        issued_at = int(lease["issued_at"])
        expires_at = int(lease["expires_at"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("lease requires string identity fields and integer timestamps") from exc
    if int(lease.get("schema_version", 0)) != 1 or not values[0] or not values[1] or not values[2]:
        raise ValueError("lease requires schema_version=1 plus lease_id, subject, and plan")
    if issued_at < 0 or expires_at <= issued_at:
        raise ValueError("lease must have a future expiry after issued_at")
    parts = ["v1"]
    for value in values:
        encoded = value.encode("utf-8")
        parts.append(f"{len(encoded)}:{value}")
    parts.extend((str(issued_at), str(expires_at)))
    digest = lease.get("runtime_spec_sha256", "")
    if digest:
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError("runtime_spec_sha256 must be a SHA-256 hex digest")
        parts.append(f"{len(digest.encode('utf-8'))}:{digest}")
    return "|".join(parts).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Sign a DSCO cloud activation lease with P-256.")
    parser.add_argument("--in", dest="input", type=pathlib.Path)
    parser.add_argument("--out", dest="output", type=pathlib.Path)
    parser.add_argument("--key", required=True, type=pathlib.Path,
                        help="offline P-256 issuer private-key PEM")
    parser.add_argument("--print-public-key-b64", action="store_true",
                        help="print the base64url uncompressed P-256 public key for cloud-build")
    args = parser.parse_args()
    if not args.key.is_file():
        raise SystemExit("issuer key must be a regular file")
    if args.print_public_key_b64:
        public_der = subprocess.run(["openssl", "ec", "-in", str(args.key), "-pubout",
                                     "-outform", "DER"], check=True, capture_output=True).stdout
        raw_public = public_der[-65:]
        if len(raw_public) != 65 or raw_public[0] != 4:
            raise SystemExit("issuer key did not yield an uncompressed P-256 public key")
        print(base64.urlsafe_b64encode(raw_public).rstrip(b"=").decode("ascii"))
        return 0
    if not args.input or not args.output:
        raise SystemExit("--in and --out are required unless --print-public-key-b64 is used")
    lease = json.loads(args.input.read_text(encoding="utf-8"))
    signed = payload(lease)
    with tempfile.TemporaryDirectory(prefix="dsco-lease-sign-") as td:
        message = pathlib.Path(td) / "payload"
        signature = pathlib.Path(td) / "signature.der"
        message.write_bytes(signed)
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(args.key),
                        "-out", str(signature), str(message)], check=True)
        der_signature = signature.read_bytes()
    lease["signature"] = base64.urlsafe_b64encode(der_signature).rstrip(b"=").decode("ascii")
    encoded = json.dumps(lease, indent=2, sort_keys=True) + "\n"
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as out:
        out.write(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
