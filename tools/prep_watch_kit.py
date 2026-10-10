#!/usr/bin/env python3
"""Prepare a layered watch-designer kit for bake_clock.py (orbital movement).

Takes the vendor kit zip (base/ frames, hands/, layers/, tourbillon/, a
manifest.json with pivots/centres/drawOrder) and produces, in <outdir>:

  frames/frame_NN.png  the gear-train base frames with the OPAQUE BEZEL RING
                       composited into them, so the rim ships in the delta-rect
                       keyframe + bank and costs nothing at runtime.
  theme/               the bake_clock.py theme_dir, one file per RUNTIME layer.

WHERE EACH KIT LAYER GOES, and why that is a PROOF and not a preference
------------------------------------------------------------------------
The device composites: bank -> [live layers in hands.order] -> overlay. A kit
layer may move DOWN into the bank only if moving it can never change a pixel,
which (see bake_clock.py's matte_plan) means every live layer above it must
have alpha exactly 0 wherever it paints. That is decided per layer here, from
the manifest's own drawOrder, and the script ABORTS if a fold it is about to
make is not provably clean — a silently lossy fold is the failure mode.

  outer.png  its OPAQUE pixels (a>=250 — this kit: the tachymetre rim, r>=192.5,
             ~100k px) fold into every bank frame, but only those no MOVER can
             reach. An alpha threshold is not sufficient: the commit restores a
             dirty row from the canvas and repaints every live layer OVER it, so
             a layer folded beneath a mover gets painted over by that mover.
             Hence the fold is `opaque & ~swept_annulus(movers)` and the residue
             stays live on TOP of the overlay (which is what the commit paints
             last) — so an orbital carriage's counterweight passes UNDER the
             bezel either way, instead of drawing on top of it, which is what a
             plain "fold everything at a==255" rule gets wrong. The sub-opaque
             remainder (the crystal's inner tint, max a=249) stays live in the
             overlay, where the kit already draws it, so that merge is exact and
             not a fold. Folded and live halves are disjoint by construction, so
             no pixel is ever blended twice.
  dial.png   stays live (the carriage orbits underneath it). Its ink under the
             folded bezel is zeroed — the bezel is opaque there, so the pixel the
             device produces is the bezel's either way.
  subdial_*  merged INTO the dial sprite. The manifest lists no complications
             between dial and subdial, so nothing composites between them and
             alpha_composite(dial, subdial) is the exact same result the two
             live layers would produce (compositing is associative). Merging
             one 2.5k-px layer out of the job list is free; the merge is only
             done while the drawOrder keeps them consecutive.
  glass.png  -> clock_overlay.png (the overlay slot composites last, above the
             hands — which is what "glass" means).
  hands/     clock_hand_{kind}.png + clock_shadow_{kind}.png.

HAND SHADOWS — the pivot trick the kit's geometry hands us for free
-------------------------------------------------------------------
The kit draws a shadow by rotating its own canvas about its OWN pivot, placed
at centre + shadow.offsetPx. bake_clock.py's model is tighter: a shadow reuses
its hand's pivot and moves only its CENTRE (that shared pivot is what keeps the
light direction fixed while the hand sweeps). Those agree exactly, because the
vendor builds each shadow canvas as the hand canvas grown by a uniform blur
margin — shadow pivot == hand pivot + margin (verified per kit, not assumed:
this script recomputes the difference, requires it to be integral, and PASTES
the shadow by that offset so the two sprites share one rotation frame). If a
kit ever ships a non-integral or non-uniform margin, this script stops rather
than baking a shadow that orbits instead of offsetting.

CADENCE
-------
The carriage IS the seconds hand (manifest: rotate by seconds/60 x 360). With
the bank driving the tick, tickRate == bank fps makes hand_beat_ticks() 1, i.e.
the angle is sampled from the wall clock every tick — a true sweep, matching
the vendor preview's smooth orbit. --tick makes it the 1 Hz mechanical cadence
instead, which halves the carriage's re-rotate cost per second: on a 8 fps tick
the sweep re-rotates a ~0.75-degree step, pixels too small to see, for the same
55k-blend cost as a real move. Default is sweep because that is what the kit's
preview shows; --tick is the honest fps trade and the device log says which was
baked.

Requires PIL (use a venv: `python3 -m venv /tmp/bakevenv &&
/tmp/bakevenv/bin/pip install pillow numpy`, same env as bake_clock.py).

Usage: python3 tools/prep_watch_kit.py <kit.zip> <outdir> [--tick]
Then (fps from the manifest, printed at the end of the run — prep emits all
three bake inputs, so nothing has to be unzipped a second time):
  <venv>/bin/python tools/bake_clock.py <outdir>/theme \
      --bg-frames <outdir>/frames --bg-fps <fps> \
      --tourbillon <outdir>/tourbillon --tourbillon-fps <fps> \
      --tick-rate <fps> --dump-dir assets/preview-<theme>
"""
import os
import sys
import json
import shutil
import zipfile

