# Contributing

## Scope

This repository accepts contributions for runtime behavior, tools, docs, CI, and scripts.

## Development Setup

```bash
./scripts/bootstrap.sh
make -j8
make test
```

## Branching and PRs

1. Create a focused branch.
2. Keep changes scoped and atomic.
3. Add/update tests for behavior changes.
4. Update docs for user-facing or architecture-impacting changes.
5. Fill out the PR template.

## Required Local Checks

```bash
make test
make docs-check
```

Optional but recommended:

```bash
pre-commit run --all-files
```

## C Style and Formatting

- Use `.clang-format` for C/C++ formatting.
- Use `.editorconfig` defaults.
- Keep public declarations in headers and implementation details in `.c`.

## Docs Requirements

- If tool registry changes: regenerate `docs/TOOL_CATALOG.md`.
- If header declarations change: regenerate `docs/API_REFERENCE.md`.
- Update architecture/operations docs for behavior/config/storage changes.

## Commit Guidance

- Use descriptive commit messages.
- Keep generated docs and source changes in the same PR.
- Do not include unrelated refactors in bugfix PRs.

## Code Review Expectations

Reviewers focus on:

- correctness and regressions
- runtime safety (timeouts, cancellation, error handling)
- observability impacts (baseline/trace)
- documentation and test completeness
