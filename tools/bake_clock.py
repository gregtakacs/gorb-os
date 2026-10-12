#!/usr/bin/env python3
"""
bake_clock.py — offline analog-clock theme baker for gorb-os.

Reads an orb-os (Arduino) theme folder — its clock_style.json, theme.json and
the clock_*.png layer art — and produces two artefacts for the IDF firmware:

  assets/clock_assets.bin   one packed blob of raw pixel planes
  assets/clock_assets.h     geometry parsed offline from clock_style.json. The
                            blob is NOT referenced from the app image — it is
                            flashed into the `assets` data partition and mapped
                            at boot by main/art/art_bank.c. NO runtime JSON
                            parser needed on the device.

Why offline: same reasoning as bake_gif.py. Decoding PNG (zlib) and reading the
theme JSON on-device would put both in the boot path and fight the single busy
LVGL core; we bake finished RGB565 planes and read them as flash-resident
rodata (target_add_binary_data) that LVGL samples with the CPU.

Two format facts drive every byte here (read from the vendored LVGL 9.6 source,
not guessed):

  * RGB565A8 is PLANAR, not interleaved (lv_draw_sw_img.c:248,
    lv_draw_sw_transform.c:181):
        [ color plane : w*h*2 bytes, stride w*2, native-LE RGB565 ]
        [ alpha plane : w*h   bytes, stride w  (LVGL forces color_stride/2) ]
    so data_size = w*h*3 and the alpha plane is APPENDED after the color plane.
    (The Arduino CPU blitter stored an INTERLEAVED 3-byte-per-pixel RGB565+A —
    a different layout — so its byte order is deliberately NOT reused here.)

  * The colour plane is packed little-endian uint16 exactly like LVGL's
    LV_COLOR_DEPTH=16 in-memory order (low byte first). The panel byte-swap then
    happens in the flush callback for everything LVGL renders, identically to
    the orb_bg path, so no pre-swapping is done here.

The hands/shadows/overlay are TRIMMED to their opaque (alpha) bounding box. That
is the point of the exercise: LVGL dirties the rotated-corner AABB of an image
object (lv_image.c:303 lv_image_buf_get_transformed_area), so a full 542x542
minute-hand canvas (3.4% ink) would dirty the whole screen every tick, whereas
the trimmed ink + its stored crop offset dirties only the swept wedge. The crop
offset and the pivot (re-based into crop coords) are baked into the header so
the firmware reproduces the exact same screen geometry.

Usage:
  python3 tools/bake_clock.py <theme_dir> \
      [--bin assets/clock_assets.bin] [--header assets/clock_assets.h] \
      [--pad 2] [--preview-dir assets/preview]

  --bg-gif assets/orb_bg.gif   replaces the theme's plate/bgAnim background
      with a GIF frame bank stored as DELTA RECTS: frame 0 is baked as one
      full keyframe, every later frame only its perceptibly-changed rects
      (vs the previous frame, threshold --diff-thresh). The device keeps a
      PSRAM canvas of the frame currently on the glass, memcpy's the next
      frame's rects into it, and invalidates exactly those areas plus the
      hands' swept wedges — instead of flushing the full page every frame.

  --bg-frames DIR|ZIP|GLOB   the same delta-rect bank, fed from a PNG frame
      sequence instead of a GIF (a directory of PNGs, a .zip containing them,
      or a shell glob). Frames are natural-sorted (frame_2 before frame_10),
      composited over black, and paced at --bg-fps (PNGs carry no timing).
      USE THIS FOR NEW ARTWORK rather than round-tripping through a GIF: a
      PNG sequence is the only source that survives re-baking. A GIF caps
      every frame at 256 palette entries (this dial's artwork has ~61k unique
      colours per frame, so the GIF pass costs up to 68/255 per channel where
      the direct PNG->RGB565 bake costs at most 7), and a baked .bin is even
      worse as a source — it is already the final 5-bit/6-bit/5-bit
      quantisation, so re-baking it re-quantises lossy data, cannot be
      dithered differently, has no alpha, and carries no size/fps/byte-order
      metadata at all. GIF stays supported because it is what existing assets
      (assets/orb_bg.gif) and other people's exports arrive as.

  <theme_dir> e.g. ~/docker/Projects/orb-os/themes/unpacked/steam-punk-t2is
"""
import argparse
import fnmatch
import glob as _glob
import io
import json
import os
import re
import sys
import time
import zipfile
from PIL import Image

SCREEN_W = 466
SCREEN_H = 466

# orb-os hand kinds (index == the theme's kind integer)
KINDS = ["hour", "minute", "second", "static1", "static2"]


def load_rgba(theme_dir, name):
    p = os.path.join(theme_dir, name)
    if not os.path.exists(p):
        return None
    return Image.open(p).convert("RGBA")


def rgb565_le(rgba_img):
    """Colour plane: native-LE RGB565, w*h*2 bytes. Alpha is IGNORED here
    (every texel keeps its stored colour so the planar alpha plane alone
    decides coverage, which avoids a dark edge fringe)."""
    w, h = rgba_img.size
    px = rgba_img.load()
    out = bytearray(w * h * 2)
    o = 0
    for y in range(h):
        for x in range(w):
            r, g, b, _ = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[o] = v & 0xFF
            out[o + 1] = (v >> 8) & 0xFF
            o += 2
    return bytes(out)


def alpha_bytes(rgba_img):
    """Alpha plane row-major, stride w (LVGL's forced RGB565A8 alpha stride)."""
    return rgba_img.getchannel("A").tobytes()


def crop_layer(img, pad):
    """Return (cropped_rgba, offx, offy): the tight alpha bbox grown by pad and
    clamped to the canvas. A fully-transparent/absent image -> (None, 0, 0)."""
    if img is None:
        return None, 0, 0
    bbox = img.getchannel("A").getbbox()
    if not bbox:
        return Image.new("RGBA", (1, 1), (0, 0, 0, 0)), 0, 0
    l, t, r, b = bbox
    l = max(0, l - pad); t = max(0, t - pad)
    r = min(img.size[0], r + pad); b = min(img.size[1], b + pad)
    return img.crop((l, t, r, b)), l, t


# ---------------------------------------------------------------------------
# Bake-time static composite ("matting") -- see --matte.
#
# Why this exists: a full-coverage static layer (a translucent dial disc, a
# glass sheen) costs the device a blend_px() on EVERY commit across the whole
# marked span, measured at 82-95 cycles per blend -- ~40% of a tourbillon
# commit. Folded into the background it sits on (the plate, or every frame of
# the delta bank) it costs nothing at runtime, which is exactly how
# prep_watch_kit.py already handles the bezel rim.
#
# It is only legal where it cannot change a single pixel. Moving a layer to the
# bottom of the stack re-orders it against every layer that used to sit under
# it, and alpha-compositing is NOT commutative -- so the fold is exact iff that
# layer's alpha is 0 wherever any lower layer can ever paint. (An alpha of 255
# does not rescue it either: matted, a lower layer would now paint *over* the
# opaque one instead of under it.) That predicate is checkable straight off the
# art, which is why this is proved rather than hand-picked: the orbital kit's
# dial carries partial alpha over 55% of the pixels the carriage can reach and
# its glass is 98% partial alpha over everything, so both must stay runtime
# layers no matter what the fps line says. matte_plan() says so in numbers.
# ---------------------------------------------------------------------------


def _q565(arr):
    """RGB888 (numpy, 0..255, last axis 3) -> RGB565 ints, the exact
    truncation rgb565_le() applies -- the device never sees more precision
    than this, so the matte must not either."""
    import numpy as np
    return ((((arr[..., 0] & 0xF8).astype(np.int64)) << 8) |
            (((arr[..., 1] & 0xFC).astype(np.int64)) << 3) |
            ((arr[..., 2] >> 3).astype(np.int64)))


def _unq565(v565):
    """RGB565 -> the RGB888 that re-quantises back to THE SAME 565, so a matte
    composed in 565 survives rgb565_le() bit-for-bit (5 bit: x<<3|x>>2, 6 bit:
    y<<2|y>>4 -- both saturate to 255 and are exact under the 0xF8/0xFC mask)."""
    import numpy as np
    r = (v565 >> 11) & 31
    g = (v565 >> 5) & 63
    b = v565 & 31
    return np.dstack([(r << 3) | (r >> 2), (g << 2) | (g >> 4),
                      (b << 3) | (b >> 2)]).astype("uint8")


def _mix565(dst, fg, a):
    """blend_px()'s NORMAL path (mix_16_16(fg, dst, a)) in numpy, per channel.

    Deliberately the device's own maths and not Image.alpha_composite(): the
    panel is RGB565 and every runtime composite happens there, so a matte
    folded in 8-bit sRGB would land a quantisation step away from the pixel the
    old live blend produced -- a diffuse error over the whole plate that no fps
    counter can see. a==255 reduces to fg and a==0 to dst here exactly as
    blend_px() short-circuits them."""
    import numpy as np
    a = np.asarray(a, dtype=np.int64)
    mix5 = (a + 4) >> 3                       # lv's (mix+4)>>3, 0..32
    dr, dg, db = (dst >> 11) & 31, (dst >> 5) & 63, dst & 31
    fr, fg6, fb = (fg >> 11) & 31, (fg >> 5) & 63, fg & 31
    r = (((fr - dr) * mix5 >> 5) + dr) & 31
    g = (((fg6 - dg) * mix5 >> 5) + dg) & 63
    b = (((fb - db) * mix5 >> 5) + db) & 31
    return (r << 11) | (g << 5) | b


def matte_fold(base_img, over_img):
    """Fold one full-canvas RGBA layer into an RGBA base with the device's
    blend. Alpha-0 texels pass through untouched, so the base keeps its own
    colour exactly where the layer never painted (the planar-alpha rule
    rgb565_le() documents: coverage is the alpha plane's business)."""
    import numpy as np
    dst = _q565(np.asarray(base_img.convert("RGB"), dtype=np.int64))
    ov = np.asarray(over_img.convert("RGBA"), dtype=np.int64)
    out = dst.copy()
    hit = ov[..., 3] > 0
    if hit.any():
        out[hit] = _mix565(dst[hit], _q565(ov[..., :3])[hit], ov[..., 3][hit])
    return Image.fromarray(_unq565(out), "RGB").convert("RGBA")


def _opaque_only(img):
    """A copy of a layer keeping ONLY its fully opaque pixels. The bg-bank
    packer pre-composites this and still ships the layer live: an opaque
    source texel wins that blend outright no matter what sits below it, so
    the baked copy and the later live blit agree exactly, everywhere. Any
    texel below full alpha is gated out -- a half-alpha pixel composited
    twice (once folded into the bank, once live over it) is composited
    wrong, which is precisely why the whole-page glass tint (a<=247) gains
    nothing here and the AA fringe of a dial aperture must not ride along."""
    import numpy as np
    a = np.asarray(img.convert("RGBA")).copy()
    a[a[..., 3] != 255] = 0
    return Image.fromarray(a, "RGBA")


def ink_count(img):
    """How many texels a layer actually paints (what a fold takes off the
    commit's blend budget). A frame bank counts the UNION over its frames --
    any one of them can be the frame on the glass."""
    import numpy as np
    imgs = img if isinstance(img, (list, tuple)) else [img]
    m = None
    for im in imgs:
        a = np.asarray(im.convert("RGBA").split()[3]) > 0
        m = a if m is None else (m | a)
    return int(m.sum()) if m is not None else 0


def _panel_placements(img, pad, cx, cy, px, py):
    """A layer's texels as the DEVICE positions them, on a panel-size canvas.

    Art comes from the studio either full-canvas or trimmed to its own ink
    (the kit's hands are 40x178, not 466x466), and the bake never rescales it
    back: emit_sprite ships the crop plus a pivot in CROPPED coords, and the
    rotator pins that pivot to the layer's centre. So the sprite's screen origin
    is (centerx - (pivotx - offx), centery - (pivoty - offy)) -- reproduce that
    exactly, or the reachability proof describes a layer the device never
    paints. `img` may be a frame bank, whose frames share ONE ink bbox (as
    bake_tourbillon does) so the whole bank lands on one placement."""
    frames = list(img) if isinstance(img, (list, tuple)) else [img]
    import numpy as np
    l = t = 1 << 30
    r = b = -1
    for im in frames:
        bb = im.convert("RGBA").getchannel("A").getbbox()
        if not bb:
            continue
        l = min(l, bb[0]); t = min(t, bb[1])
        r = max(r, bb[2]); b = max(b, bb[3])
    if r <= l or b <= t:
        return []
    l = max(0, l - pad); t = max(0, t - pad)
    r = min(frames[0].size[0], r + pad); b = min(frames[0].size[1], b + pad)
    ox, oy = l, t
    out = []
    for im in frames:
        crop = im.convert("RGBA").crop((ox, oy, r, b))
        canvas = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 0))
        # Pinned so the pivot lands on the layer's centre, exactly as the
        # device's posx/posy do (see paint_ctx_init).
        canvas.paste(crop, (cx - (px - ox), cy - (py - oy)), crop)
        out.append(canvas)
    return out


def _ink_reachable(panels, cx, cy, rotating):
    """Every panel pixel this layer can ever paint, as a panel-size bool array.

    A static layer paints its own ink. A hand sweeps a full circle in an hour,
    so it can paint ANY pixel of the annulus its ink traces about the point it
    rotates around: the pivot is pinned to (cx,cy), so a texel's orbit radius is
    simply its distance from (cx,cy). +-1 px absorbs the quantised sweep step and
    the rotation's own rounding."""
    import numpy as np
    mask = np.zeros((SCREEN_H, SCREEN_W), dtype=bool)
    for im in panels:
        mask |= np.asarray(im.split()[3]) > 0
    if not rotating:
        return mask
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return mask
    r = np.hypot(xs - cx, ys - cy)
    Y, X = np.mgrid[0:SCREEN_H, 0:SCREEN_W]
    R = np.hypot(X - cx, Y - cy)
    return (R >= r.min() - 1.0) & (R <= r.max() + 1.0)


class _Layer:
    """One compositor layer, already positioned on the panel canvas, for the
    matte proof. `panels` is one panel-size RGBA (or several for a bank)."""
    def __init__(self, name, panels, cx, cy, blend, rotating):
        self.name, self.panels = name, panels
        self.cx, self.cy = cx, cy
        self.blend, self.rotating = blend, rotating
        self.forced = False    # set by matte_plan when the fold overrode the proof
        self.foldable = True   # matte_plan also skips value-driven layers (comps)