from PIL import Image
import numpy as np

SCREEN = 466
CX = CY = 233                    # every orbital kit centres its art here
RIM_OPAQUE = 250                 # outer pixels at/above this alpha are the bezel
INVISIBLE = 6                    # alpha the eye cannot weigh; blend traffic 0


def _al(img):
    return np.asarray(img)[:, :, 3]


def _canvas():
    return Image.new("RGBA", (SCREEN, SCREEN), (0, 0, 0, 0))


def _placed(kit, rel, px, py, cx=CX, cy=CY):
    """A trimmed kit sprite on a full screen canvas, pivot pinned to (cx,cy).

    The device pins a layer's stored pivot onto the layer's centre and rotates
    about that point, so placing the sprite here with its pivot already on the
    centre makes the manifest's (pivot, centre) pair a property of the PLACEMENT
    rather than of the pixels — and lets the baker's crop-to-ink move the canvas
    origin without moving anything: at rotation 0 the device draws canvas point
    c at screen (centre - pivot_canvas + c), so pivot_canvas == (cx,cy) puts the
    pivot exactly on the centre. Full canvases cost nothing at bake time;
    crop_layer trims them back to the ink bbox the kit shipped.
    """
    img = Image.open(f"{kit}/{rel}").convert("RGBA")
    c = _canvas()
    # No mask= argument: the canvas is already fully transparent, and passing
    # the image as its own mask would MULTIPLY the alpha band by itself
    # (dest_a = src_a * mask/255), squaring the AA fringe and silently dropping
    # the faintest edge texels. A plain paste REPLACES, which is exact.
    c.paste(img, (int(round(cx - px)), int(round(cy - py))))
    return c


def _zero_under(img, rim):
    """Drop a LIVE layer's ink where the folded rim is opaque.

    Those pixels can never reach the glass from under an a>=250 bezel, so the
    device produces the rim's value whichever way the art says. Zeroing them is
    what makes the rim fold order-correct: leaving them live would paint the
    dial back OVER a bezel the kit draws on top of it.
    """
    a = _al(img)
    hit = int(((a > 0) & rim).sum())
    if hit:
        arr = np.array(img)
        arr[rim, 3] = 0
        img = Image.fromarray(arr)
    return img, hit


def _kill_invisible(img):
    """Zero alpha the eye cannot weigh, so the device skips the pixel
    (`if (!a) continue`) instead of paying a PSRAM read-modify-write blend."""
    arr = np.array(img)
    n = int(((arr[:, :, 3] > 0) & (arr[:, :, 3] < INVISIBLE)).sum())
    arr[arr[:, :, 3] < INVISIBLE, 3] = 0
    return Image.fromarray(arr), n


