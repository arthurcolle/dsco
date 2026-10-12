# Native windows in Kitty

Run `dsco --native` in Kitty, then press **Ctrl+G** or enter **`/windows`**.
When explicitly requested, the workspace keeps notes, buffer snapshots and
workflow outcomes visible while DSCO works. Its panels are retained inside the
native compositor. Native mode and multi-step tasks do not authorize unsolicited
panels, widgets, overlays, or extra windows. A disabled presentation preference
remains in effect until the human specifically re-enables the relevant surface;
use the existing conversation and headless checks in the meantime.

## Session details

In `dsco --native`, **`/status`** or **`/windows session`** opens a live,
read-only **Session Details** window. Repeating the command raises the same
window, rather than making duplicates. It shows the model, state, elapsed time,
tokens, context, tools, queue, workers, budget and separately labelled cost
observations. Configuration (provider, effort, trust, session ID and integrations)
is refreshed at each prompt and labelled accordingly. Unknown cost stays unknown;
reference estimates and provider reports are not added together.

Drag, resize, zoom and scroll it like other panels. Hide or close is respected:
background updates never reopen it or take focus from another panel. It is never
opened automatically at startup. Outside native mode, `/status` keeps its text
output. Existing running binaries need a new session to use newly installed code.

## Direct manipulation

- Drag a title to move and raise its panel. Drag the lower-right corner to resize.
- Click the title's `+` to zoom; click it again to restore. `×` closes the view.
- Scroll over a panel to move its own text. Scroll elsewhere to move the transcript.
- Use **Tile**, **Cascade**, and **Hide** in the workspace toolbar. Tiled panels
  rearrange when the terminal changes size. Ctrl+G restores a hidden workspace.
- Ctrl+G focuses panels; Tab/Shift+Tab switch them. Arrows move the selected
  panel, Shift/Ctrl+arrows resize, Z zooms, T tiles, R cascades, X closes, and
  Page Up/Down scroll. Escape returns to the composer. Enter while a panel has
  focus returns to the draft without submitting it.

Typing ordinary text returns to the composer. Draft bytes, cursor position,
multiline input and partial bracketed pastes survive workflow action delivery
and the handoff between idle and in-flight input readers. Menus and modal
questions take precedence over panel controls.

## Outcomes and evidence

A workflow panel contains an outcome, its current status, the next proposed
step and observed evidence. Continue, Revise, Retry and Inspect feed a typed
request into the agent at its next safe boundary. The input is recorded once;
reading receipts never consumes or duplicates it. Progress and final responses
should stay concise. The native session's refreshed system guidance requires
explicit opt-in before showing these panels and preserves running tasks when
hiding their views.

Panel status is **agent-reported**. `done` requires nonempty evidence, but the UI
does not independently verify that evidence. Terminal input may come from a
person or from automation; it steers the existing task and is not an approval
receipt or additional authority. The normal execution gates still apply.

## Agent tools and commands

`native_window` is registered in the live tool catalog. Retrieve its schema with
`load_tools` or `discover_tools`, then call it directly when advertised or through
`invoke_tool`. It supports list, open, update, buffer, refresh, move, resize,
focus, zoom, tile, cascade, close, show, hide, scroll and events.

Examples in an active native session:

```text
/windows open {"kind":"workflow","title":"Check the build","text":"Produce a working executable.","status":"running","next_step":"Run the focused checks."}
/windows open {"title":"Working notes","text":"Keep the decision and its reasons here."}
/windows tile
/windows list
/windows zoom 1
/windows close 2
/windows events
```

Attach a persistent named buffer created with `/buffer new notes`:

```text
/windows buffer {"name":"notes"}
/windows refresh 3
```

The direct shortcut is **`/buffer edit notes`**. Click the panel text or press
Ctrl+G to edit, Ctrl+S to save, and Escape to return to chat. The panel shows its
editing/saving/unsaved state and these controls. AI-managed buffers use the same
editor through `native_window` action `buffer`; a note containing copied text
does not edit a persistent buffer.

Use the actual panel ID returned when refreshing. Native editing requires the
complete UTF-8 buffer to fit within 8,191 bytes; larger buffers can be edited
with `/buffer open NAME`. Refresh after an external edit. Saves check the
canonical revision and retain your edits on conflict. Closing or refreshing a
dirty panel is rejected. Closing a saved panel does not delete its buffer.
Buffer reads and writes retain the caller's tier through the public gate.

