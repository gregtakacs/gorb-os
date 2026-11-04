# Theme text: fonts and labels a theme carries, so an app's words ride L4

Status: **ruled 2026-11-04 (owner, §0). Building on `main`.** This is the E4 readout
(docs/history/element-model-plan.md §6, §10) generalised to every theme-aware app, and the
way the flight tracker gets back onto L4 (flight-tracker-design §11.6, "still owed").

## 0. Owner rulings, 2026-11-04

1. **An app's words are theme assets.** A label's font, size, colour, finish, panel,
   border and corner are the theme's, baked into it, and the OS draws the app's string in
   them exactly as it draws a date window: from the theme's pixels, as a row the
   compositor paints. The app says only *what* the text is, and in which tone.
2. **Static decoration is plate art.** The tracker's range rings and home dot never move,
   so they go into its background at bake time; each family draws its own.
3. **Moving decoration is a row.** The selection and emergency rings are theme rows the
   app poses, like a plane.
4. **Stale contacts dim** by an opacity the app gives a row, not by more frames.
5. **The fix order:** rings into the plate, then the overlays (text rows), then the L4 veto
   comes off for an app whose screen holds no widget of its own.

## 1. Why not snapshot the app's widgets

The first proposal (flight-tracker-design §11.6) was for the OS to snapshot each LVGL
widget into a plane L4 could paint. It works, but the look stays the app's: LVGL's
built-in fonts, a colour the app hard-codes. Ruling 1 puts the look in the theme, where
every other pixel of the app's screen already lives, and the compositor learns nothing
new: a label is one more row with a plane.

## 2. Design

### 2.1 A font is a TEXT row

Class 0 (`text`) has been in the asset-class enum since D23 and carries app-picked frames
(`CLASS_APP_ADV`). A **font** is one TEXT row:

| column | meaning |
|---|---|
| `slot` | the font's id, 0..15, which labels name it by |
| `w`, `h` | one glyph cell; every glyph of the font shares it |
| `n_frames` | glyphs, in **`FONT_CHARSET` order**: ASCII 0x20..0x7E, then ↑ ↓ → ← ° (100) |
| `pivot_x` | the space's advance, px |
| `pivot_y` | tracking: px between one glyph's ink and the next glyph |
| `x`, `y`, `order`, `radius`... | zero: a font is never drawn where it stands |

Each glyph is RGB565A8 like every other frame, drawn from the cell's left edge on the
font's baseline, so a font can be any colour or finish the bake can paint (a brass
engraving, a blueprint stencil). A glyph's **advance** is its ink's right edge + 1 +
tracking, measured once by the OS when the font is first used (the bank is not resident at
projection time); a glyph with no ink advances the space. No metrics table rides the wire.
A character outside the charset draws as `?`.

### 2.2 A label is an AUX text row

An AUX row with **shape `text`** and **source `text`** (both reserved since D42 and refused
until now) is a **label**. Its own art (one frame) is the label's **panel**: fill, border,
corner, shadow, or a fully transparent box. Its box is the text area.

| column | meaning on a label |
|---|---|
| `x`, `y`, `w`, `h`, pivot | the panel, placed like any row; the app may move it by a pose |
| `str` | its fonts by tone: four nibbles, tone 0 in the low one; 0xF = none (tone 0 stands in) |
| `radius` | inner padding, px |
| `digit` | horizontal alignment: 0 left, 1 centre, 2 right |
| `end` | vertical alignment: 0 top, 1 middle, 2 bottom |
| `ease` | extra px between lines |
| motion columns | zero: a label has no domain; the app poses it |

Tones are the app's vocabulary for *why* a line looks different: **0 normal, 1 accent,
2 warning, 3 quiet**. The theme decides what each looks like, and a theme may leave any but
tone 0 out.

The string table E4 planned (a clock readout's `HH:MM` format) is not built here. A clock
readout would be a label whose source is a clock value, with its format in a string table
that `str` would then point at; the `text` source keeps `str` for its fonts.

### 2.3 At runtime

- **The reader** (`orb_clock3.c`, `theme_schema.py`) accepts TEXT rows as fonts and AUX
  text rows as labels, and refuses a label whose tone 0 font is missing.
- **The stage** gives each label a row-owned PSRAM plane its box's size, and composes it on
  `orb_stage_text`: the panel, then the glyphs, clipped to the padded box, lines split at
  `\n`. It recomposes only when the string or tone changes. The plane is the row's art on
  both paths: the LVGL image points at it, and the layer row's base is it.
- **New wasm imports:** `orb_stage_text(row, str, len, tone)` and
  `orb_stage_opacity(row, opa)`. A row's opacity multiplies its art's alpha, the same on
  both paths.
- **The layer table holds 32 rows** (`ORB_LAYER_MAX`, was 16). The tracker needs the arm,
  15 planes, two marker rings and four labels.
- **The L4 veto stands for widgets.** An app whose screen holds only the stage's rows gets
  L4. One that adds a widget still renders through LVGL.

## 3. Slices

| Slice | Content | Gate |
|---|---|---|
| **T1** | Rings and home dot in the tracker's plates; selection and emergency rings as theme rows; the app's box widgets go | bake runs; Bronze shows rings in each family's colour |
| **T2** | Row opacity: the paint path and LVGL, `orb_stage_opacity`; stale contacts dim | parity of the two paths on the host; dimmed contacts on Bronze |
| **T3** | `ORB_LAYER_MAX` 32 | internal heap at boot no lower than the job array's growth; config server up |
| **T4** | Fonts and labels on the wire: schema, baker, C reader, host suites | every new refusal has a mutant that bites |
| **T5** | The stage composes labels; `orb_stage_text`; SDK | host composition test; a label on Bronze |
| **T6** | The tracker's fonts and labels baked per family; the app's last widgets go; L4 | `render path` log silent, L4 fps on Bronze with live traffic |
