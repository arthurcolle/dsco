# @distributed.systems/dsco

Install DSCO from npm without Python.

DSCO is a local-first agentic CLI runtime written in C. This npm package is a thin installer/launcher that downloads the appropriate DSCO binary from GitHub Releases and exposes:

- `dsco`
- `dsc`
- `dsco-lite`

## Install

```bash
npm install -g @distributed.systems/dsco
```

Then:

```bash
dsco --version
```

## Use with npx

```bash
npx @distributed.systems/dsco --version
```

## Supported platforms

Initial npm binary distribution targets:

- macOS arm64
- macOS x64
- Linux x64

Linux arm64 can be added once GitHub Release artifacts are available.

## How it works

On install, `scripts/install.js` downloads a tarball from:

```text
https://github.com/arthurcolle/dsco/releases/download/v<version>/dsco-<platform>-<arch>.tar.gz
```

and places binaries under `vendor/` inside the npm package.

No Python is used.

## Environment variables

- `DSCO_SKIP_DOWNLOAD=1` — skip binary download, useful for packaging tests.
- `DSCO_BINARY_HOST=<url>` — override release host, useful for mirrors.

## Alternative installs

Homebrew:

```bash
brew install arthurcolle/dsco/dsco
```

Source:

```bash
git clone https://github.com/arthurcolle/dsco
cd dsco
make
```
