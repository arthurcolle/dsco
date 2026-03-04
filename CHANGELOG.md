# Changelog

All notable changes to `dsco-cli` should be documented in this file.

The format loosely follows Keep a Changelog with an `Unreleased` section.

## [Unreleased]

### Added

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
