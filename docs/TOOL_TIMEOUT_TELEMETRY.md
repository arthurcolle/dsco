# Tool timeout telemetry repair

Chronicle tool.call.completed now includes execution_status, failure_class and
timeout_origin in addition to existing ok/timeout/latency_ms.

Origins: harness_wall and harness_idle are set at run_cmd_ex terminal branches;
tool_deadline is the existing outer watchdog fallback; none means no timeout
reported. Status comes from thread-local execution metadata, not output parsing.
Watchdog start resets the calling thread. Concurrent worker slots transfer the
origin to the recording thread. Agent timeout recognition now includes shell
wall/idle origins, so these outcomes are not cached as ordinary completed calls.
Existing governance routing is unchanged.

Tool-call starts also carry wrapper_tool_name and requested_tool_name for
invoke_tool's name field. Requested identity is not asserted to be resolved or
executed identity. Raw argument fields are not copied by this addition.

Verification performed:
- make dsco passed (build only).
- Thread-local isolation/reset/first-origin unit test passed.
- Linked production Chronicle round-trip fixture passed for idle/wall/non-timeout,
  output-marker spoof resistance, requested target and argument exclusion.
- make test-gate-claims: 9 passed, 0 failed, 0 skipped before the final
  requested-target metadata addition.

Tests: tests/test_tool_telemetry.c and tests/test_telemetry_chronicle.c.
The Chronicle fixture accepts a disposable Chronicle directory as argv[1] and
links against runtime objects excluding main.o. No LLM required.

Limits: historical events are not rewritten. Worker-specific deadline origins,
resolved remote identities, graph projection of these new fields and generalized
failure classification remain separate work. Non-timeout failures are explicitly
unspecified. No arbitrary result strings are interpreted as trusted status.
The tests exercise metadata isolation and serialization, not a full model-driven
agent timeout or every nested/thread-hopping dispatch path.
