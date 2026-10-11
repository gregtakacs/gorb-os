# watch-composer selftests

Two headless harnesses. Both print a line report into `<pre id="out">` and set
`<title>` to PASS or FAIL, so CI can grep the title from a dumped DOM.

| File | Covers |
|---|---|
| `selftest.html` | engine level: `WatchEngine.textKits()` caps/slice/glyph/byDate output, plus the posed preview frame (`previewDate`, `PREVIEW_MOON`) against new/full controls |
| `export-test.html` | end to end: drives the real `Component.exportKit()` and inspects the bytes of the zip it builds (manifest, `text/<id>_<kind>/` sprites, `preview.png` moon pose) |
| `probe-shade.html` + `probe-shade.js` | measurement probe, not a test: prints the per-row/per-column alpha profile of a window kit's `shade.png` and how a digit-cell's mean shade changes as it slides across the strip |

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
without a budget the dump happens before the zip is built.

The width assertion in `export-test.html` is the regression guard for the bug where
`manifest.json` declared a `currentWidth` narrower than the widest `byDate` run —
`capL.w + capR.w + 2·padX + Σ advance` exceeded it and clipped a glyph. Formula lives
in `watch-engine.js` `WatchEngine.textKits()`.
