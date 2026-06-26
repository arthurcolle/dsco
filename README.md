# dsco-cli

> A local-first, self-introspecting agentic CLI, written in pure C.

[![CI](https://github.com/arthurcolle/dsco-cli/actions/workflows/ci.yml/badge.svg)](https://github.com/arthurcolle/dsco-cli/actions/workflows/ci.yml)
[![Docs](https://github.com/arthurcolle/dsco-cli/actions/workflows/docs.yml/badge.svg)](https://github.com/arthurcolle/dsco-cli/actions/workflows/docs.yml)
[![Security](https://github.com/arthurcolle/dsco-cli/actions/workflows/security.yml/badge.svg)](https://github.com/arthurcolle/dsco-cli/actions/workflows/security.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**DSCO** (Distributed Systems Corporation Operator) is an agent runtime that lives
in your terminal. It is not a wrapper around a model API — it is a tool-first C
runtime with hierarchical sub-agents, AST-level code intelligence, streaming data
pipelines, pure-C cryptography, and the ability to read and edit its own source.

This repository (`dsco-cli`) is the canonical home of the project.

---

## Contents

- [Highlights](#highlights)
- [At a Glance](#at-a-glance)
- [Architecture](#architecture)
- [Capabilities](#capabilities)
- [Quick Start](#quick-start)
- [Usage](#usage)
- [Build Targets](#build-targets)
- [Documentation](#documentation)
- [Repository Layout](#repository-layout)
- [Contributing](#contributing)
- [Security](#security)
- [License](#license)

---

## Highlights

- **Local-first.** Runs entirely from your terminal; state lives on your machine.
- **Tool-first.** 171 built-in tools (plus any MCP-provided tools) span files,
  git, shell, compilation, data pipelines, crypto, market data, and web research.
- **Self-introspecting.** Reads, analyzes (via AST), and edits its own codebase.
- **Swarm-capable.** Nested sub-agents and pre-built orchestration topologies.
- **Pure C.** ~344K lines across `src/` and `include/`; no model code linked in.

## At a Glance

| Property       | Value                                                    |
| -------------- | -------------------------------------------------------- |
| Version        | `1.0.2`                                                  |
| Language       | C (C11)                                                  |
| Source size    | ~344K LOC across `src/` + `include/`                     |
| Source files   | 104 `.c` / 108 `.h`                                      |
| Built-in tools | 171 (see [`docs/TOOL_CATALOG.md`](docs/TOOL_CATALOG.md)) |
| Orchestration  | Hierarchical swarms, documented topologies               |
| Platforms      | macOS (primary), Linux (CI: gcc + clang)                 |
| License        | MIT                                                      |

> Counts above are ground-truthed against the source tree. Regenerate the tool
> catalog with `./scripts/gen_tool_catalog.sh` after registry changes.

## Architecture

DSCO is organized as three cooperating layers.

| Layer             | Responsibility        | Mechanisms                                                                                                        |
| ----------------- | --------------------- | ----------------------------------------------------------------------------------------------------------------- |
| **Wings**         | Autonomy & emergence  | Pheromone-style stigmergy, three-tier memory (working/episodic/semantic), hierarchical swarms, capability routing |
| **Talons**        | Competitive execution | Goal hunt-states, grip-strength retry semantics, strategy tournaments that learn over time                        |
| **Immune System** | Governance & safety   | OODA loops, kill switches, resource budgets, principal tiers, hardcoded safety invariants                         |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/DIAGRAMS.md`](docs/DIAGRAMS.md) for runtime flows and diagrams.

## Capabilities

| Area                | What it provides                                                               |
| ------------------- | ------------------------------------------------------------------------------ |
| Code intelligence   | AST introspection, call graphs, dependency analysis, in-place self-editing     |
| Data & pipelines    | Coroutine-based streaming pipeline stages via the `pipeline` tool              |
| Market intelligence | Financial/market tooling, including Kalshi contract ingestion and analysis     |
| Orchestration       | Swarm orchestration, dynamic topology selection, multi-executor support        |
| Local integration   | macOS-native bridges (Spotlight, AppleScript/JXA, notifications, LaunchAgents) |
| Cryptography        | Pure-C SHA-256, HMAC, HKDF, JWT, UUID (no OpenSSL dependency)                  |
| Media & docs        | FFmpeg, ImageMagick, and Pandoc-backed document conversion                     |
| Observability       | SQLite timeline, trace spans, TUI with markdown rendering                      |

The full, generated tool list lives in [`docs/TOOL_CATALOG.md`](docs/TOOL_CATALOG.md).

## Quick Start

### Install with npm

```bash
npm install -g @distributed.systems/dsco
dsco --version
```

Or run directly:

```bash
npx @distributed.systems/dsco --version
```

The npm package downloads the native DSCO binary from GitHub Releases. No Python is required.

### Install with Homebrew

```bash
brew install arthurcolle/dsco/dsco
dsco --version
```

### Build from source

Prerequisites:

- A C compiler (`clang` or `gcc`)
- `make`
- macOS or Linux

```bash
git clone https://github.com/arthurcolle/dsco.git
cd dsco
./scripts/bootstrap.sh   # provision dependencies
make -j8                 # build the dsco binary
make test                # run the test suite
```

### Run

```bash
export ANTHROPIC_API_KEY=...   # or another supported provider key
./dsco "summarize the architecture of this repository"
```

Check the build:

```bash
./dsco --version
# dsco v1.0.2 (built ..., <commit>)
```

## Usage

```bash
# One-shot task
./dsco "refactor src/json_util.c to add a streaming parser"

# Local model via Ollama
./dsco --local --ollama --pull-and-use gpt-oss:20b

# Inspect available tools
./dsco "list your tools"
```

For provider configuration, environment variables, and storage locations, see
[`docs/OPERATIONS.md`](docs/OPERATIONS.md) and
[`docs/CONSTANTS_ENV_INDEX.md`](docs/CONSTANTS_ENV_INDEX.md).

## Build Targets

Common Makefile targets (run `make help`-style discovery via the Makefile itself):

| Target                | Purpose                                      |
| --------------------- | -------------------------------------------- |
| `make` / `make all`   | Build the release `dsco` binary              |
| `make test`           | Run the test suite                           |
| `make fast-build`     | Fast dev build (`-O0 -g3`) into `build/fast` |
| `make fast-quick`     | Fast build + smoke + targeted tests          |
| `make docs`           | Regenerate generated docs                    |
| `make docs-check`     | Verify generated docs are in sync            |
| `make format`         | Apply `clang-format`                         |
| `make lint`           | Static analysis (`clang-tidy`, `cppcheck`)   |
| `make asan` / `ubsan` | Sanitizer builds                             |

The accelerated edit→compile→test loop is documented in
[`BUILD_FAST.md`](BUILD_FAST.md).

## Documentation

Start at [`docs/INDEX.md`](docs/INDEX.md). Key references:

- [Architecture & Runtime Flows](docs/ARCHITECTURE.md)
- [C Module Reference](docs/C_MODULE_REFERENCE.md)
- [API Reference](docs/API_REFERENCE.md) *(generated from headers)*
- [Built-in Tool Catalog](docs/TOOL_CATALOG.md) *(generated from `tools.c`)*
- [Operations & Troubleshooting](docs/OPERATIONS.md)
- [How-To Guides](docs/HOW_TO.md)
- [Runbooks](docs/RUNBOOKS.md)

## Repository Layout

```text
dsco-cli/
├── src/            # C runtime implementation (104 .c)
├── include/        # Public headers (108 .h)
├── tests/          # Runtime and CLI tests
├── scripts/        # Build, packaging, docs-generation, smoke helpers
├── docs/           # Full documentation set (see docs/INDEX.md)
├── web/            # Local web surface
├── Formula/        # Homebrew formula
├── Makefile        # Canonical build
└── build.ninja     # Generated dev build graph (optional)
```

## Contributing

Contributions for runtime behavior, tools, docs, CI, and scripts are welcome.
Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md) first.

Before opening a PR:

```bash
make test
make docs-check
pre-commit run --all-files   # optional but recommended
```

If you change the tool registry or header declarations, regenerate the affected
docs (`./scripts/gen_tool_catalog.sh`, `./scripts/gen_api_reference.sh`) and
include them in the same PR.

## Security

Please report vulnerabilities via GitHub Private Vulnerability Reporting. Details
and response targets are in [`SECURITY.md`](SECURITY.md). Do not file public
issues containing exploit details.

## License

Released under the [MIT License](LICENSE).

---

Built by Arthur Colle ([distributed.systems](https://distributed.systems)) as the
foundation for a local-first agentic infrastructure platform.

> "Big things have small beginnings."
