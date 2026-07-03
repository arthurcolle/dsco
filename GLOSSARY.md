# Glossary

dsco is a C command-line tool that runs LLM agents: streaming, prompt caching,
tool execution, a REPL, and multi-agent spawning. That is the project.

This file is the rename ledger for the language cleanup. Three rules:

1. **Names describe mechanism, not metaphor.** A permission check is called a
   permission check.
2. **Model-visible strings use only the plain column.** The agent reads its own
   error messages, tool names, and help text. When errors speak of immune
   systems and self-surgery, the model role-plays the mythology back into new
   code and docs. That is how the sediment accumulated, and it bills by the token.
3. **New code uses plain names.** Old symbols migrate in the order at the bottom.

Status: **wired** = runs in the normal path · **dormant** = compiled, nothing
consumes it · **fiction** = documented/claimed, no working code · **cosmetic** =
fun extra, honest about it.

## Renames — metaphor → plain

| Term | What it actually is | Plain name | Status |
|---|---|---|---|
| Immune System | The tool-call permission gate in `tools.c` (stages G1–G10): killswitch, budget, approval, protected-file checks | **tool gate** | wired |
| Immune surface / self-surgery | The gate's own source files, write-protected without external auth | **protected files** / **protected-file write** | wired |
| `DSCO_IMMUNE_SURGERY_AUTH` | Env var authorizing writes to protected files | **`DSCO_PROTECTED_WRITE_AUTH`** (accept both during migration) | wired |
| Overmind / Triad / Soul | 127 markdown files in `~/.dsco/workspace/doctrine/` describing a three-layer organism; headers quote them as scripture | **architecture notes (historical)** — archive | fiction as an organism; the ~6 useful invariants fit on one page |
| Wings / Talons | Labels sorting modules into "creative" (avian, pheromone) vs "disciplined" (ooda, killswitch, talons) halves | drop the labels | ceremonial |
| `wings_talons_status` (tool) | Reports gate/signal state | **`gate_status`** | wired |
| Avian mechanisms (Nesting, Brooding, Fledging, Roosting, Molting) | Bird-lifecycle names for create / retry-tend / promote / pause / refresh of in-memory workspaces | **workspace states** | dormant |
| Pheromones (deposit / sense / stigmergy) | Decaying per-tool warning counters; high recent-failure count doubles exec cost in the gate | **signals** (decaying warning counters) | wired |
| OODA loop | Confidence-threshold decision helper (≥0.8 execute, <0.3 escalate) | **decision thresholds** | wired via governance |
| GSU ("GraphSub Units") | Abstract budget points charged per tool call (read 0, write 1, exec 3, spawn 5) | **tool budget points** | wired |
| Legion, Angels (777) / Demons | Registry of agent presets: thorough-and-verified vs fast-and-loose personas | **agent presets** (thorough / fast) | dormant |
| Frontier ledger / "on the frontier" | Per-turn cost decomposition: productive spend vs five waste channels; a session efficiency report | **efficiency ledger** | wired (feeds budget-raise checks) |
| `/pareto` (slash command) | Renders the efficiency report | **`/efficiency`** | wired |
| `executive_decision` (tool) | Model-callable session control: end, pause spending, bounded budget raise, escalate | **`session_control`** | wired |
| Shadow check | Substring scan of tool input for self-praise / reward-hack phrases | **self-praise string check** | wired, low value |
| RSI curriculum | Static table of 20 hand-written "skills" (claims 150) with week-by-week prose; plus 7 numeric promotion thresholds | thresholds → **promotion checklist**; skill table → delete | fiction (gate function is real but fed self-reported numbers) |
| self_improve ("meta-cognitive layer") | Per-tool success/latency stats and 6 advisory strategy weights that nothing reads back | **usage stats** | dormant |
| bg_learn | Background thread turning frequent tool pairs into auto-generated SKILL.md files; pruning stubbed | **skill miner** | off by default |
| baseline / `baseline.db` | SQLite ledger of events, tokens, cost per session — there is no "baseline" being compared against | **usage log** | wired |
| Chronicle | Content-addressed SQLite recorder of full activity | **activity log** (name tolerable) | wired |
| "claw workspace" | `~/.dsco/workspace` | **workspace** | wired |
| Magnum / "MAGNUM COQ EDITION" | Large parametric test blocks in `tests/test.c` | **parametric tests** | wired |
| Germline / genome law / signed pipeline | Cited law that promotions need a human signature; no code, and no source text even in the doc that cites it | delete the references | fiction |
| PRAXIS (Sensorium, Cartographer, Strategos, Actuator, Governor, HELIX) | Proposed replacement architecture, currently HELD; prose only | archive under `docs/history/` | fiction |

## Names that already say what they are — keep

`killswitch` (5-level emergency stop) · `spend_governor` (graduated budget
throttle) · `plan_cache` · `learned_cost` · `cost_model` · `audit_log`
(HMAC-chained, tamper-evident) · `tamper` guard · `sealed_store` ·
`se_store` (Secure Enclave keys) · trust tiers · sandbox · `router` ·
`session_memory` · swarm (industry term) · topology (orchestration pattern;
borderline but standard) · `pets` (it is honestly ASCII pets) — cosmetic.

## Four overlapping logs (name them, unify later)

1. `baseline.db` — usage log (events, tokens, cost)
2. chronicle — activity log (content-addressed)
3. `audit_log` — tamper-evident security chain
4. `~/.dsco/workspace/memory/traces/*/tools.jsonl` — per-session tool traces

Four write paths for "what happened". A later cleanup should pick one primary
and make the others views or delete them. The glossary just refuses to let
them share the word "ledger".

## Migration order

1. **Model-visible strings first** — gate error stages and reasons
   (`G2b_immune_self_surgery` → `protected_file_write_blocked`), tool names
   (with aliases), help text, slash commands. Highest value: stops the
   mythology feedback loop at its source.
2. **Env vars** — new names, old ones accepted with a deprecation note.
3. **Internal symbols** — functions, types, comments; mechanical, low risk.
4. **File renames last** — highest churn, zero behavior change.
5. **Fiction** — archive doctrine/PRAXIS prose to `docs/history/`, delete dead
   references. Deletion decisions are the operator's, marked above.
