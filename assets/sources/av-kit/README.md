# av-kit — the source of the compiled-in clock art bank

This is the Watch Composer layer kit that `assets/clock_assets.{bin,h}` was baked
from. `assets/clock_assets.h:3-4` names it: `Source theme: av-kit`, `Source
background: /tmp/p1/av-kit/frames`. The kit itself was never committed until now, so
the shipped bank had no source in the repo — only a `/tmp` path on one machine.

| File | What it is |
|---|---|
| `watch-aviator-kit.zip` | the composer's kit export, byte-faithful, sha256 `bfbe99a8…d679f3d3`, 6,318,355 B |
| `DESIGN_ID` | the `WCD5-` seed that regenerates the design; identical to the zip's `manifest.json` `designId` |

Composer preset `Aviator`, 15 base frames at 15 fps, no tourbillon. `main/art/art_bank.c:638`
calls this bank "the av-kit primary (18 pages)" and pins its size at
`LAT_BYTES_PRIMARY 1173563u`, which is `CLK_BLOB_BYTES` in the committed header.

## How to rebuild the bank

`theme.json`'s name comes from the output directory, so use `av-kit` there or the
header's provenance line will read differently.

    /tmp/bakevenv/bin/python tools/prep_watch_kit.py assets/sources/av-kit/watch-aviator-kit.zip /tmp/p1/av-kit
    /tmp/bakevenv/bin/python tools/bake_clock.py /tmp/p1/av-kit/theme \
        --bg-frames /tmp/p1/av-kit/frames --bg-fps 15 --tick-rate 15 --force-sweep \
        --bin /tmp/out.bin --header /tmp/out.h

Pass `--bin`/`--header` outside the repo root, or you overwrite the committed pair.
The bake needs a venv with `pillow numpy`; nothing in this repo pins those versions.

## What "reproduces" means here, measured 2026-10-12

- **Header: exact.** 96 code lines, 0 differing, geometry and rect tables included.
- **Blob: same size to the byte** (1,173,563) and **59 differing bytes**, 0.005%.
  They sit at one-scanline stride (466 x 2 = 932 B) along one rect edge, i.e. a
  1-px antialiasing fringe: Chrome's rasterizer stepped a half-pixel differently
  between the build that exported this zip and the one re-rasterizing it.

That residual is why the zip is committed instead of re-exported on demand. The
pixels come from Chrome's `canvas.toBlob('image/png')` in `exportKit()`
(`tools/watch-composer/Watch Composer.dc.html:1583`), so kit bytes are only as
reproducible as the browser build that made them. The Python half — `prep_watch_kit.py`
then `bake_clock.py` — is deterministic: the same inputs baked twice give byte-identical
output. A headless export hook (`window.__composerExport`, planned as P2 in
`docs/watch-composer-plan.md:97`) would close the remaining gap.

Baking from a *different* kit is how the old claim in `.clinerules` went unnoticed:
the kit3 recipe yields 3,858,874 B, not this file. Do not re-bake the shipped bank
without comparing the header first.
