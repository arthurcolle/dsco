# Native DSCO harness review

Reviewed 2026-09-05 against user-supplied Astra and prompt caching guidance. Those excerpts are supplied specifications, not independently verified service availability.

## Implemented and verified

- Shared autonomy instructions select native DSCO workers, including cheap-mode prompts.
- Main harness instructions encourage bounded recursive delegation, coordinator continuation, result collection, proportional tests, and precise reporting of conflicting skill instructions.
- Tool-level codex/openai-codex/chatgpt-codex executor aliases resolve to DSCO.
- swarm_spawn_executor also routes EXECUTOR_CODEX through native spawning, covering direct C callers.
- Swarm schema tells models to use DSCO and separates provider authentication from runtime selection.
- Existing native spawn path clears DSCO_EXEC before exec, preserves lineage, sets depth, isolates credentials, and selects a worker profile.
- Extended real-binary dsco-subgoal test exercises both native spawn and legacy EXECUTOR_CODEX spawn, checking successful DSCO version output and EXECUTOR_DSCO metadata.
- make and ASan runner build succeeded. DSCO_TEST_ONLY=dsco-subgoal under ASan passed against rebuilt ./dsco. This verifies process spawning, not paid model task completion.

## Remaining migration gaps

1. Direct OpenAI provider currently uses Chat Completions. Responses implementations in provider.c are specialized for the ChatGPT subscription and Abliteration routes; they are not a verified direct OpenAI Responses adapter. User-supplied Astra specification requires Responses for tools. Do not advertise Astra tool compatibility until a direct adapter is implemented and request/stream/tool-roundtrip fixtures pass.
2. GPT-5.6-specific cache predicates do not recognize Astra. Migrate via a model/endpoint capability table with wire-level tests, rather than broad name matching that inadvertently changes other gateways.
3. Preserve stable instructions and ordered tool definitions before dynamic state. Test serialized prefixes across native child requests, tool discovery, and followups. Cache keys should group intended compatible agent/principal sessions without exposing credentials; do not assume session-unique keys optimize cross-worker reuse.
4. Verify prompt_cache_options.ttl=30m, explicit breakpoint placement after reusable context, and exclusion of prompt_cache_retention on Astra against the actual Responses endpoint.
5. configuration_update, asynchronous function call completion by original call_id, and WebSocket mid-turn steering are not established by this review. They require explicit pending-call lifecycle, cancellation, recovery, and transport tests; local parallel processes alone do not implement those APIs.
6. Verify reasoning effort normalization and exclusion of temperature, top_p, and logprob fields before claiming Astra request compatibility. EU fast-mode restrictions need explicit region-aware validation.
7. Existing external Claude/Grok/Kimi executor code remains available for explicit callers. Harness guidance selects DSCO; this change specifically reroutes Codex execution and does not claim a general shell-level external-executable prohibition.

## Evidence and rollback

Build logs: /tmp/dsco-native-release-build.log and /tmp/dsco-native-harness-build.log. Test log: /tmp/dsco-native-harness-test.log.

Changes are local and uncommitted in include/config.h, src/tools.c, src/swarm.c, and tests/test.c. Preserve pre-existing edits in these files when reverting individual hunks. No provider model defaults, credentials, governance rules, or production deployment changed.
