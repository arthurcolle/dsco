# Chronicle TokenLedger

Chronicle TokenLedger is DSCO's local-first provenance and observability subsystem for agent sessions.

## Purpose

Store small timeline events as references while complete payloads live in a blob store. The ledger exists for replay, debugging, token/cost accounting, safety audits, hallucination forensics, memory contamination detection, benchmark reproducibility, and OpenClaw enterprise observability.

## Event names

Canonical event names:

1. `session.started`
2. `turn.started`
3. `context.materialized`
4. `llm.request.created`
5. `llm.response.delta`
6. `llm.response.completed`
7. `tool.call.created`
8. `tool.call.completed`
9. `memory.read`
10. `memory.write.proposed`
11. `memory.write.committed`
12. `skill.loaded`
13. `skill.write.proposed`
14. `artifact.created`
15. `artifact.promoted`
16. `session.completed`

## Runtime controls

Chronicle starts at process entry from `main.c` and is updated when DSCO enters interactive, one-shot, or timeline-server mode. Calls are idempotent, so later runtime configuration refines the same local activity stream rather than creating an unrelated subsystem.

Environment controls:

| Variable | Purpose |
|---|---|
| `DSCO_CHRONICLE_MODE` | Set to `off` to disable Chronicle for a run. Runtime code also uses mode labels such as `startup`, `interactive`, `oneshot`, and `activity-server`. |
| `DSCO_CHRONICLE_DIR` | Override the ledger/blob directory. Useful for tests, demos, and sandboxed repros. |
| `DSCO_CHRONICLE_SESSION_ID` | Session correlation ID; normally set by DSCO. Do not put this in shell startup files. |

Timeline server endpoints:

| Endpoint | Purpose |
|---|---|
| `/chronicle` | Local HTML activity view. |
| `/chronicle.json` | JSON activity export. |
| `/chronicle/blob/<sha256>` | Blob lookup for referenced payloads, capped by server-side read limits. |

## Storage shape

Timeline events are compact JSONL records:

```json
{
  "ts": "2026-06-26T00:00:00Z",
  "event": "tool.call.completed",
  "session_id": "...",
  "turn_id": "...",
  "span_id": "...",
  "parent_span_id": "...",
  "refs": {
    "input_blob": "sha256:...",
    "output_blob": "sha256:..."
  },
  "usage": {
    "input_tokens": 0,
    "output_tokens": 0,
    "cost_usd": 0.0
  }
}
```

Payload blobs are content-addressed, fsynced, and linked by SHA-256. Secrets must be redacted or encrypted before durable persistence.

## Integration points

- Process/session startup emits local activity metadata before provider resolution.
- Streaming LLM deltas emit `llm.response.delta` activity for visible text and thinking streams.
- LLM request/response construction emits `llm.*` events.
- Tool dispatch emits `tool.call.*` events, including start/end status, timeout state, elapsed time, inputs, and outputs by blob reference.
- Memory operations emit `memory.*` events before and after commitment.
- Skill load/write paths emit `skill.*` events.
- Artifact creation/promotion emits `artifact.*` events.

## Governance

Chronicle is provenance infrastructure, not a decorative timeline UI. It is a control-plane record and should follow DSCO durability, security, reversibility, and observability doctrine.
