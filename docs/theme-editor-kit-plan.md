# The theme editor kit: what every app's editor gets from the OS

Status: **RULED 2026-11-03 (owner, §0.1). K1–K10 and H1 built (§7). §8, the hosted editor, ruled
2026-11-08 and not built.** Written at `7cc1221` (gorb-os-2 `main`).
The owner asked for it while the flight tracker's theme is being drawn in the sibling
clone (`flight-tracker-design.md` §11). The clock is the first consumer, flights is the
second, and flights is the proof.

## 0. The ask, and the rulings it stands on

Owner, 2026-11-03: the clock's theme editor does a lot that other apps' editors need too:
saving, exporting and sending, previewing, and measuring frame rate. Those should be
helpers any app's editor can call, and maybe the layout and framing too. The aim is that
an app carries as little as possible, while an app that wants to do everything itself
still can.

### 0.1 Owner rulings on this plan, 2026-11-03

1. **The shell takes as much as it can.** Reuse is the point, so the layers panel and the
   field builders are part of `studio@1` from the start (K6), not held for a second editor.
2. **True-size calibration is per browser,** kept in that browser's local storage.
3. **The runtime ids are `theme@1` and `studio@1`.**
4. **The Themes page is per app.** Under Themes, each app has its own tab with its themes
   and a link to its editor. The engine behind the page is all hub code (H1). This is
   D49's page layer, reopened from §46.
5. **Flights ships a baked theme before it has an editor** (corrected by the owner,
   2026-11-04; this ruling first read "hard-coded in firmware", which was wrong). The
   tracker's first theme is a real baked `.orbtheme` that the stage reads like any other,
   but it is not made in a theme editor, because that waits on the editor being universal,
   which is this kit. So nothing in flights steps 1–2 waits on the kit, and the kit does
   not wait on flights. Its consequences: a non-clock section reaches a device before the
   flights editor exists (K5's builder is the OS's way to bake one, §7), and a second app's
   theme is resident beside the clock's from that first bake (D49, H1, §5).

Rulings already in force that this plan builds on:

- **The OS builds the theme** (composer-stack-plan ruling 7, §9.1). The format, its checks
  and its device mirrors are OS code in `baker@1`. An app authors content.
- **Apps are atomic** (flight-tracker-design §11.1 ruling 1). An app knows nothing of
  another app. Whatever two editors share comes from the OS or the SDK, never from one app
  reaching into another. So nothing here moves from `apps/clock/` into `apps/flights/`.
  Shared code moves into the runtime.
- **The tracker's editor is small** (§11.1 ruling 2): a fraction of the Watch Composer.
- **The runtime is versioned per id** (composer §9.1, §9.4). An editor's `editor.json`
  lists the runtime ids it uses, and the hub refuses an editor that lists one it lacks.
  New ids are added, never changed under an editor that already lists them.

## 1. What is shared today, and what is not

| Already OS (`/runtime/`) | Still inside the clock's 5,322-line document |
|---|---|
| `baker@1`: container and sections, row checks, RGB565 and dither, banks and deltas, `estimate3`, the panel blend mirror (`panelRGBA`), the design region, `writeBundle3` | the send flow, the session (autosave, undo, My designs, open and export), the stage and its views, motion measurement and the pace meter, the studio layout |
| `ui@1`: the hub's tokens and plain controls | |
| `support@1`, React, Babel, fonts | |

The clock-shaped bake `bakeClock3` also lives in `baker@1`, though §9.1 put an app's
vocabulary on the app's side. Flights cannot call a clock bake (§4, K5).

## 2. Three layers, each optional

1. **`theme@1`: headless services.** Plain JS with no React, so an editor written any way
   can call them. Four modules: `send`, `session`, `preview`, `measure` (§3).
2. **`studio@1`: the shell, as much of the editor as is not the app's content** (§0.1
   ruling 1). The three-pane frame, title bar, step bar, status line and send modal; the
   layers panel (rows, nesting, eyes, drag and arrows, with the app's own rules deciding
   what may move); the field builders (sliders, chips, colours, art slots, grouped
   sections) driven by a field list the app declares; the Render step (fit, estimate,
   send, downloads). The app fills the stage and declares its steps, fields and rows.
3. **An editor's own way.** ~~An editor that lists only `baker@1` (or nothing) in
   `editor.json` keeps full control, as it does today.~~ Retired by the owner 2026-11-08
   (§8.1 ruling 1): Orb Manager owns the editor page, and an app supplies its content to
   it. An app cannot replace the start step, the Render step, the stage or the readouts.

## 3. The services, as the clock does them today

What the app hands in is a few callbacks. What comes back is a finished behaviour.

**`send`** (from `postTheme3ToOrb`, `_push3`, `_watchSend3`, `_shipHeldReplace`,
`replaceArmed3`, `hubOrbIp`, `cardStemFor`, the duplicate modal state). Bytes in. Out:
finding the orb through the hub, the identity precheck (§32.7), the duplicate question
with Replace, the commit token for a held bake, upload progress, cancel, and waiting for
the orb to come back (c8919d4). The app supplies the bytes, the name, and a hook to show
the duplicate question in its own UI (the shell's modal by default).

**`session`** (from `SNAP_KEYS`, `trackHistory`, `saveSession`/`loadSession`, `mineList`/
`saveMine`, `designDoc`, `applyDesignDoc`, `migrateDesign`, `openFile`, `openThemeParam`,
the downloads). The app supplies its slug, design schema and migration steps (composer
ruling 12), `snapshot()` and `restore(s)`. Out: autosave to the browser, undo and redo with
burst merging, My designs, the design file's wrapper and its refusals (another app's
design, a newer app release), and opening from a file, the card, the shelf, or the hub's
Edit door (`?theme=`).

**`preview`** (from `drawMain`, `drawPanel`, the Compare wipe, the stage fit, zoom and ruler,
`startTimer`, the time modes). The app supplies `draw(ctx, t)` for a frame at a time `t`,
and the layers for Panel view. Out: the frame clock (play, slow, live, a fixed pose,
cycling dates for the clock or simulated traffic for flights), Design / Panel / Compare
through `GorbBake.panelRGBA`, fitting to the round 466 px glass, and **true size**:
`apps/flights/art/review.html` draws the face at its physical 1.75 in (about 266 ppi),
calibrated against a bank card's outline. That belongs in the OS, so every editor's
preview can show it. The calibration is per browser, in its local storage (§0.1 ruling 2).

**`measure`** (from `measureFrames3`, `measureCarriage3`, `remeasureSoon`, `_budget`,
`devFps`). Frames in. Out: which pixels move from frame to frame (the mask and bounds the
bank policy uses), the budget meter, and `estimate3` hooked up. The pace model is a hook:
the clock keeps `_devPace` (a seconds-hand sweep model, 141 lines), and an app without
one gets the generic estimate.

What stays in the clock: the watch engine, dial renderer and sound engine, the stack rules
(`stack.js`), the presets, its fields and steps, and the translation from clock parts into
rows (`t3bake`, `compPass3`, `auxPass3`, `wheelPass3`, and `bakeClock3` once it moves, K5).

## 4. Slices

Each slice moves code without changing what the clock does. The clock's headless suites
(about 1,500 checks that drive the real component) are the gate for that, along with a
send to Gold where the slice touches the glass. A suite moves with the code it tests.

| Slice | Content | Gate |
|---|---|---|
| **K1** | `theme@1/send`, and the clock calls it | `headless-send` moves to `web/runtime/_selftest/` and passes; clock suites green; a send, a duplicate Replace and a cancel benched on Gold — **built and benched on Gold 2026-11-03** (§7) |
| **K2** | `theme@1/session` | `headless-session` and the session half of `headless-design` move and pass; a design saved before the move opens after it — **built and benched on Gold 2026-11-03** (§7) |
| **K3** | `theme@1/preview`, including true size | the clock's stage, Panel and Compare equal before and after (`headless-panel`, a screenshot pair); true size matches `review.html`'s calibration — **built and benched on Gold 2026-11-03** (§7) |
| **K4** | `theme@1/measure`, the pace hook | `headless-pace` and `headless-estimate` green with `_devPace` as the clock's override; an app with no hook gets the estimate — **built 2026-11-04**, benched in real Chrome (§7) |
| **K5** | a generic stage bake in `baker@1` (a plate, rows, sprites, row pools, scalars) for any app the stage draws; `bakeClock3` becomes the clock's translation on top of it | the clock's bundles are byte-identical before and after (`headless-bundle3`). Flights' first theme is baked without an editor (§0.1 ruling 5), so this does not wait on flights; its format is checked against flights' when that first theme reads on a device — **built 2026-11-04** (§7) |
| **K6** | `studio@1`: frame, title and step bar, status, send modal, the Render step, slots | the clock's P5 checklist (nothing lost), no page scroll at 1280×720 and 1920×1080 inside the hub — **built and benched on Gold 2026-11-04** (§7) |
| **K7** | `studio@1`: the layers panel and the field builders, driven by what the app declares | the clock's layers and inspector behave as before (`headless-stack-ui`, `headless-forms`, `headless-ui`); the clock's own rules (`stack.js`) stay the clock's — **built and benched on Gold 2026-11-04** (§7) |
| **H1** | the Themes page per app: a sub-tab for each theme-aware app, with its themes and a link to its editor, all hub code (§0.1 ruling 4) | Go and UI suites green; on Gold, the clock's tab lists the card's themes and opens the editor; an app with no themes or no editor gets a tab that says so — **built 2026-11-04** (§7) |
| **K9** | the hub's disk is the editor's store, and the hub's library is its editor (owner, 2026-11-04): My designs are files on the hub, autosave is gone, every theme sent or imported is kept on the shelf by app and name, and the hub serves its own editor when it is the orb's release (corrected 2026-11-05: an orb behind the hub is edited with its own editor, which says the hub has a newer one), and says when a design it opens was made by an older editor | Go suites for the store, the shelf copy and the region choice; the session suites with the hub stubbed; on Gold, a design saved and reopened after a reload, a send that lands on the shelf and replaces itself under its name |
| **K10** | one library per app, one entry per name (owner, 2026-11-04): an entry is a draft, a theme, or a theme with unsent changes; the same list on the app's Themes tab and the editor's Start step; sending folds the draft into the theme; `.orbtheme` is the one file that leaves the hub; "made with" and "needs a rebake" labels, and Rebake all | Go suites for the entries, the fold, the stale-format read and the routes; the session suites with the hub stubbed; on Gold, a draft, a send that folds it, the Themes tab's two sections, an Edit from each |
| **K8** | the flights editor on the kit (flights step 3, in its own clone) | no reference to `apps/clock/`; a line count recorded here against the Watch Composer's |

