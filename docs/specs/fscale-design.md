# fscale — the flights scale and pill, as ruled in `docs/flights-map-plan.md` §7 (owner, 2026-11-10)

This design builds the **fscale** slice: the flights tracker shows every distance as a
**round number taken from a theme-drawn scale** (a ring or a bar), exactly as the radar does,
and the **range pill wears the theme's accent on the card's frame** instead of grey. It is the
third of §7's rulings (1 and 2 — the radar zoom ladder and the press-and-turn pill — are
already built in flights v1.6.3). The specs `fscale-1`..`fscale-3` implement it; `fscale-chain.json`
runs them. **No OS change and no new internal RAM allocation** anywhere in the slice.

## The gap (what is wrong today)

`apps/flights/main.c` prints the outer ring's *odd* radius. `range_text_at` (main.c:83)
lrounds `range_km_at(step)/UNIT_KM` — e.g. "87 km" at the Area step — and the same odd figure
is what the pill and header say (main.c:867-882) and what the no-theme `draw_frame` paints
(main.c:769-771, `orb_cv_text(..., CX+122, CY+136, 61, ...)`). Nothing draws a scale figure
(ring or bar); the plate's own `scope.rings` are decoration and carry no figure. The pill
(slot 38) is baked with the **body** tone (`PILL_FONT` font uses `W.body`, flights-bake.js:979)
on a plain grey fill (`pillCanvas`, flights-bake.js:794-800 draws only `F.fill`).

## The rule to copy (do not import radar code)

Radar's round length lives in `apps/radar/radar.c:259` `rd_scale`:

```c
static const int NICE[] = {1,2,5,10,20,50,100,200,500,1000,2000};
mpp = radius_m / (RD_GLASS/2.0);          // radar's ring IS the glass edge (233 px)
best = largest NICE with NICE*u/mpp <= max_px;  *px = round(best*u/mpp);
```

Flights **copies this rule into its own `fl_scale()` in `main.c`**; it must not link, `#include`
or textually depend on `apps/radar/*`. Two flights-specific differences:

- flights' ring stands at `RING_R` = 184 px, not the glass edge, so `mpp = radius_m / RING_R`
  (a round length then lands correctly on the ring at that step);
- flights carries its own `UNIT_KM[3]` (main.c:45). `fl_scale(radius_m, unit, max_px, *px)`
  returns the round length in the orb's unit.

## The scale is the theme's (ruling §7.4)

As on radar (`radar-plan` `layout.scale`), the theme chooses **Ring** — a ring drawn at the
round length, labelled on it up and to the right (radar main.c:531-534, 688) — or **Bar** — a
bar at the foot with the length under it (radar main.c:536-548, 689), with the radar's look:
ink, edge, thickness (2-8 px), end ticks (0-24 px) and longest bar (60-200 px), plus the
label's face, size and colour. Flights carries these in the theme's values carrier.

### Carrier: `fl_values_t` grows to 136 B, `FL_VALUES_VER` -> 2

Today `fl_values_t` (main.c:946-959) is 120 B and is only baked when the map is on
(flights-bake.js:969 `if (Mp) ... S.values(...)`). Two changes:

1. **Always bake values.** A new theme bakes the values row even with the map off, so the
   scale is carried regardless. `role_on[k] == 0` for every step still means "no map"
   (unchanged: `read_values` main.c:981-984 derives `s_map` from it), and the map path stays
   gated on `s_map`. `VALUES_VER` in `flights-map.js` and `FL_VALUES_VER` in `main.c` both go
   to **2**; `_Static_assert` (main.c:960) becomes 136; `bake_check.py` `VALUES_B` becomes 136
   and its "values with no map" refusal (bake_check.py:96-97) is retired.
2. **A scale region** (little-endian), which **reuses the struct's existing trailing `pad[3]`
   (bytes 117-119) as its first three fields** and appends the rest after the map fields (so the
   struct grows 120 -> 136 and `scale_ink` stays 4-aligned):

