#!/usr/bin/env python3
"""Summarize or slice a compositor component-delta timeline without screenshots."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re


def interval(text):
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*(ms|s|seconds?|m|minutes?)?\s*", text)
    if not match:
        raise argparse.ArgumentTypeError("use an interval such as 500ms, 10s, or 2 minutes")
    unit = match[2] or "s"
    return float(match[1]) * (1 if unit == "ms" else 60000 if unit.startswith("m") else 1000)


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    return round(values[min(len(values) - 1, int((len(values) - 1) * fraction))], 3)


def summarize(records, start=0, duration=float("inf"), component=None):
    selected = [r for r in records if start <= r.get("ms", 0) <= start + duration]
    frames = [r for r in selected if r.get("event") == "frame"]
    gaps = [b["ms"] - a["ms"] for a, b in zip(frames, frames[1:])]
    changes = defaultdict(Counter)
    candidates = []
    states, pixel_at = {}, {}
    # Reconstruct prior state before the requested interval so a slice retains context.
    for record in records:
        at = record.get("ms", 0)
        if at > start + duration:
            break
        if record.get("event") != "component_delta":
            continue
        ident = record["id"]
        if component and ident != component:
            continue
        if record["op"] == "remove":
            states.pop(ident, None)
            pixel_at.pop(ident, None)
            if at >= start:
                changes[ident]["removed"] += 1
            continue
        state = states.setdefault(ident, {})
        delta = record.get("changes", {})
        previous_expected = state.get("visible") and state.get("animation_expected")
        if "pixel_crc" in delta and previous_expected and ident in pixel_at and at >= start:
            gap = at - pixel_at[ident]
            threshold = max(150, state.get("expected_interval_ms", 0) * 3)
            if gap > threshold:
                candidates.append(dict(component=ident, from_ms=round(pixel_at[ident], 3),
                                       to_ms=round(at, 3), gap_ms=round(gap, 3),
                                       expected_interval_ms=state.get("expected_interval_ms")))
        for name, pair in delta.items():
            state[name] = pair[1]
            if at >= start:
                changes[ident][name] += 1
        if "pixel_crc" in delta or not previous_expected:
            pixel_at[ident] = at
    end = min(start + duration, max((r.get("ms", 0) for r in records), default=0))
    for ident, state in states.items():
        gap = end - pixel_at.get(ident, end)
        if state.get("visible") and state.get("animation_expected") and gap > max(150, state.get("expected_interval_ms", 0) * 3):
            candidates.append(dict(component=ident, from_ms=round(pixel_at[ident], 3),
                                   to_ms=round(end, 3), gap_ms=round(gap, 3),
                                   expected_interval_ms=state.get("expected_interval_ms")))
    ticks = [r["values"] for r in selected if r.get("event") == "scheduler"]
    return dict(
        schema="dsco.native_trace_summary.v1", interval_ms=[start, end],
        finalized=any(r.get("event") == "end" for r in records),
        frames=dict(Counter(r["kind"] for r in frames)),
        frame_ms=dict(p50=percentile([r["frame_ms"] for r in frames], .5),
                      p95=percentile([r["frame_ms"] for r in frames], .95),
                      maximum=max((r["frame_ms"] for r in frames), default=0)),
        frame_gap_ms=dict(p95=percentile(gaps, .95), maximum=max(gaps, default=0)),
        scheduler=dict(waits=len(ticks), one_ms_waits=sum(t[0] <= 1 for t in ticks)),
        trace_sample_ms_p95=percentile([r["values"][0] for r in selected if r.get("event") == "trace_sample_cost"], .95),
        wire_bytes=sum(r.get("wire_bytes", 0) for r in frames),
        component_changes={k: dict(v) for k, v in sorted(changes.items())},
        component_state_at_end=states, animation_stall_candidates=candidates,
        interpretation="Pixel checksums describe submitted composite regions; stalls are candidates, not terminal display acknowledgements.",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--start", type=interval, default=0)
    parser.add_argument("--duration", type=interval, default=float("inf"))
    parser.add_argument("--component", help="e.g. header, composer, live_tool_deck, window/1")
    parser.add_argument("--events", action="store_true", help="emit the selected JSONL deltas instead of a summary")
    args = parser.parse_args()
    if args.trace.stat().st_size > 8 * 1024 * 1024:
        parser.error("trace exceeds the recorder's 8 MiB bound")
    records = [json.loads(line) for line in args.trace.read_text().splitlines() if line]
    if args.events:
        for record in records:
            if not args.start <= record.get("ms", 0) <= args.start + args.duration:
                continue
            if args.component and record.get("id") != args.component:
                continue
            print(json.dumps(record, separators=(",", ":")))
    else:
        print(json.dumps(summarize(records, args.start, args.duration, args.component), indent=2))


if __name__ == "__main__":
    main()
