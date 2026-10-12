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

## The hot path — what the model actually reads, priority order

Audited 2026-07-03. These are the exact surfaces where mythology reaches the
model, ranked by tokens × frequency:

1. **`SYSTEM_PROMPT` (`include/config.h:513–568`)** — injected every request
   (`llm.c:3219`). Carries: "Overmind Soul architecture", "Wings (soar) +
   Talons (win) + Immune System (survive)", stigmergic pheromones, avian
   nesting/brooding/fledging/roosting/molting, hunt states
   `nascent→stalking→striking→gripping→captured/escaped`, grip strengths up to
   `death_grip`, GSU budgets, "Founder" tier, "soul evolution".
   **`SYSTEM_PROMPT_CHEAP` (`config.h:571–593`) is already clean** — it is the
   template for the rewrite.
2. **Gate denial strings (`tools.c:33966–34140`)** — `governance_block` JSON
   with stages `G2b_immune_self_surgery` etc., reasons naming immune surfaces,
   surgery, and `DSCO_IMMUNE_SURGERY_AUTH` (the env var is printed to the model
   inside the error).
3. **Tool registry names + descriptions (`tools.c:27669–30332`)** — tools named
   `pheromone`, `avian`, `ooda`, `talons`, `legion`, `wings_talons_status`;
   descriptions selling "Bird-inspired Wings mechanisms", "military-history
   canon" strategies, "Sakana Fugu lane".
4. **Goal-tracker result strings (`tools.c:17850–17887`)** — tool results emit
   `"new_state":"stalking"`, `"death_grip"`, action enum
   `stalk|strike|grip|capture|escaped|abandon`.
