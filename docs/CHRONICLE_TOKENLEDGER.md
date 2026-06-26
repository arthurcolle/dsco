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

- LLM request/response construction emits `llm.*` events.
- Tool dispatch emits `tool.call.*` events.
- Memory operations emit `memory.*` events before and after commitment.
- Skill load/write paths emit `skill.*` events.
- Artifact creation/promotion emits `artifact.*` events.

## Governance

Chronicle is provenance infrastructure, not a decorative timeline UI. It is a control-plane record and should follow DSCO durability, security, reversibility, and observability doctrine.
