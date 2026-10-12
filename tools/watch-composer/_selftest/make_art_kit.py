"""Build a custom-art kit fixture from a REAL composer kit, so prep_watch_kit and
bake_clock run against byte formats the composer actually emits.

    python3 tools/watch-composer/_selftest/make_art_kit.py <kit.zip> /tmp/artkit
    python3 tools/prep_watch_kit.py /tmp/artkit/watch-artfixture-kit.zip /tmp/artkit/out
    python3 tools/bake_clock.py /tmp/artkit/out/theme --bg-frames /tmp/artkit/out/frames \
        --bg-fps 8 --tourbillon /tmp/artkit/out/tourbillon --tourbillon-fps 8 \
        --tick-rate 8 --bin /tmp/artkit/clock_theme.bin

The kit must be a composer kit that HAS a sub-dial and a tourbillon: this script
strips the sub-dial and re-measures the tourbillon. The bank the clock boots with
(assets/sources/av-kit/watch-aviator-kit.zip) has neither, so it is not a valid
input here. The repo ships no such kit any more; the orbital kits that did were
removed on 2026-10-12, so supply one exported from the composer.

Run the bake from OUTSIDE the repo root, or pass --bin there: bake_clock.py also writes
assets/clock_assets.h relative to its cwd, and that file is tracked in this repo.

What changes from the source kit, and why each one is the point:
  * layers/subdial_*.png is dropped and its drawOrder entry removed: the sub-dial is
    'transparent', so no plate ships -- prep must fall to show2=False with no static2.
  * hands/0*_c3.png is replaced by an art-backed sprite whose ink does not touch the
    canvas edges, and the manifest pivot is re-recorded from its real alpha centroid.
  * layers/outer.png is redrawn translucent everywhere: the case that used to be a
    hard exit in prep ("no opaque pixels: nothing to fold") must now run to completion.
  * sources/ + manifest.assets[] + manifest.backgroundArt + manifest.surfaceArt are added
    the way kitAssets writes them, so both backdrop slots and the bundle contract are
    exercised end to end. The dial-surface art is composited into layers/dial.png itself,
    masked on the dial's alpha so no aperture is painted shut: prep must still merge its
    disc, and the gold field must survive the fold into the static images.
"""
import json, os, shutil, sys, zipfile
import numpy as np
from PIL import Image, ImageDraw

SRC = sys.argv[1] if len(sys.argv) > 1 else ''
if not SRC:
    raise SystemExit("usage: make_art_kit.py <kit.zip> [out_dir] — the kit needs a "
                     "subdial and a tourbillon; the shipped av-kit has neither")
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/artkit"
S = 466

shutil.rmtree(OUT, ignore_errors=True)
import json, os, shutil, sys, zipfile
K = os.path.join(OUT, "kit")
os.makedirs(K)
with zipfile.ZipFile(SRC) as z:
    z.extractall(K)

man = json.load(open(f"{K}/manifest.json"))

# ---- 1. the transparent sub-dial: no plate ships, and drawOrder says so -------------
subs = [d for d in man["drawOrder"] if d.get("layer") == "subdial"]
assert subs, "the source kit has no subdial to make transparent"
gone = [s["id"] for s in subs]
man["drawOrder"] = [d for d in man["drawOrder"] if d.get("layer") != "subdial"]
for f in os.listdir(f"{K}/layers"):
    if f.startswith("subdial_"):
        os.remove(f"{K}/layers/{f}")
# the comps hosted on it go under the dial, which is what compHost now records
for d in man["drawOrder"]:
    if d.get("layer") == "complications" and (d.get("where") or "").startswith("under subdial"):
        d["where"] = "under dial"
man["transparentSubdials"] = gone

# ---- 2. art-backed hand: a sprite with a real, measured pivot ----------------------
def centroid(im):
    a = np.array(im)[:, :, 3]
    ys, xs = np.nonzero(a)
    w = a[ys, xs].astype(float)
    return float((xs * w).sum() / w.sum()), float((ys * w).sum() / w.sum())

sub_c = None
for h in man["hands"]:
    if str(h.get("mount")) in gone:
        sub_c = h.get("centre")
sp = Image.new("RGBA", (96, 30), (0, 0, 0, 0))
d = ImageDraw.Draw(sp)
d.polygon([(28, 10), (88, 13), (88, 17), (28, 20)], fill=(240, 240, 245, 255))
d.ellipse([20, 8, 36, 24], fill=(240, 240, 245, 255))     # the hub the pivot must sit on
name = "hands/90_art_c3.png"
sp.save(f"{K}/{name}")
px, py = centroid(sp)
mask = Image.new("RGBA", (sp.width + 12, sp.height + 12), (0, 0, 0, 0))
blk = Image.new("RGBA", sp.size, (0, 0, 0, 255))
blk.putalpha(sp.split()[3].point(lambda v: int(v * 0.4)))
mask.paste(blk, (6, 6), blk)
mask.save(f"{K}/hands/90_art_c3_shadow.png")
rec = {
    "z": 9, "file": name, "mount": gone[0], "kind": "second",
    "centre": sub_c or [233, 233],
    "zeroRotationDeg": 0, "pivot": [round(px, 2), round(py, 2)], "size": [sp.width, sp.height], "cap": 0,
    "art": {"id": "c3-hand-art", "usedBy": ["hand " + gone[0]], "placed": True, "kind": "png",
            "sha256": None, "naturalSize": [sp.width, sp.height], "drawnSize": [sp.width, sp.height],
            "resampled": "1:1"},
    "shadow": {"file": "hands/90_art_c3_shadow.png", "size": [mask.width, mask.height],
               "pivot": [round(px + 6, 2), round(py + 6, 2)], "offsetPx": [4.0, 4.0],
               "directionDeg": 45, "distancePx": 5.66, "blurPx": 2.0, "opacity": 0.4},
}
man["hands"].append(rec)
if man.get("auxHands") is not None:
    man["auxHands"].append({"file": name, "mount": gone[0], "kind": "seconds",
                            "pivot": rec["centre"], "centre": rec["centre"], "zeroRotationDeg": 0,
                            "shadow": rec["shadow"]})

