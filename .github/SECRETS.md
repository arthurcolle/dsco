# DSCO CI/CD — Required GitHub Secrets
#
# This document lists every GitHub secret the CI/CD pipelines reference.
# Set these in: Settings → Secrets and variables → Actions

## Required Secrets

| Secret Name | Purpose | How to Generate |
|---|---|---|
| `HOMEBREW_TAP_TOKEN` | Push to arthurcolle/homebrew-dsco on release | GitHub → Settings → Developer settings → Personal access tokens → Fine-grained → Repository access to `homebrew-dsco` → Contents: Read & Write |
| `NPM_TOKEN` | Publish `@distributed.systems/dsco` to npm | npm → Access Tokens → Generate New Token → Automation token with publish rights for the `distributed.systems` org | Package Manager.

## Auto-Provided (No Action Needed)

| Secret Name | Purpose |
|---|---|
| `GITHUB_TOKEN` | Auto-provided by GitHub Actions. Used for PR labeling, stale management, CodeQL, gitleaks. |

## Optional Secrets (for future use)

| Secret Name | Purpose |
|---|---|
| `CODECOV_TOKEN` | Upload coverage reports to codecov.io (if coverage job is added) |
| `HOMEBREW_TAP_TEST_BOT` | Trigger brew test-bot on tap repo after formula update |

---

## Setup Instructions

### 1. Create HOMEBREW_TAP_TOKEN

```bash
# Go to GitHub → Settings → Developer settings → Personal access tokens → Fine-grained tokens
# Click "Generate new token"
# Name: DSCO Homebrew Tap CI
# Repository access: Only select repositories → arthurcolle/homebrew-dsco
# Permissions:
#   Contents: Read and write
#   Metadata: Read-only (auto-selected)
# Generate token, copy it
```

### 2. Add to dsco repo secrets

```bash
# Go to arthurcolle/dsco → Settings → Secrets and variables → Actions
# Click "New repository secret"
# Name: HOMEBREW_TAP_TOKEN
# Value: <paste token>
```

### 3. Verify branch protection

```bash
# Go to arthurcolle/dsco → Settings → Branches → Branch protection rules
# Add rule for: main
# Require status checks:
#   - build-test (ubuntu-latest, gcc)
#   - build-test (ubuntu-latest, clang)
#   - build-test (macos-latest, clang)
#   - format-check
#   - version consistency
# Require branches to be up to date before merging
# Require conversation resolution before merging
# Do NOT require linear history (squash merges are fine)
```

### 4. First Release

```bash
# Tag a release
git tag v1.0.3
git push origin v1.0.3

# The release workflow will:
# 1. Build macOS Intel + ARM binaries
# 2. Create source tarball
# 3. Compute SHA256
# 4. Create GitHub Release with artifacts
# 5. Auto-update arthurcolle/homebrew-dsco Formula/dsco.rb
# 6. Users can then: brew install arthurcolle/dsco/dsco
```
