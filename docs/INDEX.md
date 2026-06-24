# Documentation Index

This documentation set is intended as a full-codebase reference for `dsco-cli`, covering architecture, module internals, runtime behavior, configuration, and extension points.

## Scope

- C source/header modules and supported build/test surfaces
- Public CLI, API, operations, plugin, and runbook documentation
- Supported helper scripts under `scripts/`
- Web assets under `web/`
- Runtime configuration, environment, storage, and observability

## Document Map

- [Architecture & Runtime Flows](ARCHITECTURE.md)
- [C Module Reference](C_MODULE_REFERENCE.md)
- [API Reference](API_REFERENCE.md)
- [Built-in Tool Catalog](TOOL_CATALOG.md)
- [MetaConstruct DSL](META_CONSTRUCT_DSL.md)
- [Operations, Config, Storage, and Troubleshooting](OPERATIONS.md)
- [Constants and Environment Index](CONSTANTS_ENV_INDEX.md)
- [Sakana Fugu Provider](SAKANA_FUGU_PROVIDER.md)
- [Plugin Manifest + Lockfile Spec](PLUGIN_MANIFEST_LOCK_SPEC.md)
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

## Root-Level References

- [`README.md`](../README.md)
- [`CHANGELOG.md`](../CHANGELOG.md)
- [`CONTRIBUTING.md`](../CONTRIBUTING.md)
- [`SECURITY.md`](../SECURITY.md)

## Public Code Areas

- `src/` and `include/` for the C runtime
- `tests/` for supported runtime and CLI tests
- `scripts/` for build, packaging, docs-generation, and provider smoke helpers
- `web/` for the local web surface

## Reading Order

1. Read [Architecture & Runtime Flows](ARCHITECTURE.md) for system-level mental model.
2. Use [C Module Reference](C_MODULE_REFERENCE.md) for per-file implementation details.
3. Use [Tool Catalog](TOOL_CATALOG.md) when working on tool selection/execution behavior.
4. Use [Operations](OPERATIONS.md) for env/config/runtime storage and troubleshooting.
5. Use [Runbooks](RUNBOOKS.md) for operational procedures.
