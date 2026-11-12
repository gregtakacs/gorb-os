# overlay-3: each app's editor sets the theme's overlay colour

> **Read first: `docs/specs/overlay-design.md` §7 and §8 (check 6); then `overlay-1`'s contract (the
> byte this step fills in).** Base `RING_TIP` + overlay-1. Independent of overlay-2: an editor's
> field is a byte, not a behaviour.

## Goal

One field per themed editor — the kit's colour picker, in each app's own look step, defaulting to
that look's accent — and one derivation function per app so a design that never touches the field
still bakes a concrete colour. After this step a bake of any of the four apps writes a `0x01RRGGBB`
overlay word into its container's index head, and the owner can see and change the colour the OS
will ring the app with.

## Read first

- `docs/specs/overlay-design.md` §7 (the table: whose accent, which step, why not radar's
  `overlay` step) and §8 check 6.
- The kit's picker: `tools/orb-manager/web/runtime/studio@1/studio.js` — `fieldKit()` (:409) and
  `Co` (:641), its `X = {noAuto, preset, none}` options, the draft/hex handling and the preset chip
  ("Back to the preset's #3ddc84"), and `S.PALETTE_KEY`/`S.PALETTES` for what the swatches offer.
- One existing usage per shape to copy: `apps/radar/editor/radar-editor.js:78` (the app's local
  `C()` wrapper over `K.Co`, with `noAuto: true` and a `preset`), and `:152` (a `K.Co` straight onto
  a rain-ramp colour, i.e. a field whose preset lives elsewhere).
- Each app's bake module: `apps/radar/editor/radar-bake.js` (`PRESETS` :39, each look's `glass.ring`,
  `d.base = P.base` :99, `preset(k)` :273, the `St.finish` call :698, the exports at :712),
  `apps/weather/editor/weather-bake.js` (`PACKS` :124-130 with each pack's `roles`, `PRESETS_DEF`
  :136-176, the `roles` read at :216, the `S.finish` call :839),
  `apps/flights/editor/flights-bake.js` (`WORDS` :115-121, `d.scope.ground` :178, `S.finish` :1026), and
  `apps/clock/editor/clock-logic.js`'s two `G.bakeClock3(opts)` call sites (:2688, :2996) with
  `apps/clock/editor/watch-engine.js` (`canonDesign` :5106, `bezelInk`'s rule :5114, the wheel
  `accent` rule :1393 and the engine's fallback colour :1498).
- The gates: `apps/<app>/editor/_selftest/bake_check.py` (each reads its baked theme back through
  `orbbundle`/`theme_schema`), `tools/orb-manager/web/runtime/_selftest/headless-test.js`
  (`make check-editor`), and `tools/rebake_shelf.py --check`.

## Contract

**One derivation per app, exported, and one call site each.** Named `overlayOf(design)` on each
app's bake module (`RadarBake.overlayOf`, `WeatherBake.overlayOf`, `FlightsBake.overlayOf`,
`WatchEngine.overlayOf`) so the editor's preset, the bake and the app's own selftest all read the
same rule:

```js
// radar-bake.js — the colour the OS draws its overlays in over this look (§64.9).
var HEX = /^#[0-9a-f]{6}$/i;
function overlayOf(d) {
  if (d && HEX.test(d.overlay)) return d.overlay;
  var g = (d && d.glass) || preset(d.base).glass;      // the look's own accent, live or preset
  return HEX.test(g && g.ring) ? g.ring : '#3ddc84';
}
```

`preset` and `d.base` are radar's own (`radar-bake.js:273`, `:41`), so the fallback is the same
look-picking the rest of the file does.

