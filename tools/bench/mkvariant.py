#!/usr/bin/env python3
"""Build the art-side bench variants the fps-budget sweep needs, host-side.

  rows  SRC_FRAMES DST Y0 Y1   paste frame_01's rows back over every later
                               frame OUTSIDE the inclusive band [Y0..Y1], so
                               the baked delta rects cannot escape it. The
                               row-band lever (docs/fps-budget-benchmark-plan.md
                               Phase 3, and V-A at [140..330]).
  cart  SRC_KIT DST_KIT RMAX   alpha-zero every tourbillon pixel further than
                               RMAX from (233,233) — the thinner-carriage lever
                               (Phase 4 V-B). Pass the result through
                               tools/kit_check.py before baking it.
  animband SRC_FRAMES DST Y0 Y1 [--amp N]   SYNTHESISE background animation
                               that does not exist in the art: modulate every
                               pixel of full-width rows [Y0..Y1] frame-to-frame
                               with a strong periodic (8-cycle) pattern, so the
                               baked delta bank genuinely pushes those rows
                               every tick. Rows outside the band stay identical
                               to the base frames. This is how the rows-capacity
                               sweep makes 'X rows of the bg animate' real at any
                               X; kit3's own GIF only animates rows 128..335, so
                               band-clamping alone cannot price rows above or
                               below that. Amplitude 70 keeps every 16-px tile
                               above the bake's diff-thresh 12 cluster rule.
  static SRC_FRAMES DST        DST = 8 copies of frame_01: a GIF whose delta
                               bank is empty (plate stays on glass, tick still
                               rides the baked delay). The true no-animating-bg
                               control with the real dial art.

Both keep everything else byte-identical, so the only thing that moves between
the control and a variant is the quantity under test.
"""
import os
import shutil
import sys
import zipfile

from PIL import Image
import numpy as np

CX = CY = 233


def rows(src, dst, y0, y1):
    names = sorted(n for n in os.listdir(src) if n.lower().endswith('.png'))
    if not names:
        raise SystemExit(f'no PNGs in {src}')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    base = np.asarray(Image.open(os.path.join(src, names[0])), dtype=np.uint8)
    for n in names:
        im = np.asarray(Image.open(os.path.join(src, n)), dtype=np.uint8).copy()
        mask = np.ones(im.shape[:2], dtype=bool)
        mask[y0:y1 + 1, :] = False
        im[mask] = base[mask]
        Image.fromarray(im).save(os.path.join(dst, n))
    print(f'rows {src} -> {dst}: live band [{y0}..{y1}] '
          f'({y1 - y0 + 1} rows), {len(names)} frames')


def cart(src_kit, dst_kit, rmax):
    zin = zipfile.ZipFile(src_kit)
    names = zin.namelist()
    if os.path.exists(dst_kit):
        os.remove(dst_kit)
    with zipfile.ZipFile(dst_kit, 'w', zipfile.ZIP_DEFLATED) as zout:
        for n in names:
            data = zin.read(n)
            if n.startswith('tourbillon/') and n.lower().endswith('.png'):
                import io
                im = np.asarray(Image.open(io.BytesIO(data))).copy()
                h, w = im.shape[:2]
                yy, xx = np.mgrid[0:h, 0:w]
                r = np.sqrt((xx - CX) ** 2 + (yy - CY) ** 2)
                cut = r > rmax
                im[cut, 3] = 0
                buf = io.BytesIO()
                Image.fromarray(im).save(buf, format='PNG')
                data = buf.getvalue()
            zout.writestr(n, data)
    print(f'cart {src_kit} -> {dst_kit}: tourbillon alpha zeroed beyond '
          f'r={rmax} from ({CX},{CY})')


def animband(src, dst, y0, y1, amp=70.0):
    """Fill the band toward an 8-cycle sequence of tints at fixed strength s.

    out = base*(1-s) + s*tint[f]. Every pixel in the band moves EVERY tick
    (channel delta ~16..55 >= the bake's diff-thresh 12), regardless of the
    base value — pure black rows change too, which additive/sine patterns get
    wrong via clipping and zero-crossing. The whole band is contiguous changed
    tiles, so the packer merges to a few full-width rects and stays inside the
    bank's 16-rects/frame cap even for the 448-row band (a fragmented mask
    overruns the cap and the bake falls back to full-page rects — measured).
    Tints are chosen so every consecutive pair (incl. the loop wrap) differs by
    >=45 in each channel.
    """
    names = sorted(n for n in os.listdir(src) if n.lower().endswith('.png'))
    if len(names) != 8:
        raise SystemExit(f'animband: expected an 8-frame loop, found {len(names)}')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    s = amp / 200.0                                   # 0.35 at the default
    tints = [(20, 20, 25), (170, 150, 90), (40, 35, 45), (190, 170, 100),
             (30, 28, 35), (180, 160, 95), (50, 42, 50), (160, 140, 85)]
    for f, n in enumerate(names):
        base = np.asarray(Image.open(os.path.join(src, n))).astype(np.int16)
        live = np.zeros(base.shape[:2], dtype=bool)
        live[y0:y1 + 1, :] = True
        for c in range(3):
            ch = base[..., c]
            ch[live] = np.round(ch[live] * (1 - s) + s * tints[f][c]).astype(np.int16)
            base[..., c] = ch
        Image.fromarray(np.clip(base, 0, 255).astype(np.uint8)).save(os.path.join(dst, n))
    print(f'animband {src} -> {dst}: rows [{y0}..{y1}] animated s={s:.2f}, '
          f'{len(names)} frames')


def static(src, dst):
    names = sorted(n for n in os.listdir(src) if n.lower().endswith('.png'))
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for i in range(len(names)):
        shutil.copy(os.path.join(src, names[0]),
                    os.path.join(dst, f'frame_{i + 1:02d}.png'))
    print(f'static {src} -> {dst}: {len(names)} identical frames')


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'rows':
        rows(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
    elif cmd == 'cart':
        cart(sys.argv[2], sys.argv[3], float(sys.argv[4]))
    elif cmd == 'animband':
        amp = 70.0
        if '--amp' in sys.argv:
            amp = float(sys.argv[sys.argv.index('--amp') + 1])
        animband(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), amp)
    elif cmd == 'static':
        static(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
