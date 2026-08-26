# Native Agent Compositor

DSCO's UI is a retained semantic scene, not a terminal transcript with decorations.
The compositor models what an autonomous system is doing, lays it out once, diffs
successive scenes, and sends the resulting paint operations to a backend. Kitty RGB
is the first backend. Metal, ANSI, web, remote framebuffer, and accessibility trees
can consume the same scene without reimplementing product behavior.

The core implementation lives in [`include/native_ui.h`](../include/native_ui.h)
and [`src/native_ui.c`](../src/native_ui.c). `native_masthead` and
`native_composer` build the first live session regions entirely from stable
retained nodes, while `px_backend` maps semantic tokens and primitives onto
transport-owned raster operations. The native terminal workspace uses
`native_ui_agent_shell_layout()` for its responsive regions.

## Composition model

```text
agent events + user events
          │
          ▼
 semantic retained scene ── focus / hit routing / accessibility
          │
          ▼
 responsive layout ── compact / dense / expanded
          │
          ▼
 keyed scene diff ── bounded dirty regions
          │
          ▼
 backend operations ── Kitty RGB / Metal / ANSI / DOM / remote
```

Stable 64-bit keys preserve identity between frames. Primitive element type and
semantic role are separate: a tool activity can be rendered as a row, badge,
overlay, or custom visualization without losing its meaning to automation or
assistive technology.

Scenes use fixed-capacity storage. Layout, diffing, focus traversal, hit testing,
and rendering allocate nothing in the frame hot path. Labels are scene-owned so
streaming producers cannot invalidate a frame by releasing transient strings.

## Primitive elements

| Element | Contract |
|---|---|
| root | Viewport and root focus/event boundary |
| surface | Visual grouping, background, border, elevation |
| stack / row | Linear flex layout with min/preferred/max and grow/shrink |
| grid | Responsive repeated-field or agent-card layout |
| overlay | Z-layered popovers, palettes, scrims, toasts, and modals |
| scroll | Clipped content viewport with stable child identity |
| text | Backend-shaped prose, Markdown runs, code, or math |
| icon | Named semantic icon; glyph choice belongs to the backend |
| rule | Structural separation, never content |
| badge | Compact categorical state |
| meter | Bounded scalar such as context, queue, cost, or progress |
| sparkline | Dense time-series telemetry |
| input | Focusable command/composer surface |
| image | Artifact or generated visual surface |
| custom | Backend extension that retains a semantic role |

## Autonomous-agent roles

The role vocabulary covers the system states a human operator must be able to
inspect and command:

- `agent-shell`, `header`, and `status` establish global identity and liveness.
- `transcript`, `message`, `markdown`, `math`, and `code` carry durable discourse.
- `reasoning-activity` reports activity and timing without displaying private
  chain-of-thought.
- `tool-activity` represents governed calls, arguments, progress, result, duration,
  and failure as one atomic object.
- `agent-topology` represents parent/child workers, delegation, state, ownership,
  and returned artifacts.
- `artifact` is a first-class inspectable output rather than pasted console noise.
- `permission` presents a capability request, risk boundary, scope, and decision.
- `plan`, `queue`, and `timeline` expose intent, scheduled work, and causal history.
- `composer` is the persistent command surface.
- `command-palette` exposes actions by semantic command identity.
- `notification`, `toast`, `modal`, and `scrim` provide escalating interruption
  levels without disturbing the underlying scene.

Agent lifecycle state is normalized as `idle`, `reasoning`, `executing`,
`responding`, `waiting`, `blocked`, or `error`. A backend may animate or color these
states, but state meaning never depends on color or motion.

## Layout and density

Every node has min/preferred/max dimensions, grow/shrink weights, padding, gap,
alignment, and z-index. Containers support row, column, grid, and overlay flows.

The standard agent shell has three density profiles:

| Profile | Condition | Behavior |
|---|---|---|
| compact | width below 720 px or height below 320 px | Command-critical content only |
| dense | ordinary terminal/window | Dominant transcript plus persistent composer |
| expanded | at least 1400 × 440 px | Adds a bounded agent/tool inspector rail |

The inspector never steals space from an ordinary laptop transcript. The composer
remains a stable bottom region. These are defaults, not hardcoded backend geometry:
other products can build a different shell from the same primitives.

