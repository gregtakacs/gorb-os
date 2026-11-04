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
| **K4** | `theme@1/measure`, the pace hook | `headless-pace` and `headless-estimate` green with `_devPace` as the clock's override; an app with no hook gets the estimate — **built 2026-11-04**, benched in real Chrome (§7) |
| **K5** | a generic stage bake in `baker@1` (a plate, rows, sprites, row pools, scalars) for any app the stage draws; `bakeClock3` becomes the clock's translation on top of it | the clock's bundles are byte-identical before and after (`headless-bundle3`). Flights' first theme is baked without an editor (§0.1 ruling 5), so this does not wait on flights; its format is checked against flights' when that first theme reads on a device — **built 2026-11-04** (§7) |
| **K6** | `studio@1`: frame, title and step bar, status, send modal, the Render step, slots | the clock's P5 checklist (nothing lost), no page scroll at 1280×720 and 1920×1080 inside the hub — **built and benched on Gold 2026-11-04** (§7) |
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
  page has not kept up with hands becoming parts (D43 P5c); it is owed its own repair.

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
