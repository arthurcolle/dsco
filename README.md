# dsco-cli

**DSCO** (Distributed Systems Corporation Operator) is a **local-first, self-introspecting, agentic CLI** compiled from **135,000+ lines of pure C**.

It is not a wrapper around an LLM. It *is* a 320+-tool runtime with hierarchical swarms, AST-level code intelligence, streaming pipelines, pure-C cryptography, market intelligence tooling, and the ability to recursively modify and extend itself.

This repository (`dsco-cli`) is the canonical home of the project.

## What DSCO Is

- A pragmatic, high-performance agent that lives in your terminal.
- A tool-first system: 364+ callable capabilities across filesystems, git, compilation, debugging, data pipelines, financial markets, web research, binary analysis, media processing, and more.
- Self-aware: it can read, analyze (via AST), and surgically edit its own 129-source-file codebase.
- Swarm-native: supports up to 6 levels of nested sub-agents, 64 concurrent agents, and 60 pre-built orchestration topologies.
- Continuous and evolving: identity, skills, doctrine, and memory are versioned and improved across sessions.
- Built by Arthur Colle (distributed.systems) as the foundation for a commercial agentic infrastructure company.

Current version in the screenshot you shared: **v1.0.0** (with 18/40 features active, swarm-ready, streaming I/O, AST introspection, crypto toolkit, coroutine pipelines, plugin system, etc.).

## Core Architecture (Wings + Talons + Immune System)

- **Wings** — Autonomy & emergence via pheromone-style stigmergy, three-tier memory (working/episodic/semantic), hierarchical swarms, and semantic capability routing.
- **Talons** — Competitive execution engine. Tracks goals through "hunt states," uses grip strength for retry semantics, runs strategy tournaments, and learns which approaches win over time.
- **Immune System** — Governance, OODA loops, kill switches, GSU (governance spending unit) budgets, principal tiers, and hardcoded safety invariants.

## Key Capabilities

- **Code Intelligence**: Full AST introspection, call graphs, dependency analysis, self-surgery via `self-surgeon` skill.
- **Data & Pipelines**: 30+ coroutine-based streaming pipeline stages (`pipeline` tool).
- **Market Intelligence**: 80+ financial/market tools (Bloomberg-lite). Native Kalshi contract ingestion, analysis, and landscape tools.
- **Orchestration**: `swarm-orchestrator`, dynamic topology selection, multi-executor support (includes Claude Code and OpenAI Codex as rival agents).
- **Local-first**: Full macOS integration (Spotlight, AppleScript/JXA, Swift bridge to Vision/CoreML/NaturalLanguage, LaunchAgents, notifications).
- **Crypto-native**: Pure C implementations of SHA-256, HMAC, HKDF, JWT, UUID, etc. (no OpenSSL).
- **Media & Docs**: FFmpeg, ImageMagick, Pandoc, universal document conversion.
- **Self-Evolution**: 25 installed skills (meta-skill `skill-weaver` creates and audits others), 5 doctrine documents, 5 rituals, structured memory system.
- **Observability**: SQLite timeline, trace spans, TUI with markdown rendering.

## Philosophy & Doctrine

DSCO is governed by explicit doctrine located in `.dsco/workspace/` (or `~/.dsco/workspace/`):

- **EPISTEMOLOGY.md** — 6-tier knowledge hierarchy (Computed > Observed > Analyzed > Researched > Inferred > Recalled). Calibration is mandatory.
- **TOOL_PHILOSOPHY.md**, **SWARM_DOCTRINE.md**, **REASONING.md**, **FAILURE_MODES.md** — define how tools are chosen, when to swarm, cognitive traps, and 30 catalogued failure modes with mitigations.
- **Rituals** (SESSION_OPEN, DEEP_WORK, SELF_EVAL, SESSION_CLOSE, etc.) — enforce disciplined session lifecycles.

Core values: Accuracy over theater. Action over narration. Evidence over intuition. Minimal diffs over maximal rewrites. Earned complexity. Calibration over confidence.

## Quick Start

```bash
# Bootstrap, build, run
./scripts/bootstrap.sh
make -j8
export ANTHROPIC_API_KEY=...
./dsco "your command here"
```

See the full [Quick Start](https://github.com/arthurcolle/dsco-cli#quick-start) and [docs/](docs/) for details.

## This Repository Represents

The living, breathing implementation of a new class of developer tooling — deeper, more local, more self-improving than existing LLM coding agents.

It is the foundation for DSCO's commercial vision: $300K ARR within one year by proving extreme local-first agentic capability, then expanding into always-on cloud daemons, multi-user orchestration, and infrastructure products.

> **"Big things have small beginnings."**

— Arthur Colle, CEO, Distributed Systems Corporation

---

**Canonical identity & soul documents live at `~/.dsco/workspace/` (SOUL.md, IDENTITY.md, USER.md, doctrine/, rituals/, skills/, memory/).**

The system is designed to be continuous: one Claw, growing across every session.

Welcome to DSCO. Let's build.