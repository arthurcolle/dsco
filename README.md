# dsco-cli

`dsco` is a C-first agentic CLI with streaming LLM interaction, tool execution, swarms/sub-agents, MCP integration, plugins, markdown rendering, semantic routing, and timeline/trace observability.

## Quick Start

### Bootstrap Dependencies (macOS/Linux)

```bash
./scripts/bootstrap.sh
```

### Build

```bash
make -j8
make debug   # build dsco-debug with symbols for stepping/debugging
```

### Run (Interactive)

```bash
export ANTHROPIC_API_KEY=...
./dsco
```

For step-through debugging in development mode, use the symbolized artifact:

```bash
make debug
DSCO_TRACE_STDERR=1 ./dsco-debug -m sonnet
```

`dsco-debug` defaults to `DSCO_TRACE=debug` unless `DSCO_TRACE` is explicitly set (for example `DSCO_TRACE=0`).

### Run (One-Shot)

```bash
./dsco "inspect this repository and summarize module boundaries"
```

### Run Tests

```bash
make test
```

### Hygiene Checks

```bash
make lint
make static-analysis
make asan-test
make ubsan-test
make check-version
make docs-check
pre-commit run --all-files
```

## Core Capabilities

- Streaming LLM responses with tool-use loop (`agent.c`, `llm.c`)
- 288 built-in tools plus MCP-discovered and plugin-provided tools (`tools.c`, `mcp.c`, `plugin.c`)
- Multi-provider model support (Anthropic + OpenAI-compatible providers) (`provider.c`)
- Hierarchical swarm/sub-agent orchestration (`swarm.c`, `ipc.c`)
- Rich terminal UI + markdown renderer (`tui.c`, `md.c`)
- SQLite timeline logging and trace spans (`baseline.c`)
- Setup/env profile management (`setup.c`)

## Documentation

Start with the documentation index:

- [Documentation Index](docs/INDEX.md)

Detailed references:

- [Architecture & Runtime Flows](docs/ARCHITECTURE.md)
- [C Module Reference (all `.c`/`.h` files)](docs/C_MODULE_REFERENCE.md)
- [API Reference (auto-generated from headers)](docs/API_REFERENCE.md)
- [Built-in Tool Catalog](docs/TOOL_CATALOG.md)
- [Python & Web Asset Reference](docs/PYTHON_AND_WEB_REFERENCE.md)
- [Operations, Config, Storage, and Troubleshooting](docs/OPERATIONS.md)
- [How-To Guides](docs/HOW_TO.md)
- [Architecture Diagrams](docs/DIAGRAMS.md)
- [Docs Contributing Guide](docs/DOCS_CONTRIBUTING.md)
- [Runbooks](docs/RUNBOOKS.md)

## Repository Layout

- `main.c` / `agent.c`: CLI entrypoints and interactive loop
- `llm.c` / `provider.c`: request building, SSE streaming, provider abstraction
- `tools.c` / `integrations.c`: built-in tools and external API wrappers
- `swarm.c` / `ipc.c`: process orchestration and SQLite-backed inter-agent IPC
- `md.c` / `tui.c`: rendering and terminal UX
- `baseline.c`: timeline web server + trace span persistence
- `scripts/`: large domain-specific Python analyzers
- `www/`: static freight dashboard/report pages

## Governance

- [Contributing Guide](CONTRIBUTING.md)
- [Security Policy](SECURITY.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Code Owners](CODEOWNERS)

## License

This project is licensed under the [MIT License](LICENSE).