Up to 12 panels are retained per native session. Notes and arrangements are
session-local; persistent buffers survive restarts. Window coordinates are
logical pixels, and list returns the current work area and panel rectangles.
Text is bounded to 8,191 UTF-8 bytes; evidence and next-step fields to 1,023 bytes.
Events retain the latest 64 receipts and return pages of 16 with `since` and
`next_since`. The pending input queue has 64 slots and reports saturation without
silently losing an accepted request.

## Graphics and terminal surfaces

The persistent composer also hosts native voice. Use **Ctrl+O**, `/voice`, or
launch with `--hands-free`; the native footer shows connecting/listening/speaking
state and interim recognition without submitting partial text. A finalized
utterance follows the exact same input and safe-boundary queue as typed text, so
it directs the current coordinator and its existing subagents/swarms rather than
opening a second agent. The composer retains any typed draft and byte cursor.
Capture and recognition require the Mac's on-device Speech path and never fall
back to cloud audio processing; the finalized text still follows the current
session's ordinary model/provider route, which may be remote. Only the final
public assistant answer is synthesized locally. While that answer plays, the
microphone is paused through a short echo cooldown, so spoken barge-in is not
accepted; **Ctrl+O** can stop voice. The Voice badge is clickable in the native
compositor, and the exact local stop phrases remain available in both native and
ANSI TUI modes. Permission denial is shown as unavailable, not as a ready mic.

Panels rasterize through the existing Kitty framebuffer and damage-patch path,
with panel-local clipping and a protected composer region. Button-motion input
uses SGR mouse reporting; resize hit areas account for the terminal cell size.
Visible native panels and TUI worker cards automatically enable drag reports;
hiding them returns ordinary terminal selection. `DSCO_MOUSE=0` disables automatic
tracking; `DSCO_PIXEL_TUI_MOUSE=0` overrides it specifically in native mode.
After installing a fix, `/restart` loads the new binary and preserves conversation
history (panel arrangements remain session-local).
An existing `ui_render` scene is retained while the workspace hides its placement
and is restored when the workspace hides or closes.

Kitty's graphics protocol manages image data and placements; actual terminal
processes, tabs and OS windows use its remote-control protocol. DSCO exposes
those through `surface`, `buffer_view`, `pty_session`, and `kitty_remote`.
See the official [graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/)
and [remote-control interface](https://sw.kovidgoyal.net/kitty/remote-control/).
`--tui` continues to select the text interface; `--native` selects this compositor.

For animation, input, or layout problems, run `/ui trace 10s` to capture a
[component-delta timeline](NATIVE_UI_TRACE.md) with frame timing. The returned
JSONL file can be inspected directly by the agent or another local assistant.

The native_window tool operates in its calling process's active native session.
A separate headless MCP process can inspect its own empty state, but it does not
silently attach to another process's compositor. Use an owned Kitty surface for
cross-process terminal control.

## Refresh and dynamic editor state (2026-09-20 repair)

`refresh` reads through the existing governed `buffer` tool, then atomically compares the captured buffer binding, canonical revision and edit generation under the window-model mutex. If a newer edit/save or refresh won during the read, it returns `buffer_changed_during_refresh` without replacing the newer view. A pane title changed during the read remains human-owned.

An unchanged clean snapshot retains editor focus, cursor/selection, undo history and viewport instead of resetting the editor. Dirty/in-flight-save views remain protected. Refreshing genuinely changed clean content still loads a new bounded snapshot; it never auto-saves.

Tab/Shift-Tab activates the window selected by stable ID after z-order changes. Focusing, resizing, zooming, tiling and terminal reflow keep an active buffer caret visible, including the empty row after a trailing newline. Ordinary manual scroll remains independent.

Headless regressions: `make test_native_buffer_lifecycle test_native_trace_activation`. The lifecycle fixture injects stale-read/save and read/read interleavings and tests editor preservation, title ownership, Unicode pending input, Tab focus and caret reflow. The timing fixture checks that enabling telemetry mid-frame does not record machine uptime as a frame duration. No desktop windows or clipboard are used.
