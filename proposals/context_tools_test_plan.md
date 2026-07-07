# Context Tools Test & Live-Smoke Plan — Quorum C

**Status:** PROPOSED (pre-commit gate)
**Scope:** `context_search`, `context_get`, `context_get_batch`, `context_stats`,
`context_summarize`, `context_pack`, `context_fuse`, `context_pin`, `context_gc`
**Source ground truth:** `src/tools.c` L16098–L16864 (implementations),
L1382–L1431 (data structures), L1563–L1660 (store/evict/reset),
L2549–L2640 (ladder ranking), L28166–L28201 (dispatch), L31144–L31174 (registry)

---

## 1. Architecture Summary (from source)

All nine tools operate on a single global in-memory store:

```
ctx_store_t g_ctx:
  chunks[CTX_MAX_CHUNKS=2048]   // flat array, compaction on evict
  count                          // active chunk count
  next_id                        // monotonically increasing (starts at 1)
  total_bytes                    // sum of text_len+1 across active chunks
  bucket_df[CTX_EMBED_DIM]       // document-frequency for BM25-like scoring
```

Each `ctx_chunk_t` carries: `id`, `tool[48]`, `text*` (heap), `text_len`,
`pinned`, `turn_pinned`, `hash` (FNV-1a 64), `embed[]`, `created_at`.

**Eviction:** `ctx_evict_index(idx)` does `memmove` compaction (O(n) shift),
frees text, decrements count. Auto-eviction on store happens when
`count >= 2048 || total_bytes + len + 1 > 24MB`. Oldest unpinned chunk evicted
first (`ctx_oldest_evictable_index`).

**Dedup:** `ctx_chunk_exists(hash, text, len)` — content hash + length +
`memcmp`. Identical content is silently dropped.

**Ranking ladder:** `ctx_rank_hits_ladder` falls back: strict → source_only →
facet_only → no_meta → degraded. Mode string emitted in results.

**Pin/GC interaction:** `context_gc` skips `pinned || turn_pinned` chunks.
`context_pin` mutates `pinned` directly on the chunk by index lookup.

---

## 2. Unit Test Battery (`tests/test.c`)

New test function: `test_context_store_lifecycle(void)` — 34 assertions.

### Phase A: Empty Store (6 tests)

| # | Test | Method | Expected |
|---|------|--------|----------|
| A1 | `context_stats` on empty store | call with `g_ctx.count==0` | Returns `"context store is empty"` + offload counters, `return true` |
| A2 | `context_search` with empty store | `{"query":"test"}` | Returns `"no retrieval hits"` or empty-hit message, `return true` (not crash) |
| A3 | `context_get` with invalid id on empty store | `{"chunk_id":1}` | Returns `"error: chunk_id 1 not found"` with `active_chunk_range=-1--1 total_chunks=0`, `return false` |
| A4 | `context_get_batch` with empty store | `{"chunk_ids":[1,2,3]}` | Returns `"error: none of 3 chunk_ids found"`, `return false` |
| A5 | `context_summarize` with empty store | `{"query":"test"}` | Returns `"no hits for summary query"`, `return true` |
| A6 | `context_gc` on empty store | `{"max_chunks":10}` | Returns `"removed=0 chunks=0"`, `return true` (no crash, no underflow) |

### Phase B: Store → Search → Get Roundtrip (6 tests)

| # | Test | Method | Expected |
|---|------|--------|----------|
| B1 | Store a chunk, verify `context_stats` reflects it | Inject via `ctx_store_chunk`, call stats | `chunks=1`, per-tool breakdown present, bytes match |
| B2 | `context_search` finds stored chunk | `{"query":"<matching term>"}` | Returns `chunk_id=1`, non-zero `score`, `return true` |
| B3 | `context_get` retrieves full text | `{"chunk_id":1}` | Returns `chunk_id=1 tool=<name> bytes=<len>` + full text |
| B4 | `context_get` respects `max_chars` clamp | `{"chunk_id":1,"max_chars":50}` | Text truncated to ≤50 chars (after 200 minimum clamp) |
| B5 | `context_get` respects upper bound | `{"chunk_id":1,"max_chars":99999}` | Clamped to 24000 |
| B6 | `context_get_batch` retrieves multiple | `{"chunk_ids":[1,2]}` after storing 2 | Returns both chunks, `"found=2 missing=0"` |

### Phase C: Eviction & Missing Chunks (5 tests)

