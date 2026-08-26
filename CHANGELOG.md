# Changelog

All notable changes to `dsco-cli` should be documented in this file.

The format loosely follows Keep a Changelog with an `Unreleased` section.

## [Unreleased]

_Nothing yet._

## [1.1.0] - 2026-08-26

Promoted from the mixed working tree per the principal's release authorization:
candidate isolated and committed (`edf15e1`), build clean on arm64 macOS, full test
suite green on the local tree and on a fresh clone of the release commit
(5,227/5,227 each; clone builds bake embedded data and derive a new scattered key),
capability-gate end-to-end claim verifier passed (6/0/2 env-skipped).

### Added

- Agent Client Protocol server mode, cloud/runtime ceilings, Context Fabric,
  expanded provider subscription lanes, and native compositor subsystems.
- Durable swarm supervision, provider-fabric coverage scheduling, richer executor
  lanes, and evidence-preserving composite reduction.
- A 48-slot adaptive tool register with larger quorum-scored working and compact
  progressive-discovery banks.
- Deno-style resource-scoped capability grants (`--allow-read=/path`,
  `--allow-net=host`) and background-shell manager tools.
- Chronicle local activity ledger docs now cover startup behavior, env controls,
  and timeline-server endpoints.
- Integration catalog docs now surface Codex app-directory discovery and doctor tooling.
- External tool catalog docs now materialize the cached Codex app-directory universe separately from built-in tools.
- Repository coverage docs now generate a tracked-file manifest for docs review.
- Cosmopolitan build docs now include the Makefile `cosmo-info` target and native-dependency gating notes.

### Changed

- Swarm workers now return engineering-grade evidence, failure modes,
  implementation details, and verification criteria for composite synthesis.
- Tool execution, dynamic external tools, and field-device launchers use tighter
  capability classification and non-escalatable control defaults.
- Regenerated generated docs: API reference, built-in tool catalog, and constants/env index.
- `make docs` / `make docs-check` now include constants/env, external tool catalog, and repo coverage generators.
- README and docs index now reflect current source counts, built-in tool count, Chronicle, integrations, and Cosmopolitan build lane.

### Fixed

- Corrected register-bank allocation invariants and canonical Python tool naming.
- Bounded embedded-data registry traversal and cryptographic keystream blocks.
- Removed undefined standard-stream manipulation from TUI tests and closed a
  C-string self-test failure-path resource leak.
- Classified `tm__*` dynamic tools as network plus untrusted-input capabilities.
- Fixed a hard build failure on fresh clones: make could not resolve the
  generated key/registry headers (no rule existed; order-only phony bakes are
  invisible during dependency resolution). Real delegating rules now produce
  them via `scripts/bake_data.py`, and the key header is rewritten only when
  its content changes, ending perpetual rebuilds of dependents.
- Vision input: downscaled images now report the correct media type. When a large
  PNG/HEIC/WEBP is resized to JPEG, `load_and_encode_image` now propagates the
  corrected `image/jpeg` media type to the caller instead of mislabeling the JPEG
  bytes with the original extension's type (which some providers reject).
- Hardened `print_tool_result_ex` against a NULL `result` pointer (latent null
  dereference in the size-preview path; the function already null-guards the body
  preview below it).

## [1.0.2] - 2026-06-26

### Added

- Terminal cleanup now has a shared sane-restore path for interactive exits.

### Changed

- Cursor-position DSR probes are opt-in via `DSCO_TUI_DSR=1`.
- Version bump tooling now updates `include/config.h`.

### Fixed

- Normal `/quit` and keep-terminal handoff restore shell line discipline,
  bracketed paste, cursor visibility, alt-screen state, and pending terminal
  replies before returning control to the shell.

## [1.0.0]

### Added

- Public release baseline: agent runtime, TUI, MCP/provider integrations,
  vector store, security modules (tamper, sealed store, kill switch), and the
  cross-OSI connector seam.
- Portable build: libsodium-optional `tamper.c`, clang-compatible thread pool,
  and CI dependency provisioning so Linux (gcc + clang) matches macOS.

### Added (pre-1.0 history)

- Comprehensive docs bundle under `docs/`:
  - architecture reference
  - C module reference
  - operations and troubleshooting
  - python/web asset reference
  - full built-in tool catalog
  - how-to guides
  - diagrams
  - docs contributing guide
  - operational runbooks
- Auto-generated API reference from headers: `docs/API_REFERENCE.md`.
- Docs generation scripts:
  - `scripts/gen_api_reference.sh`
  - `scripts/gen_tool_catalog.sh`
- Docs CI workflow:
  - generated-doc drift checks
  - markdown lint
  - link checking
- `Makefile` docs automation targets (`docs`, `docs-check`).
- Cross-platform CI workflow with Linux/macOS build+test matrix:
  - sanitizer jobs (`asan`, `ubsan`)
  - static analysis jobs (`clang-tidy`, `cppcheck`)
  - format/docs/version/pre-commit checks
- Security workflow:
  - CodeQL analysis
  - gitleaks secret scanning
- Release/version tooling:
  - `scripts/bump_version.sh`
  - `scripts/check_version_consistency.sh`
- Developer bootstrap script:
  - `scripts/bootstrap.sh`
- Makefile hygiene targets:
  - `format`, `format-check`, `lint`
  - `clang-tidy`, `cppcheck`, `static-analysis`
  - `asan`, `ubsan`, `asan-test`, `ubsan-test`
  - `check-version`
- Repository governance and contribution hygiene files:
  - `LICENSE`
  - `CONTRIBUTING.md`
  - `SECURITY.md`
  - `CODE_OF_CONDUCT.md`
  - `CODEOWNERS`
  - issue/PR templates under `.github/`

### Changed

- `README.md` and `docs/INDEX.md` expanded for discoverability and navigation.

### Fixed

- N/A

## [0.7.0] - 2026-03-02

### Added

- Baseline project release definition aligned with `DSCO_VERSION`.

### Changed

- N/A

### Fixed

- N/A

## Historical Notes

- Existing historical releases before this changelog was introduced are not yet backfilled.
