# Native UI component timelines

Click **Record UI issue** in the native composer's footer. Reproduce the glitch
while the ten-second countdown runs. When it finishes, **Ask AI about trace**
sends the completed recording's local path to the agent for diagnosis. The
agent receives it at its next turn boundary. Your unsent draft and cursor stay
in place, and recording alone never starts an AI turn. No screenshot upload or
remembered command is needed. The control also follows recordings started by
the agent or a slash command. A denied recording displays the failure in the
footer; `/ui trace 10s` provides its full details.

Completion updates even with reduced motion and without mouse or keyboard
events. The control uses the same capability gate as `ui_trace`; asking for
diagnosis grants no extra tool authority. Diagnostic requests and the latest
recording's control state are local to the running native session.

In the native session, run `/ui trace 10s` and reproduce the issue. The command
returns a private JSONL file path. Recording stops after the interval and the
file is finalized by a writer thread. `/ui trace status` reports completion;
`/ui trace stop` finishes early. Intervals include `500ms`, `10 seconds`, and
`2 minutes`, up to 10 minutes. A trace also stops at 8 MiB or session exit.

The agent can do the same through `ui_trace`:

```json
{"action":"start","duration_ms":10000}
```

After completion, the agent reads the returned path with `read_file`. An
external assistant can read that same local file. The tool runs in its calling
native process; a separate MCP server does not attach to an existing session.
`status` is read-only, while starting/stopping retains the normal write gate.
An optional `path` selects a new file; existing files are never overwritten.
With a scoped write grant, provide an explicit permitted path.

## Read an interval

From the checkout:

```sh
python3 scripts/native_trace_report.py /path/to/trace.jsonl
python3 scripts/native_trace_report.py /path/to/trace.jsonl --start 2s --duration 5s --component header
python3 scripts/native_trace_report.py /path/to/trace.jsonl --component window/1 --events
```

The summary includes frame timings and gaps, one-millisecond scheduler waits,
component change counts, final component state, and candidate animation stalls.
`--events` emits just the selected deltas. Each changed field is `[old, new]`;
`null` is the initial value. Unchanged components emit no delta; disappearing
components emit `remove`.

## What is recorded

| Event | Meaning |
| --- | --- |
| `component_delta` | Retained state changes and checksums of composite pixels submitted for that region. |
| `frame` | Full/patch/identical/failed frame, queue/render/encode/upload/flush time, wire bytes, and damage-rectangle count. |
| `scheduler` | `[requested wait ms, observed wait ms, phase, animation expected]`. Observed `-1` means recording began during the wait. |
| `repaint_requested` | `[structural, phase, already pending, composer accepts input]`. |
| `composer_input` | `[byte length, cursor byte offset, active, menu item count]`; no typed text. |
| `stream_input` | Incoming text byte count; no response text. |
| `pointer` | `[button, logical x, logical y, released]`, including wheel events. |
| `trace_sample_cost` | Time spent collecting component metadata and checksums. |
| `end` | Stop reason, duration, event/frame totals, failures and maximum frame time. |

Components cover viewport, header, transcript, inspector, composer, live tool
deck, individual tool lifecycle records, menu, modal, retained-scene metadata,
each native window, and the `ui_diagnostics` control. Its status is
`0=record`, `1=recording`, `2=saving`, `3=ask`, `4=queued`; bounds remain in
screen coordinates during partial repaints. Phases are `0=idle`, `1=reasoning`, `2=executing`,
`3=responding`. Window status is its kind (`0=note`, `1=buffer`, `2=workflow`).
Tool status is `0=running`, `1=done`, `2=error`. Transcript `cursor` is the total
revealed byte count; composer/window cursor is an editing byte offset.

Bounds are logical pixels. `viewport.count` is the backing scale.
`animation_expected` and `expected_interval_ms` describe the intended clock;
`pixel_crc` changes describe observed submitted pixels. For example, a header
expecting 50 ms updates whose checksum remains unchanged for 800 ms is a useful
stall candidate. A buffer window records focus, byte count, cursor, dirty/save
state and revision changes, so a missing edit can be traced to input, state,
painting or saving without copying the document.

The recording contains no transcript, draft, buffer body, screenshot, or raw key
contents. It does include tool names, process ID, geometry, timing and state
metadata. Files use mode 0600. Capture is opt-in; collecting checksums adds
measured overhead. Region checksums describe the final composited rectangle,
including any overlapping pixels, rather than an isolated layer. Retained
`ui_render` scenes are currently aggregate metadata, not per-node deltas.

These are terminal submissions, not terminal acknowledgements or measured
display refresh. A successful upload cannot prove Kitty displayed the image.
The report calls long unchanged intervals candidates for that reason.

Validation: `make test_native_trace test_native_activity test_native_buffer_edit`.
Control and handoff validation: `make test_native_trace_ui test_native_trace_controls`.
For a real CLI edit/save timeline in an owned PTY:

```sh
python3 tests/test_native_buffer_edit.py --binary ./dsco --trace-output /tmp/native-edit.jsonl
```
