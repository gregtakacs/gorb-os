# watch-composer selftests

Two headless tests and three probes. The tests print a line report into
`<pre id="out">` and set `<title>` to PASS or FAIL, so CI can grep the title
from a dumped DOM. The probes print measurements, or draw pictures, for a
human to read; they assert nothing.

| File | Covers |
|---|---|
| `selftest.html` | engine level: `WatchEngine.textKits()` caps/slice/glyph/byDate output, plus the posed preview frame (`previewDate`, `PREVIEW_MOON`) against new/full controls |
| `export-test.html` | end to end: drives the real `Component.exportKit()` and inspects the bytes of the zip it builds (manifest, `text/<id>_<kind>/` sprites, `preview.png` moon pose, calendar-hand + perimeter blocks), and bakes the same files through the real `Component.finishTheme()` to re-read the `.orbtheme` container Chrome's own PNG encoder produced (`themeCheck`, below) |
| `probe-shade.html` + `probe-shade.js` | measurement probe: per-row/per-column alpha profile of a window kit's `shade.png` and how a digit-cell's mean shade changes as it slides across the strip |
| `probe-geom.html` | measurement probe: where the additional hand's tip really lands, in device pixels, against the band its ring prints |
| `probe-calendar.html` | visual probe: the additional hand in each of its four roles, each ring with the hand that reads it, the five invented tips, and the ring display controls (size, position, orientation, colour, face) shown rather than described |

## The one additional hand and the ring it reads

Besides hour, minute and seconds a dial carries ONE additional centre pointer. What it
indicates — `hours24`, `gmt`, `day` or `date` — is the single choice (`design.extra`, from
`CAL_HANDS`). The printed ring is derived from it through `EXTRA_RING`, so the rim can never
carry a scale that disagrees with the hand reading it, and there is no per-function show/hide
box to lose track of. `TURN` gives the angle the hand travels, `PERIM_SPEC` the scale it reads.
`canonDesign` collapses a design that names several functions (an old preset or design ID)
onto the one its ring points at, and strips every perim field when the choice is `none`.

A dial with a ring pulls its whole chapter print inward by the one factor `perimK`, and the
hand length reads the same factor, so the two can never drift apart. `perimShrink` reaches
1.00: at full chapter size nothing is left out there to print, which is how the ring is
switched off without losing the hand (the tip then reverts to the short `CAL_DIM` reach
inside the numerals).

`perimGeom` is the single source of ring geometry: `out` is the reed,
`tick1 = out - spec.tl`, `lab` sits inside the ticks, `inr` closes the band inside the labels.
`perimSize` scales `fs` and the band follows it, so the seven day names can be spread across
the rim without touching the chapter print. `perimPos: 'between'` prints the labels half a
division on, and the hand of that ring is offset by the same half in `buildHands`, so the
pair always reads together. `perimColor`, `perimFont` and `perimOrient` set the ring the way
the other dial elements are set; `perimFont: 'same'` falls back to the dial typeface.
`perimRot` turns the finished ring about the dial centre, which is where the first day of the
week, the first of the month, or hour 24 sits — 0 puts index 0 at 12 o'clock. `perimGeom`
folds the between-the-lines half division and that turn into the ONE number `offTurn`, and
`buildHands` adds it to the turn of the hand whose own ring prints, so ticks, labels and
pointer can never desynchronise. `ringTop(spec, rot, between)` names the label nearest 12 for
any turn, and `ringStarts(spec, between)` turns that around: it yields the chips the composer
offers (`MON at 12`, `1 at 12`, …), each carrying the exact turn that puts that label there.
Small rings (n ≤ 12) get one chip per division; a 31-division ring offers its quarters only.
Both come off the same `PERIM_SPEC` the print reads, so the control row cannot disagree with
what the rim ships. Adding a fourth ring means adding one `PERIM_SPEC` entry; nothing else
has a magic number in it.

`export-test.html` checks the contract that holds this together on both sides
of the wire:

- `canonDesign` collapses a design that names all four functions onto the one its ring
  reads, and a design with no additional hand keeps none of the perim fields;
- for each role, `canonDesign` derives the matching ring and the face builds exactly ONE
  centre pointer: on the centre arbour, above the seconds hand, with real ink in its sprite,
  reaching `perimGeom.reach` within 7 px;
