# AGENTS.md — dsco-cli

Repo-scoped briefing for coding agents (agents.md v1.1 hierarchical scope).

## What this is
dsco: a local-first agentic CLI with a native C runtime. Determine source counts
from the current tree rather than copying historical inventory figures.
Overmind Soul architecture: Wings (autonomy) + Talons (competition) + Immune System (safety).

## Map (load details on demand — do not bulk-read)
- `src/` — all C sources. Hotspots: `tools.c` (39K lines, tool registry + immune gate),
  `main.c` (CLI entry), `agent.c` (loop), `llm.c` (providers), `mcp.c` (MCP client),
  `mcp_server.c` (MCP server mode), `supervisor.c`/`swarm` (orchestration)
- `include/` — headers; `tools.h` defines `tool_def_t` and the execute-for-tier gate
- `docs/` — ARCHITECTURE.md, API_REFERENCE.md, C_MODULE_REFERENCE.md

## Build & test
- `make` (see Makefile targets); binary: `./dsco`
- Tests: `make test` where present; smoke: `./dsco --help`

## Hard rules
1. **Capability gating (not file locks)**: the gate protects by *capability*, not filename
   — `src/capability.c` / `include/capability.h`, wired into `tools_execute_for_tier()`.
   Tools classify into `fs_read`/`fs_write`/`net`/`exec`/`secrets`/`untrusted_in`/`control`.
   Deno-style grants (`DSCO_ALLOW_{READ,WRITE,NET,RUN,SECRETS,CONTROL}`) override per-tier
   defaults; reads are always allowed. Read-only governance introspection (e.g. `killswitch {"action":"status"}`)
   is exempt so a denial can be inspected/resolved mid-incident; mutating verbs
   (`trigger`, etc.) always hit the gate. Verify all of this end-to-end against
   the live binary with `make test-gate-claims` (drives `dsco mcp serve` over
   JSON-RPC; no LLM needed). Two hard denials: (a) **lethal trifecta** — a call that
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

## Intellectual property and documentation
- Describe this repository from inspected source and reproducible checks. Do not invent
  third-party ancestry, endorsements, equivalence, implementation details or founder history.
- Access to private notes, OCR, screenshots, source or research does not establish ownership,
  authenticity, a license, permission to publish or authorization to derive product claims.
- Keep third-party code, licenses and attribution distinct from local implementation. A Git
  author, repository location, successful test or model-generated explanation does not prove
  exclusive ownership or independent development. Preserve notices; record unknown provenance.
- Cite public primary sources for external facts. Label proposals, synthetic examples,
  estimates and measured behavior explicitly. Never promote them into production guarantees.
- Write only documentation needed for the requested change. Keep private source summaries,
  inventories, local paths and sensitive review evidence out of product docs and release archives.
- Run `make ip-check` before documentation changes or publication. A passing pattern check
  is a regression safeguard, not ownership clearance. Historical objects require separate review.

## Outcome-ownership directive
- Treat each user objective as a contract to reach a verified terminal state, not as a request
  for plans, suggestions, or partial progress.
- Infer measurable acceptance criteria, inspect current state, build the dependency graph, and
  execute every safe, reversible, authorized step without waiting for routine confirmation.
- Default to continuation: after each result, take the next highest-value unblocked action.
  Diagnose recoverable failures and try a bounded alternative rather than stopping at the first
  error. Stop only when acceptance criteria are verified or a concrete authority, safety, or
  resource boundary is evidenced.
- Parallelize independent, non-conflicting work while keeping integration and verification on the
  critical path. Do not substitute narration, delegation, or artifact creation for end-to-end
  completion.
- Report the outcome, evidence, and residual risk. Autonomy never expands authority: capability
  gates, principal constraints, and approval requirements for irreversible or external actions
  remain binding.
- If a critical error prevents continued work or leaves a consequential action's outcome
  uncertain, print a conspicuous `[!! CRITICAL ERROR !!]` stamp in the conversation before stopping.
  State the concrete error, impact, what was and was not completed, and the next safe step.
  Continue independent safe work where possible; do not stamp routine recoverable errors or
  silently present partial work as complete. A process killed before it can respond cannot
  guarantee a printed stamp; inspect its durable state on resumption.

## Model-selection evidence (read on demand)
When a task needs a provider or model choice, run `python3 scripts/model_lab_watch.py`
(GET-only, no inference) and read its output before relying on a model name in source
or memory. Catalog listings are candidate review items, not authorization to switch
defaults. Confirm exact model access and the task's modality/tool behavior in a bounded
test, and keep provider, billing lane, cost, and rollback explicit.

## Harness engineering workflow
For editing-tool, provider/executor, skills/memory, and native/TUI work, load
`.agents/skills/dsco-harness-engineering/SKILL.md` and announce its use. Retain evidence and
invalidation conditions for priors. Research alone does not promote a skill,
prior, model route, or product feature into an operational default.