| # | Test | Method | Expected |
|---|------|--------|----------|
| C1 | `context_get` on evicted chunk_id | Store 1 chunk (id=1), evict it, then `get(1)` | Error with `active_chunk_range` showing no chunks, recovery hint |
| C2 | `context_get` on never-existed chunk_id | `{"chunk_id":99999}` on populated store | Error with active range hint |
| C3 | `context_get_batch` with mixed found/missing | Store ids 1,2; batch_get `[1,999,2]` | Returns found=2, missing=1; does NOT return false |
| C4 | `context_get_batch` with all-missing | `{"chunk_ids":[999,998]}` | Returns false with evicted error |
| C5 | `context_get_batch` empty array | `{"chunk_ids":[]}` | Returns false: `"chunk_ids array is empty"` |

### Phase D: Pin / GC Mutation (7 tests)

| # | Test | Method | Expected |
|---|------|--------|----------|
| D1 | `context_pin` existing chunk | `{"chunk_id":1,"pin":true}` | Returns `"chunk 1 pinned"`, `return true`; verify `g_ctx.chunks[idx].pinned == true` |
| D2 | `context_pin` default is pin=true | `{"chunk_id":1}` (no pin field) | Chunk gets pinned |
| D3 | `context_pin` unpin | `{"chunk_id":1,"pin":false}` | Returns `"chunk 1 unpinned"` |
| D4 | `context_pin` missing chunk | `{"chunk_id":999}` | Returns false: `"error: chunk_id 999 not found"` |
| D5 | `context_gc` preserves pinned chunks | Store 5 chunks, pin #2 and #4, gc with `max_chunks=1` | Pinned survive; unpinned evicted; `removed` count correct |
| D6 | `context_gc` respects `keep_recent` | Store 10 chunks, none pinned, gc `max_chunks=2 keep_recent=3` | 3 most-recent survive + 2 max → actually 2-3 survive (whichever is more restrictive); verify no crash |
| D7 | `context_gc` clamps invalid params | `{"max_chunks":1,"max_bytes":1,"keep_recent":-1}` | Clamped to `max_chunks>=8`, `max_bytes>=4096`, `keep_recent>=0`; no crash |

### Phase E: Ranking & Fusion (5 tests)

| # | Test | Method | Expected |
|---|------|--------|----------|
| E1 | `context_search` with `tool` filter | Store chunks from toolA and toolB; search with `{"query":"x","tool":"toolA"}` | Only toolA chunks returned |
| E2 | `context_summarize` returns abbreviated previews | Store chunk, call summarize | Each hit line ≤ `max_chars_per_chunk`; shorter than full `context_get` |
| E3 | `context_pack` respects total budget | Store large chunks, pack with `max_chars_total=500` | `packed_chars <= 500` (approximate; budget enforced in loop) |
| E4 | `context_fuse` with multiple queries | `{"queries":["q1","q2"]}` on populated store | Returns fused results with `fused` score and `hit_count`; RRF ordering |
| E5 | `context_fuse` fallback to single `query` | `{"query":"solo"}` without `queries[]` | Works with single query, `queries=1` in output |

### Phase F: Schema & Error Handling (5 tests)

| # | Test | Method | Expected |
|---|------|--------|----------|
| F1 | `context_search` missing query | `{}` | Returns false: `"error: query required"` |
| F2 | `context_pack` missing query | `{}` | Returns false: `"error: query required"` |
| F3 | `context_fuse` missing both query and queries | `{}` | Returns false: `"error: query or queries[] required"` |
| F4 | `context_get` missing chunk_id | `{}` | Returns false: `"error: chunk_id required"` |
| F5 | `context_get_batch` missing chunk_ids key | `{"max_chars_each":500}` | Returns false: `"error: chunk_ids array required"` |

---

## 3. Live Smoke Protocol (post-build, pre-commit)

Executed against the compiled `dsco` binary via tool dispatch.
Requires a running dsco agent session or direct tool invocation harness.

### Smoke Sequence (ordered, 14 steps)