- `gmt` must NOT share the `hours24` turn, and must wrap past midnight in whole hours
  (`@18:00 +5h` = 23/24, `@20:00 +5h` = 1/24);
- printing a ring must actually change the dial in the 198–222 px band, and
  `perimShrink 1.00` (the ring's off switch) must not; the 31 > 24 > 7 ink ordering holds;
- each ring display control moves the printed ink on its own (Label size > 400 px, radial
  orientation, ring colour, ring typeface, `perimPos: 'between'` and a 90° `perimRot` each
  > 200 px — a 1° turn still moves > 20), and `perimPos: 'between'` moves the hand's turn by
  half a division with the labels;
- `perimGeom.offTurn` adds a quarter turn for `perimRot: 90`, wraps a negative turn into
  0–360, stacks with between-the-lines, and the same turn lands on every ring hand's
  `turn(date)`; every `ringStarts` chip really brings its named label to 12 o'clock;
- every centre pointer ships a `drive` formula in the manifest and
  `deviceReady: false`. The firmware's `prep_watch_kit.py` accepts only
  hour/minute/second, so a device that claimed these were drivable would draw
  a dead hand. The subdial `date`/`day` hands must NOT carry that metadata,
  and `mount` (not `kind`) is the only unique key, because a watch can carry a
  date subdial and a centre date pointer at once. The manifest's perimeter
  block also declares `labelPlacement`, `labelOffsetDeg`, `orientation`,
  `typeface`, `colour`, `index0Deg`/`rotationDeg` (the ring turn the drive
  formula carries) and `fontSizeMm` (the print's real size on the panel), so a
  between-the-lines or turned ring ships its drive formula already carrying the
  correction.

`probe-geom.html` measures the same claims instead of re-deriving them: it
renders each case twice, hand hidden and hand shown, and diffs the frames along
the hand's own ray. A tip belongs in `[tick1 .. out]`; with the ring switched
off it belongs inside the chapter numerals (`CAL_DIM` lengths are tuned so the
four heads land at four different radii rather than one clump). The cross-ray swath
it samples is ±24 px, wide enough to see the two horn tips of the crescent,
which sit about 0.9 R off the ray — a narrower swath reads the crescent as
SHORT because its middle is scooped out. The turned-ring cases measure along the
TURNED ray (`turn + perimRot/360`): if print and pointer desynchronised the walk
would find no ink at all, so `tip=0`/SHORT is the failure signature there, and
`between + 45° turn` proves the half-division and the rotation stack.

## The crescent opens outward

`handParts()` builds the crescent tip as the outer disc less a second disc
offset along the hand's own axis, so the opening faces the scale it indicates
and the tick being read sits in the cup between the horns. Two consequences
that are easy to break:

- the ornament is hung off the **horn** line, not the disc centre
  (`cy = -len - horn`, `horn` from the two-circle intersection), because the
  reach only means something where there is ink to put the tick under;
- the needle ends at `ny`, which for the crescent is tucked into the belly
  under the opening. Ending it at the disc centre prints a wedge into the cup.

## Composer UI contract (one additional hand, one ring)

What the spare hand indicates is the only calendar choice the studio makes, and it is the
same control on both tabs. `Watch Composer.dc.html` keeps that in four places, and
`export-test.html` `uiCheck()` drives the real control rows to pin it:

- the Dial tab group **Additional hand · outer ring** opens with one `Additional hand`
  dropdown: `None` plus the four roles, each option label carrying the split count read from
  `PERIM_SPEC` through `perimGeom`. There is no separate ring "Scale" dropdown to disagree
  with it, and no show/hide row per calendar function.
- the Hands tab carries the identical dropdown beside the hand list, so the pair is never
  split across two tabs. Both write `d.extra`; `canonDesign` derives `d.perim` from it.
- choosing a role switches its pointer on and prints its ring in the same click; taking it
  back to `None` retires hand and ring together, and the hands list drops the row with it
  (`handSlots` lists a calendar pointer only while it is the chosen one). The row's button
  reads `remove`, not `shown/hidden`, because that is what it does.
- **Outer ring** (chips: Printed on the rim / Hidden) is the ring's display switch, driven by
  `perimShrink`: Hidden sets 1.00, and Chapter pull-in therefore reaches 1.00 rather than
  pegging at 0.99. Below it sit the ring's display controls — Ring colour, Ring typeface
  (Same as dial + every typeface), Orientation (Bottom aligned / Circular), Label size
  (0.6–2.6×, its readout quoting the real millimetre height the print gets on the panel) and
  Position (On the lines / Between the lines) — the same kind of control set the numerals
  get. Between the lines moves the labels and the hand that reads them together.
- **Ring start** (one chip per division, `MON at 12`, `1 at 12`, …) and **Turn** (0–359°) set
  where the first division sits; both write `d.perimRot`, and the note under them names the
  label that ended up at 12 o'clock. `WatchEngine.ringStarts`/`ringTop` derive chips, note and
  print from the same `PERIM_SPEC`, and the pointer follows through `perimGeom().offTurn`.
- a ring sets the pointer's tip (`perimGeom.reach`); the pointer's Length slider then trims
  off that 100 % reference rather than being overridden.

## The stage is the watch, not the artwork

The panel is 466 px across 1.75 in, so one dial px is 0.095 mm; a preview drawn at the
artwork's own 466 px flatters small type — that is how a dial with 3 px day names ships.
`watch-engine.js` keeps the mapping (`PANEL`, `MM_PER_PX`, `pxMm`, `REAL_CSS`, `REAL_Z`,
`RULER_MM`, `realCss`, `rulerPx`, `screenInch`) so there is one copy of each number, and the
stage under the dial offers three modes (`viewOpts`, `viewVals()`, caption in `viewText`):

- **True size · 1.75 in** (default): the canvas is laid out at `realCss(cal)` CSS px and the
  bezel ring scales with it. This is the only preview that tells the truth about legibility.
- **Full · 1:1 · 466 px**: the old stage, kept because authoring detail at 168 px is impossible;
  the caption says how many inches it really is and how far that is from the watch.
- **Magnify** (1–4×): the window holds 466 px and a bigger frame scrolls through it
  (`stageFlow: 'auto'`). Every layer is baked at 466 px, so this enlarges the artwork and adds
  no detail — the caption and the button title both say so rather than pretending otherwise.

A page cannot measure its own screen: Chrome draws 1 CSS px as 1 device px at 100% zoom, so the
nominal 168 px is 1.75 in on a 96 ppi monitor and under an inch on a dense one, and browser zoom
shifts it again. **True size therefore carries a screen calibration**, and it is the only view
that shows the instrument:

- a 40 mm ruler (`RULER_MM`) drawn in CSS px at the calibrated scale, with mm ticks, taller
  marks and numerals every 10 mm;
- a slider 0.5–3×, `−`/`+` nudge buttons, and a readout of the factor and the implied
  css-px-per-screen-inch;
- a *the ruler measures ___ mm → solve* field, which is the fast path: match the bar to a real
  ruler, type what it reads, and the factor is computed rather than hunted for.

`cal()` clamps to 0.5–3, and the factor is stored under `wc.viewCal` in `localStorage` (guarded:
file:// and private mode can throw) because it describes the monitor, not the dial. It must
never reach `designId()`, `opts()` or the exported kit — `export-test.html` asserts exactly that
plus the invariance that makes the whole scheme honest: for any calibration, `screenInch(realCss
(cal), cal)` stays 1.75 in and the ruler box stays 40 mm, so calibration changes whether the CSS
px realise the claim, never the claim itself.

`export-test.html` `uiCheck()` pins the rest: the stage opens at true size, the caption agrees
with the box, ruler/slider/nudges/solve/reset all move the one factor, an empty or absurd ruler
reading is refused, Full reproduces the old 466 px clipped circle without a ruler, and Magnify
keeps the window at 466 px while the frame grows and the scrollbars appear.

The studio keeps the watch visible while the options scroll: the outer shell is
`height:100vh;overflow:hidden`, the dial column and the options column scroll
independently, and the tab bar is `position:sticky` inside the options column. Both
scrolling columns get the same thin dark scrollbar the palette uses (`scrollbar-width:thin`
plus `::-webkit-scrollbar` rules in the `<style>` block): a 10 px track on the page
background with a rounded, inset thumb, never the default bright gutter.

## What the date window ships, and why the shade is a separate layer


The day-of-month window (`shows:'date'`) is assembled on device from ten digit
tiles plus the window's plate, instead of 31 whole-value PNGs. Measured on the
`modern` preset: 14 files / ~8.6 KB, where the 31 sprites cost 31 files / 28.9 KB,
because only ~4.5k pixels of the 40k in that set ever differ.

Weekday and month stay whole-value sprites: `SUN…SAT` needs 14 distinct letters and
`JAN…DEC` needs ~19, both more files than the 7 / 12 word sprites they replace.

The window's shade — frame ring, its drop shadow, the aperture's inner shade —
is a function of **position in the window**, not of the glyph. `probe-shade.html`
on a 30×22 date hole reports a 12 px digit cell averaging α 81 in the middle of
the strip against α 102 near either cap (spread 27.5/255, ~34 % darker). A moving
digit therefore cannot bake the shade into its own tile, and it cannot be a
fixed left/right cell either, because the run is centred: `5` sits in the centre
cell while the `2` of `28` sits in the left one. So each window kit ships ONE
shared `shade.png` at full window rect, and the assemble rule in `manifest.json`
draws it LAST, over the finished strip.

To keep the ink under exactly one shade, `textKits()` cuts the pieces from two
plates: caps and slice from the engine crop (shade included — nothing is drawn
over them), and the glyph band from the same crop with the host layer subtracted
inside the hole path, so a tile's ink is composited over the bare disc.

## Custom art: what the five slots must keep doing

Uploaded art reaches the kit through five places, and each has a failure mode that is
invisible on screen:

| Slot | Rule that must hold | Where |
|---|---|---|
| Plate backdrop | drawn at the plate slot, so it is baked into the base frames and costs nothing per tick | `watch-engine.js` `movement()` |
| Dial surface art | drawn into the dial canvas AFTER the surface and BEFORE the chapter print, so the user's numerals, markers and ring still paint over it and openwork still cuts it out; ships inside `layers/dial.png`, never as its own layer | `buildDial()`, `artInDial()` |
| Hand sprite | answers the procedural contract `{c, px, py}` and suppresses the metal hub cap; Length 100% is normalised on the ink, not the image: the sprite is scaled up or down until its farthest ink along the facing's axis lands at the reach the drawn hand of that role has (`inkTipOf`, one alpha scan per asset and pivot, from a ≤512 px copy) | `handSpriteAsset()`, `inkTipOf()`, `buildHands()` |
| Transparent sub-dial | ships no layer and no `drawOrder` entry; its holes, flares and hosted comps move to the dial | `layout()`, `buildSubdials()`, `exportKit()` |
| Any asset | the design stores a reference; only the `.wckit` and `sources/` carry bytes | `canonDesign()`, `kitAssets()`, `exportBundle()` |

`export-test.html` asserts these on a live kit build (`artKitCheck`): that the plate
pixels change when a backdrop is placed, that the dial pixels change for the surface art,
that the print ink measured from the same dial with the print blanked stays put under the
surface art (art laid OVER the print moves that whole set), that `cross` openwork removes
the surface art from the cut-away rather than leaving it in the hole, that a
`viewBox`-only SVG is stamped to 200x100 rather than decoded at Chrome's 300x150 fallback,
that texels below alpha 6 are gone from the stored bytes, that an upsample is refused with
its scale named, that the transparent plate is absent from both `layers/` and `drawOrder`,
that the manifest names the plane each backdrop was baked into, and that the `.wckit`
round-trips the bytes into a browser that never held them.

The headless `headless-engine.test.js` proves the same ordering without pixels: the canvas
stub logs its op sequence, and the art's `drawImage` must be indexed before the first
numeral `fillText` on the dial canvas.

## Headless suites (no browser)

`headless-test.js` runs the decisions that a pixel diff cannot see, against stubbed
canvas/DOM — what a design serializes, what a kit ships, what the control row offers,
and that nothing throws when an asset reference dangles:

```sh
python3 -m pip install --user --break-system-packages nodeenv \
  && python3 -m nodeenv --node=lts /tmp/nodeenv
/tmp/nodeenv/bin/node headless-test.js          # or one suite: ... headless-test.js ui
```

| File | Checks |
|---|---|
| `headless-engine.test.js` | `canonDesign` art defaults vanish (Design IDs stay stable), transparent-subdial layout, registry, fit gate (incl. the zoomed box), sprite pivot scaling, centroid + ink-bbox maths, a pivot outside the ink, the length-normalised sprite fit (big uploads shrink, small ones enlarge, zoom rides on top, the measure follows the facing's axis, an ink-free asset keeps the old box fit, and the minute hand's tip lands where the drawn one reaches at every slider value), the aperture frame metal (`hole.fc` plumbing, gilt is the no-choice and never reaches the Design ID, non-framed styles drop the field), the pick stack, the no-case opening (the layer records zero drawing calls; `artClipR` opens the art clip to the panel there and keeps the dial circle elsewhere; `dynScale` eases the DRAWN content into the ring and `compose` turns the dynamic layers through it, proven on the recorded geometry — and every hand shadow cast is the full-opening cast times `dynK`, proven on the recorded device-space offsets), the hands-row eye on a centre hand, a round aperture |
| `headless-component.test.js` | localStorage save/load, a blocked store admitted honestly, `manifest.assets[]`, `.wckit` round trip (incl. off-art arbour + facing), loader refusals |
| `headless-ui.test.js` | the control row offers each field, the hands-row eye hides and shows a centre hand through the engine and reads back on the row, the build-gate seatbelt re-arms on a seconds-rate flip but still caps one rate, drag arms the right target, the pane jumps with the selection, the click cycle walks the stack, a complication already picked on the tab wins the pixel through the hand painted over it (and the same-spot cycle still reaches that hand), a pointer-down on the stage surround deselects while a bubbled canvas click does not, the ghost's picture turns with its facing while the read ray stands at the time angle, the frame-metal row appears only on styles that strike a ring, the guide draws what it claims |
| `headless-regression.test.js` | all 15 presets still build; procedural sprites keep `{c, px, py}` |
| `headless-orbtheme.test.js` | the in-browser `.orbtheme` baker held against the Python that defines the format: the geometry blob, the pixel bank and keyframe, the container bytes, PIL's `alpha_composite` integer maths on 14k sampled texels, the device caps and size rule, PNG decode, and the whole shipped kit baked both ways (bench `prep_watch_kit.py` + `bake_clock.py --orbtheme` versus `GorbBake.bakeKit`), byte for byte apart from the bake stamp |

`headless-orbtheme.test.js` needs the bake venv as well as node, because its reference
side runs the real Python (`pillow` + `numpy`) and compiles nothing:

```sh
python3 -m venv /tmp/bakevenv && /tmp/bakevenv/bin/pip install pillow numpy
ORBTHEM_VEC=/tmp/orbtheme_vectors /tmp/nodeenv/bin/node headless-orbtheme.test.js
```

Its real-kit section caches the bench bake in `/tmp/orbtheme_e2e` and reuses it until
`assets/sources/watch-orbital-kit3.zip` is newer than the cache, so deleting that
directory is how you force a fresh bench run.

The stub records path/turn calls in a second log, `ctx.geom`, deliberately NOT in
`ctx.ops`: the ops cap is sized for the `drawImage`/`fillText` assertions the older
suites make, and folding geometry in there would evict those entries from a busy
render. Read `geom` to assert that an overlay was actually drawn, in what order, and
at what radius.

`pivot_passthrough_check.py` is the bake side of the same features, run offline with
no browser at all:

```sh
python3 tools/watch-composer/_selftest/pivot_passthrough_check.py
```

It calls `prep_watch_kit._placed` and `bake_clock.snap_pivot_to_hub` directly and pins
the two behaviours the composer now depends on: a declared arbour outside the artwork
is placed as given and returned untouched (there is no hub ink to measure, so the
scorer reads zero wobble and the pass-through must not be confused for a disabled
snap — the file asserts both halves), and `zeroRotationDeg` reaches the theme record at
tenth-of-a-degree precision, so a facing off the 15-degree steppers survives.

These are stubs, not a renderer — gradients, shadows and text metrics are no-ops, though
the 2D context now tracks the real CTM (so `getTransform()` answers under `compose`'s
opening lens) and records each draw's live shadow offsets for assertions. Pixel
truth is still `export-test.html` under Chrome. This box has no browser, so deploy it to
the Windows side and open it there:

```sh
python3 ../deploy_test_folder.py     # -> /mnt/c/Users/greg/Downloads/test, prints both file:// URLs
```

`make_art_kit.py` turns a real kit zip into a custom-art kit (transparent sub-dial,
art-backed hand, translucent case) so `prep_watch_kit.py` and `bake_clock.py` can be run
end to end offline. Run the bake from outside the repo root: it also writes
`assets/clock_assets.h` into its cwd, and that file is tracked here.

## Regression guards

The width assertion is the guard for the bug where `manifest.json` declared a
`currentWidth` narrower than the widest `byDate` run —
`capL.w + capR.w + 2·padX + Σ advance` exceeded it and clipped a glyph. Formula
lives in `watch-engine.js` `WatchEngine.textKits()`.

`export-test.html` additionally assembles all 31 days the way the manifest tells
the device to and diffs each against the engine's own render of that window:

- **placement is fatal** — the best-fit integer offset over the aperture must be
  0 px for every day, which is what catches a run that lands off-centre or a
  missing pad.
- **ink brightness is reported** — the residual after placement is exact. It is
  fatal only under `?STRICT_VISUAL=1`, which keeps the default run green while
  leaving the gap in the report instead of hiding it. Add that flag to CI once
  the shade ordering is proven byte-equal.
- `?DEBUG=1` appends base64 PNGs of the reference, the assembled strip, the
  strip without its shade, the shade itself, a cap and a digit tile. Grep them
  out of the dumped DOM, decode, and compare images directly — that is how the
  placement/shade split above was found.

Both load `../dial-renderer.js` and `../watch-engine.js` from the project folder.
`export-test.html` additionally needs `component.js`, because the DC app boot React
from unpkg and that network bootstrap is unreliable headless. `component.js` is the
`<script type="text/x-dc">` body of `Watch Composer.dc.html` wrapped in a factory, so
regenerate it after editing the component:

```sh
python3 - <<'PY'
src = open('../Watch Composer.dc.html', encoding='utf8').read()
i = src.index('<script type="text/x-dc" data-dc-script>')
j = src.index('</script>', i)
body = src[i + len('<script type="text/x-dc" data-dc-script>'):j]
open('component.js', 'w', encoding='utf8').write(
    'window.COMP_CLASS = function (DCLogic) {\n' + body + '\nreturn Component;\n};\n')
PY
```

Run (Windows Chrome from WSL, or any Chrome with `--headless`):

```sh
chrome --headless=new --disable-gpu --no-sandbox --virtual-time-budget=120000 \
  --dump-dom 'file://<win-path>/_selftest/export-test.html'
```

`--virtual-time-budget` matters: `exportKit()` yields between sprite renders, and
without a budget the dump happens before the zip is built. Budget is a TIMER budget
though, and blocks that just grind `getImageData` never yield to a timer, so they can
be dumped mid-run and look stranded with no verdict at all. `?ONLY=feat` runs the
feature block on its own and nothing else, which is how you tell a slow block from a
broken one; the verdict lines it must print are listed in `deploy_test_folder.py`.

The feature block (`featSetup` → `featAperture` → `featCaseZoomHands`) is the pixel
evidence for the authoring controls the stub suites can only describe:

* a round window is measured by how much of its own disc box the disc fills — 0.785
  for a circle against ~1.0 for the square, both measured in the same run so the
  square case calibrates the probe. The same window then goes through the kit door:
  `textKits()` must produce a strip, two caps, a slice, a shade and ten digit tiles of
  one height. A circle hole names radii and never `w`/`h`, so sizing that crop off the
  sides gave `NaN`, a zero-size canvas, and a `drawImage` throw that killed the whole
  export — the watch previewed perfectly and downloaded nothing, which is what an
  importing author hit;
* "no case" paints **no pixel at all** — no mask, no rim shade — and the composited art is
  the same pixels with that layer shown or hidden, at opening 221.5 and at 150, at the same
  box: the opening moves every other pixel and leaves the uploaded image exactly where the
  author put it. Its own uploaded image also reaches the panel corners there (59,571 px of
  it outside the dial circle in the current run), because that image carries the device's
  case; under a drawn case the same art stays clipped to the dial circle (0 px outside it),
  where the metal does the covering. The drawn pieces do follow the ring: hand ink beyond
  r=158 falls from 104 px at the default opening to 0 at 150 (`dynScale`);
* the hands' **projected shadows stay pinned** while the opening shrinks. Canvas draws
  shadows in device space — offsets and blur ignore the CTM — so `drawHands` pre-scales
  every cast by `dynK` itself (`devS`). The probe measures the dark low-alpha cloud around
  hands-only ink at both openings: the cloud must survive at ~dynK² of its pixels and its
  centroid offset must land at dynK× the full-opening offset, or the hands read as lifted
  off the dial;
* the Motion chips survive a **4/s → sweep → 4/s round trip** on Broad arrow (a
  15-fps-capable config). Each flip re-prices the tempo and needs its own rebuild; the
  build-gate seatbelt counts passes per art **and** wanted rate, and the frame timer only
  advances while `gt.length === B`. The check drives the real lifecycle and requires the
  train to still be advancing frames after every flip — the reported bug froze it on the
  third;

* the seconds hand is authored with the **seconds** list, not the hand-style list. Both are
  measured on the sprite the kit ships (`hd.sp.c`): `classic` 801 px against `lollipop` 1060 px
  proves the seconds list reshapes it, while none of six hand styles moves it by a single
  pixel (breguet/spade cut a hollow counterweight there and the dark underlay strokes it back
  in), and the hour hand on the same probe moves 342 px between `poire` and `leaf`. That is
  the whole reason the hands pane swapped the two dropdowns on that row;
* zoom 1→2 multiplies the art actually painted, and at zoom 4 nothing spills past the
  dial circle, which is what proves the clip is applied after the zoom and not before;
* a facing of 90 turns the drawn ink a quarter of the way round the arbour, and a
  pivot 30 px outside the artwork grows the plane so the arbour is inside it;
* an uploaded hand's **Length 100% is the drawn hand's reach, not the upload's
  resolution**. One bar is uploaded twice — tight-cropped and buried in transparent
  padding at a different scale — and both sprites, plus the procedural minute hand of
  the same dial, must tip within 2.5 px of the same radius off the arbour, read from
  each sprite's own alpha; 50% Length draws half that reach, and the composited hands
  layer must show it at the same pixels. The old box fit capped every upload at 1:1,
  which is the bug this answers;
* the same window struck in **steel instead of gilt** moves a few hundred dial px and
  their red-minus-blue mean falls from ~+80 to ~0 — the frame ring is the author's
  metal now, and the comparison is a two-face difference so the dial underneath cannot
  fake the pass;
* a finish **blended over art** is measured on both sides of the seam. One fixture is red
  on one half and a green 148 levels away on the other; with `Multiply` and no key both
  halves move, and keyed to the red the keyed half still moves (14121 px) while the other
  moves **0**. Only pixels can tell those two apart: in a call log a texture that leaked
  across the seam and one that never reached the keyed half both look like a pass that
  drew its geometry. The same block pins the tolerance window from both ends (a key 30
  levels off the art's colour moves 0 px at tolerance 8 and 53816 px at 60), confirms a key
  absent from the picture moves nothing, and confirms `enamel` under a blend still moves 0;
* the alignment reference — hub cross and the 24 spokes out of it — is painted at the
  same pixels with the dial art at offset 0 and at offset 120. The stub suites can say
  the strokes were asked for; only the pixels say the author has something stationary to
  line an off-centre graphic up against. The hand ghost's **read ray stands at the angle
  the clock shows** while the facing swings the dashed picture under it — the headless
  ui suite pins that geometry, so a facing change can never take the sightline with it;
  and grabbing works sticky: a complication already picked on the tab wins its pixel
  through the hand painted over it, while a click on the black around the watch lets go
  of everything.

Two probe-design traps bit this block and are worth remembering, because both produced
a confident wrong answer rather than an obvious one:

* sample the DISC's box, not the hole's. The disc overshoots the aperture by `5*k` on
  every side so its edge tucks under the frame; at `k = 2.2` that is 11 px over a 33 px
  hole, and a correct round window measured 0.994 — indistinguishable from a square —
  when sampled on the hole.
* comparing two dial angles needs a shortest-signed-difference helper, not
  `(b - a + 540) % 360 - 90`, which reported a real 90.4° turn as a failure.

The width assertion in `export-test.html` is the regression guard for the bug where
`manifest.json` declared a `currentWidth` narrower than the widest `byDate` run —
`capL.w + capR.w + 2·padX + Σ advance` exceeded it and clipped a glyph. Formula lives
in `watch-engine.js` `WatchEngine.textKits()`.

## The .orbtheme block (`themeCheck`)

`headless-orbtheme.test.js` proves the baker matches `prep_watch_kit.py` + `bake_clock.py`
byte-for-byte, but it feeds both sides the SHIPPED kit's PNGs. Only Chrome can prove the
chain the export button walks: canvas -> `toBlob()` -> `DecompressionStream` ->
`GorbBake.decodePngAsync` -> `bakeKit`. So `themeCheck` bakes the files the suite already
collected and reads the container back the way `orb_theme.c` does — magic, format, ABI,
the 466x466 panel, the sector-aligned payload start, and a re-CRC of the header (with its
own checksum word zeroed), the geometry blob and the pixel payload. It then demands the
button's bytes equal a fresh `bakeKit` of the same planes byte for byte, which is what
catches a page that handed the baker a plane it had already touched.

The block exists for one texel. A canvas composited onto a premultiplied backing store
would bake a washed-out plate through that chain and throw nothing at all, so the block
paints `rgba(200,60,20,alpha 128)`, encodes it with Chrome's own encoder, decodes it back
and compares against `getImageData`: 100 levels of red separate pass from fail, so no
tolerance can hide the bug. Two refusals close it out — a dirty-but-rescuable slug must
download under the normalized name the header itself carries, and `***` plus a missing
`window.GorbBake` must report rather than ship a container.

All three gates are mutation-verified: premultiplying the decoder's RGB, flipping the last
payload byte after its CRC was taken, and renaming the download each turn the block red
with its own message.

## Assertions that never ran

Three checks in `export-test.html` were committed red and stayed green-looking,
because the block that owns them threw before reaching them and the exception died in
a `setTimeout` callback with no handler. `artKitGo` now runs inside a `try/catch` and
`featSetup` likewise, both reporting through `fail()`, so a throw there is a verdict
instead of a page that simply stops printing. The specific traps:

* the art fixture ran on `state.dial = 'art'`, an id with no entry in `PRESETS`, and
  `handDef` dereferences `preset(id).hands` — so `face()` threw for the whole block.
  A harness instance must use a **registered preset id** and key `state.designs` by
  it; `face(id)` takes that id, not an options object, and per-face options come from
  `state`, so a probe that wants `caseStyle`/`caseOpen` set has to assign them on the
  instance.
* `assetFits(id, 40, 40, 'contain')` on an 8×8 source is a 5× upscale and is refused,
  because contain into a square box from a square source *is* cover. An upscale-gate
  assertion needs an aspect that differs from the box, and `manifest.assets[]` is
  keyed by asset id — one image filling four slots is legitimately one entry.

If a block prints no `say()` line at all, suspect an exception, not a pass.

## Running the probes

```sh
# measurements, as text
chrome --headless=new --disable-gpu --no-sandbox --virtual-time-budget=600000 \
  --dump-dom 'file://<win-path>/_selftest/probe-geom.html'
# the dial grid, as an image to look at
chrome --headless=new --disable-gpu --no-sandbox --hide-scrollbars \
  --window-size=3400,1600 --virtual-time-budget=600000 \
  --screenshot='C:\...\probe.png' 'file://<win-path>/_selftest/probe-calendar.html'
```

The full-bleed-upload ring check that `probe-case.html` used to provide is folded into
`export-test.html`'s no-case block: it composites a solid-colour full-bleed upload under
Case "None" at openings 221.5 and 150 and prints the art pixel count with the case layer
shown and hidden. Both settings must print the same ink box, and the polished control line
says the mask still exists where a case is actually drawn.

`probe-geom.html` renders 39 full dials, so give it a large virtual-time budget;
frames are cached per design. Re-pose both probes by editing `POSE`.

The tip ornaments in the needle-hand family carry a lozenge counterweight
outboard of the arbour on purpose. Without it the needle reads, at 1-bit
e-ink resolution, as a decoration falling off the end of the hand rather than
as its head.
