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
| **K1** | `theme@1/send`, and the clock calls it | `headless-send` moves to `web/runtime/_selftest/` and passes; clock suites green; a send, a duplicate Replace and a cancel benched on Gold |
| **K2** | `theme@1/session` | `headless-session` and the session half of `headless-design` move and pass; a design saved before the move opens after it |
| **K3** | `theme@1/preview`, including true size | the clock's stage, Panel and Compare equal before and after (`headless-panel`, a screenshot pair); true size matches `review.html`'s calibration |
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
