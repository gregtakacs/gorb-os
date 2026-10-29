# Bug-fix batch 2026-10-29: bench-reported six, plus the forget and the flash lock

Six bugs the owner found on the bench after the first-flash provisioning run.
Each entry names the symptom, the root cause read off the code, and the fix.
The fixes land in the order below; one commit per bug. Bug 7 is a regression the
first six left behind, reported on the same bench session. Bug 8 surfaced during
the v1.1.30 bench pass this batch set up, and it is a hub change, not a firmware
one.

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

**Root cause.** Both places that build a registry row memset it and never set the enable bit:
`orb_app_install.c:755` inside `install_ladder` (the install door) and `orb_app_install.c:969`
inside `orb_app_install_resolve_boot` (the card-sourced boot row, the file's own comment calls
it `card_fill`). The row's default was therefore `false`. `apply_enable` is the rung that could
have carried the bit and it does honour an absent word as ON, but it starts with
`if (!orb_app_card_count()) return;` — a slot whose card copy is gone, or a card the scan
refuses, leaves the whole rung skipped and the row standing disabled. The hub's ladder POSTs
enable after an install, which is the only reason the bench ever saw a clock come on.

**Fix.** Both fill sites now set `row.enabled = true`, so an installed row is an enabled row
wherever it came from and whatever the card is doing this boot. The `st` word the broker holds
stays readable and writable for recovery, but no surface treats it as a toggle: the dial's door
page loses its enable switch (bug 3's change carries this). The uninstall door stays, because an
installed app is still removable.

**Second correction, same day, on this section's own claim.** The first version of this section
named `orb_app.c/card_fill` and said the fix was one line there. It was not: `fae7c23` changed
`apply_enable`'s arms and the compiled shell row's `.enabled`, and wrote a comment claiming
`card_fill` set the bit. Nothing set it. The bench re-report of 2026-10-29 (fresh board, clock
reported installed, dial showing no face, door offering uninstall) is the direct consequence, and
it is the second fix in this document that was described and never written. The pin is
`TestBootRowLandsEnabled` in `library_test.go`, which reads both function bodies out of the
firmware file and goes red on the mutation — verified red, not assumed.

**Correction on the hub step, same day.** This section claimed `installflow.go`
stopped printing the `enabled` step. It does not, and it should not: the ladder
POSTs enable after a fresh upload (`installflow.go:388`) and after a skip whose row
the ledger holds off (`installflow.go:309`). The second is the only way a board
running pre-fix firmware gets its stale off word cleared, and the first still buys
C2's fault clear. The step is honest for exactly as long as one board on the bench
can still land a row disabled.

## 3. App menu offers install and uninstall together

**Symptom.** One app row in the orb's APPS menu shows both install and
uninstall. They are mutually exclusive states.

**Root cause.** `menu_view.c` `appact_id` emits, for a registry row, either
`ACT_SHOW` or `ACT_INSTALL`, and then appends `ACT_UNINSTALL`
unconditionally. A row that is off or faulted therefore reads "install" and
"uninstall" at once.

**Correction, same day.** The section above described a fix that was never
written, and the "off: `enable` only" bullet below it is wrong: it contradicts
both `install means enabled` and `host_menu_test.py`, which already specified
`install`, `uninstall`, `remove from card` for an off row. The page kept drawing
`enable` with no uninstall. The owner's bench pass caught it on a freshly flashed
board, and `tools/host_menu_test.py` reported 13 failures against the file as
committed.

**Fix, now actually in the file.** `appact_id` emits the row list the test
specifies, and `label_appact` names the off door `install`:

- on the dial, enabled, not failed: `show on the dial`, `uninstall`, `remove from card`
- off: `install`, `uninstall`, `remove from card`
- failed: `retry`, `uninstall`, `remove from card`
- card-only file: `install`, `remove from card`

Row 0 is the only row that moves with the state, so install and uninstall cannot
appear as two answers to one question, which was the reported symptom. The card
door stays unconditional on purpose: the walk may not have landed when the page
paints, and the worker answers "the card held no copy" instead of the page
guessing. `host_menu_test.py` passes 235 checks on this change.

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

All six of these are fixed. No test "stays red on purpose" — an earlier draft of this
section named `TestTheAviatorStillStandsForNoPreview`, and no such test exists anywhere in
the tree; a suite the Makefile gates on cannot carry one. The two real pins are
`TestShelfPreviewQuietOnNoPlane` (a planeless bake answers no plane rather than a false
square) and `TestThemePreviewQuietFortyFourRelaysQuietly` (the relay passes the device's 404
through quietly). What changed is that the shelf's own theme — the file a wiped board
seeds from — now pictures itself without install.

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

## 8. A flash refused with "another flash or provision holds its lock"

**Symptom.** During the bench pass on the new device, the first-flash write and the
Wi-Fi send both refused on `/dev/ttyACM2` with *"port is busy - another flash or
provision holds its lock"*. Nothing was on the wire. The write landed on the retry:
the board answers `v1.1.30+b190c32` at 12:41:04.

**Root cause.** Two hub processes were live on the bench. `:3779` ran the rebuild at
`b6dadba`, and a dev hub on `:3791` still ran `e7c525a`. Each runs its own background
sweep, and each sweep takes the port's `flock` for the whole round. The registry
eviction that `3e33f0a` added yesterday works inside one process only. A press in one
hub cannot cancel a sweep in the other, so it took the plain refusal and blamed the
operator for a round it never started. The board was a blank chip, so neither hub ever
earned the `IsOrb` identity that makes `proveCable` stand down (`cableproof.go:382`),
and both kept asking. The refusal appears five times in the `:3791` log, at 11:08:20,
12:03:31, 12:06:11, 12:37:44 and 12:38:49, and never once in the `:3779` log. The
losing hub was always the one carrying the operator's press. The first refusal lands
32 s after `:3791` launches, which is before any rebuild, so the pair caused it.

**Fix.** `internal/orbbind/press.go` adds two advisory flags per canonical port and
one lock order: press, then sweep, then the flock.

- `<port>.press` is held for one whole operator write. A sweep checks it before it
  touches the wire and declines, in any process, so no background round starts on a
  cable a human is using.
- `<port>.sweep` is the sweep lease. One hub at a time runs background work on a
  cable, and the loser skips the round. Its ladder re-arms later.
- `PressPort` covers flash, lonely write, provision, command and console read, plus
  `serprov`. It takes the flag, then waits up to `pressWindow` (12 s) for the one
  round already in flight.
- `SweepPort` covers the cable proof, the launch and cadence probes, and `orbprobe`.
  It declines without touching the wire, which is how every sweep already behaved.
- Presses still refuse presses at once. `LockPort` is unchanged, so the theme store
  pass and any older hub keep the semantics they shipped with.

Residual, and on purpose: a round already past its flag check keeps the flock until
its own budget ends. That is 8 s for a proof and up to 40 s for a probe, so a press
can still wait the window and then read the busy line. One hub per box stays the bench
rule. This only makes two of them lose politely.

**Evidence.** `internal/orbbind/press_test.go` pins the lease to one hub, pins the
press flag stopping every sweep, pins a press waiting out a live round and then
winning it, pins press-vs-press refusing inside a second, and pins an abandoned press
dropping its flag so the cable is not muted forever. `go test ./internal/orbbind/` is
green with the four pre-existing `LockPort` tests untouched.

## 9. The hub reported the clock installed with nothing on the card

**Symptom.** After a clean flash and the Wi-Fi setup, the hub's Library card says
the clock is installed. The dial's APPS page says `clock off`, its door page offers
`remove from card`, and `/apps` on the card is empty.

**Root cause.** `installflow.go` `installOneApp` asked one question and treated the
answer as the whole promise: it reads the dial's app ledger, finds a row for the
slug, prints `skipped / already on the dial`, posts the enable word, and returns
success. The installed tier is a list of `apps` partition headers, written by
`orb_app_install_resolve_boot`. For the native apps that exist today that header
carries no `code_size` to point at — the bench's known-good orb-B reports
`geom_crc:0, code_crc:0, code_size:0` for its healthy clock — so a header says a row
exists and says nothing about the bytes. The card tier is where a card app's code
lives, and the ledger reports it separately in `apps`. A row over an empty `/apps`
is a row that enables, never reaches the glass, and answers `installed` to a hub
that never looked at the second tier.

**Fix.** `installOneApp` believes an installed row only when the card tier backs it
(`needsCardBytes` and `cardHolds` in `installflow.go`). A hollow row falls through to
the upload and streams `installing` with the reason in the note, so a board in this
state repairs on the next press instead of being reported as satisfied. A row the
card does back still skips: no join re-sends 663 KB over the room.
`TestHollowRowSendsBytesInsteadOfSkipping` pins both halves and fails against the
code as it was.

The same run left no trace in the hub's log. The ladder's refusal went to the browser
as one streamed step and nowhere else, and a page that closed with the board rebooting
took the reason with it, so the log showed only the probe exemption saying
"installing". `h_libraryInstall` now logs a refused ladder with its reason.

## 10. A join-time install never seeded a theme, so the clock had no face

**Symptom.** The clock installs, enables, and still does not appear on the dial. Bug 1's
fix did not prevent it on the freshly flashed board.

**Root cause.** The clock is a themed face: with no theme stored, the shell keeps the
glass and the clock row never goes on it. `installRun` gated the theme step on
`only == ""` — the full-library run — and `web/app.js:1552` drives the join-time install
with `only=clock`. The one path that runs on a fresh board was the one path that could
not reach the step. Bug 1's `CardCount` gate is correct and was shipped; it sits inside a
step the join path never enters.

**Fix.** The theme step runs when the clock is live on the dial and the run is the full
library or the clock specifically, which keeps D36's rule that a lone `color_picker`
press seeds no face. `installTheme` still skips whenever the card already carries themes,
so the change cannot tread on an owner's shelf. `TestJoinRunSeedsTheTheme` fails against
the old gate and passes on this one.

**Bench witness.** On the board that reported this, seeding the skeleton face moved the
clock row from `on_glass:false` to `on_glass:true` with the same row, the same card, and
no other change. `on_glass` is the dial's own answer to "is this the face it is showing",
and the hub's `AppInstalledRow` had no field for it, so no caller could ask the question
the owner was asking. It does now.

## 11. A "clean flash" leaves the app registry and the enable words standing

**Symptom.** The board behaves as though it were wiped and yet arrives holding an
installed clock row, an off enable word, and an empty card.

**Root cause.** The bundle's own `flasher_args.json` writes four regions: `0x0`
bootloader, `0x8000` partition table, `0x11000` otadata, `0x20000` app. The
installed ledger lives in `apps` at `0xD80000` and the enable words in `storage`
NVS at `0xD60000` (`partitions.csv`), and a flash touches neither. The retention
invariant at the head of that file is deliberate — it is what lets credentials
survive an upgrade — and it applies to a reflash exactly as it applies to an OTA.
So `apps` keeps the row and `storage` keeps the stored off word the pre-bug-2
firmware wrote, while the card the owner formatted holds nothing.

**Fix.** None in the flash path: erasing `storage` on every reflash would take the
owner's Wi-Fi credentials with it, which the invariant exists to prevent, and it
would turn a reflash into a factory reset. The honest change is the one bugs 9 and
10 make, because those two together repair the surviving row on the next install
press instead of trusting it, and the hub now says so in its log. A board that must
arrive genuinely empty is provisioned through the dial's own `RESET ALL`, which
erases `storage` and `nvs` on purpose, or `idf.py erase-flash`.

## 12. A first flash adopts a board; an upgrade keeps its history

**Ruling (owner, bench, 2026-10-29).** "A new flash of a firmware should absolutely
retain the old registry and use it when it's a flash upgrade. When it's a new add of a
new orb and a first flash essentially, that is when we need to wipe it all and install
the initial clock only app and the skeleton theme to both flash and the sd card."

**Why the hub has to remember this rather than read it.** Bug 11 is the reason no
device answer exists: a reflash writes four regions and leaves `apps` (the installed
rows) and `storage` NVS (the enable words) standing on purpose, so a leftover row and a
row the owner chose look identical from the far side. The only moment the two cases
separate is the flash itself, and the question must be asked before the flash files the
chip as proven: had this MAC already been an orb on this hub's books.

**Implementation.** `flash.go` asks that question through `fleetKnown` and
`storeProvesOrb` before `MarkRemembered`, and marks an adoption in `cleanslate.go`'s
set. The automatic join-time install spends the mark: `cleanSlate` uninstalls every
non-`compiled` row, then erases the card's `apps` and `themes` trees, and the ordinary
ladder follows and lands the clock plus the skeleton face. Uninstall precedes the card
erase for the reason bug 9 gives: a header over an empty card is the broken state, and a
wipe that took the files first would manufacture it. The shell row is never touched
because the dial cannot run without it. Only the automatic arm wipes, so a hand press
never erases a shelf. `dropThemeCache` runs after, so the next read is a dial.

**Limitation, stated.** The mark is in memory. A hub restarted between the flash and the
provisioning loses it and the board keeps whatever it carried; `installOneApp`'s
hollow-row check then repairs a leftover row that cannot run, and a row that runs is a
row the owner can uninstall by hand. A wipe gated on state that survived a restart by an
accident of timing is the worse failure, so the failure chosen is the recoverable one.

**Tests.** `TestCleanSlateUninstallsBeforeItErasesCard` pins read-then-uninstall-then-erase
and the shell exemption; it fails when the two phases swap. `TestCleanSlateMarkIsOneShot`
pins that `pending` does not consume, `clear` does, and a nil set owes nothing.

## 13. A lonely-erased board kept the last orb life's whole shelf

**Symptom.** The owner lonely-erased a board, flashed it again, and it came back with clock
*and* color_picker on the card. The expected first-boot shelf is the clock alone, with the
picker staying on the hub shelf.

**Root cause.** Two facts meet. A lonely erase is a ROM chip erase, and a chip erase erases the
module's SPI flash — `nvs`, `storage`, `apps`, the works. It says nothing to the SD card, which
is a separate medium the ROM never sees, so `/apps/*.orbapp` and `/themes/*.orbtheme` from the
previous orb life stand exactly where they were. Second: the join-time seed gate lived in
`web/app.js`, ahead of the hub's install route, and it treated *any* card file as the owner's
own doing. On an adopted board those files are leftovers, not a shelf. The gate returned, the
route never ran, and bug 12's clean slate — which would have wiped both card trees — never got
its turn. Bug 12's mark was also never armed on this path: `handleLonely` wipes NVS regions only
and files no slate, so the following orb flash asked `storeProvesOrb`, was told the chip had
been an orb, and chose upgrade semantics.

**Fix.** Three parts.

- `endAsLonely` (`lonely.go:212`) now marks the clean slate on the chip it just blanked. A
  lonely erase means "this board's history is over," so the next install wipes the card and
  reseeds, whether or not the chip's row was already re-filed in between.
- The shelf gate moved out of the page and into the route (`installflow.go`,
  `shelfHoldsOwnersContent`), where it sits *behind* the slate check instead of in front of the
  whole run. An owner's curated shelf still stops a refill — the route streams `skipped` and the
  panel says so — and an adoption outranks it.
- `web/app.js` `autoInstall` no longer reads the ledger itself. One rule, one place.

**Test.** `TestAdoptionOutranksTheOwnersShelfGate` (`library_test.go`) fails if the page grows a
ledger gate back, or if the hub's shelf check moves ahead of the slate check.

## 14. "Open this orb" after provisioning landed nowhere

**Symptom.** At the end of provisioning the operator pressed *Open this orb* and did not land on
the orb's management page.

**Root cause.** `#ob-manage` dialed `orbIP`, the address the join dialog happened to be holding.
That address is a DHCP lease, and the join-time ladder reboots the board at least once on the way
through (the theme step ends with `action: "device"`, and D32 documents the reboot). A renew
during that window moves the lease, the button dials a host that stopped answering, and the
management view opens onto a card that never fills — indistinguishable from the button doing
nothing. The MAC does not move.

**Fix.** The press now asks the hub where that chip is *now* (`/api/fleet`, matched on MAC) and
prefers that answer, falling back to the held address when the hub has not re-announced the board
yet. `orbIP` is updated to the resolved address so every later dial in the view agrees.

**Not verified on hardware.** The board was off the LAN while this was written. The mechanism is
read out of the code; the fix needs the owner's next provisioning pass to confirm.

## 15. Provisioning reported a clean seed while the card held nothing

The owner's third report, same board, after a fresh adoption: the Apps face said the clock was
installed (Uninstall on the button), the SD card held no app file and no `skeleton` theme, the
carousel drew nothing, and *Open this orb* from the provisioning window still landed nowhere. The
owner also ruled that the provisioning window must stay modal until the whole baseline is on.

The prior session's summary claimed symptom 2 was fixed by `shelfHoldsOwnersContent` and the
clean-slate mark. That part is in the code and is correct. It did not explain this board, because
a wipe-and-seed that **ran and then lied** produces exactly this: gate passes, card wiped, ladder
installs, `done` printed — and a card that came back empty because the install never actually
landed, or landed and the row is off the glass.

The root cause is that `h_libraryInstall` reports `done` off `installRun` returning nil, and
`installRun` reports each app `installed` the moment the ledger shows a row, and a row is a promise
rather than the bytes behind it (`installOneApp`'s own skip gate says this in prose, and #9 is the
same failure one rung earlier). The theme step is gated on `installClockLive`, which reads the row,
not the glass. So a board whose row exists but whose card copy vanished, or whose clock row never
got the enable word, sailed through to `done` with a faceless dial. This is #9 and #2 recombined at
the far end of the ladder instead of the near end.

The fix adds a post-write verify the hub had never done. `seedverify.go:verifySeed` re-reads the
device's two authoritative answers after the whole run, post-reboot — the ledger's enable/failed
bits and card-copy presence, and the card's own theme count — and `h_libraryInstall` refuses `done`
on the automatic arm unless that verify is clean. A refusal leaves the slate mark armed, so the
next announce retries the seed instead of leaving the board half-written and marked finished. Four
tests (`seedverify_test.go`) pin the faceless-row, disabled-row, empty-theme refusals and the green
board. `TestBootRowLandsEnabled` still pins the firmware enable bit from #2.

*Open this orb* moved off the raw fleet scan and onto a new `GET /api/orb/where?mac=` that resolves
a chip to its live address by MAC through the fleet store — the same answer, reached by the one
field that does not move when the theme reboot changes the lease. The provisioning window is now
modal while the seed is in flight: `seedBusy` blocks every exit (Cancel, backdrop, Escape, and the
done-panel buttons), and the verdict words the headline honestly — "installed, enabled, and on the
glass" only after verify, otherwise the refusal sentence.

**Still not verified on hardware.** Bronze was unreachable (`.32` did not answer, hub state empty)
while this was written, the same limitation as #14. The verify gate is the mechanism that turns a
silent false-green into a named refusal, so the owner's next pass will either go green or print the
exact fact the dial reported. The board must be flashed and provisioned **through the hub** for the
verify arm to run.

**Update, same day:** verified on Bronze over hardware — and the pass exposed that the gate above had
a hole of its own: the automatic arm's owner-shelf skip reached `done` without ever calling
`verifySeedRun`. That is #16.

## 16. The owner-shelf skip lied, and the theme door was the corpse under it

**Symptom (owner, bench, 2026-10-29 PM, Bronze).** After a join the done panel read
"clock installed, enabled, and on the glass", the glass showed only MENU and would not walk to a
clock, and the card's `apps/` and `themes/` folders were empty. "Open this orb" still did not land.

**Root cause, first layer — the skip that never verified.** `h_libraryInstall`'s automatic arm asks
`shelfHoldsOwnersContent` before the ladder and skips to `skipped`+`done` when it answers true. Two
defects compounded. The gate counted ANY non-compiled ledger row as the owner's shelf — Bronze's
`clock` row was hollow (disabled, zero card bytes, a leftover of an earlier broken life), so it
counted — and the skip path returned BEFORE the verify arm, so the run reported `done` with no read
behind it and the page called it verified. The fix: a hollow, disabled row no longer counts as
curatorship (`shelfHoldsOwnersContent` now requires the enable bit or a card copy), and a skip that
does stand must still pass `verifySeedRun` to earn its `done` — a shelf that cannot verify is
reseeded through the ladder, not skipped. The verify arm also dropped its slate-mark precondition:
on the automatic arm, `done` now always follows `verified`.

**Root cause, second layer — why the card stayed empty.** The join-time theme upload died at
`POST /api/v1/sd/theme` with 503 `"no card in the socket"` while `/api/v1/sd` reported
`mounted:true` — deterministically, on every join run since the seed returned. `orb_theme_upload_begin`
resolves its destination through `orb_files_src("clock","themes")`, the CLOCK app's registry entry,
and that registry is only built when the dial is built at boot. A clock installed-and-enabled inside
a live boot has no entry, `upload_begin` returns `ESP_ERR_NOT_FOUND`, and the door maps BOTH failures
to the same "no card" sentence. Bench proof: the same 2.7 MB upload the door had refused twice
landed and stored within seconds of a reboot of the same board, with no other change. The hub fix
is version-agnostic: on that exact 503, `installTheme` re-reads the mount; if the card swears it is
mounted it reboots the dial, waits for it back, and hands it the upload once more — the store reboots
the board anyway, so the extra boot is invisible to the owner. A genuinely missing card keeps the
honest error untouched. The firmware-side message split is owed by the next image: the door must
name a NULL registry as what it is, not as a missing card.

**Verified over hardware.** Full broken state restored first (clock uninstalled, both card trees
erased, `card_count=0`), then one auto-arm run through the rebuilt hub (`v1.1.16-91`, commit below):
`clock: uploading… installed: enabled` → theme crosses 2668 KB → `theme: stored` → store reboot →
`verify: verified — "clock on the glass, theme on the card"` → `install: done`. The device answers
afterwards: `sd.card_count=1`, `sd.active=Skeleton`, card tier
`clock.orbapp file_size=663673 ok=true`, rows `clock enabled=true on_glass=true`. `/api/orb/where`
replies `{ok:true, ip:"192.168.2.32", mac:"44bd8d608088"}`. Covered by `TestShelfGateRefusesToCountTheHollowRow`,
`TestShelfGateHonoursARealShelf` and `TestThemeDoorRetryStaysArmedAndConditional` in `seedverify_test.go`. The last of those is now `TestThemeDoorCarriesNoRebootRetry` — see the v1.1.33 update below.

## 17. Every post-reboot wait died on the address, not the chip

The owner's bench run after fix 16's binary landed told the rest of the truth: the
ladder copied the files, the store armed, and then the page filled with
`orb unreachable: ... i/o timeout` — at the ledger re-read, at the erase, and at the
join seed's first status probe, after which "the clock and the theme never gets
sent." "Open this orb" returned to the fleet list instead of the board.

**The cause is one shape repeated five times.** The store's own reboot (and 16's
theme-door reboot) cuts this firmware's WiFi for longer than any single 8 s dial,
and the lease can move while the board is gone — the DHCP server owes the MAC a
number, not the hub a memory. Every hub-side wait dialled only the address it
started with: `installWaitBack` polled the old lease for 75 s and gave up;
`waitForStore` re-armed its excuse at a dead address; the ladder's first status
probe answered once and refused; `cleanSlate` read the ledger and erased each tree
with one 8 s dial. The hub already had the answer in `liveIPforMAC` — the fleet
tracks the chip and its check-ins update the row's address — and none of the waits
consulted it. The UI inherited the same brittleness twice over: the page's wake
retry re-dialled one frozen address 30 times, and `route()` refused `#orb` without
a MAC, so a serial join that never carried one bounced the operator off the board
page into the fleet list.

