#!/usr/bin/env python3
"""Prepare a layered watch-designer kit for bake_clock.py (orbital-tourbillon).

Takes the vendor kit zip (base/ frames, hands/, layers/, tourbillon/, a
manifest.json with pivots/centres) and produces, in <outdir>:

  frames/frame_NN.png  the gear-train base frames with the BEZEL (outer.png)
                       alpha-composited into them, so the opaque rim ships in
                       the delta-rect keyframe + bank and costs nothing at
                       runtime: marked rows inside the rim never occur (no
                       layer's ink reaches r>=220 on this kit — the minute
                       hand tops out at r=199) and the ring is restored from
                       the canvas with the background, in DMA time only.
  theme/               the bake_clock.py theme_dir: hands as
                       clock_hand_{hour,minute}.png, dial as
                       clock_hand_static1.png (z-order puts it OVER the
                       tourbillon and UNDER the hands — kit drawOrder), glass
                       sheen as clock_overlay.png (the overlay slot composites
                       last, above the hands — which is what "glass" means).

Both layers get INVISIBLE-ART ZEROING before matting: alpha values the eye
cannot weigh (bezel's inner tint under r<218, glass sheen below a=6) are set
to 0 so the device skips those pixels (`if (!a) continue`) instead of paying a
PSRAM read-modify-write blend for ~64k near-nothing pixels per commit.

The clock_style.json is written from the kit's manifest (pivots (20,136.32)
and (16,196.92), everything about centre (233,233), tick — the kit's
seconds/60x360 orbit formula is a ONE-DEG-PER-TICK world, never a sweep: the
carriage's swept box is ~430 px tall and a sweep would re-rotate it every
tick, which measures as half the frame rate for zero visible gain over the
1 Hz mechanical cadence).

Requires PIL (use a venv: `python3 -m venv /tmp/bakevenv &&
/tmp/bakevenv/bin/pip install pillow numpy`, same env as bake_clock.py).

Usage: python3 tools/prep_watch_kit.py <kit.zip> <outdir>
Then:
  <venv>/bin/python tools/bake_clock.py <outdir>/theme \
      --bg-frames <outdir>/frames --bg-fps 15 \
      --tourbillon <unzipped kit>/tourbillon --tourbillon-fps 15 \
      --no-sweep --tick-rate 1 --dump-dir assets/preview-orbital
"""
import os
import sys
import json
import shutil
import zipfile

from PIL import Image
import numpy as np

SCREEN = 466


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-3])
    kit_zip, outdir = sys.argv[1], sys.argv[2]
    with zipfile.ZipFile(kit_zip) as z:
        z.extractall("/tmp/_kit")
    K = "/tmp/_kit"
    manifest = json.load(open(f"{K}/manifest.json"))

    ys, xs = np.mgrid[0:SCREEN, 0:SCREEN]
    r = np.sqrt((xs - 233) ** 2 + (ys - 233) ** 2)

    outer = np.asarray(Image.open(f"{K}/layers/outer.png").convert("RGBA"))
    glass = np.asarray(Image.open(f"{K}/layers/glass.png").convert("RGBA"))
    outer2 = outer.copy()
    outer2[(r < 218) & (outer2[:, :, 3] < 30), 3] = 0     # invisible inner tint
    glass2 = glass.copy()
    glass2[glass2[:, :, 3] < 6, 3] = 0                    # sub-visible sheen

    frames = os.path.join(outdir, "frames")
    theme = os.path.join(outdir, "theme")
    for d in (frames, theme):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    outer_img = Image.fromarray(outer2)
    names = sorted(n for n in os.listdir(f"{K}/base") if n.endswith(".png"))
    for n in names:
        f = Image.open(f"{K}/base/{n}").convert("RGBA")
        Image.alpha_composite(f, outer_img).save(f"{frames}/{n}")

    hands = manifest.get("hands", [])
    src = {h.get("kind"): h.get("file") for h in hands}
    shutil.copy(f"{K}/{src['hour']}", f"{theme}/clock_hand_hour.png")
    shutil.copy(f"{K}/{src['minute']}", f"{theme}/clock_hand_minute.png")
    shutil.copy(f"{K}/layers/dial.png", f"{theme}/clock_hand_static1.png")
    Image.fromarray(glass2).save(f"{theme}/clock_overlay.png")

    json.dump({"name": os.path.basename(outdir)}, open(f"{theme}/theme.json", "w"))
    # clock_style.json authored from the manifest (kit: centre 233,233;
    # z-order hour below minute; dial over the carriage, under the hands;
    # tick, not sweep — see module docstring).
    style = {
        "hands": {
            "order": [2, 3, 0, 1],
            "shadow": {"on": False, "dx": 3, "dy": 9},
            "hour":   {"show": True, "pivotX": 20, "pivotY": 136,
                       "centerX": 233, "centerY": 233, "blend": 0},
            "minute": {"show": True, "pivotX": 16, "pivotY": 197,
                       "centerX": 233, "centerY": 233, "blend": 0},
            "second": {"show": True, "pivotX": 233, "pivotY": 233,
                       "centerX": 233, "centerY": 233, "blend": 0},
            "static1": {"show": True, "pivotX": 233, "pivotY": 233,
                        "centerX": 233, "centerY": 233, "blend": 0},
            "static2": {"show": False, "pivotX": 233, "pivotY": 233,
                        "centerX": 233, "centerY": 233, "blend": 0},
        },
        "secondSweep": False, "secondRailway": False,
        "textOverHands": False, "tickRate": 1,
    }
    json.dump(style, open(f"{theme}/clock_style.json", "w"), indent=1)
    shutil.rmtree("/tmp/_kit", ignore_errors=True)
    print(f"prepared {outdir}: frames/ ({len(names)} matted frames) + theme/ "
          f"(--tourbillon still points at the unzipped kit's tourbillon/ dir)")


if __name__ == "__main__":
    main()
