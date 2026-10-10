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


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'rows':
        rows(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
    elif cmd == 'cart':
        cart(sys.argv[2], sys.argv[3], float(sys.argv[4]))
    else:
        raise SystemExit(__doc__)
