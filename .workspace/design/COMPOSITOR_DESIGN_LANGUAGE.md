# DSCO Native Compositor — Design Language v1

**Status:** draft for ratification
**Scope:** every pixel DSCO paints — kitty graphics protocol surfaces, native macOS windows, PPM test artifacts
**Sources of truth:** `src/pixel_tui.c` (palette, transport discipline), `include/font_compat.h` (typography bridge), `include/pixel_tui.h` (surface API)
**Evidence base:** repo scan 2026-07-13 + "DSCO NATIVE RESIZE PROOF" screenshot defect audit (5.49.00 PM capture)

---

## 0. Prime rule

> Neutral surfaces do the structural work; color is reserved for current
> state, completion, warning, and failure. — `pixel_tui.c:38`

This comment is promoted from folklore to law. Any surface that uses accent
color for decoration rather than state fails review.

---

## 1. Design tokens

### 1.1 Color (ratified — matches `pixel_tui.c:40-50`)

| Token | RGB | Role |
|---|---|---|
| `bg.top` | 12,14,16 | canvas gradient top |
| `bg.bottom` | 8,10,12 | canvas gradient bottom |
| `panel` | 17,20,23 | primary surface |
| `panel.alt` | 21,24,28 | raised / alternate surface |
| `text` | 205,211,216 | primary text |
| `dim` | 100,108,116 | secondary text, chrome labels |
| `accent.cyan` | 105,139,153 | active / current state |
| `accent.violet` | 111,108,122 | reasoning / meta |
| `accent.green` | 108,142,122 | success / completion |
| `accent.amber` | 174,142,94 | warning / spend pressure |
| `accent.red` | 166,96,99 | failure / kill |

Rules:
- No color outside this table on any DSCO surface. The neon magenta /
  saturated purple in the resize proof is **rejected**.
- Accents appear at ≤ 10% of surface area. If an accent covers more, it is
  decoration, not state.
- Interpolation only via `color_mix` (linear per-channel); no HSV rainbow
  walks on pixel surfaces. (`tui_hsv_to_rgb` remains legal for the ANSI
  fallback dialect only.)

### 1.2 Typography

- Single bridge: `font_compat_draw_rgb` with `font_compat_measure_utf8` /
  `font_compat_line_height`. **Measure before place — always.**
- Overflow: `draw_text_ellipsis` or wrap. Hard clipping mid-glyph
  ("AGENTIC COM", "PERSISTENT I") is a review-blocking defect.
- Scale ladder: body 1.0×, chrome/labels 0.85× dim, title 1.2× bold.
  No other sizes.
- Labels are UPPERCASE dim; content is mixed-case text-color.

### 1.3 Spacing & geometry

- Base unit: 4 px. All padding/margins are multiples.
- Panel inset: 8 px. Panel gap: 8 px. Panel corner: square (no radii —
  this is a console, not a web app).
- One structural border weight: 1 px, `panel.alt` on `panel`.

---

## 2. Surface taxonomy