`weather-bake.js` takes the accent **role** out of its own pack table rather than hard-coding
`warn` — two of the six packs have no `warn` key (`PACKS.nebula`: `orange` is the lightning,
`PACKS.pictogram`: `red` is the accent) — so `overlayOf(d)` finds the first entry of
`PACKS[d.icons.pack].roles` whose label is `'Lightning'` or `'Accent'`, reads that key from
`d.icons.c`, and falls back to `#FFB23C`. It is the same `roles` read `:216` already makes, so the
pack table stays the only place a pack's colours are named. `flights-bake.js` answers
`d.words.accent`, falling back to `WORDS[(d.scope && d.scope.ground) || 'scope'].accent` —
the ground key is `d.scope.ground` (`:178`) and `WORDS` is keyed by it (`:116`). The clock has
no single accent field, so `WatchEngine.overlayOf(d)` follows the engine instead of inventing a
table: the first exposed `wheel` comp with an `accent` (the reading `:1393` and `:1498` already
make), else `d.handLume` when it is a hex, else `'#ff2d55'` — the engine's own fallback colour for
an accent-less wheel. Every one of these answers a `#rrggbb` string for *any* design, including one
with no glass, no pack, no words or no wheels: the bake must never write an absent word for a theme
this repo baked itself.

Each bake then carries the word — one added key in the four options objects that already name the
slug, serial and design: `radar-bake.js:698`, `weather-bake.js:839`, `flights-bake.js:1026`
(`St.finish`/`S.finish({…, overlay: overlayOf(d)})`), and `clock-logic.js`'s two `bakeClock3(opts)`
builders (:2688, :2996), which the engine's `B.pack` forwards at `orbtheme-baker.js:4338`. The clock
also gains `if (!isHex(d.overlay)) delete d.overlay;` in `canonDesign`, beside `bezelInk`'s line at
:5114, so a half-typed hex cannot reach a bake and an untouched watch still serialises to the same
design.

**One field per editor, in the step the look's colours live in.** Same shape in all four, built with
the kit's `Co` and no new kit code:

```js
out.push(K.H('The OS’s overlay'));
out.push(K.Co('Overlay colour', B.overlayOf(d), function (v) { upd('overlay', v === 'none' ? null : v); },
              '', 'overlay', { noAuto: true, preset: B.overlayOf(base) }));
out.push(K.Nt('', 'The colour the orb rings itself in over this app — its progress ring, and later '
                  + 'its copy arc and alerts. This look’s own colour is the default; the chip puts it back.'));
```

`noAuto: true` because there is nothing to follow — the accent *is* the preset — and `preset` so the
picker's chip can restore the look's colour after the owner has typed over it. `radar-editor.js:78`'s
local `C()` wrapper is the pattern; it is not reused here because its `preset` comes from
`getIn(base, path)` and these designs' presets carry no `overlay` key. Home per app, per design §7:
radar's `ground` branch of `fields()` (**not** its `overlay` branch, which is the app's own glass
overlay), weather's `ground`, flights' `scope`, the clock's `dial` step beside `bezelInk`. Choosing
"none" stores nothing in the design, which re-derives the accent — the field is never empty on the
glass, only in the document.

## Behaviour and edge cases

- An untouched design bakes a colour, not an absence: each app's shipped default bakes to its look's
  accent, and the region's second head word reads `0x01` + that RGB.
- A design with `overlay: '#2f7bd4'` bakes `0x012f7bd4`, and the picker shows `#2f7bd4` on reopen.
- A design with `overlay: '#zzz'`, `''`, `'#fff'` (three digits) or a number: every `overlayOf` falls
  back to the accent and the clock's `canonDesign` deletes the key. No throw, no bake refusal — the
  picker's draft box can hold a half-typed hex and a bake must survive it.
- The owner changes the look after setting an overlay: the field keeps the owner's colour and the chip
  offers the new look's accent; an authored value is never silently overwritten.
- The owner changes the look's *accent* (a different ring colour, a different bolt colour) with the
  overlay untouched: the baked overlay follows it, because the derivation reads the live design.
- Bake determinism: the same design bakes to the same bytes twice, and `serialOf` (which hashes the
  design document) is unchanged for a design that never sets the key — an untouched theme keeps its
  serial and its identity on the card. A design that does set it changes serial, correctly.
