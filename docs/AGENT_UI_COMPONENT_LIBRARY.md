# Agent UI Component Library

DSCO's graphics kit is a retained, backend-neutral C component system for an
agentic terminal. Components describe meaning, layout, state, focus, and
accessibility in `native_ui`; `px_backend` rasterizes that scene; Kitty carries
the exact RGB backing store to the terminal.

The library deliberately has two levels:

| Layer | Public header | Purpose |
|---|---|---|
| Foundations | `px_theme.h`, `px_widgets.h` | Shared live themes, surfaces, badges, meters, progress, charts, liveness, agent cards, and toasts |
| Agent workflows | `agent_ui_theme.h`, `agent_ui_components.h` | Transcript, reasoning, tools, plans, topology, artifacts, permissions, metrics, queues, timelines, notifications, command palette, composer, code, and theme selection |
| Raster backend | `agent_ui_canvas.h` | Exact device-scale RGB canvas and `native_ui` renderer |
| Storybook | `agent_ui_gallery.h` | Interactive Kitty catalog and deterministic PPM proof frames |

## Rendering contract

The scene always uses logical pixels. The canvas owns physical pixels and
applies `backing_scale` exactly once to geometry, radii, strokes, icons, and
CoreText sizes. A 1800 × 1000 logical gallery on a 2× display therefore sends a
3600 × 2000 RGB image. Kitty places it over the full reported cell rectangle;
there is no intentionally low-resolution intermediate surface.

```text
models → component builders → native_ui scene → px_backend → RGB backing store → Kitty
             semantics          layout/diff      tokens       DPR once          transport
```

Scenes use fixed retained storage (`NATIVE_UI_MAX_NODES`) and component
constructors allocate nothing. Stable keys make diffs, automation, and event
wiring independent of visible labels.

## Theme catalog

`px_theme` is the single color registry used by the live compositor and the
component library. `agent_ui_theme` adapts it with spacing, radius, typography,
shadow, and contrast metadata.

The 16 built-in themes are:

1. `quiet-console`
2. `distributed-crimson`
3. `synthwave`
4. `matrix`
5. `amber-crt`
6. `nord`
7. `solarized-light`
8. `paper`
9. `native-glass`
10. `industrial`
11. `warm-studio`
12. `blueprint`
13. `spatial-command`
14. `carbon`
15. `oceanic`
16. `high-contrast`

Every theme supplies canvas and panel elevations, primary and secondary ink,
two accents, success/warning/danger states, a brand tint, and six chart series.
Primary text is validated at a minimum 4.5:1 contrast against the canvas.

The live compositor selects a theme from `DSCO_PIXEL_THEME` (falling back to
`DSCO_THEME`), `pixel_tui_session_set_theme()`, or
`pixel_tui_session_cycle_theme()`.

## Foundation widgets

All constructors append to a caller-owned `native_ui_scene_t` and return the
root node index or `-1`.

| Constructor | Retained behavior |
|---|---|
| `px_widget_card` | Titled clipped surface container |
| `px_widget_kv_row` | Label/value status row |
| `px_widget_badge` | Semantic status pill |
| `px_widget_meter` | Labeled determinate meter |
| `px_widget_sparkline` | Up to 48 trend values |
| `px_widget_progress` | Determinate or phase-driven indeterminate progress |
| `px_widget_gauge` | 270-degree radial gauge |
| `px_widget_bar_chart` | Up to 32 vertical values |
| `px_widget_spinner` | Eight-dot phase-driven activity orbit |
| `px_widget_activity_dots` | Three-dot reasoning pulse |
| `px_widget_agent_card` | Identity, lifecycle, task, model, progress, and cost |
| `px_widget_toast` | Live transient status surface |

Charts and liveness are encoded as semantic custom nodes (`pxw.*`). The shared
`px_backend` draws them from generic line, circle, text, and rectangle
operations, so transports do not clone widget logic.

## Agent workflow components

`agent_ui_builder_t` binds a retained scene to a design theme. The high-level
constructors cover the core terminal workflow:

