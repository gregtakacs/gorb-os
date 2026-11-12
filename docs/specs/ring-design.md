# Ring design: the OS ring any app may set, and the `color` setting

Slice **ring** of issue #11, `docs/app-platform-plan.md` §64.1 (the ring) and §64.2 (the
`color` setting). Design only: this file, one spec per step (`ring-1`..`ring-3`), and
`ring-chain.json`. No product code here. The pomodoro (§64.3) is the ring's first user and a
**later** slice — this design only has to be usable by it, and does not design it.

## 0. The rulings that bind this slice

Every one of these is the owner's and is honoured below; where a note and the current code
disagree, the note wins and this says so.

- **Drawn by the OS, over whatever is on the glass, stamped into the DMA tiles as they leave,
  never in the mirror `s_fb`** — so `/fb` cannot see it (§64.1; `gorb-os-edge-arc`). This is
  exactly the copy arc's contract (`main/display/orb_edge_arc.c`): stamped at `display.c:969`
  (tiled flush) and `display.c:2170` (`fx_frame`, after the shadow so pixdiff reads the
  compose), never on the untiled fallback. The ring shares that path.
- **An app never draws the ring; it only says what it should be** — so an app off the glass
  that sets the ring from `orb_service` draws nothing itself (§64.1; `gorb-os-nothing-off-the-glass`).
- **One ring; the latest caller owns it**; the OS keeps each app's last ring and shows the most
  recent; on a clear or an uninstall the next most recent shows (§64.1). One small record per
  installed app, sized `ORB_APP_CAP` (8).
- **The cap `ring`** is the consent (D19), declared in `app.json` and revoked per app in the
  app's Permissions sub-menu (§58.5). Undeclared or revoked answers the refusal code the other
  caps use, and that app's record is dropped (§64.1).
- **The copy arc wins** while a card copy runs (the dial frozen for the copy, `orb_net.c`
  `copy_quiet_begin`/`_end`, ~7860-7871); the app's ring returns after it ends (§64.1).
- **Breathe is slow only**; its cost is a band of rows re-sent over a live app, so the OS caps
  the period and the redraw rate whatever the app asks; flash is bounded the same way (§64.1).
- **Internal RAM is the cliff** (~23 KB free under the clock, largest block ~7.3 KB;
  `gorb-os-internal-ram-under-clock`, `gorb-os-frozen-panel-witness`). New buffers go to PSRAM;
  a few hundred internal bytes once froze the panel. The ring's whole footprint is ~150 B and
  lives in PSRAM (§7).
- **Settings are declared, and the OS draws them** (`app-settings-on-hub-page`, §57). The
  `color` type is a new `type` in the frozen C9 int store, not a new storage type (§64.2).
- **Host suites are the cheap signal** (`gorb-os-2-host-suites-are-cheap`): the ring's pure
  logic is compiled on gcc and mutated, and a guard with no failing mutant is unwatched.

## 1. What the slice adds, mapped to §64

| §64 sentence | this slice | step |
|---|---|---|
| ring is the OS's, stamped into tiles, never the mirror | `orb_ring.c` pure core + a thin IDF stamp/service glue in display.c | ring-1, ring-2 |
| fraction 0..1000, arc+track colour+alpha, effect, period, clear | `orb_ring_set` / `orb_ring_clear` (one import family, cap `ring`) | ring-1, ring-2 |
| a real blend over the glass, not an overwrite; alpha 0 draws nothing | `ring_blend565()` (byte-swapped RGB565), alpha in the ARGB top byte | ring-1 |
| breathe slow only; flash bounded the same way | the clamp table (§6) | ring-1 |
| latest caller owns it; clear/uninstall reveals the next | the ownership table, newest-by-seq (§5) | ring-1, ring-2 |
| cap `ring`, revoked per app in Permissions; refusal drops the record | `CAP_RING` across the cap tables + `orb_ring_set` gate | ring-2 |
| copy arc wins, ring returns after | the rim dispatch picks the copy arc while `orb_edge_arc_shown()` | ring-2 |
| `/status` names the owner and state; `/fb` cannot see it | a `"ring":{...}` block in the status JSON | ring-2 |
| `color`: 0xRRGGBB in a C9 int, hub picker, dial palette with "custom" | `color` in orbapp/orbsdk/`orb_setui.c` + the hub picker | ring-3 |
| pomodoro uses it | not here (§11) | — |