- Radar's `overlay` step gains no field, and its wording still means the app's own glass overlay.
- The clock's design still serialises identically when the field is unset, so `check-migrations`'
  fixtures and the clock's goldens are unaffected — the first thing to check if that suite goes red.

## Acceptance criteria

All run here: node, chromium, python3; no ESP-IDF, no Go.

1. `python3 apps/radar/editor/_selftest/bake_check.py` → green, with a new assertion that the region's
   second head word equals the expected overlay (`0x01` + the scope look's `#3ddc84` for the untouched
   fixture, the owner's RGB for a design that sets it). `orbbundle.file_overlay` is the reader, so the
   check uses the same helper overlay-1's C vector used and not a private `struct.unpack`.
2. Same command shape and same new assertion for `apps/weather/editor/_selftest/bake_check.py` (expect
   `#FFB23C`, phosphor's lightning) and `apps/flights/editor/_selftest/bake_check.py` (expect
   `WORDS.scope.accent`, `#39FF14`).
3. The clock has no `bake_check.py`: its assertion goes in the bundle suite that already round-trips a
   bake, `apps/clock/editor/_selftest/headless-bundle3.test.js`, and runs under `make check-editor`.
   `apps/clock/editor/_selftest/bank3-drive.html` — the shelf bake's driver — gains the `overlay`
   option so `make rebake` can pass it.
4. `make check-editor` → the clock's node suite, the runtime's node suite, the JS-baker parity checks
   and `tools/verify_js_bake.py --rows` all green: that is where a `canonDesign` or `pack` drift shows.
5. `make check-migrations` → every app's schema fixture still opens (needs node + chromium; if the
   sandbox cannot run chromium, say so and run `apps/clock/editor/_selftest/headless-test.js` directly).
6. `grep -q overlayOf apps/radar/editor/radar-bake.js && grep -q overlayOf apps/weather/editor/weather-bake.js && grep -q overlayOf apps/flights/editor/flights-bake.js && grep -q overlayOf apps/clock/editor/watch-engine.js && grep -q 'd.overlay' apps/clock/editor/watch-engine.js && grep -c 'The OS’s overlay' apps/radar/editor/radar-editor.js apps/weather/editor/weather-editor.js apps/flights/editor/flights-editor.js apps/clock/editor/clock-logic.js` → every app gained the derivation and the heading, and the clock gained its canon rule, so this cannot pass by editing one app.
7. A round-trip printed once rather than only asserted: bake a radar design with `overlay` unset and
   again with `#00ff00`, and print `hex(orbbundle.file_overlay(path))` for both — `0x13ddc84` and
   `0x100ff00`.
8. `git diff --name-only -- 'apps/*/version.txt' tools/orb-manager/themes` → empty: this step changes no
   shipped bytes and bumps no version.

## Out of scope

`version.txt` and the shelf's themes (overlay-4, deliberately separate: the shelf's freshness gate is
the design's version stamp, so the byte change and the bump must land in one release and in that
order), the container legs (overlay-1), the OS's read and the ring (overlay-2), the hub and its theme
row, the pomodoro editor, `apps/sdk/orb.h`, and any kit change — `Co` already does the job. No new
editor step, no new tab, no preview of the ring: drawing the OS's ring inside an app's preview is
design §10's suggestion.

## Notes

- The rule lives in the bake, not in the OS, so one word serves five apps with different ideas of an
  accent. The alternative — store the absence and let the dial resolve the accent — would put each
  app's look vocabulary into `orb_theme.c` and make the word ambiguous.
- The colour is a `#rrggbb` **string** in the design (every editor colour field in this repo is one;
  `argb()` in `wx-map.js:83` shows the conversion idiom) and a **word** in the container. `overlayOf`
  is the only place they meet.
- Choosing "none" stores `null` rather than deleting: the editors' `upd` writes through a path, and
  `null` is how these designs say "unset" elsewhere (`radar-editor.js:79` maps `'none'` to `null`).
- The clock's `#ff2d55` is not this slice's invention: it is the engine's own fallback for a wheel with
  no accent (`watch-engine.js:1498`), so the dial's laser indexes and the ring agree.

