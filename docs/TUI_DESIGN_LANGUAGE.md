# DSCO TUI design language

The interface should feel like a quiet operating surface, not a dashboard demo.
It stays out of the way until state, risk, or user input needs attention.

## Principles

1. Structure before decoration. Use spacing, alignment, and one-pixel rules to
   establish hierarchy. Do not add a widget unless it carries actionable state.
2. Color has meaning. Neutral graphite owns the surface. Steel marks current
   activity, green completion, amber a blocked or risky condition, and red a
   failure. Never cycle colors for ambience.
3. Motion must explain a transition. There is no idle animation, shimmer,
   particle field, waveform, orbit, or decorative pulse. Agent phase changes are
   discrete surface changes. Respect reduced-motion settings everywhere else.
4. Input is infrastructure. The composer remains visible and typeable while the
   agent reasons, uses tools, and responds. Only a raw-stdin tool may temporarily
   take ownership of the keyboard.
5. Native pixels carry layout; terminal text carries content. Kitty graphics sit
   behind transcript text at negative z-index. ANSI remains the fallback and must
   use the same hierarchy and semantic colors.
6. Density is earned. Default to one primary transcript plane, one persistent
   input plane, and compact state metadata. Reveal deeper hierarchy in plans and
   focused views rather than filling the workspace with telemetry.

## Core tokens

| Role | RGB | ANSI 256 | Use |
|---|---:|---:|---|
| background | `12 14 16` → `8 10 12` | `233` | full workspace |
| surface | `17 20 23` | `234` | transcript plane |
| raised surface | `21 24 28` | `235` | input and plan cards |
| primary text | `205 211 216` | `252` | titles and user content |
| secondary text/rules | `100 108 116` | `245` | labels and boundaries |
| active / focus | `105 139 153` | `110` | current phase and caret |
| success | `108 142 122` | `108` | completed or live |
| warning | `174 142 94` | `179` | blocked, risky, waiting |
| failure | `166 96 99` | `167` | failed or destructive |

## Geometry and copy

- Outer inset: 12 native pixels. Major surfaces use one-pixel neutral rules and
  a two-pixel semantic state rail.
- Composer: bottom-anchored, one content row by default, growing upward to eight
  rows. Its scroll region is reserved for the lifetime of the interactive view.
- Labels are short and literal: `workspace`, `transcript`, `input`, `live`,
  `reasoning`, `executing`, `responding`. Avoid theatrical or invented telemetry.
- Use sentence case in terminal text. Native bitmap labels remain compact and
  may render uppercase because of the embedded 5×7 font.

## Interaction contract

- Typing is accepted throughout an agent turn and queued in submission order.
- Enter submits; Option-Enter inserts a newline; Escape pauses; Ctrl-C interrupts.
- Resize reflows the text composer immediately and regenerates only the active
  Kitty surface before atomically swapping it into place.
- State must never be communicated by color alone: every semantic accent has a
  text label or status value.
