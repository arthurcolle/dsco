# DHT Improvement Sync

`dsco` can exchange immutable improvement bundles between fleet nodes without a
central artifact server. The private Kademlia DHT answers **which mesh peer may
have a bundle**; the existing encrypted mesh transports the bytes. SHA-256 and
Ed25519 verification happen before a received object becomes visible in the
catalog.

This mechanism stages improvements. It never executes or applies received code.

## Data and trust flow

```text
payload file
   │ publish + Ed25519 sign
   ▼
SHA-256 bundle object ── announce SHA-256[:160] ──► private DHT
   │                                                    │
   │ encrypted 64 KiB chunks                           │ provider address
   └──────────────────────────────► requesting mesh peer
                                        │
                           full SHA-256 + signature check
                                        │
                           known signer? ├─ yes ─► staged/
                                        └─ no  ─► quarantine/
```

The DHT stores only provider hints. A collision in its 160-bit key cannot make
the wrong bundle acceptable because the receiver verifies the complete 256-bit
content address. The bundle signature covers its header, signer, metadata, and
payload. The signed bundle's bytes, including its signature, determine its
content address.

## Fleet startup

Every participating process needs the same DHT swarm namespace, a reachable
mesh port, and at least one private bootstrap node:

```sh
DSCO_DHT_SWARM='<shared-random-swarm-key>' \
DSCO_DHT_BOOTSTRAP='10.0.0.12:7600' \
DSCO_DHT_PORT=7600 \
DSCO_MESH_PORT=7337 \
./dsco
```

`DSCO_DHT_BOOTSTRAP` accepts comma-separated `host:port` entries. Static entries
may instead be placed in `~/.dsco/dht_bootstrap.txt`. DHT UDP and mesh TCP must
be reachable between fleet nodes. Do not point the bootstrap setting at the
public BitTorrent DHT.

The process announces local and trusted staged bundles at startup and every 30
seconds. DHT provider records are refreshed every five minutes.

## Agent tool contract

Read-only catalog operations use `improvement_catalog`:

```json
{"action":"status"}
{"action":"list","limit":50}
{"action":"inspect","hash":"<64 hex characters>"}
```

Exchange and trust-state changes use `improvement_sync`:

```json
{"action":"publish","path":"/tmp/fix.patch","kind":"patch","name":"fix-mesh-retry","base_version":"abc123","target_version":"def456","description":"Bound retry backoff"}
{"action":"fetch","hash":"<bundle SHA-256>","timeout_seconds":30}
{"action":"trust","signer":"<Ed25519 public key>"}
{"action":"promote","hash":"<bundle SHA-256>"}
{"action":"materialize","hash":"<bundle SHA-256>","output_path":"/tmp/review.patch"}
{"action":"announce"}
```

Valid kinds are `patch`, `source`, `binary`, and `config`; the maximum complete
bundle size is 32 MiB. `materialize` uses create-only semantics and refuses an
existing destination. It works only for locally published objects or trusted
staged objects.

## Review workflow

1. Publish an improvement on its producing node and retain the returned bundle
   hash, signer fingerprint, base version, and target version.
2. Communicate the signer fingerprint over an authenticated channel independent
   of the DHT.
3. Fetch by full bundle hash. An unknown signer is stored in `quarantine/` even
   when its self-contained signature is valid.
4. Inspect metadata and the signer fingerprint. Add the signer to the local
   trust root only after out-of-band verification.
5. Promote the quarantined bundle, materialize it to a new review path, inspect
   or test it, and use the normal governed filesystem/exec path for any eventual
   application.

There is deliberately no `apply` operation. Trusting a signer permits staging;
it does not authorize code execution or source mutation.

## Capability gate

The two tool surfaces enter through `tools_execute_for_tier()` like every other
built-in tool. Their verb-sensitive capability claims are:

| Operation | Capabilities |
|---|---|
| catalog status/list/inspect | `fs_read` |
| publish | `fs_read`, `fs_write`, `net` |
| announce | `fs_read`, `net` |
| fetch | `fs_read`, `fs_write`, `net`, `untrusted_in` |
| trust/promote | `fs_read`, `fs_write`, `control` |
| materialize | `fs_read`, `fs_write` |

The lethal-trifecta session rule still applies to fetched content. Explicit
`DSCO_ALLOW_*` denials and the control grant are not bypassed.

## Local state

The default root is `~/.dsco/improvements`, overrideable with
`DSCO_IMPROVEMENT_ROOT`:

```text
identity.ed25519       local Ed25519 public + secret key, mode 0600
trusted_signers        approved public-key fingerprints, one per line
objects/               bundles published by this identity
staged/                verified bundles from trusted identities
quarantine/            verified bundles from unknown identities
partials/              bounded in-progress transfers
```

Store roots and subdirectories must be real directories rather than symlinks.
Immutable file creation is atomic and refuses a conflicting existing object.

## Current bounds

- One fetch may be active per process.
- Transfers are sequential 64 KiB chunks over the mesh's authenticated-encryption
  frames; interrupted transfers restart rather than resume.
- The in-memory provider announcement set is capped at 256 bundle keys per DHT
  process. Local catalog size is not capped by this value.
- DHT discovery is eventual. A connected mesh peer can answer immediately;
  otherwise provider discovery and peer dialing may take several polling cycles.
- Signer revocation and threshold signatures are not part of the first protocol
  version. Remove a signer from `trusted_signers` to stop future promotion and
  materialization under that identity.

Implementation: `src/improvement_sync.c`, `include/improvement_sync.h`, and the
provider-key extensions in `src/dsco_dht.c` / `include/dsco_dht.h`.