5. **Orchestrator guides (`orchestrator.c:485–523, 692`)** — `list_domains`
   output and `dispatch_agent` schema expose a "wings" domain ("governance,
   OODA, pheromones, avian nesting/brooding, immune").
6. **Workspace default templates (`workspace.c:27–109`)** — identity default
   "DSCO Claw", `SOUL.md` default "I am dsco operating as a claw", bundled
   "SOUL Curator" / "SOUL Guardian" skills. Loaded into the prompt when present.

Slash commands (`/pareto`, `/soul`, `/identity`) and REPL help are
terminal-only — the model never sees them; they migrate later.

## Renames — metaphor → plain

| Term | What it actually is | Plain name | Status |
|---|---|---|---|
| Immune System | The tool-call permission gate in `tools.c` (stages G1–G10): killswitch, budget, approval, protected-file checks | **tool gate** | wired |
| Immune surface / self-surgery | The gate's own source files, write-protected without external auth | **protected files** / **protected-file write** | wired |
| `DSCO_IMMUNE_SURGERY_AUTH` | Env var authorizing writes to protected files; printed to the model inside denial strings | **`DSCO_PROTECTED_WRITE_AUTH`** (accept both during migration) | wired |
| Overmind / Triad / "Overmind Soul" | 127 markdown files in `~/.dsco/workspace/doctrine/` describing a three-layer organism; quoted as scripture in headers and the system prompt | **architecture notes (historical)** — archive | fiction as an organism; the ~6 useful invariants fit on one page |
| Wings / Talons (labels) | Labels sorting modules into "creative" (avian, pheromone) vs "disciplined" (ooda, killswitch) halves | drop the labels | ceremonial |
| `wings_talons_status` (tool) | Reports gate/signal state | **`gate_status`** | wired |
| Talons engine (`talons.c/.h`) | In-memory goal tracker: goals with priority/retries/deps, per-strategy success weights, N-way race helper | **goal tracker** | wired |
| Hunt states | Goal lifecycle strings `nascent, stalking, striking, gripping, captured, escaped, abandoned` — emitted in tool results | **`pending, starting, in_progress, retrying, done, failed, cancelled`** | wired, model-visible |
| Grip strength (`tentative/holding/locked/death_grip`) | Retry budget: 1 / 3 / 7 / 20 attempts | **retry budget (1/3/7/20)** | wired, model-visible |
| 73 military strategies (`SIEGE`, `MASKIROVKA`, `KESSELSCHLACHT`, `BESIEGE_WEI_TO_RESCUE_ZHAO`, …) | Weighted approach labels for the goal tracker (`talons.h:58–138`) | **strategy presets** — collapse to a handful with plain names | wired, accepted as tool input |
| Avian mechanisms (Nesting, Brooding, Fledging, Roosting, Molting; `egg_id`, `warmth`) | Bird-lifecycle names for create / retry-tend / promote / pause / refresh of in-memory workspaces | **workspace states** | dormant |
| Pheromones (deposit / sense / stigmergy) | Decaying per-tool warning counters; high recent-failure count doubles exec cost in the gate | **signals** (decaying warning counters) | wired |
| OODA loop | Confidence-threshold decision helper (≥0.8 execute, <0.3 escalate) | **decision thresholds** | wired via governance |
| GSU ("GraphSub Units") | Abstract budget points charged per tool call (read 0, write 1, exec 3, spawn 5); `gsu_remaining` appears in every denial the model reads | **tool budget points** (`budget_remaining`) | wired |
| Legion | Registry of 1,665 generated agent presets (model tier + turn/budget caps) | **agent presets** — collapse to a few plain ones | dormant |
| Angels (777) / Demons (888); `seraph, archangel, cherub, throne`; `asmodeus, beelzebub, lilith, abaddon, cerberus, mephistopheles`, … | Celestial/demonological codenames for thorough-vs-fast preset variants (`legion.c:256–384`); demon tags `blitz/rush/balanced` = check-skipping levels | **thorough / fast presets** with verification level; delete the codenames | dormant, names reachable via tools |
| SOUL.md / `has_soul` / "SOUL Curator" / "SOUL Guardian" | Workspace persona/values markdown loaded into the prompt, plus two auto-skills gating its edits | **`PERSONA.md`** / persona edit guard | wired |
| "DSCO Claw" / claw workspace | Default workspace identity string; `~/.dsco/workspace` | **dsco** / **workspace** | wired |
| Frontier ledger / "on the frontier" | Per-turn cost decomposition: productive spend vs five waste channels | **efficiency ledger** | wired (feeds budget-raise checks) |
| `/pareto` (slash command) | Renders the efficiency report | **`/efficiency`** | wired, terminal-only |
| `executive_decision` (tool) | Model-callable session control: end, pause spending, bounded budget raise, escalate | **`session_control`** | wired |
| Shadow check | Substring scan of tool input for self-praise / reward-hack phrases | **self-praise string check** | wired, low value |
| RSI curriculum | Static table of 20 hand-written "skills" (claims 150) with week-by-week prose; plus 7 numeric promotion thresholds | thresholds → **promotion checklist**; skill table → delete | fiction (gate function real but fed self-reported numbers) |
| "Esoteric Operations" curriculum row | Occult vocabulary as learning objectives: apophasis, transmutation, ritual protocols, initiation gates, imaginal simulation (`rsi_curriculum.c:54–60`) | delete | fiction, model-visible via curriculum tool |
| self_improve ("meta-cognitive layer") | Per-tool success/latency stats and 6 advisory strategy weights that nothing reads back | **usage stats** | dormant |
| bg_learn | Background thread turning frequent tool pairs into auto-generated SKILL.md files; pruning stubbed | **skill miner** | off by default |
| baseline / `baseline.db` | SQLite ledger of events, tokens, cost per session — there is no "baseline" being compared against | **usage log** | wired |
| Chronicle | Content-addressed SQLite recorder of full activity | **activity log** (name tolerable) | wired |
| Magnum / "MAGNUM COQ EDITION" | Large parametric test blocks in `tests/test.c` | **parametric tests** | wired |
| sigil | Local name for the TUI status glyph (`project_mux.c:444–464`) | **status glyph** | internal only |
| Germline / genome law / signed pipeline | Cited law that promotions need a human signature; no code, and no source text even in the doc that cites it | delete the references | fiction |
| PRAXIS (Sensorium, Cartographer, Strategos, Actuator, Governor, HELIX) | Proposed replacement architecture, currently HELD; prose only | archive under `docs/history/` | fiction |
| README organism prose | "distributed coding/research organism", "a biological immune system plus a market plus a research lab", "living compute organism" (`README.md:490–737`) | rewrite plainly | user-visible |

## Named presets — names can stay, descriptions must be plain

The 60 topology templates (`topology.c:106–1073`) are preset multi-agent DAG
shapes selected by name (`hydra`, `starburst`, `tribunal`, `senate`,
`gladiator`, `kitchen_brigade`, `sentinel`, `switchboard`, …). Preset names are
product naming, not mythology — they may stay, provided every description
states the mechanism ("branched fan-out with cheap executors"), which most
already do. Same policy for external product names (`Sakana Fugu`,
`fugu-ultra`, `whisper-1`): facts, keep.

## Names that already say what they are — keep

`killswitch` (5-level emergency stop) · `spend_governor` (graduated budget
throttle) · `plan_cache` · `learned_cost` · `cost_model` · `audit_log`
(HMAC-chained, tamper-evident) · `tamper` guard · `sealed_store` ·
`se_store` (Secure Enclave keys) · trust tiers · sandbox · `router` ·
`session_memory` · swarm (industry term) · `watchdog` / `heartbeat` / beacon /
`waiter` / `presence` (standard liveness terms) · `pets` (honestly ASCII pets)
— cosmetic.

## Checked and cleared — not mythology, do not re-flag

`avatar.c`/`face_sdf.c` anatomy words (`iris`, `eyelid`, `temple`) are literal
3-D face geometry · `flock` in `supervisor.c` is the POSIX `flock()` syscall ·
`whisper` is the OpenAI model · `ghost` in `chronicle.c` is a CSS button style
· `ghostty` is a terminal emulator · `sentinel` in provider/llm/tui/cost_budget
is the standard sentinel-value idiom (the *topology* named "sentinel" is a
preset, above).

## Four overlapping logs (name them, unify later)

1. `baseline.db` — usage log (events, tokens, cost)
2. chronicle — activity log (content-addressed)
3. `audit_log` — tamper-evident security chain
4. `~/.dsco/workspace/memory/traces/*/tools.jsonl` — per-session tool traces

Four write paths for "what happened". A later cleanup should pick one primary
and make the others views or delete them. The glossary just refuses to let
them share the word "ledger".

## Statistics (full-tree sweep, 2026-07-03)

Case-insensitive whole-word counts. `src+inc` = all C sources and headers;
`strings` = occurrences inside string literals (≈ model/user-visible).

| term | src+inc | strings | docs | | term | src+inc | strings | docs |
|---|---|---|---|---|---|---|---|---|
| soul | 68 | 16 | 3 | | chronicle | 32 | 20 | 73 |
| ooda | 63 | 19 | 21 | | demon/angel | 57 | 9 | 0 |
| pheromone | 53 | 24 | 29 | | gsu | 28 | 3 | 6 |
| frontier | 50 | 13 | 4 | | wings | 24 | 3 | 19 |
| baseline | 50 | 23 | 72 | | curriculum | 20 | 16 | 6 |
| immune | 45 | 10 | 13 | | legion | 15 | 7 | 9 |
| avian | 36 | 14 | 22 | | doctrine | 13 | 5 | 31 |
| talons | 35 | 10 | 30 | | overmind | 9 | 0 | 16 |

Zero occurrences in live source (fiction confirmed statistically): `praxis`,
`strategos`, `sensorium`, `cartographer`, `germline`, `genome`, `stigmergy`
(prose only), `hive`, `colony`. `helix` (3) and `spine` (1) are comment-only.

Doctrine directory: 127 files; figurative subset to archive includes
`APOPHASIS.md`, `TRANSMUTATION.md`, `RITUAL_PROTOCOL.md`, `INITIATION.md`,
`ESOTERIC_OPERATIONS.md`, `IMAGINAL_REASON.md`, `CORRESPONDENCE.md`,
`LINEAGE.md`, `SOVEREIGNTY.md`, `GRACE.md`, `GREED.md`; workspace also holds
`ANATOMY.md`, `ORGANISM_PLAN.md`, `NOESIS_LANG.md`, `rituals/`. The remaining
~110 doctrine files are plain engineering notes (ATOMICITY, IDEMPOTENCY, …).

## Migration order

1. **`SYSTEM_PROMPT` rewrite first** — the largest figurative payload, paid on
   every request. `SYSTEM_PROMPT_CHEAP` is the proven plain template.
2. **Gate denial strings** — stages and reasons
   (`G2b_immune_self_surgery` → `protected_file_write_blocked`), including the
   env var name inside them.
3. **Tool registry** — names (with aliases so nothing breaks) and descriptions;
   goal-tracker state strings (`stalking` → `in_progress`).
4. **Orchestrator guides and workspace default templates.**
5. **Env vars** — new names, old ones accepted with a deprecation note.
6. **Internal symbols, then file renames** — mechanical, zero behavior change.
7. **Fiction** — archive doctrine/PRAXIS/esoteric prose to `docs/history/`,
   delete dead references (unused soul tools `tools.c:2700–2876`, curriculum
   table, legion codenames). Deletion decisions are the operator's, marked above.
