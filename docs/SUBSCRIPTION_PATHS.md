# Subscription paths

DSCO can catalog and invoke an extensible set of paid-plan or membership CLIs
without baking every vendor into the C runtime. A path manifest describes the
full execution identity and a shell-free argv contract; secrets remain in the
vendor client or the local credential store. The registry ceiling is 65,536
distinct paths; discovery and duplicate validation remain bounded and use
amortized growth plus a sorted identity index rather than quadratic scans.

Discovery is deterministic across:

- `~/.config/dsco/subscription-paths.json`
- `~/.config/dsco/subscription-paths.d/*.json`
- each file or directory in `DSCO_SUBSCRIPTION_PATHS` (colon-separated)

Start from `examples/subscription-paths.example.json`. An entry is cataloged
even when its executable or account session is unavailable. These states are
reported independently by `dsco auth lanes` and `dsco interop status --json`:

- `cataloged`: the manifest parsed and its identity is unique.
- `installed`: the exact executable resolves locally.
- `authenticated`: manifest assertion `unknown`, `ready`, or `missing`.
- `qualified`: the operator has qualified this adapter/model/account shape.
- `automatic_eligible`: requested by the manifest *and* enabled, installed,
  authenticated, and qualified. No other entry enters automatic scheduling.

Explicit execution is:

```sh
dsco interop run vendor-plan-default \
  --model vendor-model \
  --prompt 'Solve this task'
```

The manifest uses an argv array, never a shell command. `{prompt}`, `{model}`,
and `{cwd}` are accepted only as complete arguments. For large or sensitive
prompts use `"prompt_transport": "stdin"` and omit `{prompt}` from `argv`.
Transports are process-backed: `external_process`, `acp_stdio`, `mcp_stdio`, or
`http_bridge` (an explicit local bridge executable, not arbitrary native HTTP).
New manifests default to disabled, unauthenticated, unqualified, and ineligible
unless those properties are explicitly asserted.