```
STEP  TOOL                INPUT                                  ASSERTION
─────────────────────────────────────────────────────────────────────────────
 S1   context_stats       {}                                     → "empty" or baseline state; no crash
 S2   context_search      {"query":"nonexistent_xyzzy"}          → "no retrieval hits"; return true
 S3   context_get         {"chunk_id":99999}                     → evicted error with range hint
 S4   context_gc          {"max_chunks":8}                       → "removed=0" on empty; no underflow
 S5   context_pin         {"chunk_id":99999}                     → "not found" error (no false-positive pin)
 S6   context_summarize   {"query":"test"}                       → "no hits" gracefully
 S7   context_fuse        {"queries":["a","b"]}                  → "no fused hits" gracefully
 S8   context_pack        {"query":"test"}                       → "no packable hits" gracefully
 S9   (inject test data)  —                                      → run a tool that produces ≥2KB output
 S10  context_stats       {}                                     → shows chunks≥1, per-tool breakdown
 S11  context_search      {"query":"<injected term>"}            → returns hit with score
 S12  context_pin         {"chunk_id":<found_id>}                → "pinned" confirmation
 S13  context_gc          {"max_chunks":0}                       → pinned chunk survives; clamped to 8
 S14  context_get         {"chunk_id":<pinned_id>}               → still retrievable post-gc
```

### Smoke Pass Criteria
- **Hard fail:** Any SIGSEGV/SIGABRT, any JSON parse error in tool output, any
  tool that returns `false` when the spec says `true` or vice versa.
- **Soft warn:** Unusual `mode` values, score=0.000 on an obvious match,
  `estimated_tokens_saved` underflow (negative).

---

## 4. Edge Case Deep-Dive

### 4.1 Empty Store (all 9 tools)

The store starts at `g_ctx = {0}` with `count=0, next_id=0`. After first
`ctx_store_chunk`, `next_id` becomes 1 (set in `ctx_store_reset`).

**Critical path:** `context_stats` (L16300) has an explicit empty-store branch
that returns offload counters. But `context_search` (L16113) only has the
empty path implicitly through `ctx_rank_hits_ladder` returning 0 hits —
verify no NULL deref on `g_ctx.chunks[0]` when `count==0`.

**Risk:** `context_stats` (L16326) does `strstr(g_ctx.chunks[i].text, ...)`
in a loop bounded by `g_ctx.count`. With `count==0` the loop doesn't
execute — safe. But if `count>0` and `chunks[i].text` is NULL (corruption),
this crashes. Test stores and evicts to verify text pointer lifecycle.

### 4.2 Evicted Chunks

Eviction via `ctx_evict_index` does `memmove` — so chunk indices shift after
eviction. A chunk_id that was at index 3 might now be at index 2 after
eviction of index 1.

**`context_get` on evicted id:** `ctx_find_index_by_id` scans the array
linearly. Returns -1 if not found → `context_get` produces a helpful error
with `active_chunk_range` (oldest..newdest id). This is the single most
important UX path for the public API — verify the error message format
is parseable and includes recovery guidance.

**`context_get_batch` on partially-evicted set:** Counts `missing++` and
skips. Only returns `false` if ALL are missing. This partial-success
behavior is a public API contract — test it explicitly.

### 4.3 Pin/GC Mutation Safety

**Pin invariant:** `context_gc` (L16502) checks `pinned || turn_pinned`
before evicting. If ALL chunks are pinned and `count > max_chunks`, the
inner `while(changed)` loop will spin with `changed=false` on the first
pass where nothing is evictable → loop terminates. No infinite loop.

**Risk:** If `keep_recent=0` and all chunks pinned, `cutoff=count`, loop
tries indices `[0..count)` but skips all pinned → `changed` stays false →
loop exits. Safe but produces `removed=0` even when over-capacity. Document
this as expected behavior.

**GC parameter clamping:** `max_chunks` clamped to `[8, 2048]`,
`max_bytes` to `[4096, 24MB]`, `keep_recent` to `[0, INT_MAX]`. Test with
extreme values (0, negative, INT_MAX).

**Post-GC consistency:** `ctx_recompute_df()` is called after GC (L16511).
Verify that `context_search` still works correctly after a GC that removed
chunks — no stale DF values causing division issues.

### 4.4 Schema Discovery

The tools are currently exposed via:
1. A dispatch wrapper `tool_context_dispatch` (L28166) — **marked
   `__attribute__((unused))`**, meaning it's NOT currently registered.
2. Individual tools are called directly from the main tool dispatch
   switch (L28180–L28196) but only through the dispatch wrapper.

**This is the critical finding:** The dispatch wrapper exists but is
unused — the nine sub-actions are NOT individually registered in the
tool registry (L31144+). Only `context_recall`, `token_audit`,
`context_status`, and `context_compact` are registered.

**Before public exposure, must decide:**
- **Option A:** Register 9 individual tools (`context_search`, `context_get`,
  etc.) each with their own `input_schema_json`.
- **Option B:** Register the dispatch wrapper as a single `context` tool
  with an `action` parameter.