def matte_plan(live, mode):
    """Decide which static layers may be folded into the background.

    `live` is the layer stack BOTTOM-TO-TOP (the theme's hands.order, then the
    glass overlay above everything). Returns (chosen, lines) with chosen in
    bottom-to-top fold order and lines the per-layer verdicts to print.

    A layer is foldable iff it never rotates, blends NORMAL, and its alpha is 0
    on every pixel any LOWER layer can ever paint. NORMAL matters: a MULTIPLY or
    SCREEN layer goes through mix_16_16 with a *computed* colour, so folding it
    changes pixels its own alpha never covered. plateFollow forbids all folding
    -- a layer baked into the plate would slide across the glass with it.
    mode: auto = fold only the proved ones, off = never, force = fold the
    statics regardless (the caller must then report matte_fidelity, because the
    fold is lossy by construction).
    Complication boxes ride the stack as foldable=False layers: their pixels
    follow the wall clock, so folding one would freeze a single frame into the
    bank, and their very presence is what refuses the fold of the dial frame
    that composites OVER them (the runtime-composite policy — the proof sees it
    instead of being told)."""
    chosen, lines = [], []
    for idx in range(len(live)):
        cand = live[idx]
        if cand.rotating or not getattr(cand, "foldable", True):
            continue                            # a mover/value-layer is never a matte
        import numpy as np
        al = None
        for im in cand.panels:
            a = np.asarray(im.split()[3])
            al = a if al is None else np.maximum(al, a)
        if al is None or not (al > 0).any():
            continue                            # nothing there to fold
        lower = np.zeros(al.shape, dtype=bool)
        for other in live[:idx]:
            lower |= _ink_reachable(other.panels, other.cx, other.cy,
                                    other.rotating)
        cov = (al > 0)
        viol = cov & lower
        nv, nt = int(viol.sum()), int(cov.sum())
        if cand.blend != 0:
            verdict = f"REFUSED: blend mode {cand.blend} is never order-independent"
        elif nv == 0:
            verdict = f"FOLDED ({nt} px, provably clear of every lower layer)"
        else:
            verdict = ("REFUSED: {}/{} ink px sit where a lower layer can paint "
                       "(max alpha there {})".format(nv, nt, int(al[viol].max())))
        # blend NORMAL and the proof clear: the only fold worth making.
        fold = mode != "off" and cand.blend == 0 and (nv == 0 or mode == "force")
        if fold and nv > 0:
            verdict += "  ** FORCED, LOSSY **"
        if mode == "off":
            verdict += "  [--matte off]"
        lines.append("  matte {:<16s} {}".format(cand.name, verdict))
        if fold:
            cand.forced = nv > 0
            chosen.append(cand)
    return chosen, lines


# ---------------------------------------------------------------------------
# Background occlusion cull -- see --bg-occlude.
#
# Why this exists: the kit's base frames are the bare plate and gear train, and
# the delta bank is a diff of exactly those. The dial, the raised dials and the
# glass stay LIVE layers that the commit hook paints back over every row it
# rebuilds. So a texel one of those covers at alpha 255 can never reach the
# panel, and pushing it buys nothing while charging ~0.2 ms for every row it is
# alone in (docs/rows-capacity-2026-10-10.md). A closed dial with one small
# escapement opening was still paying for the whole train.
#
# Only a NON-ROTATING, NORMAL-blend layer may occlude: a hand, a shadow or the
# carriage lifts off the texel later and the stale canvas behind it would show.
# Requiring alpha EXACTLY 255 is what makes the cut invisible rather than nearly
# so -- at a=250 one fifty-first of every moving gear texel still reaches the
# glass, and blend_px()'s mix_16_16 has no shortcut for it. A layer folded into
# the bank by matte_plan is not an occluder either: it is part of the background
# now, and cannot hide anything.
#
# One layer must cover the texel BY ITSELF. A stack of partial alphas is not
# allowed to add up to a cull: it would take the panel's own blend maths to say
# what those texels are worth, and the answer is "not zero".
#
# The cull is safe because nothing changes a layer's visibility at runtime: the
# show flags come from clock_style.json at load and clock_view.c never writes
# one (checked: no write to .show outside the style parse).
#
# Worth the trouble: on the skeleton calendar kit, where the matte proof
# correctly refuses to fold the dial at all, the cull took the bank from 39.1% to
# ~5% of a full page per frame.
# ---------------------------------------------------------------------------


def bg_occluders(live, folded):
    """The live statics allowed to hide the bank, bottom-to-top.

    One list, used BOTH to build the mask and to print the per-layer count, so
    the bake log can never describe a cull the packer did not make. The gates and
    their reasons are bg_occluder's; loopcheck's seal check pins their shape.
    """
    keep = []
    for L in live:
        if L.rotating or L.blend != 0 or L in folded:
            continue                        # a mover, a non-NORMAL blend, or in the bank
        if not getattr(L, "foldable", True):
            continue                        # value-driven: its art changes with the clock
        keep.append(L)
    return keep


def bg_occluder(live, folded):
    """Panel texels no background pixel can ever reach, or None.

    `live` is the runtime stack bottom-to-top as build_live_stack returns it,
    `folded` the layers matte_plan chose (those are IN the bank now). A texel is
    occluded when one static layer paints it at alpha 255 -- the exact case
    blend_px() short-circuits to `out = c`. The alpha predicate is _opaque_only's,
    so the fold proof and this cull cannot drift apart on what "opaque" means.

    A value-driven comp box (foldable False) never occludes: its art is the date,
    the moon, the weekday, so what it paints at a==255 this hour is not what it
    paints at midnight. Culled texels under it are bank pixels the device never
    pushes, and the moment that box's value changes they are the canvas underneath
    -- which is exactly the stale rectangle this cull is only allowed to ignore
    while something opaque sits on it permanently.
    """
    import numpy as np
    occ = None
    for L in bg_occluders(live, folded):
        for im in L.panels:
            a = np.asarray(_opaque_only(im).split()[3])
            full = a >= 255                 # opaque on its own, or not at all
            occ = full if occ is None else (occ | full)
    return occ if occ is not None and occ.any() else None


def shadow_offsets(hands_style, kind):
    """The screen-space offset a hand's shadow centre sits at.

    The device keeps ONE light direction per theme (clk_theme.shadow_dx/dy),
    but a watch-designer kit specifies the offset PER HAND — `hands[].shadow
    .offsetPx` grows with the hand's height in the stack, so a minute hand
    floating further above the dial throws a further offset than the hour hand
    under it. The runtime already supports that without knowing it: the shadow
    is its own clk_sprite_t, and it is the record's centrex/centery — not any
    theme-wide field — that place it (make_layer positions the object at
    centre - pivot). So the baker takes the per-hand override when the style
    carries one and falls back to the theme default.

    The ONE place this is computed, shared by the hands loop and by
    matte_plan's reachability stack, for the same reason resolve_hand_pivots
    is: an offset that moved after the annulus was built would make the fold
    proof describe a layer the device never draws."""
    ss = hands_style.get("shadow", {}) or {}
    hs = hands_style.get(kind, {}) or {}
    dx = hs.get("shadowDx", None)
    dy = hs.get("shadowDy", None)
    if dx is None:
        dx = int(ss.get("dx", 0))
    if dy is None:
        dy = int(ss.get("dy", 0))
    return int(dx), int(dy)


