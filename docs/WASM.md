# Browser WASM Core

`dsco-wasm` is the browser-resident control-plane slice of DSCO. It is not a
straight port of the native CLI binary. The native runtime owns shell, git,
SQLite, MCP subprocesses, plugins, terminal control, Keychain/Touch ID, Metal,
and other host capabilities. The WASM runtime owns browser-local state and pure
agent logic.

## Build

Install Emscripten, then run:

```sh
make wasm
```

Without Emscripten, the native ABI smoke still runs:

```sh
make wasm-smoke-native
```

This emits:

- `web/static/dsco_wasm.js`
- `web/static/dsco_wasm.wasm`

Open the demo through an HTTP server, not a `file://` URL:

```sh
python3 -m http.server 3142 -d web/static
```

Then visit:

```text
http://localhost:3142/dsco_wasm_demo.html
```

## Current Exports

- `dsco_wasm_version()`
- `dsco_wasm_exports_json()`
- `dsco_wasm_models_json()`
- `dsco_wasm_tools_json()`
- `dsco_wasm_route_explain(model)`
- `dsco_wasm_tool_exec(name, input_json)`
- `dsco_wasm_session_reset()`
- `dsco_wasm_session_add(role, content)`
- `dsco_wasm_session_state()`

The current pure tools are:

- `route_explain`
- `session_add`
- `session_state`
- `session_reset`
- `echo`

## Boundary

Browser-local:

- model registry snapshots
- route explanation
- transcript/session state
- pure JSON/string tools
- future planning, policy, schema, and tool-selection logic

Bridge-required:

- filesystem outside browser-approved storage
- shell commands
- git operations
- native MCP stdio servers
- SQLite-backed baseline/IPC databases
- Keychain, Touch ID, Secure Enclave, Metal, MLX, and terminal raw mode

This keeps the browser agent inspectable and offline-capable while making host
authority explicit through a future permissioned bridge.

## Durability Contract

- Public C exports are declared in `include/wasm_core.h`.
- `tests/test_wasm_core.c` validates the ABI without requiring Emscripten.
- `web/static/dsco_wasm_demo.html` checks required exports at boot.
- The Emscripten build uses `--no-entry` and `-sFILESYSTEM=0`; host authority
  must be added through explicit bridge APIs, not accidental libc filesystem
  surfaces.
