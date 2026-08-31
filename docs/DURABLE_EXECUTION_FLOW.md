# Durable Execution — Journal & Replay Architecture

**Status:** shipped (Wave B P1.1–P1.3, P2.1 read side + P1.2 write-side standardization)
**Verified:** `test_journal_wal` 31/31 · `test-gate-claims` 6/6 · live corpus (72 calls, 71 frontier)
**Branch:** `fix/ci-release-readiness` @ `1d304c4`

---

## 1. System context — where the journal sits

```
┌──────────────────────────────────────────────────────────────────────────┐
│                            dsco agent process                              │
│                                                                            │
│   user prompt ─► agent_run() ─► turn loop ─► LLM ─► tool dispatch ─► reply  │
│                       │             │         │          │                 │
│                       │             │         │          │                 │
│                       ▼             ▼         ▼          ▼                  │
│                  ┌───────────────────────────────────────────┐            │
│                  │            Chronicle (chronicle.c)          │            │
│                  │  ┌─────────────────┐   ┌────────────────┐  │            │
│                  │  │  WAL journal     │   │  blob store    │  │            │
│                  │  │  (crash-safe)    │──►│  (sha256 CAS)  │  │            │
│                  │  └─────────────────┘   └────────────────┘  │            │
│                  │  ┌─────────────────┐   ┌────────────────┐  │            │
│                  │  │  SQLite index    │   │  manifest.json │  │            │
│                  │  │  (queryable)     │   │  (run summary) │  │            │
│                  │  └─────────────────┘   └────────────────┘  │            │
│                  └───────────────────────────────────────────┘            │
│                                     │                                       │
└─────────────────────────────────────┼──────────────────────────────────────┘
                                      ▼
                        ~/.dsco/runs/<run-id>/
                          ├── journal.wal      ◄── the durable spine
                          ├── manifest.json    ◄── sealed status/cost/timing
                          └── blobs/           ◄── large prompt/result payloads

           CLI read side (no new run):     Learning read side:
             dsco runs list                  scripts/flywheel_label.py
             dsco runs show  <id>              (episodes → training labels)
             dsco runs replay <id>
             dsco runs gc
```

---

## 2. Write path — how a run becomes a journal

Every durable record is written by the **single** `chronicle_journal_append()`
writer under one mutex, framed and CRC'd, and fsynced for canonical types.

```
agent lifecycle                     chronicle writer                 journal.wal
───────────────                     ────────────────                 ───────────

chronicle_start()
  └─ chronicle_journal_open()
       ├─ mkdir runs/<id>/
       ├─ manifest.write("running")
       └─ chronicle_journal_run_start() ──► append("RUN_START", fsync) ──► [len|crc|RUN_START ]
                                                goal, model, cwd, git_head

agent_run() turn loop:
  each turn:
    journal_turn_start(prompt) ─────────► append("TURN_START", fsync) ──► [len|crc|TURN_START]
                                             turn#, prompt_sha256,
                                             prompt_blob_ref if >32KB

    for each tool the model calls:
      journal_tool_call_record() ───────► append("TOOL_CALL", fsync) ───► [len|crc|TOOL_CALL ]
        (BEFORE dispatch)                    call_id, tool, input,
                                             input_sha256, trust_tier
             │
             ▼
      tools_execute_for_tier()  ◄── capability gate; may cause external effect
             │
             ▼
      journal_tool_result_record() ─────► append("TOOL_RESULT", fsync) ─► [len|crc|TOOL_RESULT]
        (AFTER every exit,                   call_id, ok, elapsed_ms,
         incl. denial/timeout/fail)          result inline OR blob_ref,
                                             result_sha256

    journal_turn_checkpoint() ──────────► append("CHECKPOINT", fsync) ──► [len|crc|CHECKPOINT ]
                                             turn#, cost_usd, tokens,
                                             stop_reason

chronicle_stop()
  ├─ chronicle_journal_run_end() ────────► append("RUN_END", fsync) ────► [len|crc|RUN_END   ]
  │                                          status, cost_usd, turns
  └─ manifest.write("completed")

  Ordering invariant:  TOOL_CALL is on disk (fsynced) BEFORE the tool may
  cause any external effect. TOOL_RESULT is on disk after it returns.
  A crash between them leaves a TOOL_CALL with no matching TOOL_RESULT =
  the replay "frontier".
```

