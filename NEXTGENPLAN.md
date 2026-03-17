# Next-Gen Smoother Rendering Plan

## Status: Phase 1 Complete

Phase 1 delivered 8→3 lines per tool. These are the remaining gaps and next improvements.

## Gaps from Phase 1

### G1: `tui_throughput_render` adds line between result and divider
**File:** `src/agent.c:2106`
When F39 is enabled and samples ≥ 2, a standalone throughput sparkline line appears after every tool turn. Should fold into section divider or suppress for tool turns.

### G2: `tui_citation_render` adds blank + header + N lines
**File:** `src/agent.c:2108`
When F7 is enabled, citations dump a blank line + "footnotes:" header + one line per citation. These citations were already assigned inline during `on_stream_tool_start` (line 734). The render at end-of-turn is redundant noise. Should either suppress or collapse into a single dim inline annotation.

### G3: `tui_flame_render` adds N+3 lines after multi-tool turns
**File:** `src/agent.c:2403`
When F8 is enabled and tools ≥ 2, renders blank + "flame timeline:" + N bar entries + blank. Should be opt-in via `/perf` only, not auto-rendered. Suppress by default.

### G4: `tui_dag_render` adds 1 line after multi-tool turns
**File:** `src/agent.c:2405`
When F10 is enabled, renders "tool chain: A → B → C" inline. This one is fine — minimal noise. Keep.

### G5: Cache-hit path prints separate `print_tool_result` line
**File:** `src/agent.c:2263`
Cache hits don't go through spinner, so they print a standalone result line without the inline `[in:X out:Y $cost]` suffix. Should use `print_tool_start_line` + inline result for cache hits too.

### G6: No inline cost for batch tools
Batch spinner entries show result preview but not `[in:X out:Y $cost]`. The spinner_stop suffix only applies to the single-tool async spinner. Batch turns should show a single summary line after stop.

---

## Phase 2: Density Improvements

### P2.1: Fold throughput into section divider
Move F39 sparkline data into the section divider text:
```
─── turn 1 · 1 tool · $0.004 · 40 tok/s ───
```
Remove standalone `tui_throughput_render()` call for tool turns.

### P2.2: Suppress flame timeline by default
Gate `tui_flame_render` behind a `/flame` command or only render when explicitly requested. Remove auto-render from the main loop.

### P2.3: Inline citation as dim superscript-style markers
Instead of dumping a footnotes section, show `[1]` markers inline in the result preview text. Remove the end-of-turn `tui_citation_render()` call.

### P2.4: Unified cache-hit rendering
For cache hits, call `print_tool_start_line()` then immediately print a compact result line with `[cached]` badge inline, matching the same format as spinner completion.

### P2.5: Batch summary line
After `tui_batch_spinner_stop()`, print a single summary:
```
  ✓ 3 tools (142ms avg) [in:326 out:97 $0.68]
```

---

## Phase 3: Scroll-Off & Density

### P3.1: Collapsible tool output
For tools that return >5 lines of output, show only the first line in the result preview and add a `[+N lines]` indicator. Full output available via `/last` command.

### P3.2: Turn compression
After 3+ completed turns, compress older turns to single-line summaries:
```
  ↑ turn 1: 2 tools · $0.12  |  turn 2: 1 tool · $0.04
```

### P3.3: Streaming result preview
During tool execution, update the spinner label with partial result text as it arrives (for bash/exec tools that stream output). Replaces the blank spinning state.

### P3.4: Adaptive divider suppression
When consecutive tool turns happen with <100ms gap, suppress the section divider entirely — the tools are flowing as a batch and the divider breaks flow.

---

## Phase 4: Architecture

### P4.1: Render pipeline abstraction
Create a `tui_render_pipeline_t` that collects all per-turn visual elements (tool start, result, cost, throughput, citations, flame, divider) into a render queue, then flushes them in a single pass. This prevents interleaving and enables future rewriting/compression.

### P4.2: Virtual scrollback
Track rendered lines in a ring buffer. When a turn completes, compress its visual footprint by cursor-up + clear + rewrite with a single-line summary. The full output scrolls off but is recoverable via `/scroll`.

---

## Files to Modify (Phase 2)
- `src/agent.c` — suppress flame/citation auto-render, add batch summary, fix cache-hit path
- `src/tui.c` — fold throughput into section_divider, add batch summary function
- `include/tui.h` — new `tui_batch_summary()` signature

## Priority Order
1. G5 (cache-hit format) — easy fix, visible
2. P2.1 (fold throughput) — removes 1 line per turn
3. G3/P2.2 (suppress flame) — removes N+3 lines per multi-tool turn
4. G2/P2.3 (inline citations) — removes N+2 lines per turn
5. P2.5 (batch summary) — adds useful info
6. G6 (batch cost) — completes the inline-cost story