| byte(s) | field | meaning | default when absent |
|--------|-------|---------|---------------------|
| 117 | `scale_style` | 0 = bar, 1 = ring | ring (1) |
| 118 | `scale_lbl_px` | the length label's px | 14 |
| 119 | `scale_w` | bar thickness, 2-8 | 2 |
| 120 | `scale_tick` | end ticks' height, 0-24 | 9 |
| 121 | `scale_max` | longest bar, 60-200 | 150 |
| 122-123 | (pad) | | 0 |
| 124-127 | `scale_ink` (ARGB) | bar ink / ring line | 0xFF4A7A62 |
| 128-131 | `scale_edge` (ARGB) | edge around bar+ticks, alpha 0 = none | 0xFF020805 |
| 132-135 | `scale_lbl_argb` (ARGB) | the length label's colour | 0xFF8A9A90 |

The scale region is written by **`flights-bake.js` after** it calls `M().valuesBytes(m)` (so
`flights-map.js`'s `ranges()`/`valuesBytes()` bodies — owned by the separate `bare-airports`
task — are **not** touched). The bytes come from the flights design's new top-level
`d.scale`, `d.layout.scale` and `d.words.scale`, mirroring radar's (radar-bake.js:46-51, 75, 49).

### Backward and forward compatibility (fixed requirement)

- **Old theme (values layout 1, or no values), new app:** `read_values` (main.c:971) sees
  `ver != 2` (or `n == 0`), logs and zeroes `V`, and the app then draws the **default ring**
  scale (table above) at the round length. A theme baked before this slice still reads a round
  number and wears a ring.
- **New theme, old app:** an app at layout 1 refuses the 136 B/ver-2 values as "no map"
  (main.c:976-979) and shows the odd figure as before. Nothing crashes; the map is absent to
  the old app. (The pre-existing contract for a versioned values blob.)

## Runtime drawing (main.c, both modes)

The scale figure is drawn **on the canvas**, as radar does (`draw_scale` on `s_tgt`), not as a
baked row: the round length's radius changes with step, latitude and unit.

- **Themed:** flights already opens live layer 0 and draws the map into it (main.c:1094-1095
  `orb_stage_live(0, V.map_order, 0); orb_map_draw(ORB_CV_LIVE0, ...)`). From this slice the
  layer is opened **always** (map or not) and the scale is drawn into `ORB_CV_LIVE0` after the
  map by a `draw_scale()` mirroring radar main.c:526-549 (ring: `orb_cv_ring(ORB_CV_LIVE0,
  CX, CY, px, ink)`; bar: the `band()` 2 px pen with edge under ink and end ticks, at the foot).
  The length label is the theme's `LAB_RNG` row **re-posed** at runtime —
  `orb_stage_pose(s_lab[LAB_RNG], 0, x-CX, y-CY)`, x,y the ring's NE crossing
  (`CX + px*7071/10000 + 30`, `CY - px*7071/10000 - 6`) for a ring, `(CX, 414)` for a bar —
  exactly as radar re-poses `R_range` (radar main.c:436, 688-689). Its text is round length + unit.
- **No theme:** `draw_frame` (main.c:744) already paints `s_cv`; it draws the same ring/bar and
  the label with `orb_cv_text`, replacing the odd `range_text(rng,...)` at line 771.

No new `orb_cv_create`, no `malloc`, no `orb_stage_live` index beyond the existing single layer 0.

## Round numbers everywhere

`range_text_at` (main.c:83) — the one place that formats a distance — prints the
`fl_scale` round length, never `lround(range_km_at/U)`. The pill (`s_pill`) and header range
say (main.c:867-882) then read "<step name>  <round length> <unit>" (e.g. `Area  20 mi`), as
radar's zoom pill does (radar main.c:712-714). `range_req_nm` (main.c:82) — the feed radius —
is unchanged: the app still asks for the ring's true reach, only the *shown* number rounds.

## The pill wears the theme (ruling §7.5)

