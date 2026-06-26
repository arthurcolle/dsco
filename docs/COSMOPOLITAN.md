# Cosmopolitan build lane

DSCO has a first-class Cosmopolitan lane for producing Actually Portable Executables (APE) with `cosmocc`.

This is not a toy demo path. It is the portable distribution lane. Today `make cosmo` emits a real APE executable for DSCO's lite front door (`dsco.com`) and wires the full-runtime lane behind `DSCO_COSMO_EXPERIMENTAL_FULL=1`. The native macOS build remains the full-feature development target because it links Darwin frameworks, Objective-C acceleration, Homebrew curl/sqlite/readline, Secure Enclave, Touch ID, Metal, and other host-native libraries. The Cosmopolitan lane is isolated so portability work can advance without breaking the native target.

## Pinned toolchain

Default toolchain:

- Cosmopolitan `cosmocc` version: `4.0.2`
- Archive: `https://github.com/jart/cosmopolitan/releases/download/4.0.2/cosmocc-4.0.2.zip`
- SHA-256: `85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44`

The archive is downloaded to `build/cache/` and unpacked to `build/cosmocc/`. Both are gitignored build artifacts.

## Commands

```sh
make cosmo-bootstrap      # fetch + verify cosmocc
make cosmo                # build dsco.com portable APE lane
make cosmo-run            # run dsco.com --version
make cosmo-selftest       # basic portable binary checks
make cosmo-clean          # remove cosmo build outputs
make cosmo-info           # print effective cosmo target/version/mode settings
```

Direct script usage:

```sh
scripts/cosmo_bootstrap.sh
scripts/cosmo_build.sh
```

## Build modes

Set `DSCO_COSMO_MODE`:

```sh
DSCO_COSMO_MODE=normal make cosmo
DSCO_COSMO_MODE=tiny make cosmo
DSCO_COSMO_MODE=tinylinux make cosmo
DSCO_COSMO_MODE=optlinux make cosmo
DSCO_COSMO_MODE=sysv make cosmo
DSCO_COSMO_MODE=dbg make cosmo
```

## Experimental full-runtime lane

The repository can ask the Makefile to attempt a full source build under `cosmocc`:

```sh
DSCO_COSMO_EXPERIMENTAL_FULL=1 \
CFLAGS='-I/path/to/cosmo/include' \
LDLIBS='/path/to/libcurl.a /path/to/libsqlite3.a -lm' \
make cosmo
```

This is intentionally explicit. The full DSCO runtime currently depends on libraries and platform features that must be supplied as Cosmopolitan-compatible objects before a full APE binary is expected to link:

- curl / TLS stack
- sqlite3
- readline or replacement line editor
- optional libsodium / mbedTLS / libuv
- Darwin-only Objective-C and framework integrations must stay gated or stubbed

The Makefile gates Darwin frameworks, Objective-C objects, Secure Enclave, Touch ID, Metal, and LocalAuthentication behind the native build path. `COSMO_BUILD=1` keeps those host-native dependencies out of the portable lane.

## Upgrade process

1. Set `DSCO_COSMOCC_VERSION` and `DSCO_COSMOCC_SHA256`.
2. Run `make cosmo-bootstrap`.
3. Run `make cosmo-selftest`.
4. Run the cross-platform compatibility matrix before publishing artifacts.

Example:

```sh
DSCO_COSMOCC_VERSION=4.0.3 \
DSCO_COSMOCC_SHA256=<verified-sha256> \
make cosmo-bootstrap cosmo-selftest
```

## Distribution notes

`dsco.com` is an APE binary. The build also emits `dsco.com.dbg` for debugger/symbol workflows. On most supported systems it runs directly:

```sh
./dsco.com --version
```

Some Linux setups benefit from installing an APE loader / `binfmt_misc`; Apple Silicon can use `/usr/local/bin/ape` for best performance. See Cosmopolitan's upstream docs for host-specific loader notes.