K1–K4 come first: they are pure moves, they suit both editors as they are, and they can
land while flights is on its art and firmware. K6 and K7 follow; H1 is hub-only and can go
in parallel with any of them.

## 5. Coordination with the flights work

- **Two clones, two lanes.** This work touches `apps/clock/editor/`, `web/runtime/`, the
  hub, and `baker@1`. Flights touches `apps/flights/` and `flight-tracker-design.md`. Keep
  them file-disjoint. The kit lands on gorb-os-2's `main` and is pushed; the flights clone
  pulls before its step 3.
- **Two §46 revisit items are now triggered.** §46 closed them with "revisit when a second
  theme-aware app needs a resident theme", and flights is that app:
  - **D49**: the Themes page's per-app layer and the `assets` packing table (one shared
    8 MB pool, several apps' themes in it at once, §43.1's owner ruling).
  - **The stage still reads the clock's record dialect** (§43 owed 5). Flights step 2
    puts planes and an arm on the stage, so this is firmware work flights hits first.

- **The pool is being built in the other clone.** `../gorb-os` builds D49's firmware half on
  `feat/stage-app-themes` (`docs/theme-pool-plan.md`, ruled 2026-11-03, P3 benched on
  Bronze): one packed, defragmented pool of every app's themes in `assets`, no app ever
  embedding a theme (its ruling 2, the same correction as §0.1 ruling 5 here), 32 moving
  rows instead of 24, and a row that carries frames the app picks. It merges to `main`
  for this clone to take. When it does, `baker@1` mirrors its format changes
  (`CLK_ROW_MAX` 24 → 32 in the baker's checks and `stage3`'s tables, the per-row frame
  pick), and H1's page reads the pool's table rather than one slot.

  The first is H1 here (the page), plus the firmware's room for several apps' themes:
  flights' first baked theme (§0.1 ruling 5) is resident beside the clock's, so that room
  is needed from flights step 2, not from its editor. The second is flights firmware
  work: the stage has to read the tracker's rows for that first theme to draw.

## 6. Open decisions

None open. The three raised here (how much the shell owns, where calibration lives, the
runtime ids) were ruled on 2026-11-03 (§0.1 rulings 1–3). §8's preview question was ruled 2026-11-08
(§8.4).

## 7. As built

### K1 — `theme@1/send` (2026-11-03)

- **The service.** `web/runtime/theme@1/send.js`: `OrbTheme.sender(host)` with `send`,
  `post`, `replace`, `dismiss`, `cancel`, `cancelled`, `findOrb(fresh)`, `held`, `active`.
  Ported from the clock unchanged in behaviour: the head-slice precheck and its one
  retry, the duplicate hold, the exact stop, the fail-closed unknown, the late 409 with
  its commit token, the upload-status witness, the store verdicts, and the modal act
  (busy to the hub, the window guard, the cancel that locks once the hub holds the
  bytes). The host gets `say`, `ask`, `sending` and `orb`; the bake's own account of
  itself rides the verdict line as `meta.summary`.
- **The clock.** About 300 lines of send code leave the document. What stays is a `sender`
  getter and pass-throughs under the old names (`sendTheme3ToOrb`, `postTheme3ToOrb`,
  `replaceArmed3`, `dismissDup3`, `cancelSend3`, `hubOrbIp`, and `_replaceArmed` /
  `_cancel3` as accessors), so the bake and the panes call one door and the clock's
  suites ran unchanged as the regression gate. `hubOrbIp` keeps the clock's state as
  its address cache, as before, and asks the service fresh on a miss. `clock.orbapp`
  went from 969,080 B to 946,412 B.
- **Wiring.** The catalog serves `theme@1`; `make_clock_region.py` lists it in
  `editor.json` and points `send.js` at `/runtime/theme@1/send.js`; the deployer,
  `headless-stub.js` and `runtime-paths.js` load it; `runtime_test.go` and
  `host_clock_region_test.py` pin it.
- **Suites.** `web/runtime/_selftest/headless-send.test.js` drives the service through a
  fake host and hub, with no app: 31 checks, including the commit-token road the clock's
  suites never reached. Five mutants (no cancel lock, the whole bake as the precheck, no
  commit token, an unknown card read as clear, a held bake shipped under another name)
  are all caught; a watchdog turns a send waiting on an answer that never comes into a
  failure rather than a hang. The clock's suites: all 18 green, unchanged.
- **Benched on Gold (orb-B, clock `v1.2.16+2677ad9`), 2026-11-03.** The service ran in the
  hub's own window (real Chrome, the hub's cookie, real fetch and XHR) against the hub
  and Gold, with bench themes made from the shelf's Skeleton under new names:
  - a new theme landed in 60 s: precheck, upload, the hub dialing the orb, the store and
    the reboot each reported, and the verdict said the orb was back; the modal went
    cancellable → locked → closed;
  - the same name with another serial was asked about in 128 ms from the precheck alone,
    the bake held; Replace shipped it once with overwrite (no second precheck) and the
    card's copy took the new serial;
  - a cancel at 20 % of the crossing (upload throttled to 200 KB/s over DevTools, since
    loopback crosses 2.7 MB before the first progress event) aborted the upload, said
    nothing reached the orb, and left Gold's card and slot untouched.

  The clock editor booted in the same window with `/runtime/theme@1/send.js` loaded and
  no errors. Cleanup found a trap: deleting the live bench theme right after the store
  that replaced it answered "stored" (before the reboot) wiped the slot; Skeleton was
  re-stored, and Gold ended as it began (Skeleton the one theme, clock 0 faults).

### K2 — `theme@1/session` (2026-11-03)

