#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

usage() {
  cat <<USAGE
Usage: $0 [--no-pre-commit]

Installs build/test/lint dependencies for dsco-cli on macOS or Debian/Ubuntu.
USAGE
}

INSTALL_PRE_COMMIT=1
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ "${1:-}" == "--no-pre-commit" ]]; then
  INSTALL_PRE_COMMIT=0
elif [[ $# -gt 0 ]]; then
  usage >&2
  exit 1
fi

OS="$(uname -s)"
case "$OS" in
  Darwin)
    if ! command -v brew >/dev/null 2>&1; then
      echo "error: Homebrew is required on macOS" >&2
      echo "install from https://brew.sh and rerun this script" >&2
      exit 1
    fi

    brew install llvm cppcheck shellcheck pre-commit
    ;;

  Linux)
    if ! command -v apt-get >/dev/null 2>&1; then
      echo "error: unsupported Linux distro (expected apt-get)" >&2
      exit 1
    fi

    sudo apt-get update
    sudo apt-get install -y \
      build-essential clang gcc make pkg-config \
      libcurl4-openssl-dev libsqlite3-dev libreadline-dev \
      clang-format clang-tidy cppcheck shellcheck \
      python3 python3-pip pre-commit
    ;;

  *)
    echo "error: unsupported OS '$OS'" >&2
    exit 1
    ;;
esac

if [[ "$INSTALL_PRE_COMMIT" -eq 1 ]]; then
  if command -v pre-commit >/dev/null 2>&1; then
    pre-commit install
  else
    python3 -m pip install --user pre-commit
    "$HOME/.local/bin/pre-commit" install
  fi
fi

echo "bootstrap complete"
echo "next: make -j4 && make test && make lint"