Pure bake change (`flights-bake.js`). `pillCanvas` (794) draws the backing in the **card's
frame look** — `frames.fill` with `frames.line`/`frames.edge` and `frames.style`, the same
`frameSvg`/nine-slice path the labels use — not a bare grey rounded rect. The pill's font
(slot `PILL_FONT` = 5, flights-bake.js:978-979) is coloured `W.accent` (the theme's scope
green), not `W.body`. `bake_check.py` (98-100) still requires the pill at slot 38 in font 5,
and gains a check that the pill's frame is present.

## Every start and preset gets a scale

`flights-bake.js` `preset()`/`STARTS`/`SCOPES`/`FRAMES` (114-233) and its defaulting give every
start and preset a `d.scale`, `d.layout.scale` and `d.words.scale`, so no design can bake
without one (fixed requirement). The editor's Scope/Words steps gain the radar-style Scale
section (radar-editor.js:156, 193-199, 213-216) and the preview's `frame` draws the ring/bar.

## Cost and steps

No row is added to the shelf budget (the values carrier grows 16 B, the pill re-bakes in place);
the only new runtime work is one `orb_cv_ring`/few `orb_cv_line`s per frame plus re-posing an
existing label. `apps/flights/version.txt` goes v1.6.3 -> **v1.6.4** in the last step; the
shelf `.orbtheme` re-bake is the orchestrator's (not a step).

- **fscale-1** — firmware: `fl_values_t` 136 B / ver 2 with defaults, `fl_scale`, round-number
  text, `draw_scale` on the live layer + no-theme canvas, `LAB_RNG` re-pose. Ships reading sane
  defaults so it works before the carry exists.
- **fscale-2** — bake + editor + carrier: design carries `d.scale`/`d.layout.scale`/
  `d.words.scale` on every start/preset, `flights-bake.js` always bakes values and writes the
  scale region, editor fields + preview, `bake_check.py` ver/`VALUES_B` and the no-map-values
  allowance.
- **fscale-3** — the pill wears the theme (accent font + card-frame backing), `bake_check.py`
  pill check, version -> v1.6.4 (last step).



## Orchestrator's review (2026-11-10, binding on every step)

1. **The scale's label has its own font.** The length label stays the theme's `LAB_RNG` row
   (slot 23), re-posed at runtime, but the bake gives it its own font (slot 6) from
   `d.words.scale` = `{face, px, colour}`, and the row's tone names font 6. The defaults are the
   quiet tone's face, size and colour (`W.face`, `sizes(d).quiet`, `W.quiet`), so a default
   theme's figure looks as it does today. The row's box is sized for the longest label it can
   say ("2000 km" in that font, plus its padding), centred where it is posed. `bake_check.py`
   expects font 6 on every theme (fonts `[0,1,2,3,5,6]`, and 4 too with the map).
   `scale_lbl_px` and `scale_lbl_argb` in the values feed the no-theme canvas path only.
2. **The bar clears the footer.** Flights' footer sits at y≈433-456, so the bar's top is at
   y = 420 (`SCALE_Y` 420) and its label is posed centred at (CX, 400), not radar's 440/414.
   The card may cover them while an aircraft is selected; the card is in front by design.
3. **Compatibility accepted:** a values-layout-2 theme on a pre-fscale app loses its map there.
   The shelf ships the theme with its app, so this is accepted (no owner question).
4. **The pill's look is settled** (owner asked Claude to choose from the existing themes): words
   in the accent tone (`W.accent`, with `W.accent2` as its foot where a family has one, as the
   accent font does), on the card's frame (`frameSvg` with the design's `frames`). Per family
   this reads scope green on slate with a green rule, brass gold on a double brass frame,
   blueprint ice-blue on a ruled blue box, galaxy amber on its violet pill, chart green and
   sectional magenta on paper: each the colour the theme already uses for what is live.
5. **Run in the gorb worker image** (`DELEGATE_IMAGE=delegate-worker-gorb:local`: clang 21,
   lld, gcc, chromium); `bake_check.py` finds `chromium` on PATH there.
