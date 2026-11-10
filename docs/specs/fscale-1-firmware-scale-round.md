# fscale-1: the flights firmware reads round distances and draws the theme's scale

## Goal

Slice fscale (`docs/specs/fscale-design.md`; read it all first — it carries the byte table and
the `fl_scale` rule). This is the firmware half only: `apps/flights/main.c` learns to (a) show
every distance as a **round number taken from a scale** rather than the ring's odd radius, and
(b) **draw the scale figure** (a ring at the round length, or a bar at the foot) on the canvas,
in both the themed and the no-theme mode. It also grows the values carrier to layout 2 (136 B)
and **falls back to a default ring scale** when the theme does not carry one, so the app works
before the baker writes the region (fscale-2) and against an old theme.

No OS change, no `malloc`, no new canvas, no new live layer. `apps/flights/version.txt` is NOT
touched here (it moves in fscale-3).

## Read first

- `docs/specs/fscale-design.md` (the carrier table, the `fl_scale` rule, the drawing rule).
- `apps/flights/main.c`: `range_req_nm`/`range_text_at`/`range_text` (82-87), the `say`/`set_lbl`
  label helpers (630-666), the pill+header range (860-885), `draw_frame` (744-772) and
  `FL_VALUES_VER`/`fl_values_t`/`_Static_assert`/`V`/`read_values` (943-990), `map_step`
  (1039-1101) and the themed/no-theme dispatch (1106).
- `apps/radar/radar.c`: `rd_scale` (259), `rd_radius_m` (185) — the rule to COPY (do not link).
- `apps/radar/main.c`: `draw_scale` (526) and the ring/bar range label in `draw_ui` (683-689).

## Contract

Modify only `apps/flights/main.c`.

1. **Carrier to layout 2.** `FL_VALUES_VER` 1 -> **2**; grow `fl_values_t` from 120 to **136 B**
   and update the `_Static_assert` to `== 136`. The map's `name_px` (byte 116) is kept; **the
   struct's trailing `uint8_t pad[3]` (bytes 117-119) is repurposed** into the first scale bytes
   and the rest appended, so the byte offsets land exactly on the design table. Replace that
   `pad[3]` with `uint8_t scale_style; uint8_t scale_lbl_px; uint8_t scale_w;` (bytes 117,118,119)
   and then append `uint8_t scale_tick; uint8_t scale_max; uint8_t pad2[2]; uint32_t scale_ink;`
   `uint32_t scale_edge; uint32_t scale_lbl_argb;` (bytes 120,121,122-123,124,128,132). Total 136,
   `scale_ink` 4-aligned at byte 124 (design bytes 117..135). (Layout-1 blobs are still refused on
   the `ver` check, so repurposing the old pad is safe.) In `read_values`, when the
   values are usable and layout 2 keep the scale bytes; else set defaults
   `style=1, lbl_px=14, w=2, tick=9, max=150, ink=0xFF4A7A62, edge=0xFF020805, lbl_argb=0xFF8A9A90`.
   `s_map` still derives only from `role_on` (981-984): a layout-2 theme with all-zero `role_on`
   means "no map" but still carries a scale. Clamp the scale fields (w 2-8, tick 0-24, max 60-200,
   lbl_px 14-40 else default).

2. **`fl_scale`.** Add a static helper copying radar's `rd_scale` NICE ladder but with
   `mpp = (double)radius_m / RING_R`; returns the round length in the orb's unit and, out-param,
   its px (`lround(best_unit_m/mpp)`). Local to `main.c`; no `#include` of radar.

3. **Round numbers shown.** Rewrite `range_text_at` (83) to print `fl_scale` at that step's radius
   (`range_km_at(step)*1000`), NOT `lround(range_km_at/U)`. `range_req_nm` (82) is UNCHANGED. The
   pill (869), header (880) and no-theme text (770-771) keep calling `range_text*` and become round;
   the pill/header format becomes `"<name>  <round> <unit>"`.

4. **Draw the scale figure.** Add `static void scale_draw(void *cv, int step)` mirroring radar
   `draw_scale`: ring -> a ring at `px`; bar -> the `band()` 2 px pen at the foot with edge under
   ink when `scale_edge` alpha != 0. Colours from `V.scale_*`.
   - **No theme:** call from `draw_frame` into `s_cv`, draw the round label with `orb_cv_text` at
     the ring's NE crossing or under the bar (replacing the plain text at 770-771).
   - **Theme:** open live layer 0 ALWAYS (also map-off): still `orb_stage_live(0, V.map_order, 0)`
     and draw the scale into `ORB_CV_LIVE0`; keep the map draw at 1094-1095. The label is the
     `LAB_RNG` row re-posed via `say`/`orb_stage_pose` at the ring's NE crossing
     (`CX + px*7071/10000 + 30`, `CY - px*7071/10000 - 6`) for a ring, `(CX, 400)` for a bar, in
     `V.scale_lbl_argb`.

Behaviour must not change when a theme has no scale region (defaults render a ring), and a ring
scale still shows when `s_map` is false.

## Acceptance criteria

Run each; all must hold (clang required — build in the owner's env):

1. `python3 apps/sdk/orbsdk.py build apps/flights` → exit 0 (`_Static_assert(... == 136)` holds).
2. `test -z "$(git diff --name-only 4e783bf5 -- main apps/radar apps/weather apps/flights/version.txt apps/flights/editor)"` → no output (only `apps/flights/main.c` changed).
3. `grep -n 'FL_VALUES_VER 2' apps/flights/main.c` matches; `grep -n '== 136' apps/flights/main.c` matches; `grep -n '#include.*radar' apps/flights/main.c` is empty.

## Out of scope

`apps/flights/editor/*`, `flights-map.js`, `flights-bake.js`, `version.txt`, any other app, the
OS (`main/`), radar files. No shelf re-bake.

## Notes

- A layout-1 theme (or none) must still run at a **default ring** round number — that is this
  step's backward-compat check; reason about the fallback branch in `read_values`.

## Review amendments

`docs/specs/fscale-design.md` "Orchestrator's review" is binding on this step; where it and this spec disagree, it wins.