Either way, each tool needs a complete `input_schema_json` with proper
`type`, `properties`, and `required` fields. Currently none of the nine
have registered schemas.

---

## 5. Test Infrastructure Requirements

### 5.1 Unit Test Harness

```c
/* In tests/test.c, add: */
static void test_context_store_lifecycle(void) {
    /* Phase A-F: 34 assertions as specified in §2 */
    /* Uses ctx_store_reset() before each phase for isolation */
    /* Uses ctx_store_chunk() for data injection */
    /* Calls tool_context_*() functions directly */
}
```

**Registration in `main()`:** Add `test_context_store_lifecycle();` call
after existing context window tests (L19133).

**Build:** Add to Makefile `test` target (already compiles `tests/test.c`).

### 5.2 Live Smoke Harness

```bash
#!/bin/bash
# scripts/smoke_context_tools.sh
# Runs the 14-step smoke from §3 against a live dsco binary
# Requires: built dsco, a way to invoke individual tools
```

**Two approaches for tool invocation:**
1. **dsco `-e` exec mode** — if the dispatch wrapper is registered, use
   `dsco -e smoke` extended with context tool probes.
2. **Direct binary** — compile a small `test_context_smoke.c` that links
   against `tools.o` and calls each function with crafted inputs.

### 5.3 Property-Based / Fuzz Additions

| Fuzzer | Input | Property |
|--------|-------|----------|
| `context_get` chunk_id | Random ints [-1, 100000] | Never crashes; returns error for invalid |
| `context_gc` params | Random max_chunks [-100, 10000] | Clamped correctly; no underflow |
| `context_search` query | Random strings (empty, unicode, 10KB) | Never crashes; empty → error |
| `context_pin` | Random chunk_ids | Never crashes; only mutates valid chunks |
| `context_get_batch` chunk_ids | Arrays of mixed valid/invalid | Partial success never crashes |

---

## 6. Pre-Commit Gate Checklist

```
[ ] 34 unit tests pass (Phase A-F)
[ ] 14-step live smoke passes against built binary
[ ] No SIGSEGV/SIGABRT in any path
[ ] context_gc never produces negative removed count
[ ] context_stats never produces negative tokens_saved
[ ] Pinned chunks survive gc in all configurations
[ ] Evicted chunk_id returns parseable error with range hint
[ ] context_get_batch partial success returns true with missing count
[ ] Empty store: all 9 tools return gracefully (no crash, no hang)
[ ] Schema: each public tool has complete input_schema_json
[ ] Dispatch wrapper: decide registration strategy (9 tools vs 1)
```

---

## 7. Implementation Order

1. **Write `test_context_store_lifecycle()`** — 34 assertions, Phase A-F
2. **Add to `main()`** test runner
3. **Build + run unit tests** — fix any failures
4. **Write `scripts/smoke_context_tools.sh`** — 14-step live smoke
5. **Register schemas** — whichever dispatch strategy is chosen
6. **Run live smoke** — fix any failures
7. **Add fuzz targets** — 5 property tests
8. **Commit** — gate passes, tools are public

---

## Appendix: Tool Parameter Reference (from source)

| Tool | Required | Optional | Defaults | Clamps |
|------|----------|----------|----------|--------|
| `context_search` | `query` | `tool`, `facet`, `source_id`, `top_k` | top_k=5 | top_k ≤ 12 |
| `context_get` | `chunk_id` | `max_chars` | max_chars=4000 | [200, 24000] |
| `context_get_batch` | `chunk_ids[]` | `max_chars_each` | max_chars_each=2000 | [200, 8000], ids ≤ 64 |
| `context_stats` | — | — | — | — |
| `context_summarize` | `query` | `tool`, `facet`, `source_id`, `top_k`, `max_chars_per_chunk` | top_k=4, max_chars=260 | max_chars [80, 1200] |
| `context_pack` | `query` | `tool`, `facet`, `source_id`, `top_k`, `max_chars_total`, `max_chars_per_chunk` | top_k=8, total=context-scaled, per=total/5 | total [400, 24000], per [100, 4000] |
| `context_fuse` | `query` or `queries[]` | `tool`, `facet`, `source_id`, `top_k_each`, `final_k` | top_k_each=4, final_k=8 | both ≤ 12 |
| `context_pin` | `chunk_id` | `pin` | pin=true | — |
| `context_gc` | — | `max_chunks`, `max_bytes`, `keep_recent` | chunks=2048, bytes=24MB, recent=64 | chunks [8, 2048], bytes [4096, 24MB] |
