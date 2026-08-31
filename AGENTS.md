# AGENTS.md — dsco-cli

Repo-scoped briefing for coding agents (agents.md v1.1 hierarchical scope).

## What this is
dsco: a local-first agentic CLI in pure C (current checkout: branch `fix/ci-release-readiness` @ `0bd4cb7`,
recomputed 2026-08-30: **205,172 hand-written LOC** across 146 `.c` files and 147 headers (304 files),
plus 213,226 LOC of generated embedded-data blobs in `src/generated/` (3 files) — **418,398 total**.
Prior calibration (2026-07-07): ~199K LOC, 136 `.c`/138 `.h`; the 2x "growth" was the generated
blobs entering `find src include`, not code growth — see `.workspace/harness-parity/swarm-20260827/01-loc-audit.md`).
Overmind Soul architecture: Wings (autonomy) + Talons (competition) + Immune System (safety).

## Map (load details on demand — do not bulk-read)
- `src/` — all C sources. Hotspots: `tools.c` (~40K lines, 40,503 at 2026-08-30, tool registry + immune gate),
  `main.c` (CLI entry), `agent.c` (loop), `llm.c` (providers), `mcp.c` (MCP client),
  `mcp_server.c` (MCP server mode), `supervisor.c`/`swarm` (orchestration)
- `include/` — headers; `tools.h` defines `tool_def_t` and the execute-for-tier gate
- `.workspace/` — strategy docs; `harness-parity/` is the active roadmap (00_MASTER.md)
- `doctrine/` — 80 governance doctrines; `memory/sessions/` — session logs
- `docs/` — ARCHITECTURE.md, API_REFERENCE.md, C_MODULE_REFERENCE.md

## Build & test
- `make` (see Makefile targets); binary: `./dsco`
  (observed 2026-08-30: v`1.0.2`, built 2026-08-27 from build `56be6a8`; doc-repair HEAD `0bd4cb7`)
- Tests: `make test` where present; smoke: `./dsco --help`

## Hard rules
1. **Capability gating (not file locks)**: the gate protects by *capability*, not filename
   — `src/capability.c` / `include/capability.h`, wired into `tools_execute_for_tier()`.
   Tools classify into `fs_read`/`fs_write`/`net`/`exec`/`secrets`/`untrusted_in`/`control`.
   Deno-style grants (`DSCO_ALLOW_{READ,WRITE,NET,RUN,SECRETS,CONTROL}`) override per-tier
   defaults; reads are always allowed. Two hard denials: (a) **lethal trifecta** — a call that
   egresses (`net`/`exec`) after the session accessed secrets AND ingested untrusted content is
   blocked (operator override: `DSCO_ALLOW_EXFIL=1`; taint resets at session init); (b) **control**
   — control-plane *tools* (killswitch, governance, self_exit, tamper) need `DSCO_ALLOW_CONTROL=1`.
   Editing source files — including the gate's own (`capability.c`, `governance.c`, `tools.c`) — is
   ordinary `fs_write` under the trust tier: there is NO filename-based lock. `DSCO_IMMUNE_SURGERY_AUTH`
   and the old immune-surface file list are superseded by this model. Explicit hardening:
   `DSCO_ALLOW_<CAP>=0` disables that capability entirely (Deno-style opt-out).
2. Minimal diffs. No sweeping rewrites of `tools.c` or `main.c`.
3. New capabilities = new `src/<module>.c` + header + Makefile entry, wired via small
   dispatch hooks — not inline growth of megafiles.
4. Every tool-call path must route through `tools_execute_for_tier()` (governance gate).
5. Verify claims against disk: counts in identity docs drift; `ls | wc -l` beats memory.

## Current priorities
See `.workspace/harness-parity/00_MASTER.md` — 15 plans; Wave A: MCP server mode (#05),
AGENTS.md/progressive disclosure (#06); Wave B: durable execution (#01), headless (#02),
eval harness (#11), observability (#12).
