# Bug-fix batch 2026-10-29: bench-reported six, plus the forget they exposed

Six bugs the owner found on the bench after the first-flash provisioning run.
Each entry names the symptom, the root cause read off the code, and the fix.
The fixes land in the order below; one commit per bug. Bug 7 is a regression the
first six left behind, reported on the same bench session.

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
not have. The fix has two halves and both are now done.

- **Bake: already shipped, verified by parsing the artifact.** This section earlier
  claimed the shelf default carried no plane. That read the container wrong: it looked
  for a `preview` slug string, and class 9 lives as a numeric element row, not a name.
  `skeleton.orbtheme` is a FORMAT 3 / abi-4 bake whose elem8 is `cls=9 slot=0` at
  catalogue frame0, one 232x232 opaque plane at the bank's running end — the default
  was re-exported through the real composer path on purpose (commit 7fab0bb). No
  re-export is needed.
- **Serve: the half that was missing, now built.** A shelf theme's plane already lives
  in the bytes the hub parsed to list the row, so no orb is dialed to show it.
  - `orbnet.ThemePreviewPlane` (new `internal/orbnet/shelfpreview.go`) is the Go twin of
    `orb_theme_preview_read` (orb_theme.c:1094): the `indexSlugs` walk finds the one
    section's blob, the element-table stride comes from the blob's own length
    (`blob_check`'s rule, not a constant), the class-9 slot-0 row's `frame0` indexes the
    sprite catalogue (`orb_bundle_spr_at`) — never `frame0 * stride`, per `orb_elems.h` —
    and the record's `art_off` is absolute inside the file (`orb_bundle.c:365` reads
    `file + art_off`), so the plane copies straight out of the art bank. It answers
    `(nil, nil)` for a container with no class-9 row and a real error only for one that
    will not walk. `TestShelfPreviewReadsTheShippedDefault` drives it against the seed
    file and pins the 232x232 extent in the device's `[u16 w][u16 h][RGB565]` shape.
  - The route `GET /api/theme/shelf-preview?name=<stem>` (theme.go
    `handleThemeShelfPreview`, wired in main.go beside the card preview) answers that
    same shape, or 404 `{"error":"this theme carries no preview"}` for a planeless bake.
    It reads through `Library.namedTheme`, so the gate is the library's one-segment rule.
    A disk read on the hub: no `ip`, no card lane, so it needs none of the relay's
    budgets, and it sits behind the same `requireControl` as its card twin.
  - The shelf row (`web/app.js` `themeShelfRows`) now builds the same `<canvas class=pv>`
    + `.pvq` chip a card row builds and calls the new `shelfPreviewPlane`, which unpacks
    the bytes through a shared `drawPlane` helper both paths use, so the RGB565 order has
    one home. Its failure chip and quiet-404 match the device row, so a bake with no
    plane keeps the design's hidden square rather than a false "?".
- **Aviator**: unchanged, and correctly so. The factory slot is a FORMAT 3 image with
  no class-9 row and no elems table, and no route can change that, so the hub keeps the
  device's honest 404 and the row's quiet answer.

All six of these are fixed. `TestTheAviatorStillStandsForNoPreview` stays red on purpose: the
hub cannot invent a plane for factory art it does not hold. What changed is that the
shelf's own theme — the file a wiped board seeds from — now pictures itself without
install.

## 7. Forget deleted the row and the card kept the orb

**Symptom.** After the first six fixes, an operator removed an orb with the plain
Forget on the card. The hub answered `ok`. The board came back on the next frame
named `orb-<mac>`, and pressing Forget again did nothing.

**Root cause.** `handleFleetForget` deletes the fleet row and revokes the address
grants, and stops there. The mDNS browser is a separate cache, and `Candidates()`
serves a board's last announcement until TTL plus grace ages out (120 s by
`MDNS_TTL`). The next `foldCandidates` read that ghost and called `fleet.Observe`,
which files a row out of nothing. So the deletion held for one tick and the fold
undid it. The resurrected row carried no name — the announcement's instance is
MAC-derived, and the name lived in the deleted row — which is why the card printed
`orb-<mac>` instead of the orb's word. The same fold refreshed the row's presence
clock, so `forgetGate` refused the second press as "announced itself Ns ago".

The lonely erase already survives this, and the commit that fixed it
(`013434a`) named the mechanism exactly. What it did not cover is the plain door:
an erase leaves a `lonelyMark` and `outranked` refuses a packet older than the
mark, but a plain forget has no mark to leave. Reusing the mark would have been a
wrong fact — it suppresses a cable descriptor's claim and paints the port picker
blank, which describes a written-over chip, not an un-listed one.

**Fix.** Delete the evidence, do not refuse to read it.

- `Browser.ForgetMAC(mac)` (internal/orbmdns/browse.go) drops one chip from all
  three places the browser can hold it: the promoted `byMAC` row, the half-parsed
  `pending` record, and the A record cached under the instance's host name. It
  takes the MAC in either spelling through `CanonicalMAC`, refuses an unparseable
  one rather than guessing, and reports whether anything went. It marks the
  revision and wakes the card, so the page drops the row at once.
- `discovery` gains the method (discover.go), and `Server.forgetGhost` (fleet.go)
  calls it from `handleFleetForget` after the row delete, and from `unMakeOrb`
  (wipe.go) beside the existing revoke loop, so every forget door sweeps its own
  cache on the way out.
- Over-reach guard: the purge removes a stale claim, not the board's standing. A
  chip that announces again after the forget is making a present-tense claim and
  re-files its own row, which is what `forgetGate`'s refusal sentence promises.

`forgetghost_test.go` pins the door, the purge, the re-announce limit and the
stranger-is-left-alone rule; `TestForgetMACTakesTheWholeChipAndWakes` pins the
browser half. Both the resurrection test and the door test were confirmed red with
the purge call removed, then green with it back.

## Out of scope here

The re-stamp loop (a firmware-bundle commit invalidates `serflash.sh --check`
until the next re-stamp) stays as documented in
`tools/orb-manager/BUNDLE-STAMPING.md`. The bundle is re-stamped once at the end
of this batch, not per-commit.

## Status

Bugs 1-6 are firmware or hub changes the owner has not re-flashed yet; each needs
a bench pass on a rebuilt image. Bug 7 is hub-only, needs no reflash, and is
covered by `forgetghost_test.go` in the `orb-manager` suite.