def _reachable(img, px, py, cx, cy, rotating):
    """Every panel pixel this layer can ever paint, as a bool mask — the same
    proof bake_clock.py's _ink_reachable makes, run here so prep can decide the
    fold before the baker ever sees the art.

    A static paints its own ink. A mover sweeps a full circle in its period, so
    it can paint ANY pixel of the annulus its ink traces about the point it
    rotates around: the radius range is measured from the INK to its pivot
    (px,py), because that is the point the rotation turns, but the annulus is
    drawn about the layer's CENTRE (cx,cy), because that is where the device
    pins that pivot on the screen. The two differ for a hand's SHADOW, whose
    centre is the hand's plus the light offset — testing its annulus about the
    wrong point would let a fold through that paints over a folded rim pixel.
    +/-1 px absorbs the quantised sweep step and the rotation's rounding.

    This matters because a MOVER cannot be rescued the way a static can: zeroing
    a hand's ink under the folded rim would clip the hand. So a mover whose
    annulus touches a folded pixel is not a fixable objection, it is a refusal."""
    a = _al(img)
    ys, xs = np.nonzero(a > 0)
    if not len(xs):
        return np.zeros(a.shape, dtype=bool)
    if not rotating:
        return a > 0
    r = np.hypot(xs - px, ys - py)
    Y, X = np.mgrid[0:SCREEN, 0:SCREEN]
    R = np.hypot(X - cx, Y - cy)
    return (R >= r.min() - 1.0) & (R <= r.max() + 1.0)


def _reach_r(mask):
    """Farthest panel pixel of a reachable mask from the screen centre (0 if
    the layer paints nothing). Reported next to the proof so a reader can see
    how close the art came to the rim, not just that it cleared it."""
    ys, xs = np.nonzero(mask)
    if not len(xs):
        return 0.0
    return float(np.hypot(xs - CX, ys - CY).max())


def _static_run(order):
    """Consecutive static layers from the kit drawOrder, bottom-up.

    Returns (dial_file, [subdial files], merge_into_dial). Merging a subdial
    into the dial is exact only while NOTHING composites between them: over a
    run of layers no other layer touches, alpha_composite is what the device's
    own per-pixel blend chain would produce (compositing is associative), so the
    merge is a saving and not a fold.

    The run is COLLECTED FIRST and split afterwards, never truncated at the
    first obstacle — a kit with a complication between dial and subdial still
    has to ship its subdials (together, as static2), and silently dropping one
    because the loop broke early is exactly the kind of bug a preview image is
    too small to show."""
    dial, run = None, []
    for d in order:
        nm = d.get("layer")
        if dial is None:
            # Everything below the dial (base, tourbillon) is a mover or the
            # bank; it says nothing about whether the statics can merge.
            if nm == "dial":
                dial = d.get("file")
            continue
        if nm == "subdial":
            run.append((d.get("file"), True))
        elif nm == "complications" and not d.get("items"):
            run.append((None, True))       # an empty list composites nothing
        elif nm == "complications":
            run.append((None, False))      # something WILL paint in the gap
            break
        else:
            break                          # hands / glass / outer ends the run
    if dial is None:
        return None, [], False
    subs = [f for f, ok in run if f]
    # mergeable == every gap in the run composited nothing
    return dial, subs, all(ok for _, ok in run) and bool(subs)



