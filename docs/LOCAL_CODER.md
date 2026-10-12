# Local coding with real DSCO tools

`dsco-local-coder` runs the resident coding model through DSCO's native agent loop.
It is not a second Python agent or a mock tool runner: file reads, edits, and shell
commands use DSCO's registered tools and `tools_execute_for_tier()` dispatch.

## Use

```sh
dsco-local-coder --check
dsco-local-coder --directory /path/to/project \
  'Inspect the code, implement the requested fix, run its tests, and report the diff.'
```

The checked-in entry point also runs directly:

```sh
python3 scripts/local_coder.py --check
python3 scripts/local_coder.py --directory /path/to/project 'Your coding task'
```

The default endpoint is `http://127.0.0.1:18080/v1` on the inference host. In the current
fleet deployment **the inference host runs the model and a separate machine acts as the gateway**.
When running the command on the gateway machine, select its gateway explicitly:

```sh
python3 scripts/local_coder.py --endpoint http://127.0.0.1:8080/v1 'Your coding task'
```

`--check` queries the real `/ready` and `/v1/models` endpoints, confirms they agree,
and returns JSON without starting an inference session. Failure exits nonzero;
it does not silently switch providers. A running native `dsco` binary must be on
PATH for coding sessions, or be supplied with `--dsco-bin /absolute/path/to/dsco`.
`DSCO_CODER_BIN` and `DSCO_CODER_ENDPOINT` supply equivalent defaults.

## Routing and authority

- Pins the native `mlx` provider and `mlx:default_model`, with both MLX base-URL
  overrides set to the selected loopback endpoint.
- Disables hosted/default provider fallbacks, MCP headless attachment and the
  systems-agent bypass for this child process only.
- Uses worker startup and three canonical tools: `read_file`, `write_file`, and
  `bash`. A smaller register avoids the malformed-call behavior observed with the
  wider menu. An existing operator-supplied allowlist is retained.
- Requires successful governed tool execution for a coding run. Plain no-op
  responses and malformed tool markup receive at most two repair attempts, then
  fail with a nonzero exit. Tool-shaped text is never converted into execution.
  Permission denials stop the run rather than prompting a workaround.
- Defaults to 4,096 output tokens per response and a 32-turn backstop. Explicit
  operator values of `DSCO_MAX_TOKENS` and `DSCO_HARD_TURN_CEILING` are retained.
- Requests standard governance, retaining an explicitly selected paranoid model.
  Existing trust-tier and approval-mode selections are retained; defaults are
  trusted and never. Existing capability opt-outs remain in the environment.
- Denies network-class, secrets-class, and control-plane tools for this run.
  Other tools still pass through the normal capability gate.

**These are not OS sandbox guarantees.** `--directory` changes the working
directory; it does not confine filesystem access. Network-class tool denial does
not prevent an allowed shell process from opening a network connection. Likewise,
a secrets-class denial is not a filename-based lock on every credential file.
Use an actual sandbox or isolated account for stronger containment. This launcher
does not grant immunity from the existing gates or add duplicate-call protection.

Successful tool execution is not proof that an arbitrary coding task is correct.
Run independent tests and inspect the diff; do not use the model's completion text
as the acceptance test. The model has produced redundant calls in prior tests.
Use idempotency/approval controls before connecting side-effecting integrations.
No graphical windows or desktop panels are opened by this command.

The current backend retains up to two prompt-cache entries within a 4 GiB bound.
Full native coding prompts are much larger than the earlier arithmetic probes;
the previous 256 MiB cache repeatedly evicted them. Model weights remain resident
and readiness is reported only after startup warm-up. Both allocations consume
RAM on the inference host, not the gateway.

## Tests

```sh
python3 tests/test_local_coder_launcher.py
DSCO_TEST_LOCAL_CODER=1 python3 tests/test_local_coder_launcher.py
```

The opt-in test contacts the actual model service. Other tests exercise actual
CLI argument validation and a real refused loopback connection, without mocked
HTTP responses. Override `DSCO_TEST_CODER_ENDPOINT` to check Matrix's gateway.

The development trace at `reports/local-coder-integration/native-agent.log`
records the local model using real `read_file`, `write_file`, and `bash` calls to
create an initial documentation/test candidate. Coordinator review corrected
unsupported safety claims and completed its missing tests; a model's completion
claim was not treated as sufficient verification.