## 2. The API (SDK), exact signatures

The OS already passes records by pointer across the boundary (`orb_home`, `orb_units`,
`orb_net_info`, `orb_map_open` — each a fixed struct, `m3ApiCheckMem`'d, mirrored in
`orb_app_wasm.c`, `orbsdk.py` refuses a layout drift — see the ILP32 comment at
`orb_app_wasm.c:1590`). Seven values (fraction, arc rgb, arc alpha, track rgb, track alpha,
effect, period) do not pack legibly into a few i32, so the ring follows the record idiom: one
struct, two imports.

```c
/* apps/sdk/orb.h — the ring family (cap `ring`, §64.1) */
#define ORB_RING_STEADY   0     /* on while shown; redraws only when the fraction moves */
#define ORB_RING_FLASH    1     /* on/off twice a period: "waiting for you"             */
#define ORB_RING_BREATHE  2     /* slow in/out: "paused"                               */

typedef struct {
    uint16_t fraction;   /* 0..1000, clockwise from twelve; >1000 clamps to 1000        */
    uint8_t  effect;     /* ORB_RING_STEADY | _FLASH | _BREATHE; other values => STEADY  */
    uint8_t  _res0;      /* reserved, 0                                                  */
    uint32_t arc;        /* 0xAARRGGBB — arc alpha in bits 31..24 (0 => the arc draws nothing) */
    uint32_t track;      /* 0xAARRGGBB — track alpha in bits 31..24 (0 => a bare arc)    */
    uint16_t period_ms;  /* effect period, clamped by the OS (§6); 0 => the OS default   */
    uint16_t _res1;      /* reserved, 0                                                  */
} orb_ring_t;            /* 16 bytes, ILP32 LE; _Static_assert(sizeof == 16) in orb.h    */

ORB_IMPORT("ring_set")   int32_t orb_ring_set(const orb_ring_t *r);  /* sig i(i) */
ORB_IMPORT("ring_clear") int32_t orb_ring_clear(void);              /* sig i() */
```

