#!/usr/bin/env python3
"""Focused source-level ingress hardening checks; no repo build/network actions."""
from pathlib import Path
s=Path(__file__).parents[1]/"src/net_server.c"
h=Path(__file__).parents[1]/"include/net_server.h"
c=s.read_text(); d=h.read_text()
checks={
 "loopback default": '"127.0.0.1"' in c and 'DSCO_NET_BIND' in c,
 "non-loopback auth gate": 'non-loopback bind requires DSCO_NET_AUTH_KEY' in c and 'strlen(in) != crypto_auth_hmacsha512256_KEYBYTES * 2' in c,
 "TLS fail closed": 'TLS requires certificate and key' in c and 'cert parse failed' in c and 'return false;' in c,
 "public health only": 'public_health' in c and 'strcmp(path, "/health")' in c,
 "constant-time compare": 'sodium_memcmp' in c,
 "timestamp nonce replay": 'replay_seen' in c and 'NETSRV_AUTH_WINDOW' in c and 'v1:%s:%s:%s' in c,
 "client env auth": 'DSCO_NET_AUTH_KEY' in c and 'Signed request' in c,
 "TLS verify and CA": 'MBEDTLS_SSL_VERIFY_REQUIRED' in c and 'DSCO_NET_CA_FILE' in c and 'MBEDTLS_SSL_VERIFY_NONE' not in c,
}
for k,v in checks.items():
 print(("ok: " if v else "FAIL: ")+k)
assert all(checks.values())
assert 'falls back to plaintext' not in d
