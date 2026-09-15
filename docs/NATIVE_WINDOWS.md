# Native windows in Kitty

Run `dsco --native` in Kitty, then press **Ctrl+G** or enter **`/windows`**.
When explicitly requested, the workspace keeps notes, buffer snapshots and
workflow outcomes visible while DSCO works. Its panels are retained inside the
native compositor. Native mode and multi-step tasks do not authorize unsolicited
panels, widgets, overlays, or extra windows. A disabled presentation preference
remains in effect until the human specifically re-enables the relevant surface;
use the existing conversation and headless checks in the meantime.

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

Panels rasterize through the existing Kitty framebuffer and damage-patch path,
with panel-local clipping and a protected composer region. Button-motion input
uses SGR mouse reporting; resize hit areas account for the terminal cell size.
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
