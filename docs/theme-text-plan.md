# Theme text: fonts and labels a theme carries, so an app's words ride L4

Status: **ruled 2026-11-04 (owner, §0); T1–T6 built and benched on Bronze the same day
(§4).** This is the E4 readout
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

## 4. As built and benched (2026-11-04)

- **T1.** `bake_theme.py` draws the range rings (full and half range, the background's ring
  colour and opacity) and the home dot into each plate, and bakes the selection and
  emergency rings as rows at slots 16 and 17. The app's five ring widgets are gone; it poses
  the marker rows onto a contact or hides them.
- **T2.** A row's opacity is its layer descriptor's `alpha`, which theme rows always carried
  at 255. The painter scales each texel's coverage by it exactly as LVGL's image blend does
  (`LV_OPA_MIX2`, cover at `LV_OPA_MAX`), on every path: still, rotated, smooth and drum.
  `orb_stage_opacity(row, opa)`; the tracker draws a stale contact at 128.
- **T3.** `ORB_LAYER_MAX` 32. The commit's job array grew by 16 paint contexts of internal
  `.bss`; Bronze's internal heap read 45.6 KB free, 18.4 KB largest block, with the tracker
  on the glass before and after.
- **T4.** As §2.1–2.2, in `theme_schema.py`, the baker and `orb_clock3.c`, with the fonts
  checked against the table after the walk. `host_clock3_test.py` carries 9 new forgeries,
  3 rewritten, and 12 new mutants (43 guards, all caught; the T4 commit message says 13); `verify_js_bake.py --rows` holds the three to
  each other on 9 new cases and a font and a label baked through `stage3` and projected by
  the shipped reader. `ELEM_FRAMES_MAX` is 1024 (was 360): four fonts of 100 glyphs and a
  49-frame pool reach about 460 catalogue rows; the device bounds rows by the catalogue.
- **T5.** `main/stage/orb_label.c` draws a label: pure C over byte planes, so
  `tools/host_label_test.py` compiles it on the host (23 checks, 12 of 12 mutants). The
  stage finds the fonts and each label's columns when it binds (`text: 4 fonts, 4 labels`),
  gives each label a PSRAM plane with a 4-byte lead (a sprite at offset 0 reads as unset,
  the rule the layer table's own bakes keep), composes it when the string or tone changes,
  and hands the plane to both paths: the LVGL image, and the layer row's `plane`. The
  claim skips fonts, which are never drawn where they stand. `orb_stage_text(row, s, tone)`.
- **T6.** Each family bakes four fonts in its background's typeface (DejaVu Sans on the
  scope and chart, Serif on brass, Sans Mono on blueprint): body and accent at 16 px, quiet
  and warning at 14, brass's accent a gradient. The four labels are the header (normal,
  accent), the footer (quiet, warning), the info card on a rounded panel in the
  background's edge colour, and the range figure; brass puts the small three on enamel
  plaques, since its bezel is engraved to the rim. A theme is about 1.25 MB (fonts add
  about 450 KB). `apps/flights/art/label_proof.py` draws a baked theme's labels through
  the shipped C, the card included.
- **Benched on Bronze**, Flights Modern stored through the hub's send, at 80 nm with 24–30
  live aircraft: `render path: l4=yes rows=22 refused=0`, no veto, the words in the theme's
  font; **14–16 fps** on L4, against 9–12 through LVGL on the same build before the labels,
  dipping to 10–11 while the app parses a feed poll (about 140 KB of JSON). No panic, no
  fault; the owner's 10 nm range and the clock on the glass were put back after.
- **Not built.** The clock's readout (a label whose source is a clock value, with a format)
  stays E4's; the `text` source keeps `str` for its fonts.