Terminal geometry is normalized through `native_ui_terminal_viewport()`. It keeps
physical backing pixels, logical layout pixels, cell dimensions, and backing scale
as separate values. Density breakpoints always consume logical dimensions. This is
essential on Retina displays: a 1120-point window backed by 2240 pixels remains a
1120-pixel semantic viewport instead of becoming a fake expanded desktop with
half-size typography. `DSCO_PIXEL_TUI_DPR=1..4` is the explicit override for unusual
font or display configurations. The live RGB surface is allocated at the exact
physical backing dimensions while every layout coordinate and type size remains
logical; Kitty therefore places pixels 1:1 instead of stretching a half-resolution
frame and resampling glyph edges.

During reasoning and response streaming, the shell becomes transcript-first: the
optional inspector rail yields its width, activity stays in the retained masthead,
and compact 12-point body text uses two pixels of leading. Token callbacks only
mutate retained text; the compositor coalesces them on a 30 Hz frame boundary and
sends bounded Kitty damage patches. Each retained event can hold up to 128 KiB of
UTF-8 text. Rich-token and wrapped-line storage grows with the response and is
reused across frames, so long Markdown responses do not stop at the former fixed
token/line limits or allocate large parser arrays on the render-thread stack.

## Rendering contract

`native_ui_backend_t` is intentionally small:

- begin/end a frame with viewport and damage information;
- push/pop clips;
- fill and stroke semantic rectangles;
- draw text and named icons using semantic type/color tokens;
- draw a custom primitive when a backend has a native representation.

Backends map tokens to actual colors and fonts. They must not infer agent state by
parsing visible strings. Markdown and math shaping can be backend-specific while
remaining attached to `markdown` and `math` roles.

`native_ui_diff()` compares keyed scenes. Inserted and removed nodes damage their
bounds; geometry changes damage the union of old and new bounds; visual mutations
damage only the node. Touching regions coalesce, and excessive fragmentation safely
falls back to a full repaint.

## Interaction and commandability

Pointer hit testing respects visibility, enabled state, tree order, and z-index.
Focus traversal uses explicit focusable state, not visual heuristics. Events target
the hit or focused node and bubble through semantic parents until handled. This
supports keyboard-first operation, mouse/touch, remote control, and future voice or
automation adapters through one route.

All interactive elements need:

1. a stable key;
2. a semantic role;
3. an accessibility label when visible text is insufficient;
4. a focusable state when directly commandable;
5. an event handler that accepts domain commands, not backend key codes.

## System invariants

- UI state is a projection of authoritative agent/tool/governance events.
- Tool activity is atomic: start, progress, completion, and error retain one key.
- A stream failure becomes a terminal `error` state; it cannot remain “thinking.”
- Content survives resize and backend changes because it is not owned by pixels.
- Animation is interruptible and optional; reduced-motion output is complete.
- Semantic color always has a textual/iconic equivalent.
- Overlays never destroy transcript, composer, focus history, or queued input.
- Capability and permission surfaces display scope and decision from the governed
  execution path; the compositor never grants authority.
- Backend output is generated from scene state, never scraped back into state.

## Live retained regions

The masthead and composer are the first complete session regions migrated from
immediate painting to the retained architecture. `native_masthead_build()` owns
identity, model/slot, resource summary, lifecycle badge, accessibility labels,
and the Overmind Soul placeholder through stable 64-bit keys.
`native_composer_build()` owns command-deck layout, queue/clock projection,
multiline cursor semantics, accessibility, and bounded editor damage. The
shared cell editor remains authoritative for editing, history, bracketed paste,
completion, image selection, and submission; the retained custom input node
renders that same editor rather than forking input behavior.

Each migrated region keeps two fixed scene slots, so previous/current layout
and semantic damage are available without allocating in the frame hot path.

`px_backend` is the shared semantic-to-raster adapter. It owns token resolution
and standard meter, sparkline, image, clipping, text, icon, and surface behavior;
Kitty supplies the low-level canvas operations and the animated Soul extension.
The generative JSON scene path, live masthead, and live composer now use the
same adapter.

`pixel_tui_write_session_ppm()` renders the complete workspace without a TTY.
`make test_pixel` exercises compact, dense, and expanded session artifacts, all
four lifecycle states, retained masthead/composer layout and damage, compositor
geometry, Kitty transport framing, and native plan surfaces.

### Frame telemetry

Frame instrumentation is opt-in, so disabled sessions do not add clocks or locks
to raster and transport work. Set `DSCO_PIXEL_TUI_PERF=1` to print one JSON line
after the native session closes, or set it to an absolute path to append JSONL
there. The `dsco.pixel_compositor_perf.v1` record separates:

- producer lock wait, oldest queued-token latency, layout/raster, tile diff,
  compression/base64 encode, terminal upload, flush, and whole-frame time;