- **The service.** `web/runtime/theme@1/session.js`: `OrbTheme.session(host)` and
  `OrbTheme.download`. History over the keys the app names (bursts merged, a bounded depth,
  undo and redo with the app's own restore), autosave and pick-up, My designs, the design
  envelope (`format`, `schema`, `app`, `appVersion`, `made`, then the app's content, in that
  order, so the bytes a theme carries are unchanged), the migration walk, the envelope's
  refusals (a foreign document, another app's design, a missing step, a newer schema, a
  newer release, with the `+build` hash never ordering), opening a design file or an
  `.orbtheme`'s design region, the hub's Edit door, and the `.orbdesign` download. Keys are
  per app (`?app=`, else the host's slug).
- **The clock keeps** what only it knows: `SNAP_KEYS`, checking and re-canonising a stored
  session (`adoptSession`), dropping sound bytes from the autosave (`compactSnap`), the
  watch's part of the document (`designContent`), putting a design back
  (`applyDesignContent`), keeping the selection on undo (`restoreSnap`), and the export's
  file name. The old names stay as pass-throughs, with `_undo`, `_redo`, `_snap` and
  `_snapAt` as accessors and `BURST_MS`, `UNDO_MAX`, `DESIGN_SCHEMA`, `DESIGN_MIGRATE` and
  `download` read through the host on every call, so the clock's suites ran unchanged.
  `clock.orbapp` went from 946,412 B to 939,960 B.
- **Not moved: art persistence.** `saveArt`, `loadArt` and `artNote` stayed in the clock.
  They stored the watch engine's own asset registry, so a shared version needed that
  registry's interface first. **Removed instead, 2026-11-04** (owner ruling): the design file
  and the theme carry a design's art, and that is all that is needed; an upload never exported
  is lost on reload. The clock dropped `saveArt`, `loadArt` and its `wc.assets` key (deleted
  once on mount, so the old copy stops taking the autosave's room); `artNote` says the art
  lasts this session only, and saving to My designs (references, no bytes) says when the
  design's uploaded art will not be kept there. Nothing is owed to the session service.
- **Suites.** `web/runtime/_selftest/headless-session.test.js` drives the service with a
  made-up two-field app, not the clock: 46 checks. Seven mutants (no burst merge, a restart
  that keeps the undo stack, no app refusal, no newer-release refusal, keys not per app,
  history that stops early, a release check that cannot read a `+build`) are all caught.
  The clock's 18 suites: green, unchanged.
- **Benched on Gold (clock `v1.2.16+56321ff`), 2026-11-03,** in the hub's window: the Edit
  door reopened Skeleton's design from Gold's card ("Reopened “Skeleton” from the card"),
  a design an earlier editor wrote; and with no `?theme=`, the editor picked up the
  session the previous build had autosaved in that browser at 21:25. Both editors loaded
  `/runtime/theme@1/send.js` and `session.js` with no errors.

### K3 — `theme@1/preview` (2026-11-03)

- **The service.** `web/runtime/theme@1/preview.js`:
  - `OrbTheme.screen`: the panel's facts (466 px, 1.75 in, 0.0954 mm a pixel), the true-size
    calibration as one per-browser factor under `orb.screen.cal` for every app (§0.1
    ruling 2), with the clock's old `wc.viewCal` read once and carried over, and both
    instruments: `fromRuler` (what a real ruler reads against the 40 mm bar, the clock's)
    and `fromCard` (how wide the drawn ID-1 card outline really measures, `review.html`'s
    method). `review.html` is an art tool with its own dpi key (`flights-art-dpi`); the
    flights editor will use this one.
  - `OrbTheme.view` / `viewK`: Fit, 1:1, True size and Magnify, giving the stage box, the
    scroll, the bezel ring, the ruler and its ticks, the card outline, and the captions
    from one scale.
  - `OrbTheme.panel`: Panel and Split from the app's layers as pixels (`under`, `live`,
    `top`, an optional `keep` mask of undithered texels) through GorbBake's panel mirror.
  - `OrbTheme.ticker` and `OrbTheme.previewTime`: the frame clock with the slow mode, and
    live / the 10:09:36 pose / an app's own time mode.
- **The clock keeps** its layers (`composeLayers`, the white balance, `plainWheelMask`), its
  date cycle as its own time mode, and its template's field names; `viewVals`,
  `drawPanel`, `startTimer`, `now` and the calibration methods are now calls into the
  service. Captions say "face" and "on the glass" where they said "dial" and "on the
  watch". The calibrated caption then printed 1.75 in as 44.4 mm (44.45 rounds down in
  floating point); since 2026-11-04 every caption reads `screen.PANEL_MM`, "44.45". `clock.orbapp`: 939,960 B → 934,446 B.
- **Suites.** `web/runtime/_selftest/headless-preview.test.js`, 37 checks, with no app: the
  screen's facts, load/save/clamp, the legacy carry-over, blocked storage, both
  instruments, every view and caption, the ticker and the time, and Panel and Split held
  byte for byte to the pre-K3 algorithm run straight on GorbBake over random layers with
  a keep mask. Eight mutants are caught (no keep mask, every live row dithered, the glass
  undithered, a whole-canvas Split, no legacy key, the ruler inverted, True size without
  the calibration, slow at full speed). The clock's 18 suites: green, unchanged.
- **Benched on Gold (clock `v1.2.16+f6b1a10`), 2026-11-03,** in the hub's window: on a real
  canvas, `OrbTheme.panel` and the pre-K3 algorithm agree on all 868,624 bytes. In the
  editor, frozen at 10:09 and paused: two Design grabs and two Panel grabs are each
  identical, Panel differs from Design on 215,390 of 217,156 pixels, and Split equals
  Panel left of the wipe (0 of 108,578 differ) and Design right of it (0 of 108,112).
  The browser's clock calibration (1.35) carried over to `orb.screen.cal`, and True size
  drew 227 css px ("True size · 1.75 in"). No errors. A first attempt clicked a "Pause"
  label that is only the button's tooltip, so the frame kept moving and Split looked off
  by motion; the frozen rerun is the result.

### K4 — `theme@1/measure` (2026-11-04)

- **The service.** `web/runtime/theme@1/measure.js`: `OrbTheme.motion(frames)` (the cyclic
  frame diff: union mask, moved count and bounds, the measurement `splitPolicy3` decides
  a carrier from), `OrbTheme.estimate(bytes)` (`GorbBake.estimate3` on a baked bundle's
  one section), and `OrbTheme.fit({rows, cap, want, pace, est})` (the verdict: fits or
  over, at its rate or slower, its badge sentence, short form and colours).
- **The pace hook.** `fit` takes the app's own pace when it has a model, else the bundle's
  estimate, else claims no rate. The clock passes its `_devPace`, which stays the
  clock's: it is a seconds-sweep model across the clock's plates, as planned.
- **Smaller than the plan said.** The plan listed `measureCarriage3`, `remeasureSoon`,
  `_budget` and `devFps` under measure. They stay in the clock: they price its own
  carriage, its row caps and tiers, and its re-render gating, which are the clock's
  physics, not a measurement every app makes. The walk, the estimate and the verdict are
  what carry over.
- **A gap from K1–K3, closed.** The nine browser test pages under the clock's `_selftest/`
  that mount the component loaded none of the `theme@1` services, so since K3 they
  mounted a component calling `OrbTheme` with no `OrbTheme` loaded. Each now loads all
  four before the component, and `headless-pages.test.js` (19 checks) holds the editor
  document, the deployer and every such page to the runtime's list of services.
- **Suites.** `web/runtime/_selftest/headless-measure.test.js`, 20 checks: the walk held to
  the pre-K4 walk on walking, scattered and alpha-only changes, the estimate off the
  hub's shipped Skeleton bundle, and every verdict. Six mutants are caught (no loop-back
  diff, alpha read as colour, an off-by-one bound, the cap refused at the cap, the
  estimate over the model, the three-quarter line moved). The clock's 19 suites: green,
  unchanged. `clock.orbapp`: 934,446 B → 933,496 B.
- **Benched in real Chrome, 2026-11-04.** `bake3-test.html` (which drives the real
  component's `measureFrames3` and bakes through `GorbBake.bakeClock3`), deployed and run
  headless at K4 and at `7cc1221` (before K1): the two reports are line for line identical
  (Skeleton's walk: 8 frames, 326,050 texels moved, a 48,770-texel union in a 402×351 box,
  the bank policy's 8-of-8 at 4 fps), and the baked bundles are byte for byte the same
  (438,408 B, sha256 `19d444d7a4e55d98…`). The page's two failures are the same at both
  commits, so they predate the kit: every preset refused for "no centre hour/minute/second
  hand" with "the bake has hands to seat [0]", and six stills carrying 48 dirty rows. The
  page had not kept up with hands becoming parts (D43 P5c) or the 2026-10-24 tripwire swap;
  repaired 2026-11-04 (`40ecee8`): it seats hands by `handSeats3` and checks the blank-plate
  floor, and passes in Chrome with 17 of 18 presets bakeable.

### K5 — `GorbBake.stage3`, the stage bake (2026-11-04)

- **The builder.** `stage3({scr_w, scr_h, dither})` in `baker@1`, exported beside
  `bakeClock3`. It holds the art bank and the element table and offers `put` (raw bytes,
  such as PCM, at the head of the bank), `row` (a still plane), `rowFrames` (a keyframe
  row), `rowDelta` (a delta row, its closing rect, and its round-trip check), `preview`
  (the picker's plane, with its square and half-panel checks), `element` (a row on a
  sprite already in the bank, which is how a pool of rows shares one sprite), `id`,
  `pack`, `capCheck` and `finish`. `finish` is one call for an app with no bake passes of
  its own: the cap, the pack, every delta row's round trip, the meter and the estimate.
  The code is `bakeClock3`'s own, moved: the bank appends, `ditherAt`, the three row
  kinds with `diffRect` and `paintRect`, `selfVerify3` (now `verifyDelta`), the preview
  block, and the pack through `writeBundle3`. `row` and `rowFrames` now return the
  record they pushed.
- **`bakeClock3` is the clock's translation on it.** It keeps everything that is a clock
  decision: the depth ladder and `depthOrder`, the hands, shadows and pointers, the
  carriage, the wheels and comp tiles, the sound slots and hour patterns, the plate's
  carrier choice, its own wording of the art-cap refusal, the drum and layer-table rule,
  the smoothing passes and their re-packs, and the delta check after its first pack. Its
  bank, element table and ids are the builder's, so nothing it reads moved.
- **Byte identity, measured.** A preload wrapped `bakeClock3` and recorded the sha256,
  size, estimate and layer count of every bundle it returned, and every refusal's
  message, across the six suites that bake (`headless-bundle3`, `sound`, `wheels`,
  `still`, `dither`, `panel`): 81 bakes, identical line for line before and after.
- **Suites.** `web/runtime/_selftest/headless-stage.test.js`, 19 checks, driven by a
  made-up radar app (slug `radar`, not the clock): a plate, a turning arm, a pool of plane
  rows on one sprite, a preview, raw bytes first, dither on and off, a delta-animated
  scope with its closing rect, a loop that needs none, a drifting chain refused, and the
  preview and art-cap refusals. Each section is read back through `verifyBundle3`. Six
  mutants are caught (no delta check in `finish`, `put` not advancing the bank, no cap,
  dither ignoring the bake's choice, a non-square preview accepted, `rowDelta` unwatched).
- **Not checked on a device.** A non-clock section has no reader on the orb yet: the stage
  firmware reads the clock's record dialect (§5). The radar sections are checked against
  the browser's own reader, the one that guards the card. Flights' first theme is baked
  without an editor (§0.1 ruling 5, corrected 2026-11-04), so its bake is where a device
  first reads a non-clock section, and `stage3` (or `orbbundle.write_bundle`, its Python
  twin) is what that bake can be built on.

### K6 — `studio@1`, the shell (2026-11-04)

- **How the shell reaches an app.** The dc runtime renders a React component found on
  `window` from `<x-import component-from-global-scope="…">`, passing the tag's
  attributes as props (`dc-props="{{ obj }}"` spreads an object) and the tag's template
  children as `children`, bindings and all. So the shell is OS code, served and versioned
  like the services, not markup each app copies: `web/runtime/studio@1/studio.js` (React
  components on `window.OrbStudio`, plain `createElement`, no JSX) and `studio.css` (the
  layout, `st-*` classes in `ui@1`'s tokens).
- **What it owns.** `Frame` (the grid), `TopBar` (back, crumb, name, steps, undo and redo,
  the app's extra buttons, the fit dot, the primary action), `Pane` (left and right),
  `Stage`, `Glass` (the scroll and the round glass around the app's canvas, the zoom tag,
  the status toast, and Fit: it measures its own pane after every render and reports
  `onFit(px)`, the smaller side less a 40 px ring margin, never under 160), `Toolbar`
  (play, time, view, zoom while magnifying, render, the split's wipe, then the app's own
  tools), `Calibrate` (the view's caption, the ruler, its slider and reading, and the
  bank-card outline with its reading, the instrument K3 built and nothing showed), the
  send and duplicate modals, and `RenderPanel` (the fit badge, Send, the downloads, a
  warning line). Builders turn the services' answers into props: `sendProps` (the
  sender's state, in the clock's own words), `dupProps`, `fitProps`/`badgeProps`
  (`OrbTheme.fit`), `calProps` (`OrbTheme.view`), `fitOf`.
- **The clock.** Its template places the shell's parts and fills the slots with its own
  content: the layers list, the inspector, the Start sheet, the diagnostics' frame
  stepping, the guide and diagnostics buttons. Its `renderVals` keeps every value it had
  and adds `studioVals(R)`: the props each part gets, built from those values. Its frame
  CSS, `measureStage` and its `ResizeObserver` are gone; it adds `solveCard`. The values
  the clock still computes for its old send lines are held to the modal's in every send
  state by `headless-flow`. `clock.orbapp`: 933,496 B → 925,914 B.
- **Fit, a fix found on the bench.** The clock measured Fit once and then relied on a
  `ResizeObserver`. With the frame's CSS inline that first measure saw the finished
  layout; with `studio.css` a separate sheet, it can run before the sheet lands, and in a
  window Chrome is not painting (an occluded hub window) the observer never fires, so the
  glass stayed at 426 px. `Glass` re-measuring after every render fixes both, and it now
  measures the finished pane: at 1280×720 the toolbar wraps to two rows (87 px), leaving
  585 px, so Fit is 544 px, where the old clock's one early measure had 552.
- **Suites.** `web/runtime/_selftest/headless-studio.test.js`, 42 checks, renders each part
  through a recording stand-in for React and holds the builders to the services' answers:
  the top bar's order, the glass, its Fit report and threshold, the toolbar, both
  instruments in True size and none outside it, both modals in every state, and the
  Render panel. Nine mutants are caught. `headless-flow`'s two template checks follow the
  move: a value handed to the shell through `studioVals` counts as bound, the shell-built
  ones ride their `studio*` object, and the one `<header>` is the shell's `TopBar`.
- **Benched on Gold, 2026-11-04,** in the hub's window, against the pre-K6 clock measured the
  same way first. At 1280×720 and 1920×1080 the top bar, both panes and the stage sit at
  the same pixels, the toolbar is the same height, the same steps, top-bar buttons and 13
  stage buttons are there, nothing scrolls and nothing throws; the screenshots match. Fit
  differs only as above. True size drew 227 px at the browser's 1.35, the ruler 204 px with
  41 ticks and the card 437×275 px; a card reading of 64.2 mm set 1.8 and redrew 302 px
  (the calibration was put back). The Render step shows the badge, Send and both
  downloads. In a 720 px window the diagnostics panel scrolls to reach the card's
  reading: a card at true size is that big.
- **A bench trap, written down.** An app installed straight to an orb (not through the
  hub) leaves the hub's editor memo serving the old region for up to ten minutes; the
  hub's own install and a rescan clear it (`dropThemeCache`). The first K6 measurement
  read the cached pre-K6 editor and looked identical for that reason; it was re-run after
  a rescan.

### K7 — the layers panel and the fields, in `studio@1` (2026-11-04)

- **The field kit.** `OrbStudio.fieldKit({drafts, refresh, scope, inherit})` returns the
  inspector's row builders, the clock's own moved: `mk` (a field's defaults), `H`, `Rg` (a
  slider that snaps to 0 and to whole numbers, with its clamped number box), `Sl` (a select
  with its inherit link), `Ch`, `Nt`, `Sw` (swatches the app supplies as `{k, label, bg}`),
  `Co` (Auto, the ink palette, a picker and a hex box whose half-typed draft survives a
  render, `OrbStudio.parseColor`), `Tx`, `Fi`, `linkOf`, `mark`, `on`, and `fold` (a run
  folded to its first rows behind More). What was the clock's became options: where a
  draft is kept (`scope`) and which option means "follow the source" (`inherit`: rules
  of a pattern and the source it names, first rule first). A generic "Match …" rule was
  not taken: the clock has "Match movement" and "Match wheels (darkened)" as plain
  choices, so it passes its own two rules (`Same as dial` / `Match dial` follow the dial,
  `Default…` the defaults). `OrbStudio.Fields` draws the rows.
- **The layers panel.** `OrbStudio.layerRows(items, o)` lays the rows out: a head per band,
  the indent under a parent, the selection and the dragged row, the arrows with the
  reason they cannot move, the eye, and the explanation as the tooltip. The app answers
  what each layer is (`icon`, `note`, `tag`, `warn`, `hidden`), how it may move (`move`,
  `onMove`), and what its eye and pick do. `OrbStudio.layerGap` is a drop gap the app
  places; where a dragged layer lands is the clock's rule and stays in `stackVals`.
  `OrbStudio.Layers` draws the panel, the cap, the warnings and the add button, with the
  app's footer as its children (the clock's Mechanical switch).
- **The clock.** `renderVals0`'s builders are one `fieldKit` call (its `Sw` maps its dial
  colours to swatches); `foldRows` uses the kit's fold; `stackVals` hands `layerRows` the
  clock's icons, notes, eyes and `stack.js`'s moves; the template places `Layers` and
  `Fields` (`studioLayers`, `studioFields`). 31 CSS rules for the layers and the fields
  left the clock (the hand panel keeps the row classes it still draws).
  `clock.orbapp`: 925,914 B → 910,789 B.
- **Same answers, measured.** A harness ran the pre-K7 clock (`3410a2b`'s extract) and the K7
  clock side by side on every preset, every step and tab, every part, hand, dial and
  movement selection, with folds open, eyes off, drags in flight over each row both
  ways, and Mechanical on, comparing `fields` and `stackRows`; then pressed one handler of
  every field kind and every row's eye, arrows and pick on both and compared the state
  left: 5,735 comparisons and 844 presses, identical. Its first run found one
  difference, a `v` the clock's `Sw` wrapper added to the "Match dial" option; the kit
  takes `vOf(option)` instead so the options pass through untouched.
- **Suites.** `headless-studio.test.js` grows to 77 checks with a made-up app's fields and
  stack: the kit's snapping, clamping, links (the first rule and a later one), swatches,
  the colour draft and parsing, the fold; `Fields`' rows and widths; `layerRows`' bands,
  depth, marks, arrows, eye and tooltip; the gap; the panel. Eight mutants are caught,
  two of them after adding the checks their escape showed were missing (a later inherit
  rule, `vOf`). The clock's 19 suites, `headless-stack-ui`, `headless-forms` and
  `headless-ui` among them: green, unchanged.
- **Benched on Gold, 2026-11-04,** in the hub's window: the layout at 1280×720 and 1920×1080
  is K6's to the pixel. The layers panel showed 17 rows under the clock's three bands
  with its cap and footer; toggling Glass's eye dimmed it (1 → 0.45, ○) and back. The Dial
  step drew 27 field rows (6 sliders, 9 selects, 76 swatches, the "⛓ the dial" links); a
  select changed the design and gave one undo step, which was then undone. No errors.

### `baker@1` follows the theme pool's row format (2026-11-04, after the merge at `1ca82a3`)

The theme-pool work (`docs/theme-pool-plan.md`, merged from the sibling clone) changed the
moving-row format; `tools/theme_schema.py` and `main/art/orb_clock3.c` are the reference.

- **32 rows.** `CLK_ROW_MAX` 24 → 32; the two refusals that cite it read the constant. The
  baker has no slot-name list of its own that stopped at `ptr23`.
- **Frames the app picks** (theme-pool-plan §2.6). A moving row with frames and no frame time
  is one whose frame the app picks at runtime (`orb_stage_frame`): `checkElement` allows up
  to `CLK_PICK_MAX` = 64 such frames (the carriage, frames *with* a frame time, keeps
  `ROW_FRAMES_MAX` = 16), `checkClockRows` counts only the carriage as animated and refuses a
  shadow twin under a picked row, and `estimate3` prices a picked row as a moving plate,
  not as the carriage. The wording is the schema's, word for word.
- **`stage3` bakes them.** `rowPick(e, planes, masked, dith)` lays a bank of frames as one
  picked row; `pool(e, planes, rows, masked, dith)` lays the bank once and a row per entry
  of `rows`, every one naming the same `frame0`: the tracker's plane pool. Both refuse at the
  bake what the dial would refuse at load: past 64 frames, a frame of another size than
  the row (the C reader's frame-window rule; theme_schema has none), a bank of one.
- **Parity.** `tools/verify_js_bake.py --rows` (in `make check-editor`) judges twelve row
  tables with the JS checks and with theme_schema and requires the same verdict and the
  same reason (the 32nd and 33rd seats, 2, 64 and 65 picked frames, 16 and 17 carriage
  frames, a twin under a plate and under a picked row, sixteen picked rows, two carriages, a
  carriage beside picked rows); then bakes a 16-row pool over 7 × 7 = 49 frames with
  `stage3`, reads it back through `orbbundle.read_bundle` and `check_clock_rows`, and runs
  the shipped `orb_clock3.c` over it under the `flights` slug: every row projects with
  `pick_n` 49. Four baker mutants (the cap at 24, no twin rule, a picked row counted as
  animated, no pick limit) each fail it. `headless-stage` gains the pool's checks (27).
- **The clock's bundles.** 80 of the 81 clock bakes across the six baking suites are byte-
  identical before and after; the 81st is a bake that used to be refused for needing a
  25th row and now bakes. `headless-bundle3`'s three checks that pinned 24 rows pin 32
  (ptr31 seats, ptr32 refuses; a hand and 32 pointers; 16 shadowed pointers fill the rows).

### H1 — the Themes page, one tab per app (2026-11-04)

- **Most of it was there.** D37 had already made the Themes face per app: its tabs are the
  orb's own enumeration of theme-aware apps (`/api/theme/apps`: the `themes` cap, an editor
  region, or themes on the card), each pane lists that app's themes, an app with none says
  so, and each app's "Compose new theme" opens its own editor. What the theme pool changed,
  and H1 fixes: every app now wears its own theme, so a pane of everyone's rows and one
  OS-wide "drawing" name were the clock-era shape.
- **The hub.** Each app in `/api/theme/apps` now carries `on_glass` (the installed row's own
  answer) and `wearing` (the name of its card row with the `stored` witness: the theme its
  pool entry holds). An orphan group keeps its `wearing` too.
- **The face.** No "All" tab: with several theme-aware apps there is one tab per app, the app
  on the glass marked, and with none pressed the face opens on the app on the glass (else
  the first tab by slug); one app keeps its label. The state line names the app and what it
  wears (`Clock wears "Skeleton"`, or `wears no theme yet`); the listing's single `active`
  name is the fallback only when no app could be enumerated.
- **Not changed here.** Flights gets its tab once its first theme is on the card: it declares
  no `themes` cap and ships no editor yet, and adding the cap is the app's manifest, not
  the hub's.
- **Suites.** `themeapps_test.go`: two apps, each wearing its own stored theme, the flights app
  on the glass; and an app with a theme on the card and none stored, wearing nothing (two
  mutants caught). `ui_test.go`: no All tab, the tabs and the scope read `on_glass`, the
  state line says what the app wears. The chooser's behaviour was also driven in node out of
  `app.js` (two apps, two tabs, opening on the glass, a press switching, the first tab with
  none on the glass, the one-app label).
- **Benched on Gold** (pre-pool firmware, one theme-aware app): the hub reported the clock on the
  glass wearing "Skeleton"; the face drew "Clock themes (1)" and `1 theme for Clock — the only
  theme-capable app on this orb on SD32G…, Clock wears "Skeleton"`, its row marked active. The
  two-app tabs need the pool firmware and a second app's theme on the card, which Gold has
  neither of; the fake orb and the node run stand for them. Bronze (orb-A, the sibling
  clone's bench orb) has both: the owner looked at its Themes face in the hub on 2026-11-04
  and it read right.

### K9 — the hub's disk is the store, the hub's library is the editor (ruled 2026-11-04)

Owner ruling, 2026-11-04: browser storage is not something an editor should depend on. The
hub already reads themes off its disk for the Themes tab; designs come from the same place,
and the editor the hub serves is the hub's own copy.

- **My designs are files on the hub.** `<library root>/designs/<app>/`, one `.orbdesign` per
  name, art included (a disk has no 5 MB quota), so a saved design reopens whole after a
  reload and opens with "Open a file" too. A save under a held name replaces it. Routes:
  `GET /api/design/list?app=`, `GET /api/design/get?app=&name=`, `POST /api/design/save?app=&name=`,
  `POST /api/design/delete?app=&name=`. The session service's My designs calls them; an
  editor not served by the hub says My designs need the hub. Designs a browser still holds
  under `wc.mine.<app>` move to the hub once, then the key is dropped.
- **Autosave is gone.** The ruling that an unexported upload is lost on reload (§7 K2) applies
  to the whole session: a reload starts from the preset, a saved design, or a theme.
  Calibration stays per browser (§0.1 ruling 2).
- **The shelf keeps what was sent.** A theme the hub stores on an orb (Send from an editor,
  Import a theme) is written to the shelf, filed by its app and its baked name: a theme
  under a name the shelf already holds for that app replaces that file, so the shelf holds
  one copy per name. (Before K9 the Import button's tooltip promised this and nothing did it.)
- **The hub's editor.** The hub serves its library's editor when the library holds the orb's
  release (another build of it included), and reads the orb's own off the card otherwise.
  Corrected 2026-11-05 (owner): as ruled on 11-04 the hub's editor also served an orb a
  release behind it, with a warning to update before sending; now an orb not yet updated is
  edited with its own editor, whose bakes the app it runs reads (`editorFromLibrary`).
  `GET /api/editor/source?ip=&slug=` names where the editor came from and both releases; when
  the orb is behind, the editor says so ("this is the orb's own clock v1.2.22 editor; the hub
  has v1.2.23 — update the app on the orb to edit with it"), and the hub's loading veil names
  the copy it is reading and why. A design made by an older editor opens with a note that saving or sending
  it updates it to this release; one made by a newer editor is still refused (K2).
- **As built (2026-11-04).** Hub: `designs.go` (the store, `<library root>/designs/<app>/`,
  file name a readable stem plus a hash of the exact name, so one name is one file and two
  names that fold alike are two), `shelfkeep.go` (the shelf copy on a landed upload and on a
  landed commit, which finds the bytes the duplicate refusal held under its token; a theme
  already on the shelf under that app and name is overwritten in place, whatever its file is
  called), `editor.go` (`libraryEditorRegion` takes the library's copy unless the orb's
  release is newer or a stamp will not parse; `editorRelation`; `/api/editor/source`); an app
  import retires the remembered regions. `theme@1/session`: `mineList/mineRefresh/mineSave/
  mineOpen/mineDelete` through the hub, `boot()` (drops `wc.session.<app>`, moves
  `wc.mine.<app>` entries that hold a document, keeps the key if the hub refuses one, reads the
  list and the source), `orbNote`, `lastMade` and `sayOpened`; autosave, `compact`, `adopt`
  and `resumeHint` are gone. The clock saves My designs with their art, says so in the art
  line, puts `orbNote` in the Send note, and returns the delete's promise from the second press.
- **Suites.** Go: `TestDesignsAreFilesOnTheHubOnePerName`, `TestStoredThemesAreKeptOnTheShelfOnePerName`
  (an upload, and a duplicate committed over a hand-named shelf file), the region test now
  `…WhenItIsTheOrbsReleaseOrNewer` with an older shelf, another build and a newer orb, and the
  source route checked beside each. `web/runtime/_selftest/fake-hub.js` stands in for the
  routes in node; the runtime session suite has 53 checks, and seven mutants (no key drop, a
  note on any relation, a note on the same release, extras dropped, no refresh after a save,
  the autosave key kept, no history restart on open) are all caught, two after their checks
  were tightened. The clock's session suite drives save with art, list, reopen with art, and
  delete through the same fake hub.
- **Benched on Gold, 2026-11-04** (clock `v1.2.16+a2407a4` on the orb, `v1.2.16+9826cc8` in the
  library): `/api/editor/source` answered `build` and `library`, and the editor came from the
  hub; only the calibration keys were left in the browser; "Save current" filed
  `bin/designs/clock/k9-bench-63966c24.orbdesign`, which listed and reopened after a reload;
  Send to orb stored "K9 bench" and the hub kept `bin/themes/clock-k9-bench-63966c24.orbtheme`.
  A replacing send was not benched on the card (the Go test drives the commit path). Gold's
  firmware reboots onto a stored theme, so the bench left Gold wearing "K9 bench".
- **Known edge.** `make manager` copies the checked-in `themes/` over `bin/themes/`, so in a dev
  tree a kept theme whose file is a seed's name (`skeleton.orbtheme`) is overwritten by the
  seed at the next build. An installed hub's library is not rebuilt, so this is a dev-tree edge.

### K10 — one library per app (ruled 2026-11-04)

Addendum, 2026-11-05 (owner: flag themes that need a rebake after a firmware update): a card
row the orb refuses with its "— re-bake it" sentence (format, geometry version, dialect) reads
"needs a rebake", and the Themes tab's badge counts it. Such a row carries no name, so the
library pass matches it by card stem (`LibEntry.id`, `ThemeIDOf`): when the library's bake of
that name loads on this firmware the card row offers Update; when it needs a rebake too, the
card row offers Rebake, which opens the editor on the library's design.

The owner asked for a consistent model after K9 left two lists (the editor's My designs,
the Themes tab's shelf rows) that each showed half of what the hub holds. Agreed model:

| Concept | What it is | Where it shows |
|---|---|---|
| **Preset** | a starting point built into the editor | the editor's own "start from" choice; never in the library |
| **Library entry** | one name, as a **draft** (saved, never baked), a **theme** (baked, carrying its design), or a **theme with unsent changes** (both) | the app's Themes tab ("In the hub library") and the editor's Start step: the same list |
| **On the orb** | which themes that orb's card holds, and which one it wears | the Themes tab ("On <orb>"), beside the library |

- **One name, one entry.** A theme the hub keeps on the shelf (a send, an import, a rebake)
  removes the draft of that app and name: the theme carries the design. Saving over a theme's
  name leaves a draft beside it, shown as "theme · unsent changes".
- **Opening** an entry opens its newest state: the draft when there is one, else the theme's
  design. Delete removes the entry (draft and shelf theme); "Discard changes" removes only the
  draft of a theme with unsent changes.
- **Files.** `.orbtheme` is the one file that leaves the hub (Download, Import). The design-file
  download is gone; Open a file still reads an `.orbdesign` someone already has.
- **Labels, never blockers.** "made with <app> vX" when an older editor made the design (opening
  it says saving or sending updates it); "needs a rebake" when the theme's container is a format
  or geometry ABI the current firmware does not read, which the hub reads off the header. A
  stale theme whose design survives can be rebaked; one without a design cannot, and says so.
- **Rebake all** opens the app's editor with `?rebake=1`: it bakes each stale entry from its
  design and puts the new theme on the shelf in its place, showing progress. Nothing rebakes
  on its own.
- **Routes.** `GET /api/library/entries?app=`, `GET /api/library/design?app=&name=` (the entry's
  newest design), `POST /api/library/keep?app=` (a baked theme onto the shelf, folding its
  draft), `POST /api/library/forget?app=&name=&what=draft|all`. `/api/design/save` stays the
  draft door; K9's `/api/design/list|get|delete` go, their answers now the entries'.
- **As built (2026-11-04).** Hub: `libentries.go` (the entries, merged by name from the shelf
  and `designs/<app>/`; `/api/library/entries|design|keep|forget`; `from=theme` reads a theme's
  design beside its draft, `fold=0` keeps a rebake from folding the draft), `orbnet.ShelfTheme`
  (a theme's identity and design, and for a stale one why the orb refuses it: a FORMAT 3 file of
  another geometry ABI still yields its name, app and design; an older FORMAT only its name),
  `keepTheme` replacing a stale file under the same app and name in place and folding the draft.
  K9's `/api/design/list|get|delete` are gone. The Themes face draws "On <orb>" and "In the hub
  library" (`themeLibraryRows`, replacing `themeShelfRows`), each library row tagged draft, theme,
  theme · unsent changes or needs a rebake, with made with, on <orb> and size, and Edit (by
  `?entry=`), Install, Discard changes and Remove from hub library; Rebake all opens the editor
  with `?rebake=1`. The state line's library count now prints the read it belongs to (it printed
  the previous read's, as the shelf count had). `theme@1/session`: `library/libraryEntry/
  libraryRefresh/saveDraft/openEntry/forgetEntry/rebakeAll`, `?entry=` and `?rebake=1` on boot,
  `exportDoc` gone. A rebake that comes out under another name is reported, not counted. The
  clock: the Start step's list is the Library, with the same tags and verbs; `bakeBytes` (Rebake
  all's bake) waits for the opened design's movement to settle before the walk; the Render step's
  design-file download is gone.
- **Suites.** Go: `TestDraftsAreFilesOnTheHubOnePerName`, `TestLibraryIsOneEntryPerName` (three
  states, newest design, `from=theme`, a `fold=0` rebake, Discard changes, forget),
  `TestKeepFoldsTheDraftAndReplacesAStaleBake` (an ABI-5 restamp listed stale with its design
  readable, then replaced in place), `TestThemesFaceListsTheLibraryByEntry`; three mutants (no
  fold, no in-place replace, `from=theme` ignored) caught. The runtime session suite (63 checks)
  drives the library through `fake-hub.js`, which now keeps libentries.go's rules, and Rebake all
  with real `writeBundle3` bytes; six mutants caught. The clock's suites moved off the download
  (the design round trips now read `designDoc(true)`, the document a draft and a theme carry).
- **Benched on Gold, 2026-11-04,** in the hub window. The face showed "On Gold" (Skeleton, K9 bench)
  and "In the hub library" with Rebake all (1): K9 bench, its shelf copy restamped to ABI 5 for the
  bench, read "needs a rebake"; Skeleton read "theme · made with v1.2.14 · on Gold". The first Rebake
  all press failed because the bake ran before the opened design's movement had settled, which
  `bakeBytes` now waits out; the second rebaked K9 bench to ABI 6 in the same shelf file and left its
  unsent changes standing. Discard changes then dropped the draft; Edit on Skeleton opened
  `?entry=Skeleton` with "made with clock v1.2.14; saving or sending it updates it to v1.2.16". A
  send that folds a draft was not benched on the card (another store and reboot of Gold); the Go
  and session suites drive the fold.


## 8. The hosted editor: Orb Manager owns the page (ruled 2026-11-08)

Owner, 2026-11-08: theme and file management belongs to Orb Manager, not to the apps. An
editor opens on four choices: start fresh, start from one of the app's templates, open a
library entry, or import a file. It ends on the Render step with three: save to library,
send to orb, export the `.orbtheme`. The preview engine is Orb Manager's in its entirety,
not only its sizing. An app asks for the views it needs (time, weather, place, which pane),
and Orb Manager draws them as buttons and dropdowns in the centre pane. Each app and each
view shows its estimated frame rate, and a live size readout replaces "Check the bake".

### 8.0 What is true today (at `45165cd`)

- **The kit is a library the app calls.** Each app's editor is its own page
  (`apps/<app>/editor/index.html`, the clock's `Watch Composer.dc.html`), which puts the
  `studio@1` pieces together itself. Nothing stops an app from doing a step its own way.
- **Start.** Flights, Weather and Radar each fill `OrbStudio.StartSheet` (scratch, tiles,
  library, Open a file) with their own wording. The clock draws its own start sheet.
- **Render.** Send and Save to library are the kit's. Download .orbtheme and Check the bake
  are each app's own code (`bakeNow` plus `OrbTheme.download`), four copies. Check the bake
  bakes the whole design and reports its KB.
- **Stage.** The kit sizes the glass, calibrates true size, blends layers (`OrbTheme.panel`)
  and runs the frame clock. The app draws the layers and adds its own toolbar controls
  (Weather's condition select, Radar's zoom and time), each in its own style.
- **Frame rate.** `OrbTheme.fit` gets a pace only from the clock (`_devPace`). The other
  three pass rows and the cap, so their badge says "Fits" and never shows an fps.
- **Files.** The download is an anchor download, so it lands in the default Downloads folder
  of the app window's own Chrome profile, with no Save dialog.
- **Off the hub,** an editor still runs, with Download as its only output.

### 8.1 Owner rulings, 2026-11-08

1. **Orb Manager owns the editor page.** An app supplies its content through a declared
   module (§8.2) and cannot replace the start step, the Render step, the stage or the
   readouts. This retires §2's layer 3.
2. **Orb Manager is a required dependency.** The off-hub editor goes.
3. **The app's part of the editor still ships in the `.orbapp`:** its module (§8.2), and
   only that. The page, the start step, the library, the Render step, the preview and the
   readouts are Orb Manager's and never ship in an app. Any `.orbapp` stays editable in Orb
   Manager wherever it came from: the hosted page loads the module from the app's own editor
   region, and nothing app-specific is built into Orb Manager.
4. **The start step offers four choices, in this order:** start fresh, a template (the list
   is the app's), open from the library, import a file.
5. **Import puts the file in the library at once,** then opens it: an `.orbtheme` as a theme,
   an `.orbdesign` as a draft.
6. **The Render step offers three actions:** Save to library, Send to orb, Export .orbtheme.
   Export asks where to save.
7. **The preview engine is Orb Manager's:** the frame loop, the view state, the glass, the
   blend, the calibration and the view controls in the centre pane. An app declares its views.
8. **A live size readout replaces Check the bake.**
9. **Every app and every view shows its estimated frame rate,** not only the clock.

### 8.2 The contract (a sketch; the first slice settles it)

`editor.json` names the hosted contract (`"host": 1`) and the module the page loads. The
module registers what only the app knows:

- `schema`, `migrate`: the design schema and its steps, as `theme@1/session` takes today;
- `blank()` and `templates`: start fresh, and the app's starting points (key, name, note,
  design), drawn by the host as tiles;
- `steps` and the panes: the editing steps, and each step's left and right panes, built from
  `studio@1`'s fields and layers;
- `views`: the controls the stage offers, as data, for example `time: live | 10:10`,
  `weather: clear … storm`, `place: home | 1–3`, `pane: scope | info card`. Each is a
  segmented control or a dropdown, and may name when it applies;
- `draw(view, frame)`: the layers for one view at one frame, which the host blends and paces;
- `bake(design)`: the design baked to `.orbtheme` bytes, for Send, Export, the live readout
  and Rebake all.

Everything else (the library, import, export, send, history, the views' controls, the fit,
size and fps readouts) is the host's, done once.

- **Export** uses `showSaveFilePicker`, which the app window allows (`127.0.0.1` is a secure
  context), with the plain download where a browser lacks it.
- **The live readout** re-bakes about a second after the last edit and shows the size and
  the fit, marked as updating while an edit is newer than the bake. (This sketch said "in a
  worker"; the apps' bakes draw on canvases with the page's loaded faces, so they run on the
  page, after a pause that grows with the bake: §8.8.)
- **Frame rate per view:** priced from the frames the view's preview moves, on
  estimate4's compositor costs (§8.9; the sketch named `estimate3`, which prices a clock's
  seconds and gears and calls an app's own rows still). The clock keeps `_devPace` as its
  own model.

### 8.3 Slices

| Slice | Content | Gate |
|---|---|---|
| **E1** | the hosted page and the contract, with Radar on it (the smallest editor) | runtime and Radar suites; on Gold, Radar's editor opened from its Themes tab offers the four starts and the three Render actions — **built and benched on Gold 2026-11-08**, with E2's import and export (§8.5) |
| **E2** | import into the library, Export with a Save dialog | Go suites for the import route; session suites with the hub stubbed; on Gold, an imported `.orbtheme` is in the library before it opens — **built with E1** (§8.5); the Save dialog is not benched in a headed browser |
| **E3** | Weather and Flights on the host | each app's suites; no app code left that draws a start sheet, a Render step or a download — **built 2026-11-08**, benched on the bench fake at OS v1.2.38 (§8.6) |
| **E4** | declared views in the centre pane, for all three | the same controls in every app; each view reaches the glass — **built 2026-11-08**, benched on the bench fake with home and two places (§8.7) |
| **E5** | the live size readout; Check the bake goes | the clock's bake timed first; a readout that matches the bake's bytes — **built 2026-11-08** (§8.8) |
| **E6** | frame rate per view, every app | an estimate shown for each view; checked against Gold for at least one view per app — **built 2026-11-08** (§8.9); checked on Gold for Flights and Weather (Radar has no moving view; the clock is E7's) |
| **E7** | the clock on the host | the clock's headless suites (about 1,500 checks) green; nothing lost against the P5 checklist |

The clock goes last. It is the largest editor and the one where the frame rate matters
most, so it moves after the contract has carried the other three.

### 8.4 How far the preview goes (ruled 2026-11-08)

- **Now: the host runs the stage and the app's `draw` paints each view,** as §8.2 sketches.
  `draw` is kept narrow (one view, one frame, layers out) so that it can be replaced later
  without changing the rest of the contract.
- **Later: full orb OS emulation in Orb Manager** (owner: the eventual goal, not now). The
  host plays the baked theme as the orb does, running the app's wasm against a stand-in for
  the OS, so that the preview and the fps come from the bake itself and not from a model.
  It is not a slice here. How much of it `baker@1`'s panel mirror already covers is not
  checked.

### 8.5 As built: E1 and E2 (2026-11-08)

- **The page.** `runtime/host@1/page.html` and `host.js`. An `editor.json` with `"host": 1`
  names its `module` files and lists `host@1` in its runtime, and no `entry`. The hub
  (`editor.go`, `hostedEditor`, `serveHostedEditor`) answers the editor's address with that
  page: the core runtime (React, `baker@1`, `theme@1`, `studio@1`, `host@1`), `maps@1` only
  when listed, then the module files in their listed order. The launch token goes on the
  page, as on any editor's entry document. A manifest it cannot serve (another contract, no
  `host@1`, no module, a module that is a path, not a `.js`, or not in the region, a runtime
  id the hub lacks) is refused by name.
- **The contract** is `host.js`'s header: `slug`, `title`, `schema`, `migrate`, `check`,
  `complete`, `blank`, `templates`, `picture`, `steps`, `parts`, `fields(step, c)`, `views`,
  `setup(c)`, `draw(x, d, c)`, `bake`, `fit`. The design carries its `name`; the host stamps
  `base`. `draw` gets the view, what `setup` loaded (`env`), the hidden parts and a `memo`
  the app caches its own drawn parts in.
- **The host owns** the frame, the steps (Start, the app's, Render), the start sheet (Start
  fresh, the templates, the library, Import), the Render step (Save to library, Send to orb,
  Export .orbtheme, and Check the bake until E5), history, the send and its modals, the
  parts pane's eyes, the view controls (one segmented control per declared view, before
  the screen's own), and the fit. Opened off the hub, it says to open it from Orb Manager.
- **Import** (`session.importFile`): an `.orbtheme` goes to `/api/library/keep` as a theme,
  a design file to `/api/design/save` as a draft (with its picture), and then it opens. A
  name already in the library asks first ("Replace it"), and the opening cover goes up only
  after the answer: on Gold the cover sat over the question and took its clicks, and the
  suite now holds that. Another app's theme, a file that is neither, and a design with no
  name are refused before anything is kept; a theme with no design is kept (it installs)
  and says it cannot be opened.
- **Export** (`OrbTheme.saveFile`): `showSaveFilePicker` with the theme's card stem as the
  suggested name; closing the dialog saves nothing; a browser without it downloads.
- **`studio@1`**: `StartSheet` takes `importFile` (an Import section under the library) and
  shows Open a file only when the library passes `openFile`, so Flights, Weather and the
  clock keep theirs until E3 and E7. `RenderPanel` lists Save to library before Send to orb
  for every app.
- **Radar** (v1.2.2, still unpushed when this was built, so no version bump):
  `radar-editor.js` registers with `OrbHost` (347 lines, was 499), `index.html` is gone,
  `editor.json` is hosted. Its views are Zoom (Region, Area, City) and Time (Now, −40 min,
  +30 min); its parts are drawn once per change of their own part of the design, in `memo`.
- **SDK** (`orbsdk.py`): packs a hosted editor and refuses one with another contract, an
  `entry`, no `host@1`, a module that is missing or not a plain `.js`, or a page of its own;
  `module` without `host` is refused too.
- **Suites.** Go: `TestHostedEditorServesTheHubsPageWithTheAppsModule`,
  `TestHostedEditorRefusesAManifestItCannotServe` (seven refusals). Runtime: the session
  suite's import and export checks (83 checks; five mutants caught: no replace question,
  no refresh before the open, no envelope check, no Save dialog, the cover over the
  question); the studio suite's hosted start sheet and the Render order (115 checks).
  `host_sdk_editor_test.py` gained the hosted cases and four mutants (15 of 15 caught); its
  "no release stamp" mutant's anchor had gone stale with the `os_min` stamp and was fixed.
- **Benched on Gold, 2026-11-08,** with a bench hub limited to Gold and headless Chrome. The
  new Radar installed over the hub's route; its editor, opened from the hub's page, showed
  Start fresh, the six templates, the library and Import; Continue, the Map step's fields,
  the preview on Gold's own home map (Fort Worth), City and +30 min in the toolbar; Render
  listed Save to library, Send to orb, Export .orbtheme and Check the bake. Save to library
  made "unsent changes"; Import of the shelf's Radarscope asked to replace it and, pressed
  with the mouse, imported it as a theme; Send to orb asked about the card's Radarscope,
  Replace on the card stored 1,861 KB, and Gold came back drawing it; `?entry=Radarscope`
  opened on the Ground step. Export's Save dialog was not benched: headless Chrome shows no
  dialog. Gold keeps the new Radar.

### 8.6 As built: E3 (2026-11-08)

- **Weather and Flights are hosted.** Their `index.html` is gone and their `editor.json`
  names `host: 1` and their module files in load order. `weather-editor.js` went from 632
  lines to 456, `flights-editor.js` from 737 to 550; with Radar's, no app code is left that
  draws a start sheet, a Render step, a send, a download or a toolbar.
- **What the host gained**, each an Orb Manager feature any app declares rather than code an
  app carries:
  - **a frame clock** (`play: {fps, on(view), what, idle}`): one play/pause button, `c.t`
    and `c.dt` in `draw`, and `dt` only on a tick, so a frame drawn for any other reason
    moves nothing. Weather plays on Now only; Flights at its bake's sweep rate;
  - **Panel and Split** for an app that draws its layers apart (`layers: true`, `c.layer`
    `'under'`, `'live'` or `'all'`): the host blends them through `OrbTheme.panel` with the
    wipe slider. Flights' was its own; any app can now have it;
  - **view controls**: a segmented control or a dropdown (`kind: 'select'`), options fixed
    or a function of the design and what `setup` loaded (Flights' ranges follow the orb's
    distance unit), `show` (Flights' range only with its map on), `active` and `idle`
    (Weather's condition only on Now), and `init` (Flights' third range, Weather's partly
    cloudy), with `?<key>=` in the address leading;
  - **the glass's pointer** (`pointer: {down, move, up, cursor}`, in glass px): Weather's
    Layout step picks and drags Now's elements with it;
  - **the app's own preview state** (`ui`, `c.ui`, `c.setUi`), outside history: Weather's
    picked element, shared by its fields, its frame and its pointer;
  - **Reset to <its start>** in the top bar for every app (it was Weather's alone): a
    second press within 4 s, the name kept, undoable;
  - **`c.part(name, key, make, {fresh})`**: a drawn part kept until its key changes, a
    promise drawing the frame again when it lands. It replaced the `usePart` hook both apps
    carried; `fresh` keeps a stale map level from standing in for the one being fetched;
  - `start` (the template the editor opens on), `startNote`, a part's `also` (Flights'
    Words row lit on the Frames step) and `eyes: false` (Weather hides nothing).
- **Moved into the host from the apps:** Flights' "pick your plane picture first" is its
  `bake` refusing, said as "Not baked: …"; the library's meta line is the entry's template
  name for every app (Weather's said its icon pack, Flights' its family).
- **The bench fake** (`internal/orbfake`) kept an uploaded app's version and editor region
  at last, so the hub serves a hosted editor from it as from a device: its editor word
  positions read 47..49, which no container has; a real one puts the triple at 44..46 and
  the version at 47. `TestAnUploadKeepsItsVersionAndEditor` uploads the tree's own
  `weather.orbapp` and fails on the old positions.
- **Benched 2026-11-08** on the bench fake stamped `v1.2.38+abc1234` with Fort Worth as home,
  in headless Chrome; Gold was not used, because these Weather and Flights builds ask for
  OS v1.2.38 and the hub's built-in firmware is still v1.2.37. Weather: the start sheet
  (eight tiles), Now animating (the frame changes between reads, holds still on pause),
  Snow picked from the dropdown and falling over the home map, the dropdown off and the
  button's tip "The weather plays on Now" on 5 days, Layout's grab cursor, a mouse drag of
  the icon setting Angle 289° and Distance 81 px, and Undo putting it back. Flights: Info
  and Roam, the four ranges in km once its map is on, Panel and Split with the wipe, five
  parts with eyes, Check the bake (1,062 KB). Send to orb from each, through the card's
  duplicate question and Replace: Modern (Flights, 1,062 KB) and Scope (Weather, 1,282 KB)
  stored. The Export dialog is still unbenched in a headed browser.

### 8.7 As built: E4 (2026-11-08)

- **Two rows under the glass.** The upper row is what the preview shows: the play button,
  then each of the app's views under its caption (`label`, with `tip` for the longer
  sentence). The lower row is how the glass is shown: View, the zoom, Render (Design, Panel,
  Split) and the screen's ⚙. Flights' one wrapping toolbar is gone, and every app's views
  read the same.
- **The places are the host's.** `OrbHost.places(ip)` reads the orb's home, its three
  places and its distance unit once, for every app (home named Home, as the apps name it).
  `draw` and `fields` get `c.home`, `c.unit`, `c.places` and `c.place`. A view with
  `kind: 'place'` lists home and the set places; `home: false` leaves home out, and a
  place control with nothing to list is not drawn. A choice the orb does not have falls to
  the control's first option, so the glass and the control never disagree. `?place=p2` in
  the address is kept until the places are read.
- **Radar** has a Place view (Home and the orb's places, as its knob walks them) beside
  Zoom and Time. Its map is the chosen place's at the chosen zoom, read on demand and kept
  per zoom, and the preview names home "Home" as `main.c` does (it said the home's place
  name). Its `setup` is gone.
- **Weather's Places screen** shows the place control's choice from the orb's own places,
  with that place's map; with none set it still shows the made-up Lisbon. Its Place
  control acts on Places only. Its `setup` is gone. The place's clock read Lisbon's UTC until the
  places carried their zones (radar-plan §6.29, 2026-11-08); it now reads the place's own
  local time.
- **Flights** reads home and the unit from the host. Its levels are read per range on
  demand (a step back is at once), and its `setup` is gone. Its views read Shows and Range.
- **Suites.** `headless-host.test.js` (48 checks) drives the real `host.js` with a made-up
  app through a small hooks runtime: the start step, the address, `init`, `show` and
  `active`, the two rows and their captions, the place list, the frame clock (`dt` on a
  tick only, pause, a view it does not play in), Panel and Split, kept parts (a promise
  drawing again, `fresh`), Reset (asked twice, undoable), the pointer in glass px, the
  app's own state, the Render step, an app's refused bake, off the hub, and an orb with no
  home or places. Ten mutants caught.
- **Benched 2026-11-08** on the bench fake (`v1.2.38+abc1234`) with home at Fort Worth and
  places Lisbon (1) and Houston (3), its library in scratch copies. Radar: Place
  `Home | Lisbon | Houston`, Zoom, Time; Houston picked, its map built and drawn, the name
  Houston on the glass. Weather: Screen, Weather, Place `Lisbon | Houston` (no Home);
  Places showed Lisbon, then Houston with Houston's map. Flights: Shows and Range, with
  Design, Panel and Split on the lower row.

### 8.8 As built: E5 (2026-11-08)

- **Timed first**, in headless Chrome against the bench fake, every template of each app:
  Radar 230–320 ms (1.8 MB), Weather 160–220 ms (1.25–1.35 MB), Flights 160–320 ms
  (0.8–1.2 MB); the clock's Skeleton, through its own Download, 1.1 s cold and 0.44 s warm
  (3.4 MB). A heavier clock (a 30 fps sweep) will take longer. A worker was not used: the
  apps' bakes draw on canvases with the page's loaded faces.
- **The readout.** A pause after the last edit, the host bakes the design and keeps the
  bake for the design it was made from. The pause is twice the last bake, at least 1.2 s
  (`OrbHost.SIZE_PAUSE_MS`) and at most 6 s, so a slow design is not baked at every breath;
  an edit while it waits starts the wait again, and a bake an edit overtook is not shown.
  Bakes run one at a time.
- **Where it is said.** The top bar's fit reads "Fits · 1,062 KB" ("…" while it measures,
  "not baked" when the app refuses, the dot red past what an orb takes). The Render step
  leads with a Size line: "1,062 KB of the 8,320 KB an orb takes (13%) · baked in 0.2 s",
  "Measuring its size when you pause…", "Not baked: <the app's reason>", or "an orb will
  refuse it" past `GorbBake.THEME_UPLOAD_MAX`. Check the bake is gone.
- **Send to orb and Export reuse the kept bake** when nothing changed since it, so pressing
  either after the readout settles bakes nothing again.
- **Suites.** The host suite (55 checks) adds the readout: measuring before the pause, the
  size after it in the top bar and on Render, Export reusing the kept bake, an edit
  restarting the pause and two edits baking once, an app's refusal said in both places, and
  the over-the-limit line. Five more mutants caught (no kept bake, edits not restarting the
  pause, the refusal unsaid, no over-limit, the top bar without the size).
- **Benched 2026-11-08** in headless Chrome on the bench fake: Flights opened at
  "Fits · 1,062 KB" (it read MB until the top bar was put in KB, as Render is); renaming the
  theme put both back to measuring, and 1.2 s later they read 1,062 KB, baked in 0.2 s.

### 8.9 As built: E6 (2026-11-08)

- **Why not the baker's estimators.** `estimate3` and `estimate4` price a clock bake: its
  seconds hands, cadences and gear banks. Flights' arm and traffic and Weather's particles
  are rows the app poses itself, which both read as still. So the estimate comes from what
  a view's preview moves, which every hosted app already draws.
- **`OrbTheme.frameCost(a, b)`** prices one frame from the pixels that changed between two:
  per screen row one restored span, first changed pixel to last, less the widest inkless gap
  inside when it is 24 px or more (orb_gfx.c INK_GAP_MIN), and the changed pixels walked;
  `frame_ms = 0.744 restored_kpx + 0.507 walk_kpx + 7.2` (estimate4's §49.15 fit, `GorbBake.EST4`).
- **`OrbTheme.pace(frames, {base, want})`**: frames sampled at `base` fps; each rate on the
  panel's grid (`GorbBake.GRID4`, now exported) at or under the wanted one (the grid's top,
  20, caps it) is priced on frames base/H apart, its worst frame against `floor(60 / H)`
  scans, as estimate4's judge does: it holds when `cost × 1.15 + 20 ms` fits. The estimate
  is the fastest rate that holds. (estimate4's prose says 7.5 fps alternates 7 and 8 scans;
  its code prices it on 8, and so does this.)
- **The host samples a view** after the live size settles: a second's warm-up, then half a
  second at 60 fps, drawn on a canvas of its own from a fresh memo, so the live preview is
  untouched; one view at a time, kept per design and view. A view the app does not play in
  is still. The top bar and the fit badge carry the rate of the view on the glass
  ("Fits · 20 fps · 1,282 KB", "Fits, slower · 15 fps"); Render adds a Frame rate block
  listing each option of the app's `paceBy` views (`{key, with, label}`: Weather's weather
  rows are measured on Now whatever screen shows).
- **The apps.** Weather: `play.want` 20 (Now bursts at the panel's ceiling, the grid's top;
  its preview plays at 30), `paceBy` Screen and Weather on Now. Flights: `paceBy` Shows;
  its verdict now carries a rate, which §8.1 ruling 9 asked for and which the rows-only
  ruling of 2026-11-04 had left out. Radar has no moving view, and says still.
- **Measured.** Flights' Modern: ~15 of 20 fps in every mode (its worst frame priced 33.9
  ms: 24.9 kpx restored, 16.1 kpx walked). Weather's Scope: Now ~20 of 20 under every
  condition; Places, Hourly and 5 days still.
- **On Gold** (`/api/v1/debug/dial?ms=4000`, the app on the glass by `/api/v1/screen?to=`,
  the clock put back after): Flights v1.4.2 presented 18.5 fps against its 20, every commit
  under 20 ms, 12.8 kpx restored and 15.9 kpx walked a frame; Weather's Now 21.3 fps. The
  walk matches the estimate's; the restore is about twice Gold's, but Gold flew whatever
  traffic was live near home while the preview flies its twelve made-up aircraft, so this
  one point is not a calibration and the model was left as it is. On these two the estimate
  is right for Weather (20) and cautious for Flights (15 where Gold held 18.5 of 20).
- **Suites.** `headless-measure` (30 checks) adds the frame cost (still, one mover, the gap
  rule both ways) and the pace (a small mover holds 20, a whole-glass change does not, the
  wanted rate and the grid's top cap it, 7.5 on 8 scans). `headless-host` (57) adds the
  Render block and the top bar's rate. Mutants caught: a view played where the app does not
  play, the top bar without the rate, the gap not left out, the slowest rate winning.
  "7.5 priced on its mean period" is the same number for every grid rate, so no check can
  catch it.