**The fix is one patient helper used everywhere, and patience at every site.**
`waitOrbBack(ctx, ip, budget)` latches the chip's MAC, re-asks the fleet where it
lives on every miss, arms the self-reboot excuse per probe, and returns the address
it actually got an answer at — every caller dials THAT for the rest of the run.
`waitOrbCycle` wraps it for the waits that sit directly behind a reboot the ladder
caused: the firmware answers the reboot door while it is dying, so the cycle demands
the SILENCE first and only believes the answer that follows it — the bench caught the
first fix retrying the theme door against a mid-shutdown board and eating a second
"no card" exactly this way. `installRebootWait` grew 75 s → 3 min. The ladder's first
status probe now streams "waiting through its boot" instead of refusing, and the
ledger read behind it gets the same cycle-shaped wait — a status line can be the
corpse answer while the ledger dial dies. `cleanSlate` waits once and re-hands each
uninstall and each tree erase after a transport failure. `waitForStore` follows the
lease mid-wait. The join-time verify gate waits through the ladder's last reboot and
re-locates before it dials. On the page: `loadOrb` adopts the MAC off
the board's own status reply and
its wake retry re-locates through `/api/orb/where` (which now answers `?ip=` via
the fleet's reverse lookup — live rows only, so a stale address refuses); `route()`
opens the board face on MAC **or** address; `joinDone` takes the MAC off the watch
reply so the Open-orb press always carries the movable handle; the Apps face gives
the ledger re-read one more try at the tail of the boot; the card-file delete answers
a mid-erase timeout with a re-listing instead of a flat failure.

**Bench proof (Bronze @ .32, 2026-10-29 PM).** The failing flow — wipe the card,
reboot the board, fire the join seed into the silence — ran end to end:
`install: waiting` through the boot, `clock: installed/enabled`, `theme: uploaded →
storing → stored`, `verify: verified — clock on the glass, theme on the card`,
`install: done`. The device then answered `count 1`, app row `clock ok:true`, row
`clock enabled`. And `/api/orb/where?ip=192.168.2.32` answered
`{ok:true, ip:"192.168.2.32", mac:"44bd8d608088"}` live, the door the bounced
button needed.

**Honesty is preserved at both edges:** a board that never answers still refuses at
the budget with the caller's address and the transport error, the wait ends at once
on a departed browser, and `where?ip=` refuses an address no live row claims rather
than pointing at the wrong board.

Covered by `TestWaitOrbBackFollowsTheMovedLease`, `TestWaitOrbBackRefusesOnBudgetAnd
HonorsCtx`, `TestWaitOrbCycleDemandsTheSilenceFirst`, `TestWhereAnswersByAddress` and
the text gates in `TestTheLadderIsPatientAcrossItsOwnReboots`.


## 18. The provisioning window froze on one sentence with every button dead

**The report (owner, 2026-10-29 PM, right after 17 shipped).** After Wi-Fi
credentials land, the dialog reads "installing the clock from the hub library…"
under a headline still saying *Bring it onto Wi-Fi*, and nothing changes for
minutes. When the seed finally finishes, the window is still stuck: every button
blocked.

**The cause is three defects wearing one coat.** (1) The join-time run streams its
steps to `#lib-install-log` — a `hidden` `<pre>` on the Library view. The dialog
that owns the screen during provisioning never saw a single step. (2) The hub,
newly patient per 17, now spends minutes inside `waitOrbBack`/`waitForStore`
emitting NOTHING; patience without narration reads exactly like a hang. (3) The
done panel's seal — `seedGateButtons(true)`, correct by 15's ruling — releases only
when the awaited stream returns. A stream that stalls leaves the buttons dead
forever: the page had no bound on the wait, so "the hub stopped talking" and "the
hub is working" were the same screen.

**The fix makes every silent wait speak, and every spoken step visible.**
Hub side: `waitOrbBack`, `waitOrbCycle` and `waitForStore` take an optional
`waitBeat`, fired every ~20 s of missing (`waitBeatEvery`) with the elapsed, the
budget, and the last probe's error; `beatFor` words it as an ordinary `waiting`
step, so a long wait rides the stream as "still waiting for the orb: 40s of 180s
(last try: …)" instead of silence — the head probe, the ledger re-hand, the clean
slate's every uninstall and erase, both theme-door waits and the store wait all
beat. Page side: `streamSteps` gained an `onStep` relay; the join flow passes one
that writes each step onto the watch line and the visible console trail the
dialog already opens, so the operator sees `clock: uploading — 224 of 648 KB`,
`theme: storing`, `verify: verified — clock on the glass` as they happen. The
headline stops lying about the phase: joining a network is over at that point, so
the title becomes "Seeding its baseline" and `releaseSeed` re-titles the panel to
"Provisioned" or "Seeding did not finish". The seal is now bounded: the watchdog
releases the buttons with an honest sentence after **4 minutes of total silence**
(a live hub beats every ~20 s, so four blank minutes is a dead stream) — and it
releases without dropping the fetch, because the file's own law allows exactly two
client-side aborts and a write in flight is never one of them; a step that
arrives after a release lands on the watch line as the late verdict it is, and
the done verdict the run eventually reports still rewrites the record.

**Bench proof (Bronze @ .32, 2026-10-29 15:36).** Board rebooted, seed fired into
the silence within seconds: the stream opened with `install: waiting — the orb is
not answering yet — waiting through its boot before the run` at 12 s, the board
rejoined at its lease, and the run closed `clock: skipped — already on the dial`
(the card was seeded by the 17-bench pass), `verify: verified — clock enabled and
on the card`, `install: done` at 15:36:07 — 13 s, every beat visible, seal
released. The beat cadence itself is pinned by `TestThePatientWaitSpeaksWhileItWaits`:
a board that stays down 45 s earns ≥2 spoken beats carrying elapsed, budget and
error; a board that answers at once earns none.

Covered by `TestThePatientWaitSpeaksWhileItWaits` and the widened text gates in
`TestTheLadderIsPatientAcrossItsOwnReboots`; the abort-count law
(`TestUICanCancelAndQuitSafely`) still counts exactly two.

## Out of scope here

The re-stamp loop (a firmware-bundle commit invalidates `serflash.sh --check`
until the next re-stamp) stays as documented in
`tools/orb-manager/BUNDLE-STAMPING.md`. The bundle is re-stamped once at the end
of this batch, not per-commit.

## Status

### Second bench pass, same day (the owner's re-report)

The owner lonely-erased a board, flashed it again, and reported three symptoms. Two were real
defects and one was a measurement artifact of the bench setup.

**The measurement artifact came first and matters most.** The hub window the operator was driving
had been started at 13:32:33 from a binary that `make build` later replaced — `/proc/<pid>/exe`
read `(deleted)`. A running hub loads its firmware bundle at startup and never re-reads it, so
that window went on offering the v1.1.30 image it had embedded, and flashed the board with it.
The board correctly reported `v1.1.30+b190c32` afterward. Every fix committed at 13:19 (`aafd7ff`,
the door list) and 13:29 (`2678e84`, the clean slate) was therefore absent from the image the
board was running, and the re-report described code that was on the bench's disk and not on its
glass. A second hub process was also up, holding a cable sweep lease. Two hubs and one stale
binary produce exactly this class of confusing result. **Restart the hub before flashing, and
read the `firmware built-in (...)` line the launch prints.** `9ffdbfb` re-stamped the bundle to
v1.1.31, `cd5c190` to v1.1.32; a hub launched before those commits will keep offering an older
image no matter what the tree says.

**Two real defects did fall out of that pass, and both were mine.**

1. Bug 2's fix was never written. `orb_app_install.c` memsets its row at both fill sites and
   neither set the enable bit — the same failure shape as bug 3, caught the same way, by reading
   the file instead of trusting the commit message. A board booting from its card therefore
   restored the clock installed-and-off, and the dial showed no face. Fixed at both sites, pinned
   by `TestBootRowLandsEnabled`, and verified red by mutation before it was trusted.
2. Bug 12's clean slate could not run on the lonely path. The page's join-time gate read the SD
   card, saw the previous orb life's leftovers, and never POSTed. A chip erase does not touch the
   card, so every lonely-erased board had leftovers. The gate moved into the hub behind the slate
   check, and `endAsLonely` now arms the mark itself. See #13.

Symptom 3 — *Open this orb* landing nowhere — has a mechanism in the code and a fix (#14), and it
is explicitly **not verified**: the board was off the LAN the whole time the fix was written.

### Carried forward from the first pass

Bug 3's fix is in the file now and green in `host_menu_test.py`; it was absent from
the commit this document first described, so the board the owner flashed ran the old
menu. Bugs 9, 10 and 11 came out of that same bench pass. Bugs 9 and 10 are hub-only:
no reflash is needed, and both are covered by tests in the `orb-manager` suite
(`TestHollowRowSendsBytesInsteadOfSkipping`, `TestJoinRunSeedsTheTheme`). Bug 11 needs
no code. Bug 12 is the owner's ruling that came out of the same pass, and it is
implemented: a first flash of an unknown chip wipes the board and seeds the clock plus
the skeleton face, while an upgrade keeps the registry it finds.

Bugs 1, 2, 4, 5 and 6 are firmware or hub changes that still need a bench pass on a
rebuilt image. Bug 7 is hub-only and covered by `forgetghost_test.go`.

## Update (PM, second report: two reboots and a bounce that survived its own fix)

The owner ran the full join on the new hub and filed three symptoms. One is a
one-character bug. Two are honest behavior the page never said out loud until
the relay above shipped — the operator was right to be suspicious; the machine
was busy and mute.

**Three: "Open this orb" still lands on the fleet list.** The route fix (open
by MAC *or* IP) was correct and incomplete: the press never reached the route.
The handler read the name field as `$("ob-name")` — no hash. `querySelector`
treats a bare word as a *tag* name, finds no `<ob-name>` element, returns
`null`, and `.value` throws. The handler had already closed the dialog by that
line, so the throw killed the press after the mask hid and before
`openOrbByIP` ran: dialog gone, view still the fleet. Same symptom, different
organ. Fixed to `$("#ob-name")`. The regression gate
(`TestNoElementLookupsLoseTheirHash`) does not watch that one call site — it
scans every `$("…")`/`$$("…")` string literal in app.js and tabs.js and fails
on any selector starting with a bare letter, because the suite never presses a
button and an editor shows none of these.

**One: the orb reboots when Connect is pressed.** It must. The firmware arms
its provisioning reader and its boot scan exactly once, at app startup, and
only on an unprovisioned boot — so `ProvisionOn` takes the ROM's own reset
(`EnterDownloadMode` then `HardReset`) to force that fresh boot before it can
read `PROVSCAN` or accept a `PROV` line. The hub says so on the stream —
"opening the console (this reboots the orb into its app)" — and with the relay
above that line rides the dialog's visible watch row. The board rebooting the
instant Connect is pressed is the door opening, not a fault.

**Two: the orb reboots again before the theme lands.** This was the known
firmware debt, mitigated on purpose: the upload door builds its registry at
boot, so a clock freshly enabled mid-run is invisible to it, and the door
cries "no card in the socket" at a card sitting right there. The hub takes one
reboot for it and says exactly that on the step: "the upload door waits on a
boot the fresh enable never got — rebooting for it." No card is lost; one boot
is spent, and the patience above covers the silence it creates. The real fix
is firmware (`orb_net.c` distinguishing missing-card from
uninitialized-registry) and stays on the books as debt. — PAID the same
afternoon; see the debt-cleared section below.

**Tests:** new gate + the full suite green; `go vet`, `go build`, `make build`
clean.

## Update (firmware, same day): the join-time theme debt is paid — v1.1.33

Debt item "Two" above cost every fresh join a second reboot and a second
3.7 MB upload. The door's lie had one sentence to it: `orb_theme_upload_begin`
resolved its directory through `orb_files_src("clock","themes")`, a table only
`clock_view_init` fills, and answered NULL — "no card in the socket" — at a
mounted card, because the hub had installed and enabled the clock over the
wire on a boot that never ran the app's init.

**The fix.** The clock's themes declaration moves from `clock_view.c` into
`orb_theme.c` as a static const — the theme module owns the directory, so it
owns the values — and is published two ways. `orb_theme_boot()`, on the boot
ladder before any door task exists, calls the new `orb_theme_src_register()`,
so the ordinary path answers from the registry; `clock_view_init()` re-declares
through the same call, and `orb_files_register` takes the second call as an
app's init running twice and overwrites in place. Every theme door
(`upload_begin`, `upload_recommit`, `orb_theme_scan`) now reads through
`theme_src()`, which falls back to the shipped constant when the table holds
nothing — a read of a static const, never a table write, so the httpd task
cannot race the registry law. The recommit door's missing-source branch also
stops saying "no card in the socket": the mount check keeps that sentence,
because that one is true, and the source branch now names the source.

**The hub's reboot-retry is gone.** It shipped as the mitigation for the lie, and
the owner retired it the same afternoon as the firmware fix: no pre-1.1.33 board
stays in the fleet, so no orb on this path can still conflate a missing card with
an unarmed registry. `installTheme` now takes one upload and returns the door's
honest error on a real no-card failure — a join on a patched board reboots once,
for provisioning, and the theme lands on the first upload. The gate
`TestThemeDoorCarriesNoRebootRetry` (`seedverify_test.go`) fails the day a
`s.device.Reboot(` walks back into that step.

**Tests.** New gate `TestThemeDoorsAnswerWithoutTheClocksBoot`
(`firmware_contract_test.go`) pins the declaration's values, all three doors
going through `theme_src()`, the boot registration, the recommit branch's
vocabulary, and the single copy of the declaration in the tree — verified red
when the old door line is pasted back. `host_app_registry_test.py` carried a
stale D40 assertion (card rows now land *enabled*; commit 8748a72 shipped that
ruling and missed the test) and is updated to the new contract. The rest of
the host suite passes; `host_carousel_test.py`,
`host_orbapp_install_test.py` and `host_svc_test.py` fail identically on the
clean pre-fix tree — pre-existing, unrelated, and left on the books.
