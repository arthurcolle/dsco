# Cloud RuntimeSpec build factory

`scripts/cloud_build_factory.py` is the server-side boundary for one cloud
runtime. It accepts only the live `distributed.systems/v1alpha1` `RuntimeSpec`
shape (`metadata`, `connections`, `routing`, `tools`, `governance`, `build`,
`deployment`, `network`, and canonical `integrity`), separate closed
`build_context`, and an already issuer-signed activation lease. The factory
derives `runtime_spec_sha256` from canonical RuntimeSpec content excluding the
self-referential `integrity` block, requires that exact digest in the lease,
compiles it into the cloud binary, and passes it to the signed release manifest.
It never writes the lease or any signing key to the output bundle.

`runtime_spec.network.allowedHosts` is mandatory. The factory derives the
network, provider, model, and tool ceilings from the validated RuntimeSpec and
compiles them into the binary. Cloud startup overwrites mutable environment
values from those constants, so an operator cannot widen the approved surface.
The allowlist must include `tools.distributed.systems` for the ToolManagement
control plane. `routing.routes` is an ordered provider/model entry for every
eligible provider; `next-connected` preserves all of those model ceilings,
while `fail-closed` compiles cross-provider routing off. The approved
`governance.perRunBudgetUsd` is compiled into both session and executive budget
ceilings.

The current cloud RuntimeSpec tool surface is deliberately limited to `repository` (read-only
repository inspection) and `web` (host-allowlisted fetch/search). The factory rejects `shell`,
write-capable document tools, Git mutation, MCP, and plugins until a RuntimeSpec carries a signed
tenant sandbox contract with explicit workspace roots, command policy, and plugin capabilities.
Cloud startup uses a fresh mode-0700 state directory, skips plugin discovery, browser history, and
the shared IPC bus, and never reuses the host user's `~/.dsco` state.

Validate a request without requiring keys or building:

```sh
python3 scripts/cloud_build_factory.py --request /secure/request.json --validate
```

On a clean, pinned server checkout, build using externally provisioned signing
material (paths are not persisted in the bundle or output JSON):

```sh
python3 scripts/cloud_build_factory.py --request /secure/request.json --build \
  --out-root /secure/dsco-builds \
  --release-signing-key /secure/issuer-release-ed25519.pem \
  --release-public-key /secure/issuer-release-ed25519.pub.pem
```

The output is one JSON object. A successful bundle contains only the canonical
RuntimeSpec, cloud binary, signed release evidence, and digest references; it
does not contain the activation lease, its signature, customer credentials, or
private signing material.
