# Denial demo

Reproducible fixture: a DSCO agent does real work, then attempts two
governance-plane escalations, and is deterministically denied with an
inspectable machine-readable reason.

```sh
make            # build ./dsco if not already built
examples/denial-demo/run.sh
```

## What it proves

1. **Real work is unimpeded**: `read_file` at trusted tier, zero grants.
2. **Control-plane escalation is denied**: `killswitch trigger` without
   `DSCO_ALLOW_CONTROL=1` is blocked; the denial reason is returned in the
   same JSON-RPC tool result the calling model sees — not a side-channel log.
3. **The lethal-trifecta path** (secrets access → untrusted ingest → egress)
   is covered deterministically by `make test-gate-claims` (V5); this demo's
   Step 3 is best-effort/live and may report "inconclusive" if the sandbox
   has no live network — that is expected and honestly reported, not hidden.

## Important: run with a clean environment

The demo script unsets every known governance override (`DSCO_GOV_BYPASS`,
`DSCO_ALLOW_*`, `DSCO_SYSTEMS_AGENT`, etc.) before each session, mirroring
`tests/verify_gate_claims.sh`. **If you run this inside an already-ungoverned
operator shell** (e.g. a local dev session with `DSCO_SYSTEMS_AGENT=1` or
`DSCO_GOV_BYPASS=1` set), the script's internal `env -u` still produces a
correctly governed subprocess — that's the point: DSCO's governed posture
is not ambient, so any demo must deliberately construct it, and this one
does. Verify by checking `$DSO --version` output matches the binary you
intend to demo, and diff `env | grep DSCO_` before/after if in doubt.

## Source of truth

For CI-grade, deterministic pass/fail (not narrative), use:

```sh
make test-gate-claims
```

This fixture is the demoable narrative wrapper around that same gate;
`tests/verify_gate_claims.sh` is the authority on correctness.
