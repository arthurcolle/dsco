# Tool hooks

DSCO exposes an operator-controlled hook seam around the governed tool
boundary. Hooks are optional and are inactive unless an environment variable
names an executable. The executable is launched with direct `argv`—never via a
shell—and receives one JSON envelope on stdin.

```sh
DSCO_TOOL_HOOK_BEFORE=/absolute/path/policy-hook \
DSCO_TOOL_HOOK_AFTER=/absolute/path/audit-hook \
./dsco --tool-exec sha256 '{"text":"hello"}'
```

Available variables:

| Variable | Meaning |
| --- | --- |
| `DSCO_TOOL_HOOK` | Fallback executable for every hook event. Specific variables win. |
| `DSCO_TOOL_HOOK_BEFORE` | Runs after a call is proposed and before capability/governance admission. Can deny. |
| `DSCO_TOOL_HOOK_AFTER` | Runs after a successful terminal result. Observational. |
| `DSCO_TOOL_HOOK_FAILED` | Runs after an admitted tool failure. Observational. |
| `DSCO_TOOL_HOOK_DENIED` | Runs after a policy or hook denial. Observational. |
| `DSCO_TOOL_HOOK_TOOLS` | Optional comma-separated exact tool names, or `*`, to filter hooks. |
| `DSCO_TOOL_HOOK_PAYLOAD` | `metadata` (default), `input`, or `full`. Raw values are opt-in and bounded. |
| `DSCO_TOOL_HOOK_PAYLOAD_MAX` | Maximum hook envelope size, 1 KiB–64 KiB; default 32 KiB. |
| `DSCO_TOOL_HOOK_TIMEOUT_MS` | Hook deadline, 1–30,000 ms; default 2,000 ms. |
| `DSCO_TOOL_HOOK_MAX_OUTPUT` | Captured hook output, 256 B–64 KiB; default 8 KiB. |
| `DSCO_TOOL_HOOK_FAIL_MODE` | `deny` (default) or `allow` for a failed before hook. Post hooks never affect execution. |

The hook receives `schema: "dsco.tool_hook.v1"`, the event name, stable
`execution_id` and sequence, requested/dispatched tool names, trust tier,
capability metadata, terminal state, elapsed time, and SHA-256 hashes. A
`full` post-hook payload includes the result; the execution kernel deliberately
does not retain raw input for post-hook replay, so raw input is available to a
before hook when `input` or `full` is selected.

The before hook must exit zero and print one of:

```json
{"decision":"allow"}
{"decision":"deny","reason":"tool is outside this workflow"}
```

Malformed output, timeout, non-zero exit, or an unavailable executable denies
the call by default. `DSCO_TOOL_HOOK_FAIL_MODE=allow` is an explicit
fail-open choice for non-critical local workflows. A before hook can only
veto: it cannot grant `DSCO_ALLOW_*`, change the trust tier, rewrite a tool,
or bypass `tools_execute_for_tier()`.

Hook stdout is not returned to the model. Hook failures after admission are
observational and never rewrite the tool result or execution receipt. Hook
processes are bounded and shell-free, but they inherit the DSCO process
environment; use an operator-owned executable and avoid `full` payloads when
the input/result may contain secrets.

Verification:

```sh
make test-tool-hooks
make test-execution-kernel
```