# ---- 3. outer.png: translucent case, the path that used to be a hard exit ----------
o = Image.new("RGBA", (S, S), (0, 0, 0, 0))
od = ImageDraw.Draw(o)
od.ellipse([10, 10, S - 10, S - 10], outline=(70, 70, 78, 200), width=16)
od.ellipse([1, 1, S - 2, S - 2], outline=(40, 40, 46, 120), width=8)
o.save(f"{K}/layers/outer.png")
assert (np.array(o)[:, :, 3] < 250).all(), "fixture outer still has an opaque pixel"

# ---- 4. sources/ + manifest.assets[], the shape kitAssets writes -------------------
os.makedirs(f"{K}/sources", exist_ok=True)
art = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ad = ImageDraw.Draw(art)
ad.ellipse([12, 12, S - 12, S - 12], fill=(20, 40, 80, 255))
ad.ellipse([130, 130, 336, 336], fill=(220, 180, 60, 255))
for x in range(10, 40):
    art.putpixel((x, 440), (200, 200, 200, 4))          # sub-threshold: prep's INVISIBLE says no ink
art.save(f"{K}/sources/dial-backdrop.png")
man["assets"] = [{
    "id": "dial-backdrop", "file": "sources/dial-backdrop.png", "name": "backdrop.png",
    "mime": "image/png", "kind": "png", "usedBy": ["plate backdrop"], "placed": True,
    "sha256": None, "sha256Note": "fixture: digest not computed offline", "sha32": "cafebabe",
    "naturalSize": [S, S], "drawnSize": [442, 442], "bytes": len(open(f"{K}/sources/dial-backdrop.png", "rb").read()),
    "resampled": "downsampled or 1:1",
}, {
    "id": "c3-hand-art", "file": "sources/dial-backdrop.png", "name": "hand.png", "mime": "image/png",
    "kind": "png", "usedBy": ["hand " + gone[0]], "placed": True, "sha32": "01234567",
    "naturalSize": [sp.width, sp.height], "drawnSize": [sp.width, sp.height], "bytes": 4096,
    "resampled": "1:1",
}]
man["backgroundArt"] = {"asset": "dial-backdrop", "fit": "cover", "offsetPx": [0, 0],
                        "sizePx": 442, "opacity": 1, "bakedInto": "base/frame_*.png",
                        "note": "stored into the bank with the plate; not a layer to draw"}

# ---- 5. dial-surface art: painted INTO the dial sprite, exactly as buildDial does ----
# The composer bakes this art into layers/dial.png between the surface material and the
# chapter print, so the kit ships no extra plane and prep folds the art with the dial.
# The composite is masked on the dial's own alpha: art may only land where dial ink
# already is, because painting an aperture shut would stop prep merging its disc and
# quietly remove the very path this fixture exists to test.
GOLD = (214, 178, 64)
dp = f"{K}/layers/dial.png"
dialimg = Image.open(dp).convert("RGBA")
paint = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(paint).ellipse([40, 40, S - 40, S - 40], fill=GOLD + (255,))
keep = dialimg.split()[3].point(lambda v: 255 if v > 250 else 0)
dialimg = Image.composite(paint, dialimg, keep)
dialimg.save(dp)


def gold_px(path):
    a = np.array(Image.open(path).convert("RGBA"))
    near = (np.abs(a[:, :, :3].astype(int) - np.array(GOLD)) < 12).all(axis=2)
    return int((near & (a[:, :, 3] > 200)).sum())


GOLD_N = gold_px(dp)
art2 = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(art2).ellipse([40, 40, S - 40, S - 40], fill=GOLD + (255,))
art2.save(f"{K}/sources/dial-surface.png")
man["assets"].append({
    "id": "dial-surface", "file": "sources/dial-surface.png", "name": "dial-art.png",
    "mime": "image/png", "kind": "png", "usedBy": ["dial surface"], "placed": True,
    "sha256": None, "sha256Note": "fixture: digest not computed offline", "sha32": "deadbeef",
    "naturalSize": [S, S], "drawnSize": [442, 442], "bytes": len(open(f"{K}/sources/dial-surface.png", "rb").read()),
    "resampled": "downsampled or 1:1",
})
man["surfaceArt"] = {"asset": "dial-surface", "fit": "cover", "offsetPx": [0, 0],
                     "sizePx": 442, "opacity": 1, "bakedInto": "layers/dial.png",
                     "note": "inside the dial sprite; no separate layer to composite"}
json.dump(man, open(f"{K}/manifest.json", "w"), indent=2)

with zipfile.ZipFile(f"{OUT}/watch-artfixture-kit.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, fs in os.walk(K):
        for f in fs:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, K))
print("wrote", f"{OUT}/watch-artfixture-kit.zip")
print("drawOrder now:", [(d.get("layer"), d.get("id")) for d in man["drawOrder"]])
# the number to look for in the prep output: the gold art must survive the fold into the
# static/base images, which is what proves the surface slot needs no extra layer on device
print("dial-surface art painted into layers/dial.png:", GOLD_N, "gold px")

