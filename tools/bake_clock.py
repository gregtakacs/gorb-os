#!/usr/bin/env python3
"""
bake_clock.py — offline analog-clock theme baker for orb-os-esp-idf.

Reads an orb-os (Arduino) theme folder — its clock_style.json, theme.json and
the clock_*.png layer art — and produces two artefacts for the IDF firmware:

  assets/clock_assets.bin   one packed blob of raw pixel planes
  assets/clock_assets.h     geometry parsed offline from clock_style.json, plus
                            the extern symbols for the blob. NO runtime JSON
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


class Blob:
    """Append-only packed byte buffer that returns each segment's start offset."""
    def __init__(self):
        self.buf = bytearray()

    def add(self, data: bytes):
        off = len(self.buf)
        self.buf += data
        return off


def emit_sprite(blob, rgba, offx, offy, pivot_x, pivot_y, center_x, center_y, blend, show):
    w, h = rgba.size
    color_off = blob.add(rgb565_le(rgba))
    blob.add(alpha_bytes(rgba))
    # pivot expressed in CROPPED coords = original pivot minus crop origin
    return dict(w=w, h=h, offx=offx, offy=offy, pivotx=pivot_x - offx, pivoty=pivot_y - offy,
                centerx=center_x, centery=center_y, blend=blend, show=int(bool(show)),
                color_off=color_off)


def blank_sprite(cx, cy, show=0):
    return dict(w=0, h=0, offx=0, offy=0, pivotx=0, pivoty=0,
                centerx=cx, centery=cy, blend=0, show=show, color_off=0)


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
                   the risk of baking a stale folder left over from last time)
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
        if not items:
            raise SystemExit(f"--bg-frames: no PNG entries inside {p}")
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


