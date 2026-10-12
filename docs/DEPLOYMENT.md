# Deploying dsco as a persistent agent

dsco has no single built-in "run forever" mode — it's argv-driven (one-shot
prompt, `-i` REPL, `mcp serve`, `acp serve`, `--timeline-server`, `swarmd`).
`docker/entrypoint.sh` picks one of those and keeps it running as a real
network-reachable service. The default mode, `mcp`, runs `dsco mcp serve` as
a persistent MCP server — that's "the agent" this whole setup is built around.

## How it actually stays reachable

`mcp serve` speaks JSON-RPC over **stdio**, not a network port. A container
with nothing attached to its stdin has nothing to serve and exits immediately
— this is correct behavior, not a bug, and you'll see it if you `docker run`
the image with no other configuration. Set `DSCO_BIND_PORT` and the
entrypoint bridges it: `socat` listens on that TCP port and spawns a fresh
`dsco mcp serve` process per connection, wired to that connection's
stdin/stdout — the standard way to expose a stdio protocol over a network
without a code change. Each client gets an isolated process; all of them
share the same persistent state under `~/.dsco`.

Both `docker-compose.yml` and `k8s/base/configmap.yaml` set
`DSCO_BIND_PORT=8765` by default for exactly this reason.

## Quick start (Docker Compose)

```sh
cp .env.example .env      # fill in the provider keys you actually use
docker compose up -d --build
docker compose logs -f
```

Verify it's actually serving:

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
  | nc 127.0.0.1 8765
```

State lives in the `dsco-state` named volume, mounted at `/home/dsco/.dsco`
— sessions, the SQLite baseline/timeline DB, auth, plugins, capsules,
chronicle. `docker compose down` (without `-v`) keeps it; `docker compose
down -v` deletes it.

## Kubernetes

```sh
kubectl apply -k k8s/overlays/dev     # or overlays/prod
```

**Secrets are not applied by kustomize base/overlays.** `k8s/base/secret.example.yaml`
documents every key the agent reads; create the real one with:

```sh
kubectl create secret generic dsco-agent-secrets -n dsco-agent \
  --from-literal=ANTHROPIC_API_KEY=sk-... \
  --from-literal=OPENROUTER_API_KEY=sk-...
```

(For a real cluster, prefer External Secrets Operator or Sealed Secrets over
either the imperative command above or committing a filled-in secret.yaml —
neither belongs in shell history or git.)

### Why a StatefulSet, and why `replicas: 1`

dsco keeps all of its durable state under `~/.dsco` with no built-in
replication or clustering. A StatefulSet gives this one instance a stable
name and a PVC (`volumeClaimTemplates`, mounted at `/home/dsco/.dsco`) that
follows it across rescheduling — that's what makes it durable, not just
running.

Raising `replicas` does **not** scale this agent — it creates N independent
agents, each with its own empty PVC (a StatefulSet provisions one PVC per
ordinal) and no shared state. To run more than one agent, deploy the
StatefulSet again under a different name — copy `k8s/overlays/dev` as a
template — not by bumping `replicas` here.

### Health checks

Both `livenessProbe` and `readinessProbe` run `dsco --tool-exec cwd '{}'` —
the project's own canonical, no-LLM-call smoke test (also what CI and the
Dockerfile's `HEALTHCHECK` use). It's deterministic and fast: a real signal
that the binary and its dynamic libraries actually work, not just that the
process exists.

### Networking

`k8s/base/networkpolicy.yaml` restricts ingress to same-namespace pods on the
bridge port and restricts egress to DNS (53) + HTTPS (443) — which covers
every provider/tool API dsco talks to. Widen it if a toolset needs a
non-443 port.

`--timeline-server` (the local observability dashboard) binds `127.0.0.1`
only and is deliberately **not** exposed via the Service — reach it with
`kubectl port-forward` or `docker exec` for ad-hoc debugging, or run it as
its own separate `DSCO_MODE=timeline` deployment if you want it as a
standing service.

### Overlays

`k8s/overlays/dev` and `k8s/overlays/prod` patch resource requests/limits,
PVC size, and image pull policy on top of `k8s/base`. Both validate against
real Kubernetes 1.30 OpenAPI schemas (`kubeconform -strict`); there's no live
cluster wired into this repo to test against beyond that.

## Linux/container portability notes

- dsco's sealed secret store (`src/se_store.c`) has three tiers — Secure
  Enclave, macOS Keychain, IOKit machine-UUID — and all three are Darwin-only.
  In a Linux container they fail closed by design; dsco falls back to reading
  credentials from plain environment variables, which is also the documented
  default access pattern on every platform. No action needed.
- The image builds with the exact Ubuntu package list
  `.github/actions/setup-native/action.yml` uses for CI's own `ubuntu-24.04`
  build lane, plus `zlib1g-dev` — CI's hosted runners ship zlib headers
  preinstalled in their base image, so that gap doesn't show up there, but a
  bare `ubuntu:24.04` container needs it explicit since the Makefile links
  `-lz` unconditionally.
- Building this image also surfaced (and fixed) two small, pre-existing
  Linux-portability bugs in already-committed source, unrelated to the
  container work itself: an unguarded macOS-only `_NSGetExecutablePath` call
  in `standing deploy` (`src/main.c`), and a missing `#include <stdint.h>`
  in `src/swarm_accounting.c` (`SIZE_MAX` resolved only via macOS's transitive
  include chain). Both are one-line, mirror an existing correct pattern
  already used elsewhere in the same files, and are worth a look before
  assuming they're unrelated to whatever else is in flight in this tree.

## Repo hygiene: build context

`.dockerignore` is a deny-by-default allowlist, not a denylist — it excludes
`**` and then explicitly allows only what `make dsco` actually reads:
`Makefile`, `src/`, `include/`, `vendor/`, `gsl/`, `lingo/`, the specific
bootstrap/bake-data/embed-lingo scripts, `docker/entrypoint.sh`, and exactly
three files under `data/` (matching the Makefile's own
`find data -maxdepth 1 -type f` bake-data glob).

This is a security boundary as much as a size one: `data/dsco_secrets.txt`
sits directly under `data/`, matches that same glob, and is real local,
gitignored credential material. If it were ever swept into a build context,
the bake-data step would compile its contents directly into the `dsco`
binary as a linked-in byte array, extractable with `strings`. It is
deliberately not in the allowlist, and any change to `.dockerignore` should
keep it that way.
