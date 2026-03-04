# Documentation Index

This documentation set is intended as a full-codebase reference for `dsco-cli`, covering architecture, module internals, runtime behavior, configuration, and extension points.

## Scope

- All C source/header modules in the root
- Build/test surfaces (`Makefile`, `test.c`)
- Python utilities in `scripts/` and root `valuation.py`
- Web assets in `www/`
- Runtime configuration, environment, storage, and observability

## Document Map

- [Architecture & Runtime Flows](ARCHITECTURE.md)
- [C Module Reference](C_MODULE_REFERENCE.md)
- [API Reference](API_REFERENCE.md)
- [Built-in Tool Catalog](TOOL_CATALOG.md)
- [Python & Web Asset Reference](PYTHON_AND_WEB_REFERENCE.md)
- [Operations, Config, Storage, and Troubleshooting](OPERATIONS.md)
- [Plugin Manifest + Lockfile Spec](PLUGIN_MANIFEST_LOCK_SPEC.md)
- [Hardening-to-Monetization Roadmap (2026)](HARDENING_MONETIZATION_ROADMAP_2026.md)
- [Hardening Sprint Backlog (2026)](HARDENING_SPRINT_BACKLOG_2026.md)
- [How-To Guides](HOW_TO.md)
- [Architecture Diagrams](DIAGRAMS.md)
- [Docs Contributing Guide](DOCS_CONTRIBUTING.md)
- [Runbooks](RUNBOOKS.md)

## Repository Hygiene

- [Contributing Guide](../CONTRIBUTING.md)
- [Security Policy](../SECURITY.md)
- [Code of Conduct](../CODE_OF_CONDUCT.md)
- [Code Owners](../CODEOWNERS)
- [Pull Request Template](../.github/PULL_REQUEST_TEMPLATE.md)
- [Bug Report Template](../.github/ISSUE_TEMPLATE/bug_report.yml)
- [Feature Request Template](../.github/ISSUE_TEMPLATE/feature_request.yml)
- CI workflow: `../.github/workflows/ci.yml`
- Docs workflow: `../.github/workflows/docs.yml`
- Security workflow: `../.github/workflows/security.yml`
- Bootstrap script: `../scripts/bootstrap.sh`
- Version bump script: `../scripts/bump_version.sh`
- Version consistency check script: `../scripts/check_version_consistency.sh`
- Clang-format check script: `../scripts/clang_format_check.sh`
- Clang-format apply script: `../scripts/clang_format_apply.sh`

## Supplementary In-Repo Design Notes

- `TOOL_SCHEMA_REFERENCE.md`
- `TOOL_SCALING_MATHEMATICS.md`
- `LLM_AS_POLICY_AND_REWARD.md`
- `JINA_RL_LOOP_ARCHITECTURE.md`
- `POST_LLM_VIRTUAL_OS_READING_LIST.md`
- `EXTREME_TOOLKITS_EXECUTION_PLAN.md`

## Codebase Snapshot

| File | LOC |
|---|---:|
| `Makefile` | 160 |
| `main.c` | 405 |
| `agent.c` | 2142 |
| `llm.c` | 2680 |
| `provider.c` | 573 |
| `tools.c` | 9191 |
| `integrations.c` | 1487 |
| `json_util.c` | 795 |
| `baseline.c` | 1052 |
| `setup.c` | 700 |
| `swarm.c` | 556 |
| `ipc.c` | 781 |
| `mcp.c` | 445 |
| `plugin.c` | 230 |
| `semantic.c` | 532 |
| `md.c` | 2235 |
| `tui.c` | 1241 |
| `pipeline.c` | 727 |
| `ast.c` | 813 |
| `eval.c` | 686 |
| `crypto.c` | 511 |
| `error.c` | 83 |
| `test.c` | 695 |
| all `.h` files (combined) | 2057 |
| `scripts/analyst_swarm.py` | 9019 |
| `scripts/equity_kb.py` | 2479 |
| `scripts/valuation.py` | 1336 |
| `scripts/freight_api.py` | 652 |
| `scripts/freight_quant_tools.py` | 2197 |
| `scripts/hormuz_ffa.py` | 883 |
| `valuation.py` | 440 |
| `www/freight.html` | 2135 |
| `www/freight_intelligence_report.html` | 4015 |

## Reading Order

1. Read [Architecture & Runtime Flows](ARCHITECTURE.md) for system-level mental model.
2. Use [C Module Reference](C_MODULE_REFERENCE.md) for per-file implementation details.
3. Use [Tool Catalog](TOOL_CATALOG.md) when working on tool selection/execution behavior.
4. Use [Python & Web Asset Reference](PYTHON_AND_WEB_REFERENCE.md) for ancillary scripts/UI.
5. Use [Operations](OPERATIONS.md) for env/config/runtime storage and troubleshooting.