**Why this packing.** The colour words reuse the ARGB convention the canvas already carries
(`orb.h:358`, `orb_png8`'s 256 `0xAARRGGBB` palette, `cv_*`), so an app that reads a `color`
setting builds an `arc`/`track` by OR-ing its own alpha into the high byte — no new convention,
and the two independent alphas (§64.1: "each with its own alpha") ride the byte ARGB already
reserves for it. The record is one pointer (one `m3ApiGetArgMem` + `m3ApiCheckMem(p,16)`) instead
of six packed scalars, extensible (`_res`), and matches the `orb_map_view_t`/`orb_units_t` idiom
rather than inventing a new one. Both imports return `esp_err_t` (the OS's own numbers): `ESP_OK`,
or `ESP_ERR_NOT_ALLOWED` (the exact value the `SVC()` cap macro at `orb_svc.h:227` answers —
"the refusal code the other caps use"). A void `ring_clear` could not report a refusal (the
`orb_svc.c:549` note says a void entry cannot return one), so `ring_clear` also answers
`int32_t`, and an app without the cap gets the refusal from either call and its record is dropped.

`orb_ring_set(NULL)` answers `ESP_ERR_INVALID_ARG`. Fraction, effect and colours are read from
the record; nothing is stored per call (§5). Constants live only in `orb.h`; the shim binds the
two names by adding them to `s_binds[]` (`orb_app_wasm.c:1876`, beside `sound_play`), and
`binds.json` is regenerated by `orbsdk.py binds --write`.

## 3. Module layout

```
main/display/orb_ring.h    pure core API (no esp/lvgl includes)
main/display/orb_ring.c    PURE: geometry, blend, effect clock, clamps, ownership table.
                           Compiles on host gcc with zero IDF — what host_ring_test.py builds.
main/display/orb_edge_arc.c gains bool orb_edge_arc_shown(void)  (reads s_shown >= 0) — the copy-wins gate
main/display/display.c      the thin IDF glue: rim dispatch at :969 and :2170
main/main.c                 orb_ring_disp_service() beside orb_edge_arc_service() (:63, LVGL task)
main/app/orb_app_wasm.c     imp_ring_set/imp_ring_clear + s_binds rows; drop the record in slot_release()
main/app/orb_svc.h          CAP_RING = 10 (CAP_N -> 11); the owner-cap tables elsewhere
main/net/orb_net.c          a "ring":{...} block in the /status JSON
apps/sdk/orb.h, apps/sdk/binds.json   the SDK half (binds.json regen: orbsdk.py binds --write)
tools/orb-manager/web/tabs.js         the hub colour picker (ring-3)
```

`orb_ring.c` is pure (only `<stdint.h>`/`<math.h>`/`<stddef.h>`); every time-dependent entry
takes a `now_us` the caller supplies (dependency injection, so it is host-testable). The IDF-only
glue is the service (reads `esp_timer_get_time()`, calls `display_mark_rows()` and the LVGL
corner-invalidate, exactly like `orb_edge_arc_service`) and the stamp dispatch. The glue is
deliberately thin so almost all the bite is in the pure core, where the host suite can reach it.

## 4. Geometry and the blend (pure, ring-1)

Same band as the copy arc and for the same reason (§64.1, §8): a bright arc clockwise from
twelve over a dim track — the very look of §61.4's carousel-card progress ring and the edge arc's
`0x66DF`-over-`0x3186` — `R_OUT = 231`, `R_IN = 224` (7 px at the rim, glass 466×466,
`CX=CY=233`). It reuses the arc's per-pixel geometry (`cosf`/`atan2f`, **no angle table**) so
there is no new internal buffer to fund; the annulus is decided from `dy`/`dx` per pixel exactly
as `orb_edge_arc_stamp` does (`orb_edge_arc.c:73-102`). The ring is **blended, not overwritten**:

```c
/* one glass pixel: bg565_swapped is the wire-order tile pixel; argb's top byte is alpha */
uint16_t ring_blend565(uint16_t bg565_swapped, uint32_t argb);
```
Byte order is inverted, RGB565 channels unpacked, each channel lerped `bg + (src-bg)*a/255`,
repacked, re-swapped (the tile holds byte-swapped RGB565 — the copy arc's `wire()`, and `arc=
0x66DF`/`track=0x3186` already confirm that order). `a==0` returns the background untouched
(alpha 0 draws nothing). `ring_rows_for(fraction, &y0, &y1)` gives the y-range the annulus
touches (`[CY-R_OUT, CY+R_OUT]` = rows 2..464). A pixel is **arc** if its clockwise-from-twelve
angle `≤ fraction*2π/1000` and it is inside the annulus; else **track**.

## 5. The ownership table (pure, ring-1; drop glue ring-2)

`orb_ring_state` (16 B): `fraction, effect, arc, track, period_ms, owner, seq, dirty`. The table
is `EXT_RAM_BSS_ATTR static orb_ring_state s_own[ORB_APP_CAP]` — **PSRAM**, sized by
`ORB_APP_CAP` (`orb_app.h:295`, 8, "cap 8 installed, hard"). `seq` is a monotonic stamp
(`now_us`/1000, so two calls in one ms still order). `ring_set(slot, rec, now_us)` clamps,
writes that slot's record with `seq` and `owner=slot`, then re-picks. `ring_pick(now_ms,
out_shown)` walks the ≤8 records, keeps the owned ones, takes the max `seq`, and fills the shown
state with the effect resolved to this instant. On `ring_clear(slot)` that record's `owner=-1`
(the **next** most recent surfaces on the next pick). Uninstall and a revoked cap drop the record
the same way: `ring_drop(slot)` sets `owner=-1`, called from `slot_release()`
(`orb_app_wasm.c:208`, where net/blob/map already release) and from the revoke path
(`orb_svc.c`, beside the `idle_dim_app_release` hook the POWER revoke uses, :1051). No allocation
per call; the whole table is one PSRAM static. The "dirty" flag says the pick changed since the
service last stamped, so a steady ring re-marks nothing.

## 6. The clamps and their cost (ring-1)

One redraw grid and per-effect period floors, applied in the pure core (`ring_clamp`):

| constant | value | meaning |
|---|---|---|
| `ORB_RING_STEP_MS` | 500 | the ring repaints at most twice a second (2 Hz), whatever the effect asks |
| `ORB_RING_BREATHE_MIN_MS` | 4000 | slow breathe only: the period clamps **up** to ≥ 4 s |
| `ORB_RING_FLASH_MIN_MS` | 1000 | flash at ≤ 1 Hz: two transitions (on, off) a cycle |
| `ORB_RING_STEP_PCT` | 5 | a steady fraction change re-sends only if it moved ≥ 5‰ (else swallowed) |
| `ORB_RING_DEFAULT_PERIOD_MS` | 4000 | used when `period_ms == 0` (breathe/flash) |

Cost, in the repo's rows/sec currency. An effect (breathe/flash) repaints the **whole annulus**
each grid step, and because `display_mark_rows` marks whole rows, that range is the rim band rows
2..464 ≈ **463 rows**; the flush sends those rows at the dirty x-window. So:

- breathe at the clamp: ≤ 2 repaints/s × ≤ 463 rows = **≤ ~926 rows/s** (a ≥ 4 s cycle stepped on
  the 500 ms grid = ≤ 8 amplitude steps per breath — visibly slow), i.e. ~2 full-rim re-sends a
  second on top of a live app.
- flash: 2 transitions per ≥ 1 s cycle, each on the 500 ms grid = **≤ ~926 rows/s**, bounded the
  same way (§64.1).
- steady: **zero** rows between fraction moves; a fraction move re-sends only the y-band the arc
  moved across (usually tens of rows), and never faster than the 500 ms grid.

The stamp compute is trivial per tile (20 rows, `TILE_ROWS`): ~14 px per middle row × 2 spans for
the side bands, the full cap width only on the two cap tiles; the cost is the re-send, not the
ALU, so the grid is what the OS clamps. The orchestrator benches the real rows/s and the fps drop
on the orb (workers never touch a device or serial).

## 7. RAM accounting (requirement 9)

- Ownership table `s_own[8]`: 16 B × 8 = **128 B**, `EXT_RAM_BSS_ATTR` → PSRAM, not internal.
- Shown-state + step clock (a copy of one record, a `now_ms`, a step timer, a `s_ring_shown`
  flag): ~24 B, PSRAM.
- **No per-row angle table**: geometry is computed per pixel with `cosf`/`atan2f` like the copy
  arc, so no new internal buffer is needed at all.
- Net internal-RAM delta: **0 B** of new internal statics. Per tile the stamp writes only the
  annulus pixels it touches; per frame the added DMA is the marked rim rows (§6). This keeps the
  18,640 B internal DMA block that a few internal bytes once pushed under (frozen-panel-witness).

## 8. How the ring and the copy arc share the band (ring-2)

Both want the same 7 px rim annulus. One rule: **the copy arc wins**. `display.c` gets one
dispatch at each stamp site, replacing the two existing `orb_edge_arc_stamp(...)` calls
(`display.c:969` and `:2170`):

```c
if (orb_edge_arc_shown()) orb_edge_arc_stamp(row, rows, tb, tx0, tw);  /* copy in progress */
else                       orb_ring_disp_stamp(row, rows, tb, tx0, tw); /* the app ring, or nothing */
```

`orb_edge_arc_shown()` is a new one-line predicate (reads the arc's `s_shown >= 0`). The copy's
driver (`copy_quiet_begin`/`_end`, `orb_net.c:7860-7871`) is untouched: it keeps driving the arc
at 0..1000/-1 during the copy; because the arc wins, the ring silently holds its last state
meanwhile (the ownership table is unaffected), and the first frame after `copy_quiet_end` (arc →
-1) stamps the ring again — the ring returns with no work by the app. Neither is on the untiled
mirror fallback (the arc's own comment, and requirement 1).

`orb_ring_disp_service()` runs on the LVGL task in `main.c` right after `orb_edge_arc_service()`
(:63), reads `esp_timer_get_time()`, advances the effect clock, and on the 500 ms grid calls the
pure `ring_pick()`; when the pick moved it marks the rows `ring_rows_for()` names (via
`display_mark_rows`, the arc's own path) and invalidates the same off-glass corner pixel
(`lv_obj_invalidate_area(scr, {0,0,1,1})`) so a still page carries the frame. The pure core
touches neither LVGL nor `esp_timer`.

## 9. The `color` setting type (ring-3, §64.2)

- **Stored as `0xRRGGBB` in the frozen C9 int store.** In `tools/orbapp.py`: add `"color"` to
  `UI_TYPES` (line 202) and `UI_WIRE["color"] = "int"` (line 203), so it normalizes to a C9
  `int` with `min=0, max=0xFFFFFF`. `SET_TYPES` (the frozen C9 set, line 201) is unchanged.
- **`orbsdk.py` / `normalize_settings` validates it**: `default` is **required** (an explicit
  number, not an implicit 0), and must be an integer `0..0xFFFFFF`; a missing/non-integer/
  out-of-range default is refused with a sentence naming the setting. `show` defaults to `both`
  (a colour is a per-orb choice the owner also makes on the dial). Opacity needs no new type: an
  `int` `0..100` with `unit:"%"` (the §57.2 rule already covers it, per §64.2).
- **The dial steps a fixed palette; off-palette shows "custom".** In `main/app/orb_setui.c`:
  add `ORB_SETUI_COLOR` to the enum (before `ORB_SETUI_TYPES`) and `"color"` to `TYPES[]` (line
  10, so `ORB_SETUI_TYPES` becomes 9); the JSON reader's type loop maps it, `orb_setui_value`
  prints the palette's **name** for a stored value or `"custom"` when it matches none, and the
  dial's stepper walks the palette (a value dial: press-and-turn cycles Ruby→…→Pearl and back,
  storing the named colour's hex; it never edits raw hex on the knob, matching §57.4).
- **The hub offers a colour picker.** `tools/orb-manager/web/tabs.js`: in `valueRow()` (line
  1249) add an `s.type === "color"` branch rendering `<input type="color" value="#rrggbb">` with
  the same save path as the number field (`POST /api/v1/apps/cfg`), and `valueText()` (line 1239)
  prints `#rrggbb`. The hub can set any colour; the dial shows "custom" for an off-palette one.
  `jscheck_test.go` (balance) and a `ui_test.go`/`webui_test.go` grep pin the branch is present.

### 9.1 The named palette (12, on the dial, in this order)

| # | name | value | # | name | value |
|---|---|---|---|---|---|
| 1 | Ruby | `0xD62828` | 7 | Sky | `0x2E93E4` |
| 2 | Ember | `0xF77F00` | 8 | Sapphire | `0x2D5BD0` |
| 3 | Amber | `0xFFC300` | 9 | Indigo | `0x5B4BFF` |
| 4 | Lime | `0x80B918` | 10 | Violet | `0x9B43E0` |
| 5 | Emerald | `0x0FA36B` | 11 | Magenta | `0xD93B8E` |
| 6 | Teal | `0x0FBFB1` | 12 | Pearl | `0xEDEDF3` |

Spread over the hue wheel plus a near-white, each legible on the dark glass, one name each (the
eight-looks discipline). A single table in `orb_setui.c`, mirrored by the hub's default hint text;
off-list values read "custom".

## 10. The gate, as concrete checks

The slice is done when each holds. The chains' verifies run the sandbox-available half; the
firmware-only files are checked by the orchestrator's `idf.py build` + bench (workers never
touch a device or serial).

1. `python3 tools/host_ring_test.py` prints `RESULT: pass`; `--mutations` prints `N of N mutants
   caught` with every mutant biting (proves geometry, blend, clamps, ownership bite — host-suites-cheap).
2. `python3 apps/sdk/orbsdk.py binds` → `binds.json matches the firmware` (ring_set/ring_clear bound).
3. `python3 tools/host_menu_test.py --mutations`, `host_svc_test.py`, and the `host_orbapp_*`
   suites pass → the new `CAP_RING` is consistent in every owner-cap table (a wrong `CAP_N`
   breaks these gcc builds — that is the point).
4. Hub: `go vet ./...` and `go test` in `tools/orb-manager` pass with the colour-picker test
   present, and `jscheck_test.go` still balances `tabs.js`.
5. `make check-apps` (with `ORB_CLANG=/usr/lib/llvm-21/bin/clang`) passes — the SDK surface, the
   factory apps, the default theme and the shelf all still build.
6. Firmware-only (orchestrator, on the orb): `idf.py build` clean; a themed app under a running
   ring still animates; `/status` shows `ring.owner`/`fraction`/`effect`/`on_glass`; `/fb` cannot
   see it; the copy arc wins mid-copy and the ring returns; `glass.tiles` stays healthy (§7).
7. `version.txt` and any app `os_min` are the orchestrator's, not a step's (requirement 12).

## 11. What waits for the pomodoro slice (§64.3)

Not designed here. This slice must only be able to serve it: a colour per state (the ring takes
an ARGB; the `color` settings supply it), paused ⇒ `ORB_RING_BREATHE`, phase-over-and-waiting ⇒
`ORB_RING_FLASH`, running ⇒ `ORB_RING_STEADY`, off ⇒ `orb_ring_clear`; it runs off the glass from
`orb_service` (drawing nothing there — the nothing-off-the-glass ruling); the ring returns after
a copy because the copy arc wins. The ring needs no persistence of its own: after an in-place
restart (§63) the app re-sets it from `orb_service` against its wall-clock phase, and until then
no record is owned. The phase machine, the chime (cap `sound`), the knob gestures,
the theme face, `os_min` and `version.txt` all belong to the pomodoro slice.

## 12. Slices → steps

- **ring-1** — the pure core (`main/display/orb_ring.c`/`.h`) + `tools/host_ring_test.py` with
  mutations. Fully checkable in the sandbox: it adds no idf.py/`check-host` exclusion need (the
  suite needs no `sdkconfig.h`, unlike `host_idle_dim_test.py`). Geometry, blend, effect clock,
  clamps, ownership table.
- **ring-2** — firmware glue + SDK import + cap + /status: `orb_ring_disp_service`/`_stamp` glue
  in display.c+main.c, `imp_ring_set`/`_clear` and the `s_binds` rows, `CAP_RING` across the cap
  tables, `orb.h`/`binds.json`, `orb_edge_arc_shown()`, the record drop, the `/status` block.
  Sandbox half: binds + host cap suites + `make check-apps`; firmware half: `idf.py build`.
- **ring-3** — the `color` setting: `orbapp.py`/`orbsdk.py` validation + palette, `orb_setui.c`
  type + stepper + "custom", the hub picker, the hub suites.

## 13. The arbitration table (what the rim shows)

Priority is top-down; the first match owns the band that frame.

| condition | the rim shows | why |
|---|---|---|
| a card copy is running (`orb_edge_arc_shown()`) | the **copy arc** | §64.1 the copy arc wins; the ring's record is untouched and returns after |
| no copy; an app record is owned and its effect is on | the **latest owner's ring** (blend) | §64.1 latest caller owns it |
| no copy; latest owner's effect is off-phase (flash off half, alpha-0 arc+track) | **nothing** (glass untouched) | §64.1 an off-phase / alpha-0 ring draws nothing |
| no copy; no owned record (all cleared/uninstalled) | **nothing** | §5 next-most-recent already promoted; none left |
| untiled mirror fallback (no DMA ring funded) | **neither** | requirement 1 / the arc's own comment; on-glass only |

Two records for one app never coexist (one row per `ORB_APP_CAP` slot). An app off the glass still
owns the ring (it set it from `orb_service`, drawing nothing). Ties in `seq` (same ms) break by
`slot`, deterministic for the host suite and immaterial on the orb.

## Owner questions

- **Blend at mid alpha** — lerp the ring colour over the tile pixel (opaque-ish rim at high
  alpha, faint at low), **not** a hard-edge overwrite (would fail §64.1). *Chosen: lerp.*
- **Does a steady fraction move honour the 500 ms grid or repaint live?** — honour the grid (a
  countdown ticks slower than the rim needs; keeps the §6 cost ceiling). *Chosen: honour the grid.*
- **`ring` cap default at first install** — same as every owner cap: declared ⇒ granted, owner
  revokes per app; a call without the grant answers `ESP_ERR_NOT_ALLOWED` and drops the record.
  *Chosen: mirror the other owner caps exactly.*
- **Off-palette `color` on the dial** — show the literal `"custom"` and let the hub picker change
  it; the dial must not edit raw hex. *Chosen: "custom".*

## Orchestrator amendments (2026-11-11) — these override the sections above

1. **No per-pixel trigonometry in the stamp (§4, §6).** The copy arc's `atan2f`+`sqrtf` per band
   pixel is fine for a copy that lasts seconds, but the ring can stand for 25 minutes over every
   frame of every app (58 fps transitions), ~10k band pixels a frame. The stamp must do only
   the blend per pixel:
   - Per row, the annulus is at most two x-spans (left and right side, or one span on the cap
     rows). Compute them from integer arithmetic or one `sqrtf` per row edge, or from a
     233-entry half-width table (≤ 1 KB, PSRAM, built once).
   - The arc/track boundary for a row is the x where the ray at the current fraction's angle
     crosses that row. Compute it once per pick from the fraction (one `tanf`/`sinf`/`cosf` per
     pick, not per pixel). Each span then splits into at most an arc part and a track part.
   - The host suite proves the span version paints the **same pixels** as a per-pixel
     reference (the copy arc's own `atan2f`/radius test) for every row and a sweep of fractions
     (0, 1, 125, 250, 499, 500, 501, 750, 999, 1000 and 50 random values), allowing at most
     1 px of disagreement only on the ray boundary. Mutants must cover the span edges and the
     boundary side.
   - State the per-frame cost in the design's terms: spans per row and blend per pixel.
2. **The hub must accept the new cap (ring-2).** The hub refuses an `.orbapp` whose header
   declares any cap outside `appCapOwnerAll` (`tools/orb-manager/internal/orbnet/appcontainer.go`
   ~:140-160 and the refusal at ~:329). So ring-2 also:
   - adds `AppCapRing = 1 << 10` (the same bit as the firmware's `CAP_RING = 10`) to that file,
     to `appCapOwnerAll`, to `AppCapNames` as `"ring"`, and to the refusal sentence's list;
   - updates `internal/orbfake` if it mirrors caps;
   - updates `tools/orbapp.py`'s `CAP_OWNER` list;
   - adds a Go test: a container declaring `ring` parses, and bit 1<<11 is still refused.
   
   ring-2's verify drops the "no tools/orb-manager change" test and adds `make check-hub`.
3. **Mutation verifies must fail on any escaped mutant.** `host_ring_test.py --mutations` exits
   non-zero when any mutant escapes, and prints `mutants caught N/N` (the existing suites'
   wording). Verify by exit status, not by grepping a word.
4. **ring-3 runs the whole hub gate** (`make check-hub`), not a `-run` subset.
5. **The colour setting on the dial turns like an enum.** The settings page's own idiom is:
   press a setting, turn to change, press to keep. A `color` setting turns through the palette
   in that same flow. No press-and-turn on the settings page.
6. **Base:** the chain runs on `delegate/ring-design`, so this plan travels with the slice.
7. Owner questions: all accepted as chosen.
