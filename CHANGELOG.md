# Changelog

All notable changes to `dsco-cli` should be documented in this file.

The format loosely follows Keep a Changelog with an `Unreleased` section.

## [Unreleased]

### Added

- Chronicle local activity ledger docs now cover startup behavior, env controls, and timeline-server endpoints.
- Integration catalog docs now surface Codex app-directory discovery and doctor tooling.
- External tool catalog docs now materialize the cached Codex app-directory universe separately from built-in tools.
- Repository coverage docs now generate a tracked-file manifest for docs review.
- Cosmopolitan build docs now include the Makefile `cosmo-info` target and native-dependency gating notes.

### Fixed

- Vision input: downscaled images now report the correct media type. When a large
  PNG/HEIC/WEBP is resized to JPEG, `load_and_encode_image` now propagates the
  corrected `image/jpeg` media type to the caller instead of mislabeling the JPEG
  bytes with the original extension's type (which some providers reject).
- Hardened `print_tool_result_ex` against a NULL `result` pointer (latent null
  dereference in the size-preview path; the function already null-guards the body
  preview below it).

### Changed

- Regenerated generated docs: API reference, built-in tool catalog, and constants/env index.
- `make docs` / `make docs-check` now include constants/env, external tool catalog, and repo coverage generators.
- README and docs index now reflect current source counts, built-in tool count, Chronicle, integrations, and Cosmopolitan build lane.

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
