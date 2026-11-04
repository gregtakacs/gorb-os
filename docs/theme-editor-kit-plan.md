# The theme editor kit: what every app's editor gets from the OS

Status: **RULED 2026-11-03 (owner, §0.1). Nothing is built.** Written at `7cc1221` (gorb-os-2 `main`).
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
5. **Flights' interim themes are hard-coded in firmware.** Until its editor exists, the
   tracker's look on the stage comes from temporary built-in themes, not from an editor.
   So nothing in flights steps 1–2 waits on this kit, and K5 does not wait on flights.

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
3. **An editor's own way.** An editor that lists only `baker@1` (or nothing) in
   `editor.json` keeps full control, as it does today. Each layer can be used without the
   one above it: services without the shell, the shell with the app's own panes.

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
| **K4** | `theme@1/measure`, the pace hook | `headless-pace` and `headless-estimate` green with `_devPace` as the clock's override; an app with no hook gets the estimate |
| **K5** | a generic stage bake in `baker@1` (a plate, rows, sprites, row pools, scalars) for any app the stage draws; `bakeClock3` becomes the clock's translation on top of it | the clock's bundles are byte-identical before and after (`headless-bundle3`). Flights' interim themes are built into firmware (§0.1 ruling 5), so this does not wait on flights; its format is checked against flights' when the editor starts |
| **K6** | `studio@1`: frame, title and step bar, status, send modal, the Render step, slots | the clock's P5 checklist (nothing lost), no page scroll at 1280×720 and 1920×1080 inside the hub |
| **K7** | `studio@1`: the layers panel and the field builders, driven by what the app declares | the clock's layers and inspector behave as before (`headless-stack-ui`, `headless-forms`, `headless-ui`); the clock's own rules (`stack.js`) stay the clock's |
| **H1** | the Themes page per app: a sub-tab for each theme-aware app, with its themes and a link to its editor, all hub code (§0.1 ruling 4) | Go and UI suites green; on Gold, the clock's tab lists the card's themes and opens the editor; an app with no themes or no editor gets a tab that says so |
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

  The first is H1 here (the page), plus the firmware's room for several apps' themes when
  a second app ships one. The second is flights firmware work; with interim themes built
  in (§0.1 ruling 5) it is the tracker's own business until its editor ships themes.

## 6. Open decisions

None open. The three raised here (how much the shell owns, where calibration lives, the
runtime ids) were ruled on 2026-11-03 (§0.1 rulings 1–3).

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
- **Not moved: art persistence.** `saveArt`, `loadArt` and `artNote` stay in the clock.
  They store the watch engine's own asset registry, so a shared version needs that
  registry's interface first. A later slice, once flights shows what its art store needs.
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
  watch". Not changed: the calibrated caption prints 1.75 in as 44.4 mm (44.45 rounds
  down in floating point), as before. `clock.orbapp`: 939,960 B → 934,446 B.
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