### Record frame format (torn-write safe)

```
        8-byte header            variable payload
   ┌────────────┬────────────┬──────────────────────────────┐
   │ u32 len LE │ u32 crc32  │ {"v":1,"type":..,"payload":..}│ \n
   └────────────┴────────────┴──────────────────────────────┘
     the scanner rejects len==0 or len>32MB, and stops at the first
     CRC mismatch — a half-written tail frame is simply ignored.
```

### Inline-vs-blob threshold

```
   result/prompt size ≤ 32KB  ──►  stored inline in the frame
   result/prompt size >  32KB  ──►  chronicle_blob_put_text() → sha256
                                     frame carries {blob_sha256, byte_len}
```

---

## 3. Two record schemas (standardized, both replayable)

The journal carries two families that the replay engine reconciles:

```
 CANONICAL (uppercase, fsynced, W1)          AGENT-EVENT (lowercase, preview)
 ─────────────────────────────────           ────────────────────────────────
 RUN_START     payload at depth 1             run.started      payload at depth 3
 TURN_START    key: call_id                   turn.started     key: tool_id
 TOOL_CALL     full input + sha256            tool.call        args inline
 TOOL_RESULT   full result or blob ref        tool.result      result_preview (512B)
 CHECKPOINT    cost/tokens/stop               turn.checkpoint  cost/tokens/stop
 RUN_END       status/cost/turns              run.completed    status

 frame.payload = event data ───┐          frame.payload.payload = event data
                               │                      ▲
   replay engine detects canonical vs lowercase by type name and reads
   the correct nesting depth + id key for each. Frontier matching works
   across BOTH shapes.
```

---

## 4. Read path — replay & resume-frontier reconstruction

```
 dsco runs replay <id>
        │
        ▼
 chronicle_replay_run(runs_dir, run_id, out, &summary)
        │
        │  PASS 1 (collect):  scan all frames, gather every TOOL_RESULT /
        │                     tool.result id into a set.
        │        ┌─────────────────────────────────────────────┐
        │        │  results = { call_id₁, call_id₂, ... }        │
        │        └─────────────────────────────────────────────┘
        │
        │  PASS 2 (emit):     scan again, for each frame:
        ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │ TURN_START / turn.started   → step{type:turn}          turns++    │
   │ TOOL_CALL  / tool.call      → step{type:tool.call}     calls++    │
   │     └─ is call_id in results set?                                  │
   │            NO  → frontier:true, policy = idempotent|interrupted    │
   │            YES → matched, replay as memory (never re-execute)      │
   │ TOOL_RESULT/ tool.result    → step{type:tool.result}   results++  │
   │ CHECKPOINT / turn.checkpoint→ step{type:checkpoint}  cost += ...   │
   │ RUN_START/END, run.*        → step{type:run}                       │
   │ corrupt/torn frame          → stop cleanly, summary.corrupt_tail   │
   └─────────────────────────────────────────────────────────────────┘
        │
        ▼
   dsco.replay.summary.v1 {records, turns, tool_calls, tool_results,
                           frontier_calls, checkpoints, cost_usd, corrupt_tail}
```

### Resume policy per frontier call (P2.3 will move to the tool registry)

```
   frontier TOOL_CALL (no matching result at crash time)
        │
        ├─ read-class tool (read_file, grep, ls, find, ...)
        │     → policy "idempotent"  → safe to re-run on resume
        │
        └─ everything else (bash, write_file, http POST, ...)
              → policy "interrupted" → surface to model as tool error;
                let the model decide, never silently re-execute
```

---

## 5. Full end-to-end flow (one crash-and-resume cycle)