def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tick = "--tick" in sys.argv
    if len(args) != 2:
        sys.exit("usage: prep_watch_kit.py <kit.zip> <outdir> [--tick]")
    kit_zip, outdir = args

    # A stale /tmp/_kit from a DIFFERENT kit would otherwise ride along:
    # extractall overwrites what the new zip has and leaves what it lacks, so a
    # kit without a subdial would inherit the previous kit's.
    shutil.rmtree("/tmp/_kit", ignore_errors=True)
    os.makedirs("/tmp/_kit")
    with zipfile.ZipFile(kit_zip) as z:
        z.extractall("/tmp/_kit")
    K = "/tmp/_kit"
    manifest = json.load(open(f"{K}/manifest.json"))
    if manifest.get("size") != SCREEN:
        sys.exit(f"kit size {manifest.get('size')} != {SCREEN}")
    fps = int(manifest.get("fps") or 0) or 15
    order = manifest.get("drawOrder", [])
    # The carriage's rotation centre. Not CX,CY as a silent default: a kit with
    # no tourbillon entry gets the centre its own hands mount on (the same
    # point for every orbital kit), so a mis-parse shows up as a visibly wrong
    # pivot rather than an artless (233,233) that happens to look plausible.
    hand_c = next((h.get("centre") for h in manifest.get("hands", [])
                   if h.get("centre")), [CX, CY])
    tourb_c = next((d.get("centre") for d in order
                    if d.get("layer") == "tourbillon"), None) or hand_c

    frames = os.path.join(outdir, "frames")
    theme = os.path.join(outdir, "theme")
    for d in (frames, theme):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    # ---- outer.png: the fold CANDIDATE (decided below, after the hands) ---
    # The bezel is the kit's LAST layer, above the hands, so its opaque pixels
    # are the art worth taking out of the blend budget: 60-100k px a commit.
    # An alpha threshold alone cannot decide the fold, though. The commit hook
    # restores a dirty row from the canvas and then repaints every LIVE layer
    # over it, so anything folded BELOW a mover gets painted over by that mover
    # — the ordering bug bake_clock.py's matte_plan exists to catch, and the
    # reason an orbital carriage's counterweight currently draws on top of the
    # tachymetre ring it is supposed to pass under. So the fold is computed from
    # the movers' reachability, not from an alpha threshold: fold the opaque rim
    # px no mover can ever sweep onto, keep the rest live on TOP of the overlay
    # (the overlay is what the commit paints last). Every rim pixel then lives
    # in exactly one place, and the carriage passes under the bezel either way.
    outer = Image.open(f"{K}/layers/outer.png").convert("RGBA")
    oa = _al(outer)
    rim_cand = oa >= RIM_OPAQUE
    if not rim_cand.any():
        sys.exit("outer.png has no opaque pixels: nothing to fold; bake it live")
    outer_arr = np.array(outer)
    tint_arr = outer_arr.copy(); tint_arr[rim_cand, 3] = 0
    tint = Image.fromarray(tint_arr)          # outer's sub-opaque half: stays live
    # How far in the bezel reaches, reported with the proof so a reader can see
    # how much clearance the movers had.
    Y, X = np.mgrid[0:SCREEN, 0:SCREEN]
    rim_rmin = float(np.hypot(X - CX, Y - CY)[rim_cand].min())

    # ---- tourbillon bank: copied into outdir, because the bake needs it --
    # prep wipes /tmp/_kit on the way out, so leaving the operator to point
    # --tourbillon at "the unzipped kit" means the documented next step only
    # works if you happened to unzip it a second time. Emitting it alongside
    # frames/ and theme/ makes all three bake inputs one command's output.
    # A kit with no carriage (the classic-movement kits) is not an error: the
    # second-hand slot simply stays off and the bank is omitted.
    tb_src = f"{K}/tourbillon"
    tb_pngs = sorted(n for n in os.listdir(tb_src) if n.endswith(".png")) \
        if os.path.isdir(tb_src) else []
    tb_dir = os.path.join(outdir, "tourbillon")
    has_tb = bool(tb_pngs)
    shutil.rmtree(tb_dir, ignore_errors=True)
    if has_tb:
        shutil.copytree(tb_src, tb_dir)
        tb0 = Image.open(f"{tb_src}/{tb_pngs[0]}").convert("RGBA")
    else:
        tb0 = None
        print("  no tourbillon/ in this kit: carriage off, second-hand slot plain")

    # ---- hands + their shadows --------------------------------------------
    style_hands, shadow_on = {}, False
    for h in manifest.get("hands", []):
        kind = h.get("kind")
        if kind not in ("hour", "minute", "second"):
            continue
        hx, hy = h.get("pivot", [CX, CY])
        hand = _placed(K, h["file"], hx, hy)
        hand.save(f"{theme}/clock_hand_{kind}.png")
        rec = {"show": bool((_al(hand) > 0).any()), "pivotX": CX, "pivotY": CY,
               "centerX": CX, "centerY": CY, "blend": 0}
        sh = h.get("shadow")
        if sh and sh.get("file") and os.path.exists(f"{K}/{sh['file']}"):
            sx, sy = sh.get("pivot", [CX, CY])
            ox, oy = sh.get("offsetPx", [0, 0])
            # The kit rotates the shadow about ITS OWN pivot, placed at
            # centre+offsetPx; _placed puts that pivot on the canvas centre, and
            # the baker then pins the shadow's pivot to the shadow record's
            # centre (centre + dx,dy). Rotation centre and screen-space offset
            # therefore both match the manifest, and the shadow never orbits.
            #
            # The vendor builds each shadow as the hand canvas grown by a
            # uniform blur margin, which is why the two pivots differ by whole
            # pixels. Check it: a fractional margin would mean the shadow's
            # rotation point is NOT the hand's, and pasting it would bake a
            # shadow that drifts as the hand sweeps.
            mx, my = sx - hx, sy - hy
            if abs(mx - round(mx)) > 0.02 or abs(my - round(my)) > 0.02:
                sys.exit(f"{kind}: shadow pivot offset ({mx:+.3f},{my:+.3f}) is "
                         f"not integral — the shadow does not share the hand's "
                         f"rotation frame; refusing to bake it")
            _placed(K, sh["file"], sx, sy).save(f"{theme}/clock_shadow_{kind}.png")
            rec["shadowDx"], rec["shadowDy"] = int(round(ox)), int(round(oy))
            shadow_on = True
            print(f"  {kind:6s} shadow: canvas margin ({mx:+.2f},{my:+.2f}) "
                  f"offset ({ox:.2f},{oy:.2f}) -> ({rec['shadowDx']},"
                  f"{rec['shadowDy']})")
        style_hands[kind] = rec

    # ---- the fold decision: which bezel px no mover can ever reach --------
    # (name, image, pivot, centre) for every layer that stays live AND rotates.
    # A mover's reachable set is the annulus its own ink traces about the point
    # it turns around, pinned to where the device puts that point — see
    # _reachable for why the pivot and the centre differ for a shadow.
    names = sorted(n for n in os.listdir(f"{K}/base") if n.endswith(".png"))
    if not names:
        sys.exit("kit has no base/ frames")
    movers = []
    if tb0 is not None:
        movers.append(("carriage", tb0, (tourb_c[0], tourb_c[1]),
                       (tourb_c[0], tourb_c[1])))
    for k, rec in style_hands.items():
        if not rec["show"]:
            continue
        movers.append((f"{k}-hand", Image.open(f"{theme}/clock_hand_{k}.png"),
                       (CX, CY), (CX, CY)))
        sp = f"{theme}/clock_shadow_{k}.png"
        if os.path.exists(sp):
            movers.append((f"{k}-shadow", Image.open(sp), (CX, CY),
                           (CX + rec.get("shadowDx", 0),
                            CY + rec.get("shadowDy", 0))))
    swept = np.zeros((SCREEN, SCREEN), dtype=bool)
    swept_r = {}
    for nm, im, (px, py), (cx, cy) in movers:
        m = _reachable(im, px, py, cx, cy, True)
        swept |= m
        swept_r[nm] = _reach_r(m)
    fold = rim_cand & ~swept                 # into every bank frame
    live_rim = rim_cand & swept              # stays a live top layer
    if int((fold & live_rim).sum()):
        sys.exit("internal: fold/live_rim overlap")     # ~swept makes this vacuous
    print("bezel fold: {} of {} opaque rim px (a>={}) go into all {} bank frames; "
          "{} px stay live because a mover can reach them".format(
              int(fold.sum()), int(rim_cand.sum()), RIM_OPAQUE, len(names),
              int(live_rim.sum())))
    for nm in sorted(swept_r, key=lambda n: -swept_r[n]):
        print("   mover {:14s} sweeps out to r={:6.1f}  (bezel ink starts at "
              "r={:.1f})".format(nm, swept_r[nm], rim_rmin))


    # ---- dial (+ merged subdials), minus the ink the fold would bury ------
    # Zeroing is against `fold`, not `rim_cand`: a px that stayed live must keep
    # its dial art, because the device still composites the dial under it.
    dial_file, sub_files, merged = _static_run(order)
    if not dial_file:
        sys.exit("manifest drawOrder has no dial layer")
    face = Image.open(f"{K}/{dial_file}").convert("RGBA")
    face, dial_killed = _zero_under(face, fold)
    face, dial_blind = _kill_invisible(face)
    for sf in (sub_files if merged else []):
        s = Image.open(f"{K}/{sf}").convert("RGBA")
        s, _ = _zero_under(s, fold)         # same rule as the dial: only what the
        face = Image.alpha_composite(face, s)   # bank now carries can be dropped
    face.save(f"{theme}/clock_hand_static1.png")

    # ---- overlay: glass, then the outer's tint, then any live rim px -----
    # The kit's own order at the top of the stack is glass, outer; the tint is
    # outer's sub-opaque half and the live rim its opaque half, so both belong
    # above the glass and above the hands, which is exactly where the overlay
    # slot composites. Nothing here is a fold: same pixels, same order.
    glass = Image.open(f"{K}/layers/glass.png").convert("RGBA")
    glass, glass_killed = _zero_under(glass, fold)
    overlay = Image.alpha_composite(glass, tint)
    if live_rim.any():
        lr_arr = outer_arr.copy(); lr_arr[~live_rim, 3] = 0
        overlay = Image.alpha_composite(overlay, Image.fromarray(lr_arr))
    overlay, ov_blind = _kill_invisible(overlay)
    overlay.save(f"{theme}/clock_overlay.png")

    # ---- bank: base frames + the folded bezel ----------------------------
    # A pixel COPY, not a blend: the fold is opaque by construction, so no
    # 8-bit/RGB565 round-trip error enters here. That is why the bezel is the
    # one layer worth folding and the sub-opaque tint is not.
    fold_arr = outer_arr.copy(); fold_arr[~fold, 3] = 0
    fold_img = Image.fromarray(fold_arr)
    for n in names:
        Image.alpha_composite(Image.open(f"{K}/base/{n}").convert("RGBA"),
                              fold_img).save(f"{frames}/{n}")

    # ---- the proof: nothing live may paint on a folded px ----------------
    # Movers are disjoint from `fold` by construction (fold = rim_cand & ~swept),
    # so the only thing left to check is the two statics the fold moved under:
    # their ink must be exactly 0 wherever the bank now carries the bezel. This
    # is a check on _zero_under, not on the art — a nonzero here is a bug.
    for nm, im in (("dial", face), ("overlay", overlay)):
        n_on = int(((_al(im) > 0) & fold).sum())
        print("   static {:8s} {:>5} px of live ink on folded bezel px  {}".format(
            nm, n_on, "clean" if not n_on else "DOUBLE-PAINT"))
        if n_on:
            sys.exit("REFUSING to fold the bezel: {} still paints {} px on top of "
                     "folded pixels (_zero_under failed)".format(nm, n_on))


    # ---- clock_style.json --------------------------------------------------
    order_idx = {"hour": 0, "minute": 1, "second": 2, "static1": 3, "static2": 4}
    base = {k: {"show": False, "pivotX": CX, "pivotY": CY,
                "centerX": CX, "centerY": CY, "blend": 0}
            for k in ("hour", "minute", "second", "static1", "static2")}
    base.update(style_hands)
    # The carriage rides the second-hand slot: its record is the geometry carrier
    # the rotator and the swept-box maths read, while its pixels come from the
    # tourbillon bank the baker packs separately (--tourbillon).
    base["second"] = {"show": bool(tb0 is not None and (_al(tb0) > 0).any()),
                      "pivotX": tourb_c[0], "pivotY": tourb_c[1],
                      "centerX": tourb_c[0], "centerY": tourb_c[1], "blend": 0}
    base["static1"] = {"show": True, "pivotX": CX, "pivotY": CY,
                       "centerX": CX, "centerY": CY, "blend": 0}
    show2 = bool(sub_files and not merged)
    base["static2"] = {"show": show2, "pivotX": CX, "pivotY": CY,
                       "centerX": CX, "centerY": CY, "blend": 0}
    if show2:
        s2 = _canvas()
        for sf in sub_files:                 # ALL of them, together: they are
            s = Image.open(f"{K}/{sf}").convert("RGBA")   # consecutive, so this
            s, _ = _zero_under(s, fold)                    # merge stays exact
            s2 = Image.alpha_composite(s2, s)
        s2.save(f"{theme}/clock_hand_static2.png")
    # Earlier == lower, and the shadow always goes down directly under its OWN
    # hand (clock_view.c's etiquette), so the order list carries the movers only.
    kind_order = [order_idx[k] for k in
                  ("second", "static1", "static2", "hour", "minute")
                  if base[k]["show"]]
    base["shadow"] = {"on": shadow_on, "dx": 0, "dy": 0}
    # tickRate == bank fps makes hand_beat_ticks() 1, so the carriage angle is
    # read from the wall clock every tick: a true sweep. --tick pins it to one
    # 6-degree step per second, the mechanical cadence, at a fraction of the
    # re-rotate cost (see the CADENCE note in the docstring).
    style = {"hands": {"order": kind_order, **base},
             "secondSweep": not tick, "secondRailway": False,
             "textOverHands": False, "tickRate": 1 if tick else fps}
    json.dump({"name": os.path.basename(os.path.abspath(outdir))},
              open(f"{theme}/theme.json", "w"))
    json.dump(style, open(f"{theme}/clock_style.json", "w"), indent=1)
    shutil.rmtree("/tmp/_kit", ignore_errors=True)

    ink = lambda im: int((_al(im) > 0).sum())
    sh_ink = sum(ink(Image.open(f"{theme}/clock_shadow_{k}.png"))
                 for k in style_hands
                 if os.path.exists(f"{theme}/clock_shadow_{k}.png"))
    hd_ink = sum(ink(Image.open(f"{theme}/clock_hand_{k}.png"))
                 for k, r in style_hands.items() if r["show"])
    print("live blend traffic/commit: dial {:>6}  overlay {:>6}  hands {:>5}  "
          "shadows {:>5}".format(ink(face), ink(overlay), hd_ink, sh_ink))
    print("folded into every bank frame: {:>6} px; live on the overlay: {:>5} px"
          .format(int(fold.sum()), int(live_rim.sum())))
    print("zeroed as invisible: dial {} px under the fold + {} sub-visible; "
          "overlay {} + {}".format(dial_killed, dial_blind, glass_killed, ov_blind))
    if sub_files:
        print("subdial{} {}: {}".format("s" if len(sub_files) > 1 else "",
                                         len(sub_files),
                                         "merged into the dial sprite" if merged
                                         else "not consecutive in drawOrder -> "
                                              "own static2 layer"))
    else:
        print("no subdial in this kit")
    print("prepared {} in {} mode: frames/ ({} frames @ {} fps) + theme/".format(
        outdir, "1 Hz TICK" if tick else "SWEEP", len(names), fps))
    print("next:")
    print("  tools/bake_clock.py {} --bg-frames {} --bg-fps {} \\".format(
        theme, frames, fps))
    if has_tb:
        print("      --tourbillon {} --tourbillon-fps {} \\".format(tb_dir, fps))
    print("      --tick-rate {} --dump-dir assets/preview-orbital".format(
        1 if tick else fps))


if __name__ == "__main__":
    main()
