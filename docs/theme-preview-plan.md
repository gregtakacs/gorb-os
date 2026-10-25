# Theme previews: the ratified design, and what is built today

**Why this file exists.** The preview decision was argued and RATIFIED in a cline
session on 2026-10-23/24 (session id `1792879334049_3g5bk`, design §8 "Preview:
one reserved plane, zero new formats"). It lived only in that chat. Sessions break,
and the next one re-litigates a decision it was never told about — a prior agent
reported "no thumbnail field in the FORMAT 3 header, so previews need a format
decision," which read the wrong layer: the design never wanted a header field. This
file puts the decision and the honest current state where a session picks them up.

**Status: DESIGN RATIFIED; BUILT ON GLASS, PARTLY BUILT ELSEWHERE.** The class
`ORB_ASSET_PREVIEW` is
appended at index 9 in `main/assets/orb_assets.h` (D34's AUX append is the
pattern: end-of-enum, no ABI move, old firmware refuses by class). The reader's
`orb_asset_slot_bytes()` gives it the two-byte RGB565 shape. The compiled dial's
projection at `main/art/orb_clock3.c` accepts a class-9 row and skips it (the
dial does not draw it; the OS picker reads it via `orb_elems.c`). The baker at
`tools/watch-composer/orbtheme-baker.js` emits the plane on `bakeClock3({preview:
{plane, x, y}})`: class 9 slot 0, one square frame at half the panel's edge,
appended at the running end of the art bank so no other row's `frame0` shifts.
The firmware reader `orb_theme_preview_read()` (main/art/orb_theme.c) resolves
the plane transiently out of a theme's card file: it generalizes the card-open
door to any app slug (the picker's caller passes `handle`, and the file source
under that slug's `orb_files_src(slug, "themes")` is walked; `card_open_verified_id3`
is now the clock-bound wrapper of the generalized `card_open_verified_slug3`), reads
the geometry region into a PSRAM scratch, and resolves (9, 0, 0) through
`orb_elems_at` — so a weather theme's preview resolves against `weather/` without
the clock being resident, and the absent-preview answer matches the design's rule
(NO_CONTENT, never an error). The shell's paint at cursor-land is BUILT (step 4):
the app ABI answers two new optional doors — `opt_focus` (the cursor landed; may
start a job, may not paint) and `opt_preview` (the reaped plane, its extent, and
a stamp; may not touch the card) — the clock's picker posts one settled worker
read of the plane into a PSRAM buffer through them, and `menu_view.c` blits the
answer full-frame over the list on an `lv_image` that paints over the rows. The
JS/HTTP surface is still unbuilt (browser `GET /api/v1/sd/preview?id=<handle>`
and its ~30-line canvas unpack), as is content generation: existing themes in
`art/` carry no preview row yet, so every shipped theme answers NO_CONTENT and
draws its text-only row. The owner's report "I still don't see any previews"
stays correct until a theme is baked with one; on such a theme the dial shows
the plane now.

## 1. The decision, revised (2026-10-25 session)

An earlier draft called the plane a `PLATE` class slot named `preview`, so "no
class enum is appended." That was wrong on two facts and the design is corrected:

- **`PLATE` collides with the dial's own background slot.** An app with both a
  background plate and a preview cannot carry two slots under one name — the
  install gate would refuse one as `ORB_ASSET_CLASS`.
- **`PLATE` pays RGB565A8 (`w * h * 3`), and a preview wants RGB565** (opaque,
  `w * h * 2`). A third byte per pixel is a plane of 255s no reader consults.

**Revised design (owner's decision 2026-10-25):**
A theme's preview is **one reserved plane named `preview`, class `PREVIEW`
(9, appended), inside the theme section's own art bank** — not a new region,
not a header word, not a PNG, not a decoder. It rides `art_size` and `sec_crc[]`
for free, and the install gate CRCs the bank unchanged. A preview costs one class
append and zero format change.

- **Geometry**: 232x232, the half-res of the 466-square glass, upscaled 2x on draw.
  RGB565 (2 B/px) is 107,648 B; the RGB565A8 shape every other drawable class pays
  would be 161,472 B. Both fit a real theme's bank budget.
- **Opaque.** Every texel paints at `alpha=255`; the compositor draws it exactly
  like a solid plate.
- **Optional.** A theme with no `preview` plane answers `ORB_ASSET_NO_CONTENT`
  (already defined, `main/assets/orb_assets.h`), and its row stays text-only. An
  absent preview is never an error.
- **Baked from the composer's existing pose**, downscaled at export: 10:09:36
  (`previewDate()`), moon 0.34 (`PREVIEW_MOON`), `watch-engine.js`. No pose
  metadata enters the file — the pose is a bake-time fact, not a runtime one.

## 2. On glass (BUILT — step 4, this session)

Landing the cursor on a theme row queues **one** worker read of that extent's
`preview` plane (~108 KB, ~82 ms off the card), after the cursor has rested on
the row for one settle window (120 ms): a fast spin past ten themes costs one
read of the theme the owner stopped on, not ten. The read runs on the shell's
one worker (`orb_jobs_post`), never on the LVGL task and never in the flush
path — the card-read + scan blocking rule in `.clinerules` holds. The answer
publishes a PSRAM plane under the picker's own mutex, is reaped by the app's
`service()` hook on the LVGL task, and is dropped (freed on the LVGL task, one
repaint after the shell last blitted it) when the cursor moves on, the group
closes, or the page is left.

The ABI carries it through two optional doors appended to `orb_app_iface_t`
(both NULL on every pre-preview entry, exactly `opt_kind`'s NULL-is-the-old-
ABI pattern): `opt_focus(app, idx)` says the cursor landed on the app's row
`idx` — the app's licence to start the worker read, the same licence `opt_sel`
carries — and `opt_preview(app, idx, &w, &h, &stamp)` answers the reaped plane,
its extent, and a publish stamp, from already-arrived state only: the same
rule `opt_label`/`opt_value` live under. The shell asks both from `repaint()`
(the one place that knows the cursor), draws the plane full-frame over the
list on one hidden-by-default `lv_image` created last on the menu screen (so
it paints over the rows and the share card can take it down), and upscales it
to the glass by integer nearest (`antialias off`: a bake doubled, not a
picture resampled). A plane that lands while the page stands still still
reaches the glass: `menu_view_service()` re-asks `opt_preview` every poll and
repaints on a stamp change — the same reap idiom the row count already uses,
and `host_menu_test.py` pins both directions (a landing plane repaints; the
check mutates red).

## 3. In the browser (to be built)

`GET /api/v1/sd/preview?id=<handle>` streams the raw plane; ~30 lines of JS unpack
RGB565 to a canvas, reused by the PWA `/themes` page, the hub's Theme card, and the
composer's install confirmation.

## 4. The picker (a per-app OS service, not an OS-level selector)

**Owner's decision (2026-10-25), superseding the handoff's item 5:** there is
**no OS-level theme selector.** Each themeable app carries its own theme picker
inside its own settings menu. The picker is an OS *service* so no app
reimplements it, and the caller always knows which app it is picking for. The OS
resolves theme candidates by app slug, and the preview plane rides the same
slug-scoped section the rest of the theme's art does.

This means `main/clock/clock_themes.c`'s opt-hook picker is the right shape for
the clock; a weather app ships its own, and the OS renders both apps' previews
through one asset-registry path. An earlier plan step to "promote the picker to
an OS page" misread the direction: the picker stays inside the app, and the
service lives inside the OS. Item 5 of the handoff closes on this.

Still unbuilt (verified against the tree as of 2026-10-25):

- **The class `PREVIEW` at index 9 is appended (previous session, `08d6f28`),
  and the baker now writes a plane into the section's art bank. The compiled
  dial's projection at `main/art/orb_clock3.c` accepts a class-9 row and skips
  it, so a theme with a preview rides install on the shipped firmware.**
- One-section enforcement and app-scoped identity DID land (`5af35cf`, `2d27e35`,
  `0668370`): the filename carries the app slug, and FORMAT 2 acceptance is retired.
  `ORB_BUNDLE_SECT_MAX` is still `8u` in `main/art/orb_bundle.h` — the design's
  "drop it to 1" was not taken.
- The `assets`-partition **extent pool** (`orb_theme_pool.{c,h}`) is NOT built —
  residency is still one image at offset 0. `ls main/art/orb_theme_pool.*` is empty.
  A preview does not need it: read transiently out of the theme's own `.orbtheme`
  card file, the way `orb_bundle.c` already reads the live theme's geometry and
  banks. (An earlier draft of this file claimed the pool was a prerequisite; that
  was wrong. Corrected against `orb_theme.h` and the `Open tourbillon ... plate
  bank: 8 keyframes` boot line on 2026-10-25.)

So building previews means building, in order: the baker writes a `preview`
RGB565 plane into the section bank; `orb_theme.c` gains
`orb_theme_preview_read(handle, buf, &size)` resolved through the asset registry
by class; each app's picker row-land queues the worker read and draws it; the HTTP
route and the ~30 JS lines. None of it needs a container format change.

## 5. The one sentence to remember

Preview = reserved `preview` plane, class `PREVIEW` (9, appended at end per D34's
pattern), in the section's bank, 232x232, RGB565 opaque (2 B/px via a `slot_bytes`
case), optional, worker-read, per-app picker is an OS service inside each app's
own settings menu. No new region, no header word, no PNG, no decoder, no pool, no
OS-level selector. If a session is told to "add theme previews," it is implementing
this, not deciding it.