```
   ┌────────────┐   RUN_START    ┌──────────────────────────────────────┐
   │  dsco -p   │───────────────►│              journal.wal              │
   │  "..."     │   TURN_START   │  RUN_START                            │
   └────────────┘◄──┐            │  TURN_START(turn=1, prompt_sha)       │
         │          │            │  TOOL_CALL(call-1, read_file) fsync   │
         ▼          │            │  TOOL_RESULT(call-1, ok)              │
   LLM turn 1       │            │  TOOL_CALL(call-2, bash) fsync ◄──────┼── CRASH here
   read_file ───────┘            │            (no TOOL_RESULT written)    │   (kill -9)
   bash ...  ✗ crash             │  CHECKPOINT(turn=1)  ✗ never written   │
                                 └──────────────────────────────────────┘
                                              │
                     dsco runs replay <id>    │  reconstructs:
                                              ▼
                            ┌───────────────────────────────────┐
                            │ turn 1: read_file  → replay memory │
                            │         bash       → FRONTIER      │
                            │              policy: interrupted   │
                            │ cost so far: $X   corrupt_tail:true│
                            └───────────────────────────────────┘
                                              │
                     dsco resume <id> (P2.2, next)              │
                                              ▼
                            rebuild conv from journal, re-enter
                            agent loop at the frontier, surface
                            the interrupted bash as a tool error,
                            continue to completion — no duplicate
                            side effects.
```

---

## 6. CLI surface (standardized)

```
 dsco runs list [--limit N|--all] [--json]
     manifest-based table: RUN-ID  STATUS  STARTED  DUR  RECS  SIZE
     sorted recency-first; default limit 20 over the full corpus

 dsco runs show <run-id>
     raw frame dump (existing)

 dsco runs check <run-id>
     CRC-validate every frame, report torn/corrupt tail (existing)

 dsco runs replay <run-id> [--summary]
     dsco.replay.step.v1 JSONL + dsco.replay.summary.v1 footer
     frontier detection, per-call replay policy, torn-tail safe

 dsco runs gc [--days N] [--dry-run] [--include-running]
     age-based retention (default 30d); refuses status=running unless
     overridden; UUID-shaped guard; bounded recursive delete; byte report
```

---

## 7. Verification matrix

| Property | How proven | Result |
|---|---|---|
| All 6 canonical types written with exact counts | `test_journal_wal` frame counter | ✅ |
| Manifest sealed `completed` | test reads manifest.json | ✅ |
| Replay reconstructs turns/calls/results | test asserts counts | ✅ |
| Frontier = call with no result | test: write_file flagged, read_file not | ✅ |
| Checkpoint cost extracted | test asserts 0.0009 < cost < 0.0051 | ✅ |
| Torn tail stops cleanly + flagged | test truncates 7 bytes, asserts corrupt_tail | ✅ |
| >64KB result → blob ref not inline | test writes 70KB, asserts blob_sha256 | ✅ |
| Both schemas replayable | live corpus: 345 rec, 72 calls, 71 frontier | ✅ |
| Canonical authoritative (no 2x on mixed journals) | pass 1 detects canonical; lowercase passive | ✅ |
| No capability-gate regression | `test-gate-claims` 6/6 | ✅ |
| Clean-HEAD full build | isolated worktree build | ✅ |

---

## 8. What remains (roadmap)

```
 P2.1 replay engine (read side) ......................... ✅ SHIPPED
 P1.2 faithful write capture (prompt/result/blob) ....... ✅ SHIPPED (this pass)
 P2.2 dsco resume <run-id> (rebuild conv, re-enter loop)  ◄── NEXT
 P2.3 idempotency bit on tool registry (replace heuristic)
 P2.4 supervisor child-run linkage (CHILD_RUN records)
 P2.5 crash-injection harness (SIGKILL at random offsets)
 ---- hygiene: 2,133 stale status=running runs need sweep before
      resume's "refuse if running" guard can trust the flag
```
