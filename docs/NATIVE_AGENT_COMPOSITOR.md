# Native Agent Compositor

DSCO's UI is a retained semantic scene, not a terminal transcript with decorations.
The compositor models what an autonomous system is doing, lays it out once, diffs
successive scenes, and sends the resulting paint operations to a backend. Kitty RGB
is the first backend. Metal, ANSI, web, remote framebuffer, and accessibility trees
can consume the same scene without reimplementing product behavior.

The implementation lives in [`include/native_ui.h`](../include/native_ui.h) and
[`src/native_ui.c`](../src/native_ui.c). The existing native terminal workspace uses
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
font or display configurations.

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

## Adding a UI capability

Prefer composing existing elements and roles. Add a primitive only when a backend
needs a genuinely new paint operation; add a role when operators and automation need
a new stable meaning. Keep domain state in its owning module and publish a small
event/projection hook into the scene. Do not put provider, tool, or governance logic
inside a backend.

Tests should prove semantic identity, layout under at least compact and expanded
sizes, focus order, hit routing, damage bounds, and backend operation output. Visual
snapshots are useful after those behavioral contracts pass, not as a substitute.