| Constructor | Semantic role |
|---|---|
| `agent_ui_add_message` | transcript message with system/user/assistant and streaming states |
| `agent_ui_add_reasoning` | live/collapsed reasoning activity and progress |
| `agent_ui_add_tool` | queued/running/terminal tool invocation |
| `agent_ui_add_plan_step` | numbered, actionable plan state |
| `agent_ui_add_agent_card` | agent identity, model, task, lifecycle, context |
| `agent_ui_add_topology` | active agent graph and route |
| `agent_ui_add_artifact` | file/image/report result with action |
| `agent_ui_add_permission` | scoped risk and two keyboard-focusable decisions |
| `agent_ui_add_metric` | value, trend, unit, and sparkline |
| `agent_ui_add_queue_item` | ordered actionable work |
| `agent_ui_add_timeline_event` | timestamped execution evidence |
| `agent_ui_add_notification` | notification or live toast |
| `agent_ui_add_command` | selectable/disabled command palette row |
| `agent_ui_add_composer` | focused input, mode, busy/submit action |
| `agent_ui_add_code_block` | language, metadata, source, copy action |
| `agent_ui_add_theme_swatch` | focusable real-palette preview |

Components use semantic color tokens only. They never embed theme-specific RGB
values.

## Stable anatomy and actions

Use `agent_ui_component_key(kind, instance)` for the root and
`agent_ui_component_part_key(root, part)` for automation or event wiring.
Public parts include icon, eyebrow, title, body, metadata, state, progress,
primary and secondary actions, data, and detail.

For example, permission actions remain addressable even if their visible copy
or theme changes:

```c
uint64_t key = agent_ui_component_key(AGENT_UI_COMPONENT_PERMISSION, 7);
int root = agent_ui_add_permission(&ui, parent, key, &permission);

uint64_t allow_key = agent_ui_component_part_key(
    key, AGENT_UI_PART_PRIMARY_ACTION);
uint64_t deny_key = agent_ui_component_part_key(
    key, AGENT_UI_PART_SECONDARY_ACTION);
```

Both actions carry `NATIVE_UI_STATE_FOCUSABLE`; focus traversal, hit testing,
dispatch, damage, and accessibility continue through the standard `native_ui`
APIs.

## Minimal usage

```c
native_ui_scene_t scene;
native_ui_scene_init(&scene, logical_width, logical_height);
scene.nodes[scene.root].style.flow = NATIVE_UI_FLOW_COLUMN;

agent_ui_builder_t ui;
agent_ui_builder_init(&ui, &scene, agent_ui_theme_find("native-glass"));

agent_ui_tool_model_t tool = {
    .tool = "exec_command",
    .summary = "Compile the component gallery",
    .detail = "make dsco-agent-ui-gallery",
    .duration = "1.2s",
    .status = "RUNNING",
    .tone = AGENT_UI_TONE_ACCENT,
    .progress = 0.61f,
    .running = true,
};

agent_ui_add_tool(&ui, scene.root,
    agent_ui_component_key(AGENT_UI_COMPONENT_TOOL, 1), &tool);
native_ui_layout(&scene);
```

To render the scene into exact backing pixels:

```c
agent_ui_canvas_t *canvas = agent_ui_canvas_create(
    physical_width, physical_height, backing_scale, ui.theme);
agent_ui_canvas_render(canvas, &scene, NULL);
```

## Interactive storybook

Build and launch:

```sh
make dsco-agent-ui-gallery
./dsco-agent-ui-gallery
```

Controls:

- `1`–`7`: Workbench, Foundations, Conversation, Execution, Orchestration, Governance, Themes
- Left/Right or `H`/`L`: change page
- `[`/`]` or Tab: change theme
- `Q` or Escape: close

Headless and explicit 2× proofs:

```sh
./dsco-agent-ui-gallery --ppm /tmp/components.ppm --page themes
./dsco-agent-ui-gallery --ppm /tmp/components-2x.ppm \
  --width 3600 --height 2000 --dpr 2 --theme native-glass
```

List the public catalog with `--list-themes` or `--list-pages`.

## Verification

`make test_agent_ui_components` checks:

- all 16 shared live/component themes and contrast invariants;
- root and part key stability;
- foundation widgets and custom-kind state;
- focusable permission actions and focus traversal;
- bounded semantic damage after a message mutation;
- unique retained keys and in-viewport frames on all seven gallery pages;
- coverage of agent workflow roles; and
- an exact 1800 × 1400 RGB store representing a 900 × 700 scene at 2×.
