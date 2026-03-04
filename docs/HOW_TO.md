# How-To Guides

This guide provides task-oriented workflows for common engineering changes in `dsco-cli`.

## 1. Add a New Built-in Tool

1. Implement tool function in `tools.c`:
   - Signature: `static bool tool_xxx(const char *input, char *result, size_t rlen)`.
2. Define robust JSON input parsing and validation logic.
3. Add tool definition entry to the `tool_def_t` registry in `tools.c`:
   - `.name`
   - `.description`
   - `.input_schema_json`
   - `.execute`
4. If timeout-sensitive, add per-tool timeout override in timeout config.
5. Add/adjust tests in `test.c` for:
   - happy path
   - invalid input
   - edge/pathological input
6. Rebuild and test:

```bash
make -j8
make test
```

7. Regenerate docs:

```bash
./scripts/gen_tool_catalog.sh
```

## 2. Add a New External Integration Wrapper

1. Add implementation in `integrations.c` + declaration in `integrations.h`.
2. Ensure env var key handling is explicit and errors are actionable.
3. Add the corresponding built-in tool binding in `tools.c`.
4. Add setup key support in `setup.c` if it should be discoverable by `--setup`.
5. Update [docs/OPERATIONS.md](OPERATIONS.md) env-var section.

## 3. Add a New Provider

1. Extend provider endpoint registry and creation path in `provider.c`.
2. Implement:
   - request builder
   - header builder
   - stream parser callback bridge
3. Update detection heuristics in `provider_detect()`.
4. Add provider key mapping in `provider_resolve_api_key()`.
5. Validate with both interactive and one-shot flows.
6. Update architecture and operations docs.

## 4. Add a New Slash Command

1. Add command parsing branch in `agent.c` main loop.
2. Emit user-facing response via `tui_*` utilities.
3. Log command usage via `baseline_log("command", ...)`.
4. Update `/help` output block in `agent.c`.
5. Update [docs/OPERATIONS.md](OPERATIONS.md) slash command section.

## 5. Add/Modify IPC Task Flow

1. Update task state transitions in `ipc.c` (`pending -> assigned -> running -> done/failed`).
2. Ensure worker side handles task claim/start/complete/fail symmetrically.
3. Confirm stale-agent behavior and heartbeat cadence remain valid.
4. Validate with multi-agent spawning from `swarm.c` tools.
5. Add trace instrumentation if latency-sensitive.

## 6. Debug Streaming Failures

Checklist:

1. Confirm provider and key:
   - `/provider`
   - `/model`
2. Check baseline events and timeline:
   - `./dsco --timeline-server`
   - `/events.json`
3. Check stream diagnostics:
   - `/telemetry`
   - `/trace`
4. Inspect saved debug request payloads from `llm_debug_save_request()`.
5. Verify no regression in request JSON shape (`test.c` has coverage for tool-result serialization).

## 7. Add an MCP Server

1. Create `~/.dsco/mcp.json` with server command/args/env.
2. Start `dsco`, run `/mcp` to verify server + tool discovery.
3. Use `/mcp reload` after config updates.
4. Verify tools appear in `/tools` under MCP section.

## 8. Add a Plugin

1. Build shared library exposing `dsco_plugin_*` symbols.
2. Place the binary in `~/.dsco/plugins`.
3. Create `~/.dsco/plugins/plugin-manifest.json` with `name`, `version`, `hash`, `signer`, and `capabilities`.
4. Update `~/.dsco/plugins/plugins.lock` with the plugin pin (`name`, `version`, `hash`).
5. Run `/plugins validate` (or `plugin_validate` tool) to verify metadata syntax and pin consistency.
6. Run `/plugins` and `/tools` to verify runtime registration.
7. Use `/mcp` only for MCP tools; plugin and MCP are separate extension paths.

## 9. Update Docs After Code Changes

Minimum required updates per PR:

- Behavior/flow changes -> `docs/ARCHITECTURE.md`
- Public declarations changed -> regenerate `docs/API_REFERENCE.md`
- Tool registry changes -> regenerate `docs/TOOL_CATALOG.md`
- Runtime/env/storage changes -> `docs/OPERATIONS.md`

Commands:

```bash
./scripts/gen_api_reference.sh
./scripts/gen_tool_catalog.sh
make docs-check
```

## 10. Release Checklist for Docs Accuracy

1. Build and tests pass.
2. Docs generation checks pass.
3. Markdown lint and link checks pass.
4. `CHANGELOG.md` updated under `Unreleased`.
5. New/changed command surfaces reflected in `README.md` and docs index.