- bounded 0.25 ms histogram P50/P95/P99 values plus exact maxima for every timing;
- identical, damage-patch, full, and failed frames;
- raw, compressed, base64, and exact Kitty wire bytes plus chunk counts;
- streaming requests, coalesced requests, throttled repaints, and their combined
  dropped-or-deferred count;
- peak retained compositor storage and peak per-frame transport allocation;
- scheduler wakeups/renders, deadline misses, long rendered-frame gaps, wake
  lateness, and maximum/average rendered-frame gap. These distinguish motion
  cadence breaks from raster or terminal-upload cost.

Upload timing measures local terminal writes and `fflush()`. It is not presented as
a terminal-side decode or display acknowledgement.

`dsco --compositor-stream-bench [CHUNKS]` runs a deterministic provider-like stream
through the actual live native session and emits a
`dsco.compositor_stream_bench.v1` JSON summary after restoring the terminal. It
requires a Kitty-compatible TTY, defaults to 900 chunks at a 2 ms arrival interval,
and does not call a model provider. `DSCO_COMPOSITOR_BENCH_INTERVAL_US` changes the
arrival interval for burst and sparse-stream tests.

The resident image uses 32-pixel tile diffs and in-place Kitty frame edits. The
measured default permits a patch while dirty tiles cover at most 70% of the frame;
`DSCO_PIXEL_TUI_DAMAGE_COVERAGE=0.10..0.95` is an A/B/debug override. Structural
changes, fragmented damage, failed patches, and every 65th changed frame still use
an authoritative full upload. `DSCO_PIXEL_TUI_PATCH=0` disables patching.

## Dual-compositor parity contract

The native compositor is additive. The established ANSI/cell TUI remains a
first-class renderer and input implementation while native mechanisms mature.
On a Kitty-compatible terminal, `dsco --native` or `DSCO_PIXEL_TUI=1` selects
the pixel backend; `dsco --tui` or `DSCO_PIXEL_TUI=0` keeps the established
renderer.

Parity is maintained at the behavior boundary rather than by cloning business
logic into a second UI:

| Original surface | Native behavior |
|---|---|
| streaming transcript, Markdown, code, math, citations | retained messages and rich-text runs |
| multiline editing, history, paste, cursor movement | the shared cell-composer engine owns editing; native layout renders up to eight wrapped rows |
| slash completion and `@` image picker | shared registries and selection state rendered as native popovers |
| model/slot, input/output tokens, cost, budget, runway, burn, turn, tools, queue, clock | retained status projection |
| panel notices and terminal notifications | native toasts plus preserved terminal BEL/OSC delivery |
| permission, confirmation, question, and hierarchical-menu prompts | native modal overlays using the original decision/input loops |
| tool lifecycle, parallel batches, swarm topology, plans | one retained operation row per governed call, updated in place from running to result; live cards are transient projections |
| ordinary legacy tables, diffs, diagnostics, and command output | ANSI is stripped and published into the native transcript by the compatibility capture boundary |
| pager, secure lock screen, raw-stdin tools, and terminal-native specialty renderers | compositor suspend → established TUI/real TTY → atomic compositor resume |

No feature may silently disappear behind the pixel framebuffer. A new legacy
surface must either publish semantic retained state, pass through transcript
capture, or use the explicit terminal handoff. This also keeps the original TUI
independently testable instead of turning it into dead fallback code.

Native tool history is outcome-dense rather than a raw execution log. The default
`DSCO_PIXEL_TUI_TOOLS=results` shows the call immediately, then updates that same
row with status, elapsed time, result size, the first useful result line, and a
hidden-line count. `calls` suppresses result text; `full` permits a bounded 10-line,
2 KiB preview. The complete result remains available to the model and durable
Chronicle/VFS paths in every mode.

Run `dsco --compositor-parity /tmp/dsco-compositor-parity` to materialize the
shared deterministic corpus. It emits the established renderer as `legacy.ansi`
and `legacy.txt`, the populated native session as `native.ppm`, the semantic
fixture, and machine-readable density metrics. Regression tests require the
native 1120x700 review viewport to retain every fixture message and at least 95%
of the established viewport's visible transcript characters.

## Adding a UI capability

Prefer composing existing elements and roles. Add a primitive only when a backend
needs a genuinely new paint operation; add a role when operators and automation need
a new stable meaning. Keep domain state in its owning module and publish a small
event/projection hook into the scene. Do not put provider, tool, or governance logic
inside a backend.

Tests should prove semantic identity, layout under at least compact and expanded
sizes, focus order, hit routing, damage bounds, and backend operation output. Visual
snapshots are useful after those behavioral contracts pass, not as a substitute.
