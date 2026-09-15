# Harness control

For inbound MCP/ACP commands, outbound named harness adapters, and the generic
shell-free process bridge, see [Agent-system interoperability](AGENT_INTEROP.md).

Persistent scratch, file and log buffers have independent editable Kitty views.
See [Buffers and views](BUFFERS.md) for `dsco buffer`, `/buffer`, shared panes
and the `buffers` MCP toolset.

DSCO exposes control adapters through the normal agent tool registry and
the capability gate. MCP clients can enable the focused toolsets:

```sh
dsco mcp serve --toolsets core,terminal,desktop,browser --tier trusted
```

| Tool | Controls | Identity and lifetime |
| --- | --- | --- |
| `buffer` / `buffer_view` | Persistent text plus editable, browsing and live follow views | Stable buffer UUID/revision, independent owned Kitty surface IDs |
| `surface` | Owned Kitty OS windows, tabs, splits, layouts, text and keys | Named workspace plus durable `surface_id`; reconciles a private socket and tagged Kitty windows |
| `pty_session` | Interactive terminal processes, input, output, dimensions, wait and cancellation | Opaque `session_id` / `pty:` surface ID; lives within one DSCO process |
| `desktop` | macOS window inventory, accessibility trees, focus and bounds | Exact window ID and process ID, with a unique accessibility-window match |
| `browser_session` | Chrome tabs, DOM/accessibility snapshots, selectors, JavaScript and screenshots | One owned browser per DSCO process; exact browser session and tab IDs |

The existing `computer` tool provides mouse, keyboard and screenshot operations.
`kitty_remote` and `kitten` remain available for lower-level commands. Their
subprocess output is captured with a deadline and cannot consume MCP stdin or
write raw terminal sequences into MCP stdout. Interactive kittens can run in a
`pty_session`.

## Owned Kitty workspace

The command-line entry point uses the same governed tool path as the agent:

```sh
dsco surface start '{"visible":true}'
dsco surface list
```

`start` creates a separate Kitty instance with socket-only remote control and a
private local socket under `~/.dsco/surfaces/<workspace>/`. `DSCO_SURFACE_DIR`
overrides the state root. It does not enable remote control on an existing
Kitty instance. The directory and registry belong to the current user. This
ownership model prevents accidental cross-workspace targeting; it is not a
security boundary against other processes running as that user.

Use the returned IDs to launch and control an interactive DSCO pane:

```json
{"action":"create","workspace":"main","source_surface_id":"RETURNED_ID","request_id":"review-1","type":"window","location":"vsplit","command":"/Users/arthurcolle/.local/bin/dsco","args":["--tui"],"title":"DSCO review"}
```

Additional actions are `inspect`, `focus`, `resize`, `layout`, `detach`, `read`,
`send_text`, `send_key` and `close`. `create` also supports `tab` and `os-window`.
Repeat a `request_id` to reconcile a launch with an uncertain result. The
registry reserves the identity before launching and retains closed records;
closed views are not automatically recreated. Layout and resize actions reject
tabs containing panes outside the registered workspace.

Responses distinguish command acceptance from a verified observation. Inspect
`verified`, the observation, and any explanatory note. Text/key delivery does
not prove that the child application completed an operation; read its output
or inspect the application afterward. Closing a pane can terminate its process.

Separate DSCO prompt panes have independent conversation state. The optional
`run_id` is correlation metadata, not an implicit conversation clone. Existing
Command–Option–Enter shortcuts still create separate interactive sessions;
inline swarm cards and worker log companions retain their distinct behavior.

## Terminal sessions

```json
{"action":"spawn","command":"/bin/cat","cols":100,"rows":30,"ttl_seconds":1800}
{"action":"write","session_id":"RETURNED_ID","input":"hello\n"}
{"action":"read","session_id":"RETURNED_ID","timeout_ms":1000}
{"action":"resize","session_id":"RETURNED_ID","cols":120,"rows":40}
{"action":"close","session_id":"RETURNED_ID"}
```

Spawns use direct executable/argv arguments. Request `/bin/sh` explicitly if a
shell is needed. Output includes UTF-8 text, lossless base64 and byte offsets.
The bounded output ring reports discarded history. Explicit read offsets are
replayable; an omitted offset advances a shared cursor. A wait timeout merely
observes the process. `close`, the session TTL and normal harness shutdown
terminate the owned process group. Sessions do not survive harness restart.

## Desktop and browser

`desktop {"action":"status"}` reports Accessibility and Screen Recording
readiness without prompting. List windows, select an exact `window_id` and
`pid`, then inspect or snapshot that target. Focus and bounds mutations verify
the resulting state. The coordinate space is global display points. The
adapter rejects ambiguous or stale accessibility matches. Permission requests
are separate explicit actions; it never resets system permissions or keychains.

`computer` serializes input with the desktop adapter, checks input permission,
and captures fresh screenshots. Set `"screenshot":false` for an input action
when a screenshot is unnecessary. Image responses use structured content
blocks in MCP and the normal agent/headless paths, including parallel tools. Direct `--tool-exec` includes an image `content`
array; `--tool-exec-raw` also uses the JSON envelope when images are present.
Dynamic control and observation tools bypass the agent result cache.

`browser_session {"action":"launch"}` starts an isolated headless Chrome with a
temporary profile and a private CDP pipe. It preserves the operator's HOME and
does not attach to an existing browser profile. `headless:false` requests a
visible owned browser; `offline:true` supports local/data fixture testing.
Use the returned `session_id` and `tab_id` for navigation and page actions.
Snapshots include DOM text, selectors and accessibility nodes. Click/type
require one visible selector match. Screenshots return PNG image blocks.
`close` without a tab ID closes the owned browser and cleans its own profile.

## Capability behavior

All entry points, including nested Kitty operations, route through
`tools_execute_for_tier()`. Toolset exposure does not grant permission.
Executable actions require `exec`; process creation and terminal input also
require network/write authority because the target program can perform those
operations. Browser navigation and JavaScript are network/write operations.
State-changing workspace actions require write authority for durable records.

Reads of owned PTYs and registered Kitty panes are untrusted input. Raw
Kitty screen reads and computer screenshots are classified as potentially
secret and untrusted input. The existing lethal-trifecta rule can
therefore block later egress. `DSCO_ALLOW_<CAP>=0`, control-plane restrictions,
and the existing explicit exfiltration override retain their usual meaning.
The adapters do not change global grants or credential stores.

## Verification

```sh
make test_pty_session test_desktop_adapter test_surface_transport
make test_browser_session test_harness_surfaces
make test-gate-claims
```

Browser tests use an owned offline fixture. Native desktop mutation and owned
Kitty lifecycle checks require a graphical macOS session; readiness failures
must be reported as such rather than treated as successful input delivery.
See `reports/harness-wiring-20260906/` for the installed-binary verification.