def resolve_hand_pivots(tdir, hands_style, args):
    """Author pivot -> the pivot each rotating hand actually spins about.

    The ONE place the correction is computed, because two consumers must agree
    on it exactly: the hands loop bakes it into clk_sprite_t (so the rotator
    spins there) and matte_plan's reachability annulus must be measured about
    the same point -- a pivot that moved after the annulus was built would make
    the proof describe a layer the device never draws. snap_pivot_to_hub is
    pure, so calling it once and reusing the answer is bit-identical to what the
    loop did inline."""
    out = {}
    for kind in KINDS:
        hs = hands_style.get(kind, {}) or {}
        px = int(hs.get("pivotX", 0)); py = int(hs.get("pivotY", 0))
        cx = int(hs.get("centerX", SCREEN_W // 2))
        cy = int(hs.get("centerY", SCREEN_H // 2))
        if kind not in ("hour", "minute", "second") or args.no_pivot_snap:
            out[kind] = (px, py, cx, cy, "")
            continue
        img = load_rgba(tdir, f"clock_hand_{kind}.png")
        if img is None:
            out[kind] = (px, py, cx, cy, "")
            continue
        nx, ny, orb = snap_pivot_to_hub(img, px, py, cx, cy)
        note = "" if (nx, ny) == (px, py) else f" [pivot {px},{py}->{nx},{ny} orbit {orb:.2f}px]"
        out[kind] = (nx, ny, cx, cy, note)
    return out


def build_live_stack(tdir, style, pivots, args, tourb_imgs):
    """The compositor's real z-stack, BOTTOM-TO-TOP, for the matte proof.

    Mirrors what the firmware builds: the theme's hands.order (earlier == lower,
    each shadow directly under its own hand and sharing its coverage), then the
    glass overlay above everything. The second-hand slot is the tourbillon bank
    when one was baked, because that is the art the device actually paints."""
    hands_style = style.get("hands", {}) or {}
    order = [int(k) for k in (hands_style.get("order", [0, 1, 2, 3, 4]) or [])
             if 0 <= int(k) < 5]
    shadow_style = hands_style.get("shadow", {}) or {}
    shadow_on = bool(shadow_style.get("on", False))
    sdx = int(shadow_style.get("dx", 0)); sdy = int(shadow_style.get("dy", 0))
    # ---- complication boxes: where the kit says they sit -------------------
    # The paint pass lays a tile down immediately BELOW the live layer named by
    # its `first` (the z bucket prep took from the kit's own drawOrder), and the
    # plate's own frame art composites OVER the tile edge at runtime (the locked
    # runtime-composite policy). If that plate were folded into the bank the tile
    # would cover the frame instead of tucking under it — a real aperture
    # overlaps its bezel by construction — so the boxes ride the proof stack IN
    # THAT POSITION and the violation count matte_plan prints is what refuses the
    # fold. Putting every box at the bottom instead (as this did while only
    # dial-hosted windows existed) proves the wrong stacking: the moon under the
    # c4 plate proves nothing about the plate, and the fold then buries it. The
    # whole tile box stands in for the tile's ink (worst case: an opaque frame).
    # The box layers carry foldable=False: their pixels follow the wall clock,
    # and folding one would freeze a single weekday into the bank.
    import numpy as np
    import theme_schema as _ts
    _cc, _, _ = bake_complications(tdir, Blob(), verbose=False)
    boxes = {}
    for rec in _cc:
        if _ts.KINDS[rec["kind"]] not in ("window", "strip"):
            continue                                # ring/flare: validated, not painted
        a = np.zeros((SCREEN_H, SCREEN_W), dtype=np.uint8)
        x0, y0 = max(0, rec["x"]), max(0, rec["y"])
        x1 = min(SCREEN_W, rec["x"] + rec["w"])
        y1 = min(SCREEN_H, rec["y"] + rec["h"])
        if x1 <= x0 or y1 <= y0:
            continue                                # fully off-panel: nothing to prove
        a[y0:y1, x0:x1] = 255
        z = np.zeros_like(a)
        im = Image.fromarray(np.dstack([z, z, z, a]), mode="RGBA")
        L = _Layer("comp[%s]" % _ts.SOURCES[rec["src"]], [im], 0, 0, 0, False)
        L.foldable = False
        boxes.setdefault(int(rec["first"]), []).append(L)
    live = list(boxes.pop(0, []))                   # below every live layer
    for k in order:
        for L in boxes.pop(k, []):                  # directly under layer k
            live.append(L)
        kind = KINDS[k]
        hs = hands_style.get(kind, {}) or {}
        if not bool(hs.get("show", False)):
            continue
        px, py, cx, cy, _ = pivots[kind]
        blend = int(hs.get("blend", 0))
        rotating = k in (0, 1, 2)
        sdx, sdy = shadow_offsets(hands_style, kind)
        if k == 2 and tourb_imgs:
            src = tourb_imgs
        else:
            src = load_rgba(tdir, f"clock_hand_{kind}.png")
        if src is None:
            continue
        panels = _panel_placements(src, args.pad, cx, cy, px, py)
        if not panels:
            continue
        live.append(_Layer(kind, panels, cx, cy, blend, rotating))
        # Its shadow paints the same pixels one offset away, so it is part of
        # what a static layer above it must clear to be foldable at all.
        if shadow_on:
            simg = load_rgba(tdir, f"clock_shadow_{kind}.png")
            if simg is not None:
                sp = _panel_placements(simg, args.pad, cx + sdx, cy + sdy, px, py)
                if sp:
                    live.append(_Layer(kind + "-shadow", sp, cx + sdx, cy + sdy,
                                       0, rotating))
    # A bucket naming a layer that is not in the order at all (hidden, or one
    # prep never emitted) still has a window that must draw, and the device puts
    # it above every live layer — so the proof has to see it there too. Silently
    # dropping the box is how a fold gets taken that buries a window.
    for k in sorted(boxes):
        live.extend(boxes[k])
    ov = load_rgba(tdir, "clock_overlay.png")
    if ov is not None:
        # The overlay ships with pivot == crop origin == its own canvas spot
        # (see the emit_sprite call in main), so zeros here place it exactly
        # where the device draws it.
        op = _panel_placements(ov, args.pad, 0, 0, 0, 0)
        if op:
            live.append(_Layer("overlay(glass)", op, 0, 0, 0, False))
    return live


def _matte_base(tdir, args):
    """A representative background to score a forced fold against: the bank's
    keyframe, or the theme's plate. Only used by the --matte force report."""
    src = args.bg_gif or args.bg_frames
    if src:
        imgs, _ = (_load_gif_frames(src) if str(src).lower().endswith(".gif")
                   else _load_png_frames(src, args.bg_glob))
        return imgs[0]
    p = load_rgba(tdir, "clock_plate.png")
    return p if p is not None else Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 255))


def matte_fidelity(base_img, lower_layers, cand):
    """Quantify what FORCING a fold costs, so 'type 2' is a measured choice.

    Folding moves the candidate from ABOVE the lower layers to BELOW all of
    them (baked into the base), and alpha-compositing is not commutative, so
    compare exactly those two orders in the device's own RGB565:
        true   = (base . lower... . candidate)   -- today's runtime stack
        folded = ((base . candidate) . lower...) -- the baked result
    A lower MOVER enters at its authored pose, so the number is representative
    rather than exhaustive -- matte_plan's violation count is what proves the
    fold is not exact. Only ever called for an override: a proved fold agrees
    by construction and needs no number."""
    import numpy as np
    def run(seq):
        img = base_img
        for L in seq:
            for p in L.panels:
                img = matte_fold(img, p)
        return img
    true_px = _q565(np.asarray(run(list(lower_layers) + [cand]).convert("RGB"),
                               dtype=np.int64))
    got = _q565(np.asarray(run([cand] + list(lower_layers)).convert("RGB"),
                           dtype=np.int64))
    d = np.abs(true_px - got)
    per = np.maximum(np.maximum((d >> 11) & 31, (d >> 5) & 63), d & 31)
    diff = (d != 0)
    return int(diff.sum()), int(per.max()), (float(per[diff].mean()) if diff.any() else 0.0)


class Blob:
    """Append-only packed byte buffer that returns each segment's start offset."""
    def __init__(self):
        self.buf = bytearray()

    def add(self, data: bytes):
        off = len(self.buf)
        self.buf += data
        return off


def emit_sprite(blob, rgba, offx, offy, pivot_x, pivot_y, center_x, center_y, blend, show,
                diff=None):
    w, h = rgba.size
    color_off = blob.add(rgb565_le(rgba))
    blob.add(alpha_bytes(rgba))
    # pivot expressed in CROPPED coords = original pivot minus crop origin
    dx, dy, dw, dh = diff if diff else (0, 0, 0, 0)
    return dict(w=w, h=h, offx=offx, offy=offy, pivotx=pivot_x - offx, pivoty=pivot_y - offy,
                centerx=center_x, centery=center_y, blend=blend, show=int(bool(show)),
                color_off=color_off, diffx=dx, diffy=dy, diffw=dw, diffh=dh)


def blank_sprite(cx, cy, show=0):
    return dict(w=0, h=0, offx=0, offy=0, pivotx=0, pivoty=0,
                centerx=cx, centery=cy, blend=0, show=show, color_off=0,
                diffx=0, diffy=0, diffw=0, diffh=0)


def bake_tourbillon(blob, style, args):
    """Frame bank for an ANIMATED rotating second hand (tourbillon/oscillating
    balance): a PNG sequence whose frames share geometry and differ only in
    their internal motion. Every frame is cropped to the ONE ink bbox of the
    whole cycle (so all frames rotate about the same pivot and dirty the same
    box), and each frame stores the rect that differs from its predecessor --
    chained and cyclic, exactly like the GIF delta bank's rect lists. The
    device rotates the frame it shows by the seconds angle; the diff rect is
    what makes that cheap: between swaps only those pixels can have changed,
    so the tick re-marks the rotated diff rect instead of the layer's full
    swept box (see clock_view.c's mark_all_layers/tourbillon_advance)."""
    import numpy as np
    hs = (style.get("hands", {}) or {}).get("second", {}) or {}
    px = int(hs.get("pivotX", SCREEN_W // 2)); py = int(hs.get("pivotY", SCREEN_H // 2))
    cx = int(hs.get("centerX", px)); cy = int(hs.get("centerY", py))

    items = _png_frame_items(args.tourbillon, None)
    imgs = [Image.open(io.BytesIO(b)).convert("RGBA") for _, b in items]
    if len({im.size for im in imgs}) != 1:
        raise SystemExit("--tourbillon: frames must all be the same size")
    if imgs[0].size != (SCREEN_W, SCREEN_H):
        imgs = [resize_full(im) for im in imgs]
    return pack_tourbillon(blob, imgs, px, py, cx, cy, args.pad, args.diff_thresh,
                           getattr(args, "tourbillon_fps", 15) or 15)


def pack_tourbillon(blob, imgs, px, py, cx, cy, pad, diff_thresh, fps=15):
    """The carriage bank, fed with already-decoded frames. Split out of
    bake_tourbillon for the same reason pack_bg_bank was: the composer's in-browser
    baker holds itself against this maths on the same pixels (see
    tools/watch-composer/_selftest/headless-orbtheme.test.js)."""
    import numpy as np
    arrs = [np.asarray(im, dtype=np.int16) for im in imgs]

    # ONE ink bbox for the whole cycle: every frame is cropped to it, so one
    # pivot/geometry record serves the bank and the swept box never moves.
    l = t = 1 << 30
    r = b = -1
    for a in arrs:
        ys, xs = np.nonzero(a[:, :, 3] > 0)
        if not len(xs):
            continue
        l = min(l, int(xs.min())); t = min(t, int(ys.min()))
        r = max(r, int(xs.max()) + 1); b = max(b, int(ys.max()) + 1)
    if r <= l or b <= t:
        raise SystemExit("--tourbillon: frames are fully transparent")
    l = max(0, l - pad); t = max(0, t - pad)
    r = min(SCREEN_W, r + pad); b = min(SCREEN_H, b + pad)

    n = len(imgs)
    if n > 16:
        raise SystemExit(f"--tourbillon: {n} frames exceeds the baked max of 16 "
                         f"(clk_theme.tourbillon[] in clock_assets.h)")
    thr = max(1, diff_thresh) * 3        # per-sum, same feel as the bank's

    recs = []
    for i, im in enumerate(imgs):
        crop = im.crop((l, t, r, b))
        d = np.abs(arrs[i] - arrs[(i - 1) % n]).sum(axis=2)
        ys, xs = np.nonzero(d > thr)
        if len(xs):
            dl = max(0, int(xs.min()) - 3 - l)
            dt = max(0, int(ys.min()) - 3 - t)
            dr = min(crop.size[0], int(xs.max()) + 4 - l)
            db = min(crop.size[1], int(ys.max()) + 4 - t)
            diff = (dl, dt, dr - dl, db - dt)
        else:
            diff = (0, 0, 0, 0)               # identical to predecessor: mark nothing
        recs.append(emit_sprite(blob, crop, l, t, px, py, cx, cy, 0, True, diff=diff))
    print("  {:8s} tourbillon: {} frames @ {} fps, bbox=({},{},{},{}) pivot=({},{}) centre=({},{})".format(
        "anim", n, fps, l, t, r - l, b - t, px, py, cx, cy))
    return recs




def _lvgl_trig(ang_10):
    """(sinma, cosma) in 1024 units exactly as lv_draw_sw_transform.c derives
    them: lv_trigo_sin on the 0.1-degree table with linear interpolation, then
    >> (LV_TRIGO_SHIFT - 10) with NO rounding. Using the true table -- rather
    than math.sin/cos -- is essential: the truncation biases the effective
    centre of rotation, so the pivot that minimises the *observed* wobble is
    not the pivot that minimises the geometric one (2 px apart on this theme's
    second hand)."""
    import numpy as np
    def t(deg):
        d = deg % 360
        if d <= 90:
            v = int(16384 * np.sin(np.deg2rad(d)))
        elif d <= 180:
            v = int(16384 * np.cos(np.deg2rad(d - 90)))
        elif d <= 270:
            v = int(-16384 * np.sin(np.deg2rad(d - 180)))
        else:
            v = int(-16384 * np.cos(np.deg2rad(d - 270)))
        return v
    a = ang_10 % 3600
    lo = a // 10
    rem = a - lo * 10
    s1, s2 = t(lo), t(lo + 1)
    c1, c2 = t(lo + 90), t(lo + 90 + 1)
    sinma = (s1 * (10 - rem) + s2 * rem) // 10
    cosma = (c1 * (10 - rem) + c2 * rem) // 10
    if a % 3600 == 0:                    # LVGL's bit-exact no-rotation case
        return 0, 1024
    return sinma >> 4, cosma >> 4


def _hub_orbit(a, px, py, cx, cy, hub_r=15):
    """Worst distance (px) the hand's hub drifts from its screen pin (cx,cy)
    over a full revolution, AS THE PANEL RENDERS IT -- i.e. rotating the sprite
    about integer pivot (px,py) through LVGL's quantised rotator and measuring
    where the hub's ink centroid actually lands.

    This is the quantity the eye catches as the hand "bobbing". A pivot that is
    off the hub's optical centre makes the hub sweep a small circle instead of
    staying put; using the naive alpha centroid as the objective picks a pivot
    up to ~2 px wrong, because it ignores the rotator's own truncation."""
    import math
    import numpy as np
    h, w = a.shape
    ys, xs = np.mgrid[0:h, 0:w]
    dx = (xs - px).astype(np.float64)
    dy = (ys - py).astype(np.float64)     # sprite y grows down; rotator flips
    a = a.astype(np.float64)
    worst = 0.0
    for k in range(60):                   # 6 deg/frame sweep beat
        sinma, cosma = _lvgl_trig(k * 60)
        c = cosma / 1024.0
        s = sinma / 1024.0
        X = (c * dx - s * (-dy))          # about the pivot, in sprite px
        Y = (s * dx + c * (-dy))
        d = np.hypot(X, Y)
        m = (a > 128) & (d < hub_r)
        t = a[m].sum()
        if t < 40:
            continue
        hx = (a[m] * X[m]).sum() / t
        hy = (a[m] * Y[m]).sum() / t
        worst = max(worst, math.hypot(hx, hy))
    return worst


def snap_pivot_to_hub(img, px, py, cx, cy, win_y=10, win_x=5, min_gain=1.0):
    """Correct a hand's rotation pivot to the centre that keeps its hub still
    under LVGL's rotator.

    A theme's pivotX/pivotY is an authored value that is easy to get a few px
    wrong (this theme's second hand declared (16,75) for a hub LVGL rotates at
    (15,71)). Rotating about a point off the hub makes the hand's centre orbit
    that point -- the wobble -- and leaves the ink misaligned against the plate
    band it repaints, so a stale hairline shows at the swept edge. We search a
    window about the declared pivot for the one that minimises the hub's bob AS
    LVGL RENDERS IT (see _hub_orbit/_lvgl_trig) and adopt it ONLY when it
    removes a real offset (>= min_gain px). A correctly-authored pivot (hour/
    minute sit on their hub already, bob ~0) fails the gain test and is
    returned untouched.
    """
    if img is None:
        return px, py, 0.0
    import numpy as np
    a = np.asarray(img.convert("RGBA"))[..., 3]
    base = _hub_orbit(a, px, py, cx, cy)
    if base < min_gain:
        return px, py, base
    best = (base, px, py)
    for ny in range(py - win_y, py + win_y + 1):
        for nx in range(px - win_x, px + win_x + 1):
            if nx == px and ny == py:
                continue
            o = _hub_orbit(a, nx, ny, cx, cy)
            if o < best[0]:
                best = (o, nx, ny)
    if base - best[0] < min_gain:
        return px, py, base
    return best[1], best[2], best[0]




# ---------------------------------------------------------------------------
# GIF background bank (delta-rect mode)
# ---------------------------------------------------------------------------
# Frames are composited exactly like tools/bake_gif.py (GIF89a disposal
# honoured, cover-scale to the panel) so the clock shows the same pixels the
# bench page does — then stored as a keyframe + per-frame changed rects.

def _frames_to_u16(img):
    """PIL RGB/RGBA full-canvas image -> numpy uint16 RGB565 (native LE) array."""
    import numpy as np
    rgb = np.asarray(img.convert("RGB"), dtype=np.uint16)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _fit_to_panel(img, fit="cover"):
    """Fit one composited logical-resolution RGBA frame to the panel, 1:1.

    THE ONLY RESIZE IN THE WHOLE PIPELINE: shared by the GIF and PNG frame
    loaders so both land on strictly panel-sized pixels (the device canvas and
    the packed rects are 1:1 with the glass, forever after this point).

    fit="cover"   cover-scale the full logical frame to the panel, then centre
                  (crop only the rounding overshoot). Keeps the whole artwork.
    fit="crop"    take the panel-sized window out of the logical frame at 1:1
                  (no resampler at all) when the asset is big enough; falls back
                  to cover when it is smaller than the panel.

    A frame already authored at panel size (the usual case for a dial exported
    straight from the studio at 466x466) is passed through untouched, so a
    pixel-perfect asset never sees the resampler at all."""
    iw, ih = img.size
    if (iw, ih) == (SCREEN_W, SCREEN_H):
        return img
    if fit == "crop" and iw >= SCREEN_W and ih >= SCREEN_H:
        # 1:1 centre crop: no resampler, so the artwork's own pixel grid is
        # preserved exactly (this is what stops sub-pixel edge stepping).
        left, top = (iw - SCREEN_W) // 2, (ih - SCREEN_H) // 2
        return img.crop((left, top, left + SCREEN_W, top + SCREEN_H))
    # cover-scale to panel like bake_gif --mode cover
    scale = max(SCREEN_W / iw, SCREEN_H / ih)
    nw, nh = int(round(iw * scale)), int(round(ih * scale))
    r = img.resize((nw, nh), Image.LANCZOS)
    left, top = (nw - SCREEN_W) // 2, (nh - SCREEN_H) // 2
    return r.crop((left, top, left + SCREEN_W, top + SCREEN_H))


def _load_gif_frames(path, fit="cover"):
    """Composite the GIF into opaque RGBA images at panel size (disposal-aware).
    Returns (images, per-frame delays in ms).

    Frames are composited at the GIF's own logical resolution and only then
    fitted (see _fit_to_panel), so the device canvas and packed rects stay 1:1."""
    im = Image.open(path)
    n = getattr(im, "n_frames", 1)
    disposals, delays = [], []
    for i in range(n):
        im.seek(i)
        disposals.append(im.info.get("disposal", 0))
        delays.append(int(im.info.get("duration", 40) or 40))
    logical = im.size
    canvas = Image.new("RGBA", logical, (0, 0, 0, 255))
    out = []
    for i in range(n):
        d = disposals[i]
        backup = canvas.copy()
        im.seek(i)
        fr = im.convert("RGBA")
        canvas.paste(fr, (0, 0), fr)
        out.append(_fit_to_panel(canvas, fit))
        if d == 2:
            canvas = Image.new("RGBA", logical, (0, 0, 0, 255))
        elif d == 3:
            canvas = backup
    return out, delays


def _natkey(name):
    """Sort key that reads digits as numbers: frame_2 before frame_10, so a
    plain frame_1..frame_15 drop plays in the order it was made rather than the
    lexicographic one it would otherwise sort into."""
    return [int(t) if t.isdigit() else t.lower()
            for t in re.split(r"(\d+)", os.path.basename(name))]


def _png_frame_items(src, frame_glob):
    """Resolve a --bg-frames argument to an ordered [(name, bytes)] PNG list.

    Accepted shapes, in the order tried:
      *.zip        every .png entry in the archive (studio exports arrive
                   zipped; reading it in place avoids a throwaway extract and
                   the risk of baking a stale folder left over from last
                   time), or those matching frame_glob -- a kit zip also holds
                   hands/tourbillon/layers PNGs, so filter it or the bake
                   quietly turns them into background frames)
      directory    files matching frame_glob, else every .png in it
      glob         e.g. 'shots/frame_*.png'
      single .png  a one-frame background
    A raw .bin is refused by main() before this ever runs."""
    p = os.path.expanduser(src)
    if p.lower().endswith(".zip"):
        if not os.path.exists(p):
            raise SystemExit(f"--bg-frames: no such archive: {p}")
        with zipfile.ZipFile(p) as z:
            items = [(zi.filename, z.read(zi.filename)) for zi in z.infolist()
                     if not zi.is_dir() and zi.filename.lower().endswith(".png")]
        if frame_glob:
            # A kit zip carries hands/tourbillon/layers PNGs beside the base
            # frames; without this filter --bg-glob is silently ignored for
            # zips and every PNG becomes a "background frame". Match the
            # ARCHIVE PATH, not the basename: base/ and tourbillon/ share the
            # names frame_01…08.png, so a basename match cannot disambiguate.
            items = [kv for kv in items
                     if fnmatch.fnmatch(kv[0], frame_glob)]
        if not items:
            raise SystemExit(f"--bg-frames: no PNG entries inside {p}"
                             + (f" matching {frame_glob!r}" if frame_glob else ""))
        return sorted(items, key=lambda kv: _natkey(kv[0]))
    if os.path.isdir(p):
        names = [n for n in os.listdir(p) if n.lower().endswith(".png")]
        if frame_glob:
            hit = [n for n in names if fnmatch.fnmatch(n, frame_glob)]
            if hit:
                names = hit
        if not names:
            raise SystemExit(f"--bg-frames: no PNGs in {p}"
                             + (f" matching {frame_glob}" if frame_glob else ""))
        return [(n, open(os.path.join(p, n), "rb").read())
                for n in sorted(names, key=_natkey)]
    hits = _glob.glob(p) if any(c in p for c in "*?[") else []
    hits = [h for h in hits if h.lower().endswith(".png")]
    if not hits and p.lower().endswith(".png") and os.path.exists(p):
        hits = [p]
    if not hits:
        raise SystemExit(f"--bg-frames: nothing to bake from {src!r} (want a .zip "
                         f"of PNGs, a folder of PNGs, a PNG glob, or one .png)")
    return [(h, open(h, "rb").read()) for h in sorted(hits, key=_natkey)]


def _load_png_frames(src, frame_glob=None, fps=0.0, fit="cover"):
    """Opaque RGBA panel-size images from a PNG sequence + per-frame delays (ms).

    PNGs carry no timing, so the caller supplies the beat (--bg-fps, falling
    back to the theme's bgAnim fps, then 25). Frames are matted over true black
    -- the right matte on an AMOLED, where a black pixel is an off pixel -- and
    matched to the GIF path so both feed one delta packer."""
    items = _png_frame_items(src, frame_glob)
    ms = max(1, int(round(1000.0 / fps))) if fps > 0 else 40
    out = []
    for name, data in items:
        im = Image.open(io.BytesIO(data))
        im.load()
        rgba = im.convert("RGBA")
        if rgba.getextrema()[3][0] < 255:
            base = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
            base.paste(rgba, (0, 0), rgba)
            rgba = base
        out.append(_fit_to_panel(rgba, fit))
    if not out:
        raise SystemExit(f"--bg-frames: no decodable PNG frame in {src!r}")
    return out, [ms] * len(out)


def _box_density(mask):
    """Per-pixel count of True pixels in the inclusive 5x5 box around it.

    Pure numpy (integral image), so the bake needs no scipy dependency. Used
    to separate REAL motion (gear/glow edges: changes come in dense clusters)
    from sensor-style transients (a handful of isolated pixels scattered
    page-wide: GIF sparkle frames and dither)."""
    import numpy as np
    m2 = np.pad(mask, 2).astype(np.int32)
    ii = np.pad(m2.cumsum(0).cumsum(1), ((1, 0), (1, 0)))   # exclusive prefix
    h, w = mask.shape
    y, x = np.mgrid[0:h, 0:w]
    return ii[y + 5, x + 5] - ii[y, x + 5] - ii[y + 5, x] + ii[y, x]


def _pack_mask(mask, tile=16, max_rects=16, max_flush=0.62):
    """bool (H,W) diff mask -> list of (x, y, w, h) pixel-space rects.

    The mask is snapped to a tile grid (a tile is active if any pixel in it
    changed), covered by greedy maximal rectangles (classic monotone-histogram
    stack, largest-first, repeated until nothing is left), then merged pair by
    pair (cheapest bounding box first) down to `max_rects`. The cap matters:
    LVGL's invalid-area buffer holds LV_INV_BUF_SIZE (32) rects before a
    refresh degrades to a whole-screen invalidate, and the rotating hands
    claim several slots of their own each tick.

    Collapses to a single full-page rect (1 inv slot, same flush cost as any
    cover that spans the page) if the mask is near-total, if the cheapest
    remaining merge would add >25% of a page of empty area, or if the final
    cover would flush more than `max_flush` of the page anyway."""
    import numpy as np
    if not mask.any():
        return []
    ty = (SCREEN_H + tile - 1) // tile
    tx = (SCREEN_W + tile - 1) // tile
    pad = np.zeros((ty * tile, tx * tile), dtype=bool)
    pad[:SCREEN_H, :SCREEN_W] = mask
    tm = pad.reshape(ty, tile, tx, tile).any(axis=(1, 3))
    if float(tm.mean()) > max_flush:
        return [(0, 0, SCREEN_W, SCREEN_H)]

    # ---- greedy maximal rectangle cover (largest all-active tile rect first,
    # via the classic monotone-histogram stack, repeated until covered) ----
    rects_t = []                                # (x, y, w, h) in tile units
    work = tm.copy()
    while work.any():
        h = np.zeros(tx + 1, np.int32)          # +1 sentinel column
        best = (0, 0, 0, 0, 0)                  # area, x0, y0, h, w
        for r in range(ty):
            h[:tx] = np.where(work[r], h[:tx] + 1, 0)
            stack = []                          # (start_col, height), strictly increasing
            for c in range(tx + 1):
                cur = int(h[c])
                start = c
                while stack and stack[-1][1] > cur:
                    s0, hh = stack.pop()
                    area = hh * (c - s0)
                    if area > best[0]:
                        best = (area, s0, r - hh + 1, hh, c - s0)
                    start = s0
                if cur and (not stack or stack[-1][1] < cur):
                    stack.append((start, cur))
        if best[0] == 0:
            return [(0, 0, SCREEN_W, SCREEN_H)]  # cover stalled: not worth packing
        _, x0, y0, hh, ww = best
        rects_t.append((x0, y0, ww, hh))
        work[y0:y0 + hh, x0:x0 + ww] = False

    # ---- merge cheapest pairs until the rect cap is met ----
    while len(rects_t) > max_rects:
        n = len(rects_t)
        bi, bj, bad = -1, -1, None
        for i in range(n):
            xi, yi, wi, hi = rects_t[i]
            for j in range(i + 1, n):
                xj, yj, wj, hj = rects_t[j]
                x0 = xi if xi < xj else xj
                y0 = yi if yi < yj else yj
                x1 = max(xi + wi, xj + wj)
                y1 = max(yi + hi, yj + hj)
                add = (x1 - x0) * (y1 - y0) - wi * hi - wj * hj
                if bad is None or add < bad:
                    bad, bi, bj = add, i, j
        if bad is None or bad > 0.25 * ty * tx:
            return [(0, 0, SCREEN_W, SCREEN_H)]
        xi, yi, wi, hi = rects_t[bi]
        xj, yj, wj, hj = rects_t[bj]
        x0 = xi if xi < xj else xj
        y0 = yi if yi < yj else yj
        merged = (x0, y0, max(xi + wi, xj + wj) - x0, max(yi + hi, yj + hj) - y0)
        rects_t = [r for t, r in enumerate(rects_t) if t not in (bi, bj)]
        rects_t.append(merged)

    rects = [(x * tile, y * tile, min(w * tile, SCREEN_W - x * tile),
              min(h * tile, SCREEN_H - y * tile)) for x, y, w, h in rects_t]
    covered = sum(r[2] * r[3] for r in rects)
    if covered > max_flush * SCREEN_W * SCREEN_H:
        return [(0, 0, SCREEN_W, SCREEN_H)]
    return rects


def _gif_transition(canvas, tgt, painted, diff_thresh, K=12, max_rects=16,
                    max_flush=0.62, occl=None):
    """One canvas->target transition -> (rect list [(x,y,w,h),...], kept mask).

    A pixel counts as changed when its perceptual RGB565 max-channel step
    reaches the threshold AND the change is part of a CLUSTER: >= K of the 25
    pixels in its 5x5 box changed too. Real motion (gear teeth, flame licks,
    glow edges) is dense; the GIF's page-wide sparkle frames and dither are
    isolated single pixels, and blitting one of those would freeze a bright
    spark on the panel forever (nothing later dims it, since the bake only
    ever writes what it itself flagged). Pixels we previously painted (the
    `painted` map, seeded by this simulation's own rect copies) are exempt
    from the cluster test when DIMMING: that is the canvas undoing its own
    leftover, and keeping it makes the loop drift-free.

    `occl` (see bg_occluder) drops texels no live layer above the bank can ever
    uncover. It is applied to the ACCEPTED mask only, AFTER the cluster test:
    the density count has to see the whole moving gear, or a tooth tip visible
    through a narrow opening would lose its cluster and never be pushed at all.

    Heavier frames fall back through escalating thresholds before collapsing
    to a full-page transition (cheaper than a cover that spills anyway)."""
    import numpy as np
    cfr = (canvas >> 11) & 0x1F; cfg = (canvas >> 5) & 0x3F; cf = canvas & 0x1F
    tfr = (tgt >> 11) & 0x1F;    tfg = (tgt >> 5) & 0x3F;    tf = tgt & 0x1F
    sdr = tfr.astype(np.int32) - cfr
    sdg = tfg.astype(np.int32) - cfg
    sdb = tf.astype(np.int32) - cf
    m = np.maximum(np.maximum(np.abs(sdr) * 8, np.abs(sdg) * 4),
                   np.abs(sdb) * 8)             # perceptual max-channel step
    lum = sdr * 76 + sdg * 150 + sdb * 30        # signed luma step (dim < 0)
    changed = tgt != canvas
    raw = np.zeros(canvas.shape, dtype=bool)
    for t in (diff_thresh, diff_thresh * 2, diff_thresh * 3, diff_thresh * 5):
        raw = changed & (m >= t)
        if not raw.any():
            return [], None
        dens = _box_density(raw)
        mask = raw & (dens >= K)
        if painted is not None:
            mask = mask | (raw & (lum < 0) & painted)   # undo own leftovers
        if occl is not None:
            mask = mask & ~occl                        # cannot reach the glass
        lst = _pack_mask(mask, max_rects=max_rects, max_flush=max_flush)
        if (len(lst) == 1 and lst[0] == (0, 0, SCREEN_W, SCREEN_H)
                and t != diff_thresh * 5):
            continue                             # collapsed: try a stricter view
        return lst, mask
    return [(0, 0, SCREEN_W, SCREEN_H)], (raw & ~occl if occl is not None else raw)


def _temporal_snap(F, eps):
    """Kill the GIF's dither/sparkle noise at the SOURCE, before packing.

    A GIF's per-frame dither makes ~70% of this asset's pixels flicker by a
    single RGB565 code even where the artwork is static. The delta packer
    cannot represent that cheaply (page-wide single-code churn) and its
    cluster filter therefore DROPS those changes, which leaves the device
    canvas holding another frame's pixels over ~11% of the page — a moving
    edge held one frame behind is exactly the "background shifts a pixel one
    way or another" artifact.

    Pixels whose whole 20-frame cycle stays within +-eps of one code per
    channel are therefore replaced, in every frame, by the temporal median of
    that cycle: they become bit-identical forever, contribute zero delta, and
    can never be left stale. Pixels that move more than eps (the orb, gears,
    glow) are passed through untouched.

    Returns (frames, quiet_mask)."""
    import numpy as np
    n = len(F)
    if eps <= 0 or n < 3:
        return F, np.zeros(F[0].shape, dtype=bool)
    ch = []
    for sh, msk in ((11, 0x1F), (5, 0x3F), (0, 0x1F)):
        ch.append(np.stack([(f >> sh) & msk for f in F]).astype(np.int32))
    quiet = np.ones(F[0].shape, dtype=bool)
    for c in ch:
        quiet &= (c.max(axis=0) - c.min(axis=0)) <= eps
    med = [np.rint(np.median(c, axis=0)).astype(np.uint16) for c in ch]
    out = []
    for k in range(n):
        r = np.where(quiet, med[0], ch[0][k]).astype(np.uint16)
        g = np.where(quiet, med[1], ch[1][k]).astype(np.uint16)
        b = np.where(quiet, med[2], ch[2][k]).astype(np.uint16)
        out.append(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F))
    return out, quiet


def bake_gif_bg(blob, gif_path, diff_thresh, denoise=2, fit="cover"):
    """Bake the GIF background into the blob as keyframe + delta rects."""
    return bake_bg_bank(blob, gif_path, diff_thresh, denoise=denoise, fit=fit)


def bake_bg_bank(blob, src, diff_thresh, denoise=2, fit="cover",
                 fps=0.0, frame_glob=None, frame_step=1, matte=None,
                 max_rects=16, max_flush=0.62, occl=None):
    """Bake an animated background (GIF or PNG frame sequence) into the blob
    as one keyframe + per-frame delta rects.

    `frame_step` DECIMATES the source sequence: the pack is one authored loop
    sampled at the generator's fps (e.g. 30 PNGs = one second at 30 fps), and
    the display pipe plays at its own tick rate; taking every Nth frame keeps
    the loop at one authored second when N = source_frames / playback_fps
    (30 -> 15 @ 15 fps, and from a 60-frame pack 60 -> 15 @ 15 or 60 -> 12 @
    12). The step must divide the frame count so the loop still closes. For
    GIFs the kept frames absorb their skipped neighbours' delay, so the total
    period is preserved exactly; PNG delays are uniform, so re-pacing is just
    the round-trip through --bg-fps.

    Returns dict(frames, key_off, rects=[(x,y,w,h,off), ...],
                 first=[rect index where each frame's list starts..., len])
    Frame i is displayed by applying rects[first[i]:first[i+1]] over the
    canvas left holding frame i-1 (cyclically: frame 0 follows frame N-1).

    Diffs are computed against the SIMULATED device canvas, whose state is
    therefore exactly reproducible on the device. The simulation is warmed
    over a few whole cycles first and the LAST cycle is the one baked: the
    device then replays that exact periodic canvas trajectory forever, so no
    pixel ever silently drifts away from the source animation's look.

    `src` is either a GIF (paced by its own per-frame delays) or anything
    --bg-frames accepts as a PNG sequence (paced at `fps`, since PNGs have no
    timing of their own). Both go through the identical snap/warm/replay packer
    so a bank baked from PNGs behaves exactly like one baked from a GIF.

    `occl` (see bg_occluder) drops texels no live layer above the bank can ever
    uncover: the commit hook rebuilds a marked row from the canvas and paints
    those layers over it, so a texel one of them covers at alpha 255 costs DMA
    and buys no pixel. It is a pack-time cull only -- the frames themselves are
    untouched, so a bake with --bg-occlude off is byte-for-byte the old one."""
    import numpy as np
    is_gif = src.lower().endswith(".gif")
    if is_gif:
        imgs, delays = _load_gif_frames(src, fit=fit)
    else:
        imgs, delays = _load_png_frames(src, frame_glob=frame_glob, fps=fps, fit=fit)
    if frame_step < 1:
        raise SystemExit("--frame-step must be >= 1")
    if frame_step > 1:
        if len(imgs) % frame_step:
            raise SystemExit(f"--frame-step {frame_step} does not divide "
                             f"{len(imgs)} frames — the loop would not close")
        imgs = imgs[::frame_step]
        if (not is_gif) and fps > 0 and len(imgs) != max(1, int(round(fps))):
            raise SystemExit(
                f"--frame-step {frame_step} left {len(imgs)} frames but --bg-fps "
                f"{fps:g} wants {int(round(fps))} kept for one authored "
                f"loop per second — use --frame-step "
                f"{len(imgs) * frame_step // int(round(fps))} on this "
                f"{len(imgs) * frame_step}-frame pack (or export a pack whose "
                f"frame count is divisible by both target fps)")
        if is_gif:
            # GIF delays ARE the source tempo; kept frames absorb their
            # skipped neighbours so the total period is preserved.
            delays = [sum(delays[i:i + frame_step])
                      for i in range(0, len(delays), frame_step)]
        else:
            # PNG delays were synthesised from --bg-fps, which is already the
            # PLAYBACK rate of the kept frames (step = source_frames / fps), so
            # each kept frame holds exactly one of them — no re-pacing, but the
            # list still has to be decimated to the KEPT count. The header
            # declares clk_gif_delays[gif["frames"]]; leaving one entry per
            # SOURCE frame overruns that (60 initialisers into a [12]), and the
            # surplus is only silently dropped while every delay is equal.
            delays = delays[::frame_step]
        print(f"  decimated every {frame_step}th frame: {len(imgs)} kept "
              f"(loop still 1 authored period)")
    return pack_bg_bank(blob, imgs, delays, diff_thresh, denoise=denoise,
                        fit=fit, matte=matte, max_rects=max_rects,
                        max_flush=max_flush, occl=occl, src=src,
                        kind="gif" if is_gif else "frames")


def pack_bg_bank(blob, imgs, delays, diff_thresh, denoise=2, fit="cover",
                 matte=None, max_rects=16, max_flush=0.62, occl=None,
                 src="?", kind="frames"):
    """The delta-rect packer, fed with already-decoded panel-size RGBA frames.

    Split out of bake_bg_bank so the composer's in-browser baker (tools/watch-composer/
    orbtheme-baker.js) can be held against THIS code on the same frames: a browser cannot
    read a GIF or a folder of PNGs the way PIL does, but it can be given the same pixels,
    and the packing is the half where a mismatch costs someone's dial. The two share this
    one implementation, which is the only reason the mirror test can be byte-exact."""
    import numpy as np
    if matte:
        # Fold the proved static layers into every frame of the bank, keyframe
        # included. The matte is identical frame to frame, so it contributes no
        # delta and the packer's diffing/rect packing is untouched -- it just
        # starts from a background that already carries the layer.
        for m in matte:
            imgs = [matte_fold(im, m) for im in imgs]
        print(f"  bg bank carries {len(matte)} baked static layer(s)")
    F = [_frames_to_u16(im) for im in imgs]
    print(f"  {kind} bg: {src} -> {len(F)} frames @ "
          f"{SCREEN_W}x{SCREEN_H} fit={fit} "
          f"(diff thresh {diff_thresh}, cluster>=12/25, tile16<={max_rects} rects)")
    F, quiet = _temporal_snap(F, denoise)
    n = len(F)
    if denoise > 0:
        print(f"  temporal snap eps={denoise}: {float(quiet.mean()) * 100:.1f}% of the page "
              f"snapped bit-static (dither removed at the source)")

    WARM_CYCLES = 3
    canvas = F[0].copy()
    start = canvas
    per = {}
    per_m = {}
    for warm in range(WARM_CYCLES):
        start = canvas.copy()
        painted = np.zeros(canvas.shape, dtype=bool)
        per = {}
        per_m = {}
        for k in range(n):
            tgt_i = (k + 1) % n
            tgt = F[tgt_i]
            lst, _mask = _gif_transition(canvas, tgt, painted, diff_thresh,
                                         max_rects=max_rects,
                                         max_flush=max_flush, occl=occl)
            blitted = []
            for x, y, w, h in lst:
                canvas[y:y + h, x:x + w] = tgt[y:y + h, x:x + w]
                painted[y:y + h, x:x + w] = True
                blitted.append((x, y, w, h))
            per[tgt_i] = blitted
            per_m[tgt_i] = (_mask if _mask is not None
                            else np.zeros(canvas.shape, dtype=bool))
        if warm < WARM_CYCLES - 1:
            print(f"  warm cycle {warm}: canvas at cycle end "
                  f"{'== cycle start (periodic)' if np.array_equal(canvas, start) else 'still evolving'}")

    key_off = blob.add(start.astype("<u2").tobytes())
    for j in range(n):
        pct = 100.0 * sum(r[2] * r[3] for r in per[j]) / (SCREEN_W * SCREEN_H)
        print(f"  f{j:02d}: {len(per[j]):2d} rects, {pct:5.1f}% of page")

    rects, first = [], [0]
    for j in range(n):
        for x, y, w, h in per[j]:
            off = blob.add(F[j][y:y + h, x:x + w].astype("<u2").tobytes())
            rects.append((x, y, w, h, off))
        first.append(len(rects))
    tot = sum(r[2] * r[3] for r in rects)
    print(f"  delta bank: {len(rects)} rects, packed "
          f"{100.0 * tot * 2 / (n * SCREEN_W * SCREEN_H * 2):.1f}% of a full bank")
    return dict(frames=n, key_off=key_off, rects=rects, first=first, delays=delays,
                masks=per_m, key_start=start, frames_u16=F, occl=occl)


def dump_inspection(dump_dir, blob_buf, gif, sprites):
    """Write per-asset PNGs so a bake can be inspected without a panel.

    The picture on the glass is assembled from four separate things -- a
    keyframe, per-frame delta rects, a cluster-filtered change mask, and
    hand/shadow sprites with a planar alpha -- so an artifact in a photo of the
    dial cannot be attributed to any one of them. Each stage gets its own file.

    The one to look at first is stale_NN.png: where the canvas the DEVICE ends
    up holding after replaying the baked rects still disagrees with the GIF
    frame it is supposed to be showing (>=4 RGB565 codes in any channel). It is
    the ghost/sparkle map, and it should be solid black.
    """
    import numpy as np
    os.makedirs(dump_dir, exist_ok=True)
    # Drop this tool's own previous output first: an inspection PNG left over
    # from an older bake is worse than no PNG at all, because it looks like
    # evidence about the bake you just ran.
    for f in os.listdir(dump_dir):
        if (f.startswith(("canvas_", "mask_", "rects_", "stale_", "sprite_"))
                or f == "keyframe.png") and f.endswith(".png"):
            os.remove(os.path.join(dump_dir, f))

    def u16_png(arr, path):
        rgb = np.dstack([(arr >> 8) & 0xF8, (arr >> 3) & 0xFC, (arr << 3) & 0xF8])
        Image.fromarray(rgb.astype(np.uint8), "RGB").save(path)

    def mask_png(m, path):
        Image.fromarray((m.astype(np.uint8)) * 255, "L").save(path)

    def blob_u16(off, count):
        return np.frombuffer(bytes(blob_buf[off:off + count * 2]),
                             dtype="<u2").copy()

    # ---- sprites: colour plane composited over its planar alpha -----------
    for name, rec in sprites:
        w, h = int(rec["w"]), int(rec["h"])
        if w == 0 or h == 0:
            continue
        co = int(rec["color_off"])
        col = blob_u16(co, w * h).reshape(h, w)
        alp = np.frombuffer(bytes(blob_buf[co + w * h * 2: co + w * h * 2 + w * h]),
                            dtype=np.uint8).reshape(h, w)
        rgb = np.dstack([((col >> 8) & 0xF8), ((col >> 3) & 0xFC),
                         ((col << 3) & 0xF8)]).astype(np.int32)
        yy, xx = np.mgrid[0:h, 0:w]
        chk = np.where(((yy // 8 + xx // 8) % 2) == 0, 70, 130).astype(np.int32)
        a = (alp.astype(np.int32) * 256) // 255
        out = (rgb * a[..., None] + np.dstack([chk] * 3) * (256 - a[..., None])) // 256
        Image.fromarray(out.astype(np.uint8), "RGB").save(
            os.path.join(dump_dir, f"sprite_{name}.png"))

    if gif is None:
        return

    n = gif["frames"]
    F = gif["frames_u16"]

    # ---- keyframe: exactly the bytes the panel holds when the dial starts --
    key = blob_u16(gif["key_off"], SCREEN_W * SCREEN_H).reshape(SCREEN_H, SCREEN_W)
    u16_png(key, os.path.join(dump_dir, "keyframe.png"))

    # ---- per-frame replay of the baked rects, as the device performs it ----
    canvas = key.copy()
    stale_tot = 0
    stale_hid = 0
    for j in range(n):
        if j:                                    # frame 0 IS the keyframe
            for x, y, w, h, off in gif["rects"][gif["first"][j]:gif["first"][j + 1]]:
                canvas[y:y + h, x:x + w] = blob_u16(off, w * h).reshape(h, w)
        u16_png(canvas, os.path.join(dump_dir, f"canvas_{j:02d}.png"))
        mask_png(gif["masks"][j], os.path.join(dump_dir, f"mask_{j:02d}.png"))

        lay = np.zeros((SCREEN_H, SCREEN_W), np.uint8)
        for k, (x, y, w, h, off) in enumerate(
                gif["rects"][gif["first"][j]:gif["first"][j + 1]]):
            lay[y:y + h, x:x + w] = 40 + (k * 29) % 190
            lay[y:y + 1, x:x + w] = 255
            lay[y + h - 1:y + h, x:x + w] = 255
            lay[y:y + h, x:x + 1] = 255
            lay[y:y + h, x + w - 1:x + w] = 255
        Image.fromarray(lay, "L").save(os.path.join(dump_dir, f"rects_{j:02d}.png"))

        t = F[j]
        e = np.maximum.reduce([
            np.abs(((t >> 11) & 0x1F).astype(np.int32) - ((canvas >> 11) & 0x1F)),
            np.abs(((t >> 5) & 0x3F).astype(np.int32) - ((canvas >> 5) & 0x3F)),
            np.abs((t & 0x1F).astype(np.int32) - (canvas & 0x1F))])
        stale = e >= 4
        # Texels an opaque static layer above the bank covers are never pushed
        # (--bg-occlude), so the canvas under them is stale ON PURPOSE and the
        # hook paints that layer over it whatever it holds. Reported apart from
        # the visible ones, so a real ghost cannot hide inside the cull.
        occl = gif.get("occl")
        hidden = None if occl is None else (stale & occl)
        stale = stale & ~occl if occl is not None else stale
        stale_tot += int(stale.sum())
        stale_hid += int(hidden.sum()) if hidden is not None else 0
        mask_png(stale, os.path.join(dump_dir, f"stale_{j:02d}.png"))

    print("  dumped {} frames + sprites to {}/".format(n, dump_dir))
    print("  stale map totals {} px/frame >4 codes off (0 = the panel's canvas "
          "matches the GIF exactly)".format(stale_tot // max(1, n)))
    if stale_hid:
        print("           ({} px/frame of that sit under an opaque static the "
              "bank does not push -- expected, see --bg-occlude)".format(
                  stale_hid // max(1, n)))


def resize_full(img):
    """Fit a full-screen art layer to the panel.

    LANCZOS, not PIL's default NEAREST: a non-integer fit with NEAREST picks
    one source pixel per panel pixel, so neighbouring columns alternate between
    two source rows — thin strokes land a pixel high or low at random, which on
    an animated layer reads as the artwork jittering by a pixel."""
    if img.size != (SCREEN_W, SCREEN_H):
        return img.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("theme_dir")
    ap.add_argument("--bin", default="assets/clock_assets.bin")
    ap.add_argument("--header", default="assets/clock_assets.h")
    ap.add_argument("--orbtheme", default=None, metavar="PATH",
                    help="also emit a self-describing .orbtheme container to PATH, for "
                         "the SD card and the device's /themes page. The container carries "
                         "the same geometry this bake prints into --header plus the identical "
                         "pixel bytes, which is the only way a theme can reach a device that "
                         "was not built around it. Empty by default: a normal bake does not "
                         "grow a third artefact it did not ask for.")
    ap.add_argument("--theme-name", default=None,
                    help="the theme's one name, baked into the container and hashed to "
                         "form the card filename (default: the theme's name from "
                         "theme.json, else the theme directory's name). Up to 55 bytes. "
                         "The device files the card copy under sha256(name)[:12].orbtheme, "
                         "so this string — not the file's name on disk — is its identity.")
    ap.add_argument("--theme-title", default=None,
                    help="deprecated alias for --theme-name (FORMAT 1's separate title is "
                         "gone; the one name is both identity and label).")
    ap.add_argument("--pad", type=int, default=2, help="ink-bbox grow px (AA fringe room)")
    ap.add_argument("--no-bg", action="store_true",
                    help="bake only the static plate (skip bgAnim frames): lean blob "
                         "for validating the hands/partial-render without the ~2.2 MB "
                         "animated-background bank, which must be flushed full-page.")
    ap.add_argument("--bg-gif", default=None,
                    help="use this GIF as the clock background (delta-rect bank) "
                         "instead of the theme plate + bgAnim frames")
    ap.add_argument("--bg-frames", default=None, metavar="ZIP|DIR|GLOB|PNG",
                    help="same delta-rect bank, fed from a PNG frame sequence: a "
                         ".zip of PNGs, a folder of PNGs, a PNG glob, or a single "
                         "PNG. Preferred over a GIF for new art - PNG is lossless "
                         "and re-bakeable, a GIF re-quantises every frame down to "
                         "256 palette entries before the bake even starts. "
                         "Mutually exclusive with --bg-gif.")
    ap.add_argument("--bg-glob", default=None, metavar="PAT",
                    help="with --bg-frames on a folder: which PNGs are frames "
                         "(e.g. 'frame_*.png' or 'clock_plate_*_v2.png'). Default: "
                         "every .png in the folder. Natural-sorted either way.")
    ap.add_argument("--bg-fps", type=float, default=0.0, metavar="N",
                    help="with --bg-frames: playback rate, since PNGs carry no "
                         "timing. 0 (default) takes the theme's bgAnim.fps, then "
                         "25 fps if the theme has no bgAnim at all.")
    ap.add_argument("--bg-max-rects", type=int, default=16, metavar="N",
                    help="per-frame delta-rect cap for the bg bank packer "
                         "(default 16; the shipped recipe). Benching a wide "
                         "animated band needs ~one rect per 16-px tile row: a "
                         "304+ row full-width band overruns 16 and the packer "
                         "collapses to whole-page rects, silently animating "
                         "every row. Raise to 32 for those points.")
    ap.add_argument("--bg-max-flush", type=float, default=0.62, metavar="F",
                    help="packer collapse threshold: a frame whose cover "
                         "exceeds F of the page flushes the whole page "
                         "instead (default 0.62; the shipped recipe). Bench a "
                         "clean full-width band of >290 rows needs ~1.01.")
    ap.add_argument("--frame-step", type=int, default=1, metavar="N",
                    help="take every Nth source frame before baking "
                         "(DECIMATION). The pack is one authored loop sampled at "
                         "the generator's fps; playing all of it at a lower "
                         "--bg-fps runs the loop slow (30 frames at 15 fps = a "
                         "2 s loop). Use N = source_frames / playback_fps so one "
                         "pass stays one authored second: 30-pack -> step 2 for "
                         "15 fps; a 60-pack -> step 4 for 15 fps or step 5 for "
                         "12 fps. Must divide the frame count.")
    ap.add_argument("--bg-occlude", choices=("auto", "off"), default="auto",
                    help="do not pack delta rects for background texels that a "
                         "live STATIC layer above the bank (the dial, a raised "
                         "dial, the glass) covers at alpha 255: the commit hook "
                         "paints that layer back over any row it rebuilds, so "
                         "those texels can never reach the panel, yet pushing "
                         "them still charges ~0.2 ms for every row they are "
                         "alone in. auto (default) culls them; off packs every "
                         "changed texel as before. A closed dial with one small "
                         "opening is the case this pays for. Decided from the "
                         "art, printed whatever it finds.")
    ap.add_argument("--diff-thresh", type=int, default=12,
                    help="min perceptual RGB565 step (0-248) for a pixel to count "
                         "as changed when packing delta rects (default 12)")
    ap.add_argument("--bg-denoise", type=int, default=2, metavar="EPS",
                    help="animated bg (GIF or PNG frames): snap pixels whose whole "
                         "cycle stays within +/-EPS RGB565 codes of one value to "
                         "that value in every frame (kills source dither, which the "
                         "packer otherwise drops and the panel then shows as a 1px "
                         "drift). 0 disables; default 2")
    ap.add_argument("--bg-fit", choices=("cover", "crop"), default="cover",
                    help="animated bg 500->466 style: 'cover' scales the whole logical "
                         "frame to the panel (no cropping, tiny resample blur); "
                         "'crop' centre-crops the panel window out of the asset at "
                         "1:1 (no resampler at all, loses the asset's outer border). "
                         "Both happen at BAKE time only - the device is strictly 1:1.")
    ap.add_argument("--force-sweep", action="store_true",
                    help="override the theme: secondSweep on")
    ap.add_argument("--no-sweep", action="store_true",
                    help="keep the theme's secondSweep even in animated-bg "
                         "mode (--bg-gif or --bg-frames). "
                         "In animated-bg mode sweep defaults ON: a ticking second "
                         "hand over an animated dial strobes at the animation's beat "
                         "instead of sweeping, and the theme JSON's "
                         "secondSweep:false silently re-baked the dial back to "
                         "a tick (that regression shipped as sweep=0).")
    ap.add_argument("--dump-dir", default=None, metavar="DIR",
                    help="write per-asset inspection PNGs: keyframe, each "
                         "frame's settled device canvas, its change mask, its "
                         "packed rect layout, a stale-pixel map (canvas vs the "
                         "true source frame), and every hand/shadow/overlay sprite "
                         "with its planar alpha composited over a checkerboard")
    ap.add_argument("--tick-rate", type=int, default=0,
                    help="override the theme's tickRate (beats per second)")
    ap.add_argument("--tourbillon", default=None, metavar="DIR|ZIP|GLOB",
                    help="animated second-hand (tourbillon) frame bank: a PNG "
                         "sequence (dir, zip of PNGs, or glob) of full-canvas "
                         "frames that share geometry and differ only in their "
                         "internal motion (oscillating balance). Baked into the "
                         "second-hand slot; the device rotates the shown frame "
                         "by the seconds angle exactly like any hand, and "
                         "re-marks only each frame's baked diff rect per swap. "
                         "Pivot/centre come from the style's hands.second.")
    ap.add_argument("--tourbillon-fps", type=int, default=15,
                    help="playback rate of the --tourbillon frame bank "
                         "(default 15, the kit manifest's tempo)")
    ap.add_argument("--no-pivot-snap", action="store_true",
                    help="keep each hand's authored pivotX/pivotY verbatim. By "
                         "default a rotating hand's pivot is snapped to the "
                         "centre of its own hub: an authored pivot a few px off "
                         "the hub makes the hand's centre orbit it as it sweeps "
                         "(the second hand here declared (16,75) for a hub at "
                         "(16,71), a 2.9px wobble) and leaves the ink misaligned "
                         "against the plate band it repaints.")
    ap.add_argument("--matte", choices=("auto", "off", "force"), default="auto",
                    help="fold a STATIC layer (the dial disc, the glass) into the "
                         "background at bake time so it costs no blend_px at "
                         "runtime. auto (default) folds only the layers PROVED "
                         "pixel-identical to do so -- alpha 0 wherever any lower "
                         "layer can ever paint -- and prints the verdict for every "
                         "static layer; off keeps every layer a runtime composite "
                         "(the classic behaviour); force folds the statics anyway "
                         "for a measured fps/fidelity tradeoff and is NOT a "
                         "shippable setting for art the prover refused.")

    ap.add_argument("--preview-dir", default=None)
    args = ap.parse_args()

    if args.bg_gif and args.bg_frames:
        sys.exit("--bg-gif and --bg-frames are alternatives for the same thing "
                 "(the animated background bank); pick one.")
    for a in (args.bg_gif, args.bg_frames):
        if a and os.path.splitext(os.path.expanduser(a))[1].lower() in (".bin", ".raw"):
            sys.exit(f"{a!r} is a baked raw-RGB565 blob, not a source. The baker "
                     f"emits that format (assets/clock_assets.bin); feeding it back "
                     f"in re-quantises already-lossy pixels, has no alpha and no "
                     f"size/fps metadata. Bake from the PNG frames "
                     f"(--bg-frames) or the GIF (--bg-gif) instead.")

    tdir = os.path.abspath(args.theme_dir)
    style_path = os.path.join(tdir, "clock_style.json")
    theme_path = os.path.join(tdir, "theme.json")
    if not os.path.exists(style_path):
        sys.exit(f"no clock_style.json under {tdir}")

    style = json.load(open(style_path))
    theme = json.load(open(theme_path)) if os.path.exists(theme_path) else {}
    theme_name = theme.get("name", os.path.basename(tdir))

    if args.preview_dir:
        os.makedirs(args.preview_dir, exist_ok=True)

    blob = Blob()

    # ---- bake-time static composite (matte): prove it, then fold it ----
    # Decided BEFORE the background is packed, because folding means the plate
    # (or every frame of the delta bank) has to carry the layer already. The
    # verdict is printed whatever it says -- on the orbital kit both statics are
    # refused, and a bake that silently kept them as runtime layers while
    # looking like it optimised them is the failure mode to avoid here.
    hands_style_pre = style.get("hands", {}) or {}
    pivots = resolve_hand_pivots(tdir, hands_style_pre, args)
    tourb_imgs = None
    if args.tourbillon:
        tourb_imgs = [Image.open(io.BytesIO(b)).convert("RGBA")
                      for _, b in _png_frame_items(args.tourbillon, None)]
        if tourb_imgs and tourb_imgs[0].size != (SCREEN_W, SCREEN_H):
            tourb_imgs = [resize_full(im) for im in tourb_imgs]
    live_stack = build_live_stack(tdir, style, pivots, args, tourb_imgs)
    matte_chosen, matte_lines = matte_plan(live_stack, args.matte)
    print("static-composite (--matte {}):".format(args.matte))
    for ln in matte_lines:
        print(ln)
    for L in matte_chosen:
        if not getattr(L, "forced", False):
            continue
        # Someone said --matte force on art the prover refused: price the lie
        # in pixels instead of hiding it. See matte_fidelity's two orderings.
        nd, mx, mn = matte_fidelity(_matte_base(tdir, args),
                                    live_stack[:live_stack.index(L)], L)
        print("  FORCED fold of {}: {} px of the panel CHANGE vs the runtime "
              "stack (max {} 565-steps, mean {:.1f}) -- this is a visible "
              "regression, not a shippable setting".format(L.name, nd, mx, mn))
    if int(style.get("plateFollow", 0)) and matte_chosen:
        print("  plateFollow is set: the plate parallaxes, so every fold is "
              "dropped (a baked-in layer would slide with it)")
        matte_chosen = []
    # A foldable candidate is static, so it has exactly one panel (banks are
    # movers, which are never folded).
    matte_stack = [L.panels[0] for L in matte_chosen]
    # KINDS index / overlay name -> folded layer, for the emission below.
    folded = {L.name: L for L in matte_chosen}
    folded_kinds = {L.name for L in matte_chosen}
    matte_mask = 0
    for nm in folded_kinds:
        if nm in KINDS:
            matte_mask |= 1 << KINDS.index(nm)
        elif nm.startswith("overlay"):
            matte_mask |= 1 << 5
    if matte_chosen:
        # Take the folded layers OUT of the runtime stack: the firmware builds
        # both the LVGL objects and the L4 hook's job list from this dict, so
        # dropping the slot here is what makes the fold the single source of
        # truth rather than a second copy that has to be kept in step.
        hs = style["hands"]
        hs["order"] = [k for k in (hs.get("order", [0, 1, 2, 3, 4]) or [])
                       if KINDS[int(k)] not in folded_kinds]
        for nm in folded_kinds:
            if nm in KINDS:
                hs[nm] = dict(hs.get(nm, {}), show=False)
            print("  folded {} into the background: {} px of blend traffic "
                  "removed from every commit".format(nm, ink_count(folded[nm].panels)))

    gif = None
    bg_src = args.bg_gif or args.bg_frames
    occl = None
    occl_lines = []
    # plateFollow drops the cull for the reason it drops every fold: what is
    # baked into a background that moves with the plate moves with it, and a
    # texel the bank no longer pushes would sit stale under a layer that only
    # LOOKS permanent from the plate's own point of view.
    if bg_src and args.bg_occlude != "off" and not int(style.get("plateFollow", 0) or 0):
        # Decided after the fold: a layer matte_plan folded into the bank is part
        # of the background now and can hide nothing, and a mover can never hide
        # anything either -- it lifts off the texel on its next step.
        import numpy as np
        folded = set(matte_chosen)
        occl = bg_occluder(live_stack, folded)
        for L in bg_occluders(live_stack, folded):
            n_op = sum(int((np.asarray(_opaque_only(im).split()[3]) >= 255).sum())
                       for im in L.panels)
            occl_lines.append("  occludes {:<16s} {:>6} px at a==255".format(
                L.name, n_op))
        npix = 0 if occl is None else int(occl.sum())
        nrow = 0 if occl is None else int((~occl.any(axis=1)).sum())
        print("bg occlusion (--bg-occlude {}): {:>6} px and {:>3} of {} rows are "
              "under an opaque static, so the bank will not push them".format(
                  args.bg_occlude, npix, nrow, SCREEN_H))
        for ln in occl_lines:
            print(ln)
    if bg_src:
        # ---- animated background bank: one keyframe + per-frame delta rects.
        # The theme's own plate/bgAnim art is skipped entirely — the bank IS the
        # clock's background now, whether it came from one GIF or one folder of
        # PNG frames. Both take the identical packer below. ----
        bg = style.get("bgAnim", {}) or {}
        # The occlusion cull was decided above (--bg-occlude, see bg_occluder):
        # the rects a permanently-opaque live static covers would cost delta
        # traffic the eye can never observe. A folded layer is already in the
        # matte list, a value-driven comp box is excluded by name, and
        # plateFollow drops the cull for the reason it drops every fold: what is
        # baked into a moving background moves with it.
        gif = bake_bg_bank(blob, bg_src, args.diff_thresh,
                           denoise=args.bg_denoise, fit=args.bg_fit,
                           fps=(args.bg_fps or (0.0 if args.bg_gif
                                                else float(bg.get("fps", 0) or 0))),
                           frame_glob=args.bg_glob, frame_step=args.frame_step,
                           matte=matte_stack, max_rects=args.bg_max_rects,
                           max_flush=args.bg_max_flush, occl=occl)
        gif["src"] = bg_src
        plate_off = gif["key_off"]   # the keyframe doubles as the "static plate"
        baked_frames, anim_frames = [], 0
    else:
        # ---- plate: full-screen RGB565 colour plane, at the front of the blob ----
        plate_img = load_rgba(tdir, "clock_plate.png")
        if plate_img is None:
            plate_img = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 255))
        plate_img = resize_full(plate_img)
        # A folded static layer rides on the plate itself, blended in the
        # device's own RGB565 -- so the plate pixel IS the pixel the runtime
        # composite used to produce, and nothing above it changes.
        for m in matte_stack:
            plate_img = matte_fold(plate_img, m)
        plate_off = blob.add(rgb565_le(plate_img))
        plate_key = bytes(blob.buf[plate_off:plate_off + SCREEN_W * SCREEN_H * 2])

        # ---- bgAnim frames: bake each, dedup byte-identical frames against plate ----
        bg = style.get("bgAnim", {}) or {}
        frames = int(bg.get("frames", 0))
        if args.no_bg:
            frames = 0
        baked_frames = []
        seen = {}
        for fi in range(1, frames + 1):
            fim = load_rgba(tdir, f"clock_plate_{fi:02d}.png")
            if fim is None:
                baked_frames.append(plate_off)   # missing frame -> hold the still plate
                continue
            fim = resize_full(fim)
            for m in matte_stack:
                fim = matte_fold(fim, m)
            payload = bytes(rgb565_le(fim))
            if payload == plate_key or payload in seen:
                baked_frames.append(seen.get(payload, plate_off))
                continue
            off = blob.add(payload)
            seen[payload] = off
            baked_frames.append(off)
        distinct = [o for o in baked_frames if o != plate_off]
        anim_frames = len(baked_frames) if distinct else 0
        if not distinct:
            baked_frames = []

    # ---- overlay: full canvas but usually a tiny hub; trim to ink ----
    ov_img = load_rgba(tdir, "clock_overlay.png")
    if "overlay(glass)" in folded_kinds:
        ov_img = None            # folded into the background: no sprite to ship
    ov_crop, ov_offx, ov_offy = crop_layer(ov_img, args.pad)
    if ov_crop is None:
        overlay = blank_sprite(0, 0, 0)
    else:
        overlay = emit_sprite(blob, ov_crop, ov_offx, ov_offy,
                              ov_offx, ov_offy, ov_offx, ov_offy, 0, True)

    # ---- hands + shadows ----
    hands_style = style.get("hands", {}) or {}
    shadow_style = hands_style.get("shadow", {}) or {}
    shadow_on = bool(shadow_style.get("on", False))
    shadow_dx = int(shadow_style.get("dx", 0))
    shadow_dy = int(shadow_style.get("dy", 0))

    hand_recs, shadow_recs = [], []
    for kind in KINDS:
        hs = hands_style.get(kind, {}) or {}
        show = bool(hs.get("show", False))
        px, py, cx, cy, pivot_note = pivots[kind]
        blend = int(hs.get("blend", 0))

        if kind in folded_kinds:
            # Folded into the plate/bank at bake time. Emit nothing at all: a
            # zero-size record is what keeps BOTH the LVGL object and the L4
            # hook's job list from resurrecting a second copy of the layer --
            # a folded layer painted again would double-darken every pixel.
            blank = blank_sprite(cx, cy, 0)
            hand_recs.append(dict(blank))
            shadow_recs.append(dict(blank))
            print(f"  {kind:8s} FOLDED (baked into the background)")
            continue

        img = load_rgba(tdir, f"clock_hand_{kind}.png")
        # The corrected pivot comes from resolve_hand_pivots(), shared with the
        # matte reachability proof so the two can never disagree about where
        # this hand spins (see its docstring).
        if img is None:
            hand_recs.append(blank_sprite(cx, cy, 0))
        else:
            crop, ox, oy = crop_layer(img, args.pad)
            hand_recs.append(emit_sprite(blob, crop, ox, oy, px, py, cx, cy, blend, show))

        simg = load_rgba(tdir, f"clock_shadow_{kind}.png")
        if simg is None:
            shadow_recs.append(blank_sprite(cx, cy, 0))
        else:
            crop, ox, oy = crop_layer(simg, args.pad)
            # shadow shares the hand's angle/pivot; only its CENTRE moves, in
            # screen space, which is what keeps the light direction fixed.
            sdx, sdy = shadow_offsets(hands_style, kind)
            shadow_recs.append(emit_sprite(blob, crop, ox, oy, px, py,
                                           cx + sdx, cy + sdy, 0, shadow_on and show))

        rec = hand_recs[-1]
        print(f"  {kind:8s} show={int(show)} pivot=({px},{py}) center=({cx},{cy}) blend={blend}"
              + ("" if img is None else
                 f" canvas={img.size} ink={rec['w']}x{rec['h']}@({rec['offx']},{rec['offy']})")
              + (f" shadowoff=({sdx},{sdy})" if simg is not None else "")
              + pivot_note)

    # ---- tourbillon: an ANIMATED second hand (optional frame bank) ----
    # Overrides the second-hand record with frame 0 of the bank: the hand
    # record stays the geometry carrier (bbox/pivot/centre for the rotator and
    # for swept-box maths); the pixels come from clk_theme.tourbillon[f].
    tourb_recs = []
    if args.tourbillon:
        tourb_recs = bake_tourbillon(blob, style, args)
        if tourb_recs:
            hand_recs[2] = dict(tourb_recs[0], show=1)

    _finish(style, theme_name, bg, shadow_on, shadow_dx, shadow_dy,
            hand_recs, shadow_recs, overlay, baked_frames, anim_frames,
            plate_off, blob, args, gif, tourb_recs, matte_mask)


# ---------------------------------------------------------------------------
# Complications: the kit's complications/ tiles -> theme_schema records.
#
# Windows are packed RAW here and never composited against the dial: the pixel the
# panel ends up showing is the dial's own window art drawn OVER the tile at runtime,
# which is what gives an aperture its silvered gradient and bezel without the bake
# having to know what the dial looks like. So each tile carries a planar alpha
# (w*h*3) and the record does NOT set CLK_FLAG_UNDER_DIAL — a reader told 2 bytes
# per tile would stride into the next frame. See CLK_WINDOW_BYTES in orb_theme.h.
# ---------------------------------------------------------------------------

# The order a window's value indexes tiles in. Index == the frame frame_index()
# picks for that value, so a wall-clock value and the art baked for it cannot drift
# out of step; the kit ships these names, we do not trust its own listing order.
_WEEKDAY_TILES = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"]   # value 0..6, Mon first
# The moon styles the composer draws as a whole circle rather than a fan
# (watch-engine.js ROUND_MOON). They must agree: the style decides the anchor
# below, and a style this list forgets gets fanned and lands a radius high.
_ROUND_MOON_STYLES = ("sphere", "graphic", "neon")
_MONTH_TILES   = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"]           # value 0..11


def _comp_dir_for(tdir, cid):
    """The kit's complications/<cid>_* folder, or None if this comp ships no tiles.
    Matched by the `<cid>_` prefix so the bake need not agree with prep on the
    suffix word (`c2_day` vs `c2_weekday`)."""
    root = os.path.join(tdir, "complications")
    if not os.path.isdir(root):
        return None
    for name in sorted(os.listdir(root)):
        if name.startswith(cid + "_") and os.path.isdir(os.path.join(root, name)):
            return os.path.join(root, name)
    return None


def _pack_window_tile(blob, img):
    """Append one raw RGBA tile as RGB565 then its alpha plane; return the blob
    offset of the colour plane. The reader strides frames by w*h*3, the value the
    caller writes into the record's frame_bytes."""
    off = blob.add(rgb565_le(img))
    blob.add(alpha_bytes(img))
    return off


def _tile_path(cdir, name):
    for cand in (name, name + ".png", name + ".PNG"):
        p = os.path.join(cdir, cand)
        if os.path.exists(p):
            return p
    return None


def bake_complications(tdir, blob, verbose=True):
    """Read <tdir>/manifest.json's layout.comps + <tdir>/complications/ tiles and
    return (comp_records, aux_records, bank_records) to hand to pack_geom.

    Returns three empty lists whenever the kit has no manifest or no
    complications/ folder, so the blob stays four-count and a classic kit boots
    exactly as it did before the calendar existed — this runs on every bake.
    """
    import math
    import theme_schema as ts
    mpath = os.path.join(tdir, "manifest.json")
    if not os.path.exists(mpath):
        return [], [], []
    comps = (json.load(open(mpath)).get("layout") or {}).get("comps") or []
    if not comps:
        return [], [], []
    # The aux table is NOT this function's: a sub-dial pointer is a sprite with
    # a pivot, not a box of tiles, and it comes from the style prep wrote. This
    # returns an empty middle list so the three-tuple the caller unpacks stays
    # the same shape as the comp/bank tables it also hands pack_geom.

    dial_c = (SCREEN_W // 2, SCREEN_H // 2)
    centre = {}                                   # comp id -> (px, py) on the panel
    comp_recs, aux_recs, bank_recs = [], [], []

    for comp in comps:
        cid  = comp.get("id") or ""
        ctype = (comp.get("type") or "").strip().lower()

        # The angle is whole degrees with 0 at twelve, clockwise; dist is the radius.
        # A comp hosted by another (the moon on its subdial) measures from THAT comp's
        # resolved centre, which is why centres are stored as they are computed — and
        # why this runs BEFORE the skip below: a hosted comp cannot see its host's
        # centre if the subdial/opening never got one (the moon landed on the dial
        # centre with the real perpetual-calendar kit's art because of exactly that).
        host = comp.get("host", "dial")
        bx, by = centre.get(host, dial_c)
        ang  = float(comp.get("angle", 0))
        dist = float(comp.get("dist", 0))
        px = bx + dist * math.sin(math.radians(ang))
        py = by - dist * math.cos(math.radians(ang))
        centre[cid] = (px, py)
        if ctype in ("opening", "subdial"):       # the balance aperture / the host
            continue                              # plate are not window comps

        cdir = _comp_dir_for(tdir, cid)
        if cdir is None:
            if verbose:
                print("  comp {:5s} {:8s}: no complications/{}_* folder, skipped".format(
                    cid, ctype, cid))
            continue

        word    = (comp.get("shows") or comp.get("scale") or comp.get("show") or ctype)
        src_idx = ts.source_of(word)               # unknown kit word is a hard error here
        src     = ts.SOURCES[src_idx]
        if src == "weekday":
            names = _WEEKDAY_TILES
        elif src == "month":
            names = _MONTH_TILES
        else:                                       # moon/other: the folder's own order
            names = [os.path.splitext(n)[0] for n in sorted(os.listdir(cdir))
                     if n.lower().endswith(".png")]

        imgs, size = [], None
        for n in names:
            p = _tile_path(cdir, n)
            if p is None:
                raise SystemExit("comp %s: source %r needs tile %r, absent from %s"
                                 % (cid, word, n, cdir))
            im = Image.open(p).convert("RGBA")
            if size is None:
                size = im.size
            elif im.size != size:
                raise SystemExit("comp %s: tile %r is %s but the box is %s — a window "
                                 "strides by one fixed box" % (cid, n, im.size, size))
            imgs.append(im)

        w, h = size
        off0 = _pack_window_tile(blob, imgs[0])
        for im in imgs[1:]:
            _pack_window_tile(blob, im)

        rec = ts.blank("comp")
        # For a WINDOW, `first` is not a bank index but the z bucket: the KINDS
        # slot whose live layer this tile must stay UNDER (0 = below every live
        # layer, what every bake emitted before a subdial ever hosted a window).
        # The kit's own drawOrder decided it and prep wrote it into the manifest
        # as `underLayer`, so the moon under the c4 plate gets 4 and the weekday
        # window on the dial face gets 3. The paint pass honours it in
        # comp_add_jobs; orb_theme.c's bank-range check covers only the kinds
        # that address the bank, so a bucket never reads as a sprite index.
        bucket = int(comp.get("underLayer", 0) or 0)
        if not 0 <= bucket <= 4:
            raise SystemExit("comp %s: underLayer %d is not a KINDS slot (0..4)"
                             % (cid, bucket))
        # ---- the anchor: the producer's own holeBox, not a guess -------------
        # A fan moon's box is [x-a, y-a, x+a, y] (watch-engine.js holeBox for
        # kind 'moon', and the tile exporter at its line 2058 that sized this
        # very PNG): the resolved point is the middle of the tile's BOTTOM edge
        # and the arc rises above it, because the fan's flat baseline is what
        # sits on a sub-dial's arch line. Everything else -- a frame window, or
        # one of the ROUND_MOON styles the composer paints as a whole circle --
        # is centred on the point. Centring the fan (what this did) drops it
        # h/2 low, and the perpetual calendar's moon then showed its top sliver
        # under the arch while the guilloche filled the window: measured 318 vs
        # the exporter's 304 for a=29. a is the authored fan radius and the tile
        # is exactly 2a wide; h is a+2 (the exporter pads the foot), so h must
        # not be used to place it or the answer drifts by that padding.
        fan = (ctype == "moon"
               and float(comp.get("rot", 0) or 0) == 0.0
               and (comp.get("style") or "").strip().lower()
                    not in _ROUND_MOON_STYLES)
        rec.update(tag=(int(cid[1:]) if cid[1:].isdigit() else 0),
                   kind=ts.kind_of("window"), src=src_idx,
                   x=int(round(px - w / 2)),
                   y=int(round(py - (w / 2 if fan else h / 2))),
                   w=w, h=h, count=len(imgs), first=bucket, off=off0,
                   frame_bytes=ts.window_bytes({"w": w, "h": h, "flags": 0}),
                   value_min=1 if src == "date" else 0, scale=16,
                   blend=0, show=1, flags=0, reserve=0)
        comp_recs.append(rec)
        if verbose:
            print("  comp {:5s} {:8s} src={:10s} {}x{} @({},{}) {} tiles @{:+d}deg r{:.0f} "
                  "{} under {}".format(cid, ctype, src, w, h, rec["x"], rec["y"],
                                       len(imgs), int(ang), dist,
                                       "baseline" if fan else "centred",
                                       "the plate" if bucket == 4 else
                                       "the dial" if bucket == 3 else "every live layer"))

    if len(comp_recs) > ts.CLK_COMP_MAX:
        raise SystemExit("%d complications exceed the dial's cap of %d"
                         % (len(comp_recs), ts.CLK_COMP_MAX))
    return comp_recs, aux_recs, bank_recs


def bake_aux(tdir, style, blob, args, verbose=True):
    """Pack the sub-dial pointers prep wrote into clk_aux_t rows.

    prep_watch_kit placed every pointer on a full screen canvas with its pivot
    already on its centre and left the (png, how-to-drive-it) pair in
    clock_style.json's `aux` list; this turns each into one sprite in the art
    bank plus the nine driving words the dial reads. The crop-to-ink that makes
    a hand cheap makes a pointer cheap too, and it is the same emit_sprite call,
    so a pointer costs exactly what a hand costs and no more.

    A shadowed pointer is TWO rows: the hand, then its blur/offset twin flagged
    FLAG_AUX_SHADOW. The twin carries no input of its own — the driver rotates
    it by the row before it's angle, about its own centre offset by the light
    vector — which is how the five baked hands already do it, and why one record
    shape serves both.

    Returns [] for a kit with no pointers, which is the overwhelmingly common
    case and what keeps every bake that has never seen an aux hand emitting the
    blob it emitted yesterday.
    """
    import theme_schema as ts
    rows = []
    for i, a in enumerate(style.get("aux") or []):
        src_idx = ts.SOURCES.index(a["src"]) if a["src"] in ts.SOURCES else \
            ts.source_of(a["src"])
        img = load_rgba(tdir, a["file"])
        if img is None:
            raise SystemExit("aux %d: style names %s, absent from %s — re-run "
                             "prep_watch_kit.py (the theme dir and its style must "
                             "come from the same prep of the same kit)"
                             % (i, a["file"], tdir))
        px, py = int(a["pivotX"]), int(a["pivotY"])
        cx, cy = int(a["centerX"]), int(a["centerY"])
        show = bool(a.get("show", True))
        blend = int(a.get("blend", 0))
        flags = ts.FLAG_JUMP if a.get("jump") else 0
        crop, ox, oy = crop_layer(img, args.pad)
        rec = emit_sprite(blob, crop, ox, oy, px, py, cx, cy, blend, show)
        rec = _aux_record(ts, rec, a, src_idx, flags)
        rows.append(rec)
        printed = "  aux {:<5d} {:8s} {}x{} @({},{}) pivot=({},{}) centre=({},{}) " \
                  "value {}..{} zero={:+d} span={:+d}".format(
                      i, a["src"], rec["w"], rec["h"], rec["offx"], rec["offy"],
                      rec["pivotx"], rec["pivoty"], rec["centerx"], rec["centery"],
                      rec["value_min"], rec["value_max"], rec["zero"], rec["span"])
        sf = a.get("shadowFile")
        if sf:
            simg = load_rgba(tdir, sf)
            if simg is None:
                raise SystemExit("aux %d: style names %s, absent from %s" % (i, sf, tdir))
            sdx, sdy = int(a.get("shadowDx", 0)), int(a.get("shadowDy", 0))
            scrop, sox, soy = crop_layer(simg, args.pad)
            srec = emit_sprite(blob, scrop, sox, soy, px, py,
                               cx + sdx, cy + sdy, 0, show)
            srec = _aux_record(ts, srec, a, src_idx, flags | ts.FLAG_AUX_SHADOW)
            rows.append(srec)
            printed += "  + shadow twin @({:+d},{:+d})".format(sdx, sdy)
        if verbose:
            print(printed)
    if len(rows) > ts.CLK_AUX_MAX:
        raise SystemExit("%d aux records exceed the dial's cap of %d"
                         % (len(rows), ts.CLK_AUX_MAX))
    return rows


def _aux_record(ts, spr, a, src_idx, flags):
    """The 15 sprite words emit_sprite built, widened to a full 24-word aux row."""
    vmin, vmax = int(a.get("valueMin", 0)), int(a.get("valueMax", 0))
    if vmax <= 0:
        raise SystemExit("aux %s: value_max %d cannot divide a sweep" % (a["src"], vmax))
    rec = ts.blank("aux")
    rec.update(spr)
    rec.update(tag=int(a.get("tag", 0)), src=src_idx, kind=int(a.get("kind", 0)),
               host=int(a.get("host", 0)),
               span=int(round(float(a.get("span", 3600)))),
               zero=int(round(float(a.get("zero", 0)))),
               value_min=vmin, value_max=vmax, flags=flags)
    return rec


def rebucket_comps(comps, matte_mask):
    """Drop any window whose plate the bake folded into the background.

    A WINDOW's `first` names the LIVE layer the tile must stay under. Once that
    layer is matte-folded there is nothing left to composite over the tile, and
    the device would be told to keep a tile under a layer it never paints. Zero
    is the honest answer — below every live layer — and the return value is the
    list of (source name, folded layer) pairs it changed, which is what the
    caller prints beside the fold's own verdict: the fold is what changed it.
    """
    import theme_schema as ts
    changed = []
    for r in comps:
        b = int(r["first"])
        if b and (int(matte_mask) & (1 << b)):
            changed.append((ts.SOURCES[r["src"]], KINDS[b]))
            r["first"] = 0
    return changed


def _finish(style, theme_name, bg, shadow_on, shadow_dx, shadow_dy,
            hand_recs, shadow_recs, overlay, baked_frames, anim_frames,
            plate_off, blob, args, gif=None, tourb_recs=None, matte_mask=0):
    hands_style = style.get("hands", {}) or {}
    order = [int(k) for k in (hands_style.get("order", [0, 1, 2, 3, 4]) or [0, 1, 2, 3, 4])
             if 0 <= int(k) < 5]
    order_n = len(order)

    bgcolor = int(style.get("bg", 0))
    plate_follow = int(style.get("plateFollow", 0))
    second_sweep = bool(style.get("secondSweep", False))
    second_railway = bool(style.get("secondRailway", False))
    text_over_hands = bool(style.get("textOverHands", False))
    tick_rate = int(style.get("tickRate", 1))
    if args.force_sweep:
        second_sweep = True     # device-side sweeping second hand (item 3)
        second_railway = False
    elif gif is not None and not args.no_sweep:
        # A GIF background is already animating every tick; a TICKING second
        # hand over it strobes (the hand jumps 6 deg once per beat, in sync
        # with nothing) whereas the intended look is a continuous sweep. The
        # theme JSON carries secondSweep:false, so a plain re-bake reverted the
        # dial to a tick -- hence GIF-bg mode defaults to sweep, --no-sweep opts out.
        second_sweep = True
        second_railway = False
    if args.tick_rate:
        tick_rate = max(1, min(255, args.tick_rate))

    os.makedirs(os.path.dirname(args.bin) or ".", exist_ok=True)
    with open(args.bin, "wb") as f:
        f.write(blob.buf)

    def fmt_spr(r, label):
        return ("  {{ {:>4}, {:>4}, {:>5}, {:>5}, {:>5}, {:>5}, {:>5}, {:>5}, {:>2}, {:>2}, {:>9}, {:>5}, {:>5}, {:>5}, {:>5} }}, /* {} */".format(
            r["w"], r["h"], r["offx"], r["offy"], r["pivotx"], r["pivoty"],
            r["centerx"], r["centery"], r["blend"], r["show"], r["color_off"],
            r["diffx"], r["diffy"], r["diffw"], r["diffh"], label))


    hdr = []
    hdr.append("/* AUTO-GENERATED by tools/bake_clock.py -- DO NOT EDIT.\n"
               " * Re-bake:  python3 tools/bake_clock.py <theme_dir>\n"
               " * Source theme: {}{}\n */\n#pragma once\n\n#include <stdint.h>\n\n".format(
                   theme_name,
                   "\n * Source background: {}  (delta-rect bank; re-bake from the\n"
                   " * PNG/GIF source, never from a baked .bin)".format(
                       gif.get("src", "?")) if gif else ""))
    hdr.append("#define CLK_SCR_W {}\n#define CLK_SCR_H {}\n\n".format(SCREEN_W, SCREEN_H))
    hdr.append("typedef struct {\n"
               "  uint16_t w, h;            /* cropped ink size */\n"
               "  int16_t  offx, offy;      /* crop origin in the original canvas */\n"
               "  int16_t  pivotx, pivoty;  /* rotation pivot in CROPPED coords */\n"
               "  int16_t  centerx, centery;/* screen point the pivot maps to */\n"
               "  uint8_t  blend;           /* 0 normal 1 multiply 2 screen */\n"
               "  uint8_t  show;            /* draw this layer */\n"
               "  uint32_t color_off;       /* blob offset of the colour plane (w*h*2);\n"
               "                             *   the alpha plane follows at +w*h*2, stride w */\n"
               "  int16_t  diffx, diffy;    /* tourbillon frames only: rect that differs\n"
               "                             *   from the PREVIOUS frame, CROPPED coords;\n"
               "                             *   0,0 = identical to its predecessor */\n"
               "  uint16_t diffw, diffh;\n"
               "} clk_sprite_t;\n\n")
    hdr.append("#define CLK_TOURB_MAX 16\n\n")
    hdr.append("typedef struct {\n"
               "  uint32_t bg;\n"
               "  uint8_t  plate_follow;    /* 0 none 1 hour 2 minute 3 second */\n"
               "  uint8_t  shadow_on;\n"
               "  int8_t   shadow_dx, shadow_dy;\n"
               "  uint8_t  second_sweep, second_railway, text_over_hands, tick_rate;\n"
               "  uint16_t bg_frames;       /* distinct extra plate frames (0 = still) */\n"
               "  int16_t  bg_fps; uint8_t bg_loop; uint16_t bg_every;\n"
               "  uint32_t plate_off;       /* full-screen RGB565 colour plane offset */\n"
               "  uint8_t  order[5]; uint8_t order_n;\n"
               "  uint8_t  matte_mask;        /* bit i = hand slot i folded into the\n"
               "                               *   background at bake time, never\n"
               "                               *   composited at runtime; bit 5 = the\n"
               "                               *   glass overlay was folded too */\n"
               "  clk_sprite_t hand[5];\n"
               "  clk_sprite_t shadow[5];\n"
               "  clk_sprite_t overlay;\n"
               "  uint8_t  bg_mode;         /* 0 = plate + bgAnim frames, 1 = animated delta bank (GIF or PNG frames) */\n"
               "  uint32_t bg_key_off;      /* mode 1: full RGB565 keyframe (frame 0) */\n"
               "  uint16_t gif_frames;      /* mode 1: frames in the loop (incl. keyframe) */\n"
               "  uint16_t tourbillon_n;    /* second-hand frame bank size (0 = plain hand) */\n"
               "  uint16_t tourbillon_fps;  /* bank playback tempo */\n"
               "  clk_sprite_t tourbillon[CLK_TOURB_MAX];  /* shown frame's pixels replace hand[2]'s */\n"
               "} clk_theme_t;\n\n")


    hdr.append("typedef struct {\n"
               "  uint16_t x, y, w, h;  /* screen rect */\n"
               "  uint32_t off;         /* blob offset of the RGB565 payload */\n"
               "} clk_bg_rect_t;\n\n")
    hdr.append("#define CLK_GIF_FRAMES {}\n\n".format(gif["frames"] if gif else 0))
    if gif:
        hdr.append("static const clk_bg_rect_t clk_bg_rects[{}] = {{\n".format(
            max(1, len(gif["rects"]))))
        for x, y, w, h, off in (gif["rects"] or [(0, 0, 0, 0, 0)]):
            hdr.append("  {{ {:>4}, {:>4}, {:>4}, {:>4}, {:>9} }},\n".format(x, y, w, h, off))
        hdr.append("};\n\n")
        hdr.append("/* transition to frame i = rects[first[i] .. first[i+1]) applied over the\n"
                   " * canvas holding frame i-1 (cyclically: frame 0 follows the last frame) */\n"
                   "static const uint16_t clk_bg_rect_first[{}] = {{ {} }};\n\n".format(
                       gif["frames"] + 1, ", ".join(str(v) for v in gif["first"])))
        hdr.append("/* GIF's own per-frame delays (ms), for cadence-faithful playback. */\n"
                   "static const uint16_t clk_gif_delays[{}] = {{ {} }};\n\n".format(
                       gif["frames"], ", ".join(str(d) for d in gif["delays"])))
    else:
        # Classic theme: tables unused (bg_mode 0, gif_frames 0) but emitted so
        # clock_view.c's gif path always compiles, GIF bake or no GIF bake.
        hdr.append("static const clk_bg_rect_t clk_bg_rects[1] = { { 0, 0, 0, 0, 0 } };\n\n")
        hdr.append("static const uint16_t clk_bg_rect_first[1] = { 0 };\n\n")
        hdr.append("static const uint16_t clk_gif_delays[1] = { 40 };\n\n")

    frames_literal = ", ".join(str(o) for o in baked_frames) if baked_frames else "0"
    hdr.append("static const uint32_t clk_bg_frames[{}] = {{ {} }};\n\n".format(
        max(1, len(baked_frames)), frames_literal))
    hdr.append("/* total blob size; the `assets` partition must hold at least this many bytes */\n"
               "#define CLK_BLOB_BYTES {}u\n\n".format(len(blob.buf)))

    hdr.append("static const clk_theme_t clk_theme = {\n")
    hdr.append("  /* bg */ 0x{:06X},\n".format(bgcolor & 0xFFFFFF))
    hdr.append("  /* plate_follow */ {},\n".format(plate_follow))
    hdr.append("  /* shadow_on */ {}, /* shadow dx,dy */ {}, {},\n".format(
        int(shadow_on), shadow_dx, shadow_dy))
    hdr.append("  /* sweep,railway,textOverHands,tickRate */ {}, {}, {}, {},\n".format(
        int(second_sweep), int(second_railway), int(text_over_hands), tick_rate))
    hdr.append("  /* bg_frames */ {}, /* fps,loop,every */ {}, {}, {},\n".format(
        anim_frames, int(bg.get("fps", 6)), int(bool(bg.get("loop", False))),
        int(bg.get("everySec", 3600))))
    hdr.append("  /* plate_off */ {},\n".format(plate_off))
    hdr.append("  /* order */ {{ {} }}, /* order_n */ {},\n".format(
        ", ".join(str(k) for k in order), order_n))
    hdr.append("  /* matte_mask */ {},  /* {} static layer(s) baked into the background */\n".format(
        matte_mask, "baked" if matte_mask else "none"))
    hdr.append("  /* hand */ {\n" + "\n".join(fmt_spr(hand_recs[i], KINDS[i]) for i in range(5)) + "\n  },\n")
    hdr.append("  /* shadow */ {\n" + "\n".join(fmt_spr(shadow_recs[i], KINDS[i]) for i in range(5)) + "\n  },\n")
    hdr.append("  /* overlay */" + fmt_spr(overlay, "overlay")[1:] + "\n")
    hdr.append("  /* bg_mode,bg_key_off,gif_frames */ {}, {}, {},\n".format(
        1 if gif else 0, (gif["key_off"] if gif else 0), (gif["frames"] if gif else 0)))
    tb = tourb_recs or []
    hdr.append("  /* tourbillon_n,tourbillon_fps */ {}, {},\n".format(
        len(tb), int(getattr(args, "tourbillon_fps", 15) or 15)))
    blank = blank_sprite(0, 0, 0)
    hdr.append("  /* tourbillon */ {\n"
               + "\n".join(fmt_spr(tb[i] if i < len(tb) else blank, f"tourb{i:02d}")
                           for i in range(16))
               + "\n  }\n")
    hdr.append("};\n\n")

    hdr.append("/* Symbols from target_add_binary_data(... clock_assets.bin BINARY). */\n"
               "extern const uint8_t clock_assets_bin_start[] asm(\"_binary_clock_assets_bin_start\");\n"
               "extern const uint8_t clock_assets_bin_end[]   asm(\"_binary_clock_assets_bin_end\");\n")

    os.makedirs(os.path.dirname(args.header) or ".", exist_ok=True)
    with open(args.header, "w") as f:
        f.write("".join(hdr))

    # ---- the .orbtheme container, for the SD card and the /themes page ---------
    # Why this exists next to the pair rather than instead of it: the .h is compiled
    # into the app, so a theme that only exists as a .bin + .h pair can never be
    # loaded by a device that was not built around it. The container carries the SAME
    # numbers this function just printed as C, so one bake feeds both paths and they
    # cannot drift -- there is no second geometry here, only a second encoding.
    if getattr(args, "orbtheme", None):
        import orbtheme as _ot
        scalars = {
            "bg": bgcolor & 0xFFFFFF,
            "plate_follow": int(plate_follow),
            "shadow_on": int(bool(shadow_on)),
            "shadow_dx": int(shadow_dx), "shadow_dy": int(shadow_dy),
            "second_sweep": int(bool(second_sweep)),
            "second_railway": int(bool(second_railway)),
            "text_over_hands": int(bool(text_over_hands)),
            "tick_rate": int(tick_rate),
            "bg_frames": int(anim_frames),
            "bg_fps": int(bg.get("fps", 6)),
            "bg_loop": int(bool(bg.get("loop", False))),
            "bg_every": int(bg.get("everySec", 3600)),
            "plate_off": int(plate_off),
            "order0": int(order[0]) if order_n > 0 else 0,
            "order1": int(order[1]) if order_n > 1 else 0,
            "order2": int(order[2]) if order_n > 2 else 0,
            "order3": int(order[3]) if order_n > 3 else 0,
            "order4": int(order[4]) if order_n > 4 else 0,
            "order_n": int(order_n),
            "matte_mask": int(matte_mask),
            "bg_mode": 1 if gif else 0,
            "bg_key_off": int(gif["key_off"]) if gif else 0,
            "gif_frames": int(gif["frames"]) if gif else 0,
            "tourbillon_n": int(len(tourb_recs or [])),
            "tourbillon_fps": int(getattr(args, "tourbillon_fps", 15) or 15),
        }
        # Mirrors the generated header's tables EXACTLY, including the placeholder
        # rows it emits for a classic (non-GIF) theme so clock_view.c's GIF path
        # always compiles: one zero rect, one index, one 40 ms delay. Feeding the
        # device empty tables instead would make the two artefacts of this one bake
        # describe different themes.
        if gif:
            rects  = [int(v) for r in (gif["rects"] or [(0, 0, 0, 0, 0)]) for v in r]
            first  = [int(v) for v in gif["first"]]
            delays = [int(v) for v in gif["delays"]]
        else:
            rects, first, delays = [0, 0, 0, 0, 0], [0], [40]
        frames = [int(o) for o in baked_frames] or [0]
        # FORMAT 2 has ONE name field, and it is both the identity and the label the
        # /themes page shows. --theme-name wins; --theme-title remains as a deprecated
        # alias for the same value (FORMAT 1's separate title is gone). Truncated to
        # the container's NAME_MAX-1 usable bytes so pack_header cannot overflow it.
        name = (getattr(args, "theme_name", None)
                or getattr(args, "theme_title", None)
                or style.get("name") or theme_name)[: _ot.NAME_MAX - 1]
        if not name.strip():
            raise SystemExit("--theme-name is empty: a nameless theme would hash every "
                             "such theme to one card id and collide, so it is refused.")
        # Complications are packed into the CONTAINER's art only, never the
        # compiled-in .bin/.h pair: this call appends its window tiles to blob.buf
        # first (so bytes(blob.buf) below carries them) and returns the records that
        # point at those offsets. A kit with no manifest/complications yields empty
        # tables, and pack_geom then emits the old four-count blob unchanged.
        comps, _, banks = bake_complications(os.path.abspath(args.theme_dir), blob)
        for src, layer in rebucket_comps(comps, matte_mask):
            print("  comp src={} lost its plate: {} was folded, so the tile rides "
                  "below every live layer".format(src, layer))
        auxs = bake_aux(os.path.abspath(args.theme_dir), style, blob, args)
        geom   = _ot.pack_geom(scalars, hand_recs, shadow_recs, overlay,
                              list(tourb_recs or []), rects, first, delays, frames,
                              comps=comps, aux=auxs, bank=banks)
        os.makedirs(os.path.dirname(args.orbtheme) or ".", exist_ok=True)
        n = _ot.write_container(args.orbtheme, name, geom, bytes(blob.buf),
                                SCREEN_W, SCREEN_H,
                                baked_ms=int(time.time() * 1000) & 0xFFFFFFFF)
        print("wrote {} ({} B, {} B of pixels + {} B of geometry)".format(
            args.orbtheme, n, len(blob.buf), len(geom)))
        # FORMAT 2 makes the card filename a pure function of the name baked inside the
        # container: the device files the card copy under sha256(name)[:12].orbtheme, so
        # what this file is named on disk is irrelevant — the page and the card derive it
        # from the header. The name you typed here is therefore the only thing to get
        # right; print the id it produces so a hand-copied card can be named to match.
        tid = _ot.theme_id(name)
        print("  name {:<24s} -> card file {}.orbtheme".format(repr(name), tid))
        stem = os.path.splitext(os.path.basename(args.orbtheme))[0]
        if stem != tid:
            print("  (to hand-copy this to the card, name it \"{}.orbtheme\"; the page "
                  "renames the upload by its header name and ignores this file's name)"
                  .format(tid))

    if args.dump_dir:
        dump_inspection(args.dump_dir, blob.buf, gif,
                        [(f"hand_{KINDS[i]}", hand_recs[i]) for i in range(5)] +
                        [(f"shadow_{KINDS[i]}", shadow_recs[i]) for i in range(5)] +
                        [("overlay", overlay)])

    total = len(blob.buf)
    print("\nwrote {} ({} B = {:.2f} MiB)".format(args.bin, total, total / 1048576))
    print("wrote " + args.header)
    print("theme '{}': show=[{}] shadow={}({},{}) sweep={} plateFollow={} animFrames={} bg=0x{:06X}".format(
        theme_name,
        ",".join(KINDS[i] for i in range(5) if hand_recs[i]["show"]),
        int(shadow_on), shadow_dx, shadow_dy, int(second_sweep), plate_follow,
        anim_frames, bgcolor))
    print("next: idf.py build && idf.py flash — the top-level CMakeLists.txt flashes "
          "this blob into the `assets` partition (which must be >= this blob size)")


if __name__ == "__main__":
    main()



