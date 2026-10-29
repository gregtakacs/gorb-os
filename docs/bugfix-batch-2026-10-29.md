# Bug-fix batch 2026-10-29: bench-reported six

Six bugs the owner found on the bench after the first-flash provisioning run.
Each entry names the symptom, the root cause read off the code, and the fix.
The fixes land in the order below; one commit per bug.

## 1. Fresh board boots the Aviator, not the skeleton

**Symptom.** After first flash and hub provisioning, the clock shows the Aviator
dial. The agreed first face is the skeleton theme.

**Root cause.** `installflow.go` `installTheme` skips the seed when
`tl.Active != "" || len(tl.Themes) > 0`. A fresh board boots the factory bundle
baked into the `assets` partition slot, so `active` reports `"Aviator"` before
the card holds a single theme. The guard reads the slot as a card fact and skips
seeding. The boot log witnesses it: `orb_theme: slot holds bundle "Aviator"`.

**Fix.** The seed's question is "does the CARD carry themes", and
`card_count` is the firmware's own answer to that question. Gate on
`tl.CardCount > 0` instead of `active`. A factory slot is not an owner's choice,
so the skeleton seeds over it and the board reboots onto the skeleton.
The hub's `ThemeList` binding gains `CardCount`; the firmware already reports it.

## 2. Install lands disabled; a manual enable was needed

**Symptom.** After provisioning the clock shows as installed, but the owner had
to open the orb's menu to enable it. Owner ruling: installed means enabled;
there is no separate enable state.

**Root cause.** `orb_app.c` `card_fill` sets `.enabled = false` (the P4a
"install lands off" default), so a slot copy walks nothing until the word flips.
The hub's ladder already POSTs enable after install, but the device's own
install door and the boot ladder's fresh copy never did.

**Fix.** `card_fill` lands the row enabled. The `st` word the broker holds
stays readable and writable for recovery, but no surface treats it as a
toggle: the dial's door page loses its enable switch (bug 3's change carries
this), and `installflow.go` stops printing the `enabled` step for an install
that cannot land off. The uninstall door stays, because an installed app is
still removable.

## 3. App menu offers install and uninstall together

**Symptom.** One app row in the orb's APPS menu shows both install and
uninstall. They are mutually exclusive states.

**Root cause.** `menu_view.c` `appact_id` emits, for a registry row, either
`ACT_SHOW` or `ACT_INSTALL`, and then appends `ACT_UNINSTALL`
unconditionally. A row that is off or faulted therefore reads "install" and
"uninstall" at once.

**Fix.** The door list follows the state with no overlap:

- on the dial, enabled, not failed: `show on the dial` (or the showing line),
  `uninstall`, `remove from card`
- off: `enable` only — a turned-off app is one switch, not two removal doors
- failed: `install (retry)` plus the fault's verbatim reason
- card-only file: `install`, `remove from card`

This also retires the enable switch's old label wording per bug 2's ruling:
install implies enabled, so an installed-and-off row no longer exists as a
design state, and the row prints the enable word only for a row the ledger
holds off.

## 4. Carousel chip disappears under the second hand

**Symptom.** On the Aviator, rotating the knob while the second hand sweeps the
lower half of the glass erases the "MENU" chip, or shows it only in parts.

**Root cause.** `clock_layers_refresh` composites layer rows straight into the
mirror, and the second hand's rows include the chip's band (the chip sits 96 px
up from the bottom). The chip lives on LVGL's top layer, which the direct-push
painter does not know about, so a layer commit repaints the hand over the chip's
pixels. The carousel pauses the beat (`park_beat`), but the Aviator's refresh
also lands through activation/bounce repaints, and any commit that follows the
chip's show re-covers its rows.

**Fix.** The chip is the glass's topmost chrome, so the painter must respect its
box while it is shown. `gfx` gains a keep-out rect: while the chip is up,
`gfx_paint_row_ctx`/layer compositing skips rows inside the rect. The carousel
sets the rect when it shows the chip and clears it on hide. The chip already
lives on `lv_layer_top()` for the LVGL path; the keep-out closes the direct-push
path, which is the one the dial uses.

## 5. Apps and Themes tabs stopped listing the hub shelf

**Symptom.** The hub's Apps tab lists no shelf apps. Before it listed
color_picker and Clock "in the hub library".

**Root cause.** Not the shelf and not the resolver — `GET /api/library` answers
both apps from the live hub. `tabs.js` `renderAppsTab` merges `libCache.apps`
into its rows, but `libCache` is only populated by `renderLibrary()`, which
`loadApps` calls AFTER the rows are built. On the first read of the tab the
cache is null, so no library row ever merges, and the face shows nothing from
the shelf. The Themes face looks correct only because it fetches
`/api/library` itself (app.js `loadThemes`), and it then draws the shelf rows
without previews — bug 6's second half.

**Fix.** In `loadApps`, fetch the library into `libCache` before building rows
(reuse `renderLibrary`'s fetch; call it before `renderAppsTab`). The shelf rows
## 6. No previews on the Themes tab

**Symptom.** Neither the Aviator (the dial's active face) nor the skeleton shelf
theme draws a thumbnail.

**Root cause.** Two causes, one per row kind.

- **Device rows** (a theme on the card): the preview is a baked `PREVIEW`
  section the hub relays byte-for-byte from the orb (docs/theme-preview-plan.md
  §4). The Aviator is the firmware's baked-in factory bundle. Its store row
  carries no plane, so the device answers 404 "this theme carries no preview",
  and the hub cannot invent a plane for art it does not hold.
- **Shelf rows** (a theme in the hub's library, not on the card):
  `themeShelfRows` builds no canvas and never queues a preview at all. The
  plane is already in the hub's hands — it read the container to list the row —
  and throws it away.

**Fix.** The hub renders what it holds, and stops claiming a preview it does
not have. The full fix has two halves and is feature-sized, not a patch:

- **Bake**: the composer's exporter already writes a class-9 `PREVIEW` row
  (`orbtheme-baker.js:3201`, appended at class 9). The shipped `skeleton.orbtheme`
  carries no plane because the bake that produced it did not ask for one. Re-bake
  the shelf default with `preview: { plane }` from a rendered 232x232 frame, and
  wire that bake into the hub's build the way `make hub` wires the version
  package — the same "no manual step" rule `fw_embed_test.go` enforces.
- **Serve**: shelf rows need a preview route the way device rows have
  `/api/v1/sd/preview`. The bytes live in the container's own art bank, at
  class 9 slot 0, found through the elems table — `orb_theme_preview_read()`
  (orb_theme.c:1094) is the reference, and the hub's own
  `themecontainer.go` walker is half the distance. The endpoint answers the
  same `[u16 w][u16 h][RGB565]` shape so `putPlane` is unchanged.
- **Aviator**: the factory slot is a FORMAT 3 image with no class-9 row and no
  elems table, and no route can change that. The Themes face's row for a theme
  whose plane 404s already draws the design's quiet answer; keep it.

This half stays open. The other five are fixed.

## Out of scope here

The re-stamp loop (a firmware-bundle commit invalidates `serflash.sh --check`
until the next re-stamp) stays as documented in
`tools/orb-manager/BUNDLE-STAMPING.md`. The bundle is re-stamped once at the end
of this batch, not per-commit.

then merge on the first paint, and the D38 mutually-exclusive verbs apply to
them as designed.