def _gif_transition(canvas, tgt, painted, diff_thresh, K=12):
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
        lst = _pack_mask(mask)
        if (len(lst) == 1 and lst[0] == (0, 0, SCREEN_W, SCREEN_H)
                and t != diff_thresh * 5):
            continue                             # collapsed: try a stricter view
        return lst, mask
    return [(0, 0, SCREEN_W, SCREEN_H)], raw


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
                 fps=0.0, frame_glob=None, frame_step=1):
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
    so a bank baked from PNGs behaves exactly like one baked from a GIF."""
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
        # PNG delays were synthesised from --bg-fps, which is already the
        # PLAYBACK rate of the kept frames (step = source_frames / fps), so
        # they stay untouched.
        print(f"  decimated every {frame_step}th frame: {len(imgs)} kept "
              f"(loop still 1 authored period)")
    F = [_frames_to_u16(im) for im in imgs]
    print(f"  {'gif' if is_gif else 'frames'} bg: {src} -> {len(F)} frames @ "
          f"{SCREEN_W}x{SCREEN_H} fit={fit} "
          f"(diff thresh {diff_thresh}, cluster>=12/25, tile16<=16 rects)")
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
            lst, _mask = _gif_transition(canvas, tgt, painted, diff_thresh)
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
                masks=per_m, key_start=start, frames_u16=F)


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
        stale_tot += int(stale.sum())
        mask_png(stale, os.path.join(dump_dir, f"stale_{j:02d}.png"))

    print("  dumped {} frames + sprites to {}/".format(n, dump_dir))
    print("  stale map totals {} px/frame >4 codes off (0 = the panel's canvas "
          "matches the GIF exactly)".format(stale_tot // max(1, n)))


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
    ap.add_argument("--frame-step", type=int, default=1, metavar="N",
                    help="take every Nth source frame before baking "
                         "(DECIMATION). The pack is one authored loop sampled at "
                         "the generator's fps; playing all of it at a lower "
                         "--bg-fps runs the loop slow (30 frames at 15 fps = a "
                         "2 s loop). Use N = source_frames / playback_fps so one "
                         "pass stays one authored second: 30-pack -> step 2 for "
                         "15 fps; a 60-pack -> step 4 for 15 fps or step 5 for "
                         "12 fps. Must divide the frame count.")
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
    ap.add_argument("--no-pivot-snap", action="store_true",
                    help="keep each hand's authored pivotX/pivotY verbatim. By "
                         "default a rotating hand's pivot is snapped to the "
                         "centre of its own hub: an authored pivot a few px off "
                         "the hub makes the hand's centre orbit it as it sweeps "
                         "(the second hand here declared (16,75) for a hub at "
                         "(16,71), a 2.9px wobble) and leaves the ink misaligned "
                         "against the plate band it repaints.")
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

    gif = None
    bg_src = args.bg_gif or args.bg_frames
    if bg_src:
        # ---- animated background bank: one keyframe + per-frame delta rects.
        # The theme's own plate/bgAnim art is skipped entirely — the bank IS the
        # clock's background now, whether it came from one GIF or one folder of
        # PNG frames. Both take the identical packer below. ----
        bg = style.get("bgAnim", {}) or {}
        gif = bake_bg_bank(blob, bg_src, args.diff_thresh,
                           denoise=args.bg_denoise, fit=args.bg_fit,
                           fps=(args.bg_fps or (0.0 if args.bg_gif
                                                else float(bg.get("fps", 0) or 0))),
                           frame_glob=args.bg_glob, frame_step=args.frame_step)
        gif["src"] = bg_src
        plate_off = gif["key_off"]   # the keyframe doubles as the "static plate"
        baked_frames, anim_frames = [], 0
    else:
        # ---- plate: full-screen RGB565 colour plane, at the front of the blob ----
        plate_img = load_rgba(tdir, "clock_plate.png")
        if plate_img is None:
            plate_img = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 255))
        plate_img = resize_full(plate_img)
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
        px = int(hs.get("pivotX", 0)); py = int(hs.get("pivotY", 0))
        cx = int(hs.get("centerX", SCREEN_W // 2)); cy = int(hs.get("centerY", SCREEN_H // 2))
        blend = int(hs.get("blend", 0))

        img = load_rgba(tdir, f"clock_hand_{kind}.png")
        # Correct an authored pivot that sits off the hand's own hub: only the
        # three rotating hands need it (static layers never spin), and only
        # when the theme's value actually wobbles (see snap_pivot_to_hub). The
        # shadow is emitted with the SAME px/py so it stays locked to the hand.
        pivot_note = ""
        if img is not None and not args.no_pivot_snap and kind in ("hour", "minute", "second"):
            spx, spy = px, py
            nx, ny, orb = snap_pivot_to_hub(img, spx, spy, cx, cy)
            if (nx, ny) != (spx, spy):
                pivot_note = f" [pivot {spx},{spy}->{nx},{ny} orbit {orb:.2f}px]"
                px, py = nx, ny
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
            shadow_recs.append(emit_sprite(blob, crop, ox, oy, px, py,
                                           cx + shadow_dx, cy + shadow_dy, 0, shadow_on and show))

        rec = hand_recs[-1]
        print(f"  {kind:8s} show={int(show)} pivot=({px},{py}) center=({cx},{cy}) blend={blend}"
              + ("" if img is None else
                 f" canvas={img.size} ink={rec['w']}x{rec['h']}@({rec['offx']},{rec['offy']})")
              + pivot_note)

    _finish(style, theme_name, bg, shadow_on, shadow_dx, shadow_dy,
            hand_recs, shadow_recs, overlay, baked_frames, anim_frames,
            plate_off, blob, args, gif)


def _finish(style, theme_name, bg, shadow_on, shadow_dx, shadow_dy,
            hand_recs, shadow_recs, overlay, baked_frames, anim_frames,
            plate_off, blob, args, gif=None):
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
        return ("  {{ {:>4}, {:>4}, {:>5}, {:>5}, {:>5}, {:>5}, {:>5}, {:>5}, {:>2}, {:>2}, {:>9} }}, /* {} */".format(
            r["w"], r["h"], r["offx"], r["offy"], r["pivotx"], r["pivoty"],
            r["centerx"], r["centery"], r["blend"], r["show"], r["color_off"], label))

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
               "} clk_sprite_t;\n\n")
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
               "  clk_sprite_t hand[5];\n"
               "  clk_sprite_t shadow[5];\n"
               "  clk_sprite_t overlay;\n"
               "  uint8_t  bg_mode;         /* 0 = plate + bgAnim frames, 1 = animated delta bank (GIF or PNG frames) */\n"
               "  uint32_t bg_key_off;      /* mode 1: full RGB565 keyframe (frame 0) */\n"
               "  uint16_t gif_frames;      /* mode 1: frames in the loop (incl. keyframe) */\n"
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
    hdr.append("/* total blob size; the embedded clock_assets_bin must be at least this big */\n"
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
    hdr.append("  /* hand */ {\n" + "\n".join(fmt_spr(hand_recs[i], KINDS[i]) for i in range(5)) + "\n  },\n")
    hdr.append("  /* shadow */ {\n" + "\n".join(fmt_spr(shadow_recs[i], KINDS[i]) for i in range(5)) + "\n  },\n")
    hdr.append("  /* overlay */" + fmt_spr(overlay, "overlay")[1:] + "\n")
    hdr.append("  /* bg_mode,bg_key_off,gif_frames */ {}, {}, {}\n".format(
        1 if gif else 0, (gif["key_off"] if gif else 0), (gif["frames"] if gif else 0)))
    hdr.append("};\n\n")
    hdr.append("/* Symbols from target_add_binary_data(... clock_assets.bin BINARY). */\n"
               "extern const uint8_t clock_assets_bin_start[] asm(\"_binary_clock_assets_bin_start\");\n"
               "extern const uint8_t clock_assets_bin_end[]   asm(\"_binary_clock_assets_bin_end\");\n")

    os.makedirs(os.path.dirname(args.header) or ".", exist_ok=True)
    with open(args.header, "w") as f:
        f.write("".join(hdr))

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
    print("next: add target_add_binary_data() to main/CMakeLists.txt and rebuild "
          "(factory partition must hold app + this blob)")


if __name__ == "__main__":
    main()