Every visual element belongs to exactly one surface; every surface has
exactly one owner region. Two layers may never draw text into the same
region (resize-proof collision class, defects #2/#3).

| Surface | Owner | Content |
|---|---|---|
| `masthead` | session | identity line: `DSCO`, model chip, tool count, feature count, trust tier |
| `transcript` | messages | role-tagged message stream |
| `composer` | input | input text, cursor, mode hints |
| `deck` | agent phase | phase indicator (IDLE/REASONING/EXECUTING/RESPONDING), spend, queue |
| `plan` | plan renderer | hierarchical plan trees (`pixel_tui_render_plan`) |

Masthead layout contract: fixed-width chips are measured first; the
free-text identity line receives `remaining_width` and ellipsizes. A chip
never overprints the identity line.

Each surface renders one title, from one string table. Duplicate labels
("COMMAND DECK" twice) indicate two code paths owning one region — merge
them.

---

## 3. Transport law (kitty graphics protocol)

Codifies the discipline already correct in `pixel_tui.c`:

1. **Format:** direct RGB, `f=24`, zlib (`o=z`), packed 24-bit
   (`_Static_assert(sizeof(px_color_t)==3)` stays).
2. **Quiet mode:** `q=2` on every command. The terminal never speaks back
   into the transcript.
3. **Geometry queries:** `TIOCGWINSZ` only. **In-band queries (`\x1b[6n`,
   `\x1b[14t`) are banned** unless the reader is in raw-drain mode with a
   bounded timeout — the `^[[8;1R` leak in the resize proof is the canonical
   violation.
4. **Image IDs:** derived, not sequential — `hash(generation, state)`
   (golden-ratio mix, `pixel_tui.c:790`) so parallel sessions in one
   terminal cannot collide.
5. **Swap discipline:** upload next generation → verify → place → delete
   old (`a=d,d=I`). Never delete-then-upload (flicker window).
6. **Placement:** explicit `c=`/`r=` cell bounds, `C=1` (no cursor motion),
   explicit `z=` per surface: transcript z=0, deck z=1, modal z=2.
7. **Resize:** re-query geometry, regenerate at new pixel size, atomic
   swap, preserve agent phase (`pixel_tui_session_refresh` semantics).
   Scaling an old bitmap to a new cell grid is banned.

---

## 4. State machine

The four-phase model is canonical: `IDLE → REASONING → EXECUTING →
RESPONDING`. Phase is communicated by **surface swap** (pre-uploaded state
surfaces, placement switch) — not by repainting, not by animation.
Animations are the ANSI fallback's job (`tui.c:4612` already disables them
when the pixel session is active).

Phase → accent mapping: IDLE dim · REASONING violet · EXECUTING cyan ·
RESPONDING green-tinted text · failure red · spend pressure amber.

---

## 5. Dialect consolidation

Current visual dialects in the tree:

| Dialect | Files | Disposition |
|---|---|---|
| Pixel (kitty RGB) | `pixel_tui.c` | **canonical** |
| ANSI truecolor | `tui.c` (11.7k lines) | fallback; keep, but adopt token palette for non-legacy paths |
| Braille subpixel | `plot.c`, `avatar.c`, `fractal.c`, `face_sdf.c` | specialty renderers; must consume token palette |
| Half-block dense | `plot.c:1387+` | specialty; same |
| Resize-proof / command-deck | not in mainline src | **fold into `pixel_tui.c` or delete** — a second pixel dialect is how defects #1–#5 happened |

Rule: new visual capability extends the canonical dialect. No new
free-standing render loops.

---

## 6. Acceptance criteria (regression tests)

Derived from observed defects; each becomes a check against
`pixel_tui_write_plan_ppm` / session PPM artifacts:

- [ ] **No CPR leak:** grep session transcript buffer for `\x1b[` + `R`
      patterns after a resize storm; must be empty.
- [ ] **No region collision:** for each surface, assert text draw calls
      stay within owner rect (debug build instrumentation on
      `font_compat_draw_rgb` args).
- [ ] **No off-palette pixels:** PPM artifact scan — every pixel must be
      reachable from token table via `color_mix`; flag saturated outliers
      (Δ from nearest token line > threshold).
- [ ] **No hard clip:** last glyph of every text run either completes or
      is `…`.
- [ ] **Resize atomicity:** capture before/during/after PPMs across
      SIGWINCH; intermediate frame must be old-complete or new-complete,
      never blank/partial.
- [ ] **Single title per surface:** string-table audit.

---

## 7. Native compositor path (macOS window, post-kitty)

The resize proof shows the direction: same canvas, native window. Contract:

- The **canvas is the product**; kitty placement and CoreGraphics/Metal
  blit are two transports for identical `px_canvas_t` bytes. PPM artifacts
  stay the cross-transport golden files.
- Native window adds: display-scale-aware regeneration (2× Retina),
  event-driven resize (no polling), and eventually CVDisplayLink pacing —
  but the design tokens, surface taxonomy, and state machine above are
  transport-invariant.
- `vecstore_metal.m` proves the Metal toolchain is already in-tree when a
  GPU blit path is warranted.

---

*Ratification: principal review required (identity/visual-surface class).
Amend via PR against this file; palette changes require before/after PPM
evidence.*
