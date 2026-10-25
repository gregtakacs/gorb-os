# Theme previews: the ratified design, and what is built today

**Why this file exists.** The preview decision was argued and RATIFIED in a cline
session on 2026-10-23/24 (session id `1792879334049_3g5bk`, design §8 "Preview:
one reserved plane, zero new formats"). It lived only in that chat. Sessions break,
and the next one re-litigates a decision it was never told about — a prior agent
reported "no thumbnail field in the FORMAT 3 header, so previews need a format
decision," which read the wrong layer: the design never wanted a header field. This
file puts the decision and the honest current state where a session picks them up.

**Status: DESIGNED AND RATIFIED, NOT BUILT.** No code path writes or reads a
preview plane today. `grep -ri preview main/art main/menu main/sd` is empty; the
theme list is text rows. The owner's report "I still don't see any previews" is
correct — they were never baked. This is an unbuilt feature, not a regression.

## 1. The decision (verbatim intent of design §8)

A theme's preview is **one reserved plane named `preview`, class `PLATE`, inside the
theme section's own art bank** — not a new region, not a header word, not a PNG, not
a decoder. Because it is a plane in the section's bank it rides `art_size` and
`sec_crc[]` for free: the install gate already CRCs the bank, and residency rules
apply to it unchanged. Adding a preview therefore costs zero format change.

- **Geometry**: 232x232, the half-res of the 466-square glass, upscaled 2x on draw
  with the existing scaler. RGB565 is 107,744 B; the bank's native RGBA8 form is
  133,824 B. Either is inside the bank budget a real theme already carries.
- **Optional.** A theme with no `preview` plane answers `ORB_ASSET_NO_CONTENT`
  (already defined, `main/assets/orb_assets.h`), and its row stays text-only. An
  absent preview is never an error.
- **Baked from the composer's existing pose**, downscaled at export: 10:09:36
  (`previewDate()`), moon 0.34 (`PREVIEW_MOON`), `watch-engine.js`. No pose
  metadata enters the file — the pose is a bake-time fact, not a runtime one.
- **Class `PLATE`** is the right vocabulary word (it is "the full-panel background",
  `main/assets/orb_assets.h`), so no class enum is appended — a class only enters by
  PR (D23), and this one already exists.

## 2. On glass (the render path, to be built)

Landing the cursor on a theme row queues **one** worker read of that extent's
`preview` plane (~108 KB, ~82 ms off the card). It is drawn full-frame over the
list until the cursor moves. It is never in the flush path and never on the LVGL
task — the card-read + scan blocking rule in `.clinerules` applies: a theme read on
the poll freezes the dial for as long as the card mutex is held elsewhere.

## 3. In the browser (to be built)

`GET /api/v1/sd/preview?id=<handle>` streams the raw plane; ~30 lines of JS unpack
RGB565 to a canvas, reused by the PWA `/themes` page, the hub's Theme card, and the
composer's install confirmation.

## 4. What it rides on, and what is NOT done

The preview belongs to the per-app theming build, which is itself only partly
landed. Verified against the tree as of 2026-10-25:

- One-section enforcement and app-scoped identity DID land (`5af35cf`, `2d27e35`,
  `0668370`): the filename carries the app slug, and FORMAT 2 acceptance is retired.
  `ORB_BUNDLE_SECT_MAX` is still `8u` in `main/art/orb_bundle.h` — the design's
  "drop it to 1" was not taken.
- The `assets`-partition **extent pool** (`orb_theme_pool.{c,h}`) is NOT built —
  residency is still one image at offset 0. `ls main/art/orb_theme_pool.*` is empty.
- The theme picker is **still** the clock's own `main/clock/clock_themes.c`; the
  design's generic `main/menu/orb_theme_menu.c` does not exist. Item 5 of the
  handoff (OS-level theme selector for every themeable app) is the same unbuilt work.

So building previews means building, in order: the baker writes a `preview` PLATE
plane into the section bank; `orb_theme.c` gains `orb_theme_preview_read(handle, buf)`
resolved through the asset registry by plane name; the picker queues the worker read
and draws it; the HTTP route and the ~30 JS lines. None of it needs a container format
change. It also does NOT need the `orb_theme_pool` extent pool: a preview plane is read
transiently out of the theme's own card file (`.orbtheme` on the SD card), the same way
`orb_bundle.c` already reads the live theme's geometry and banks — residency of a second
bank in the `assets` partition is not involved. (An earlier draft of this file claimed
the pool was a prerequisite; that was wrong. Corrected against `orb_theme.h` and the
`Open tourbillon ... plate bank: 8 keyframes` boot line on 2026-10-25.)

## 5. The one sentence to remember

Preview = reserved `preview` plane, class PLATE, in the section's bank, 232x232,
optional, worker-read. No new region, no header word, no PNG, no decoder. If a
session is told to "add theme previews," it is implementing this, not deciding it.
