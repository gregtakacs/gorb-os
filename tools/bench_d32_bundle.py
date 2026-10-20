#!/usr/bin/env python3
"""bench_d32_bundle.py — the D32 slice-2 bench theme, hand-built per ruling R-H.

The projection must not pass only on synthetic goldens. This writes a real
FORMAT 3 clock bundle named "d32-bench": the shipped av-kit art copied
byte-for-byte off the product bundle rebake_bundle.py produces, plus the two
D32 additions drawn here so the glass cannot be mistaken for filler.

  hand/5  the carriage: eight frames of an openwork balance (gold rim, four
          spokes turning 22.5 degrees per frame, blued hub), centred on the
          dial. The dial must fan it out into the tourbillon bank, turn it by
          the seconds angle plus the row's start orientation (45 degrees), and
          swap frames at the 12 fps the scalars name.
  bank 0  a month window (kind window, src month): twelve gold month plates
          ordered at bucket 0, so it sits under every live layer. The row
          carries twelve plates; the CLOCK domain table is what says twelve is
          the number that indexes a month.

Planes encode exactly as bake_clock.py encodes a bank: little-endian RGB565
colour, then the A8 alpha plane at stride w. The id is sha256(name)[:12],
derived at the door — never pinned here.

    python3 tools/bench_d32_bundle.py [out.orbtheme]
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402
import orbtheme as ot           # noqa: E402
import bake_clock as bc         # noqa: E402  (rgb565_le / alpha_bytes)

BASE = os.environ.get("D32_BASE", "/tmp/b3/avkit.rebake.orbtheme")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d32bench.orbtheme"
SCR = 466
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"
CAR_N, CAR_D, CAR_FPS, CAR_SZ = 8, 4500, 12, 160
WIN_W, WIN_H = 108, 44
MONTHS = ("JAN", "FEB", "MAR", "APR", "MAY", "JUN",
          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")


def carriage(step):
    """One balance frame: the spoke cross at step*22.5 degrees, 8x oversampled."""
    S = CAR_SZ * 8
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c, r_out, r_in = S // 2, S // 2 - 44, S // 2 - 118
    d.ellipse([c - r_out, c - r_out, c + r_out, c + r_out],
              outline=(217, 179, 92, 255), width=44)
    a0 = step * math.pi / 8.0
    for k in range(4):
        a = a0 + k * math.pi / 2.0
        d.line([c - int(r_in * math.sin(a)), c - int(r_in * math.cos(a)),
                c + int(r_in * math.sin(a)), c + int(r_in * math.cos(a))],
               fill=(217, 179, 92, 255), width=28)
    d.ellipse([c - 44, c - 44, c + 44, c + 44], fill=(70, 110, 200, 255))
    d.ellipse([c - 18, c - 18, c + 18, c + 18], fill=(235, 225, 200, 255))
    return im.resize((CAR_SZ, CAR_SZ), Image.LANCZOS)


def month_tile(text):
    """One window plate: a gold-framed card, the month in warm white."""
    S = (WIN_W * 8, WIN_H * 8)
    im = Image.new("RGBA", S, (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([12, 12, S[0] - 12, S[1] - 12], radius=36,
                        fill=(28, 24, 18, 200), outline=(217, 179, 92, 255),
                        width=8)
    d.text((S[0] // 2, S[1] // 2), text, anchor="mm", fill=(235, 225, 200, 255),
           font=ImageFont.truetype(FONT, 190))
    return im.resize((WIN_W, WIN_H), Image.LANCZOS)


def row(n, cls, slot, order, f0, nfr, x, y, w, h, px, py, **kw):
    """One element row, blanked then filled. `class` is a python keyword, so the
    dict carries it by key, never as a named argument."""
    rec = ts.blank_element()
    rec.update(n=n, slot=slot, flags=0, order=order, alpha=255, frame0=f0,
               n_frames=nfr, x=x, y=y, w=w, h=h, pivot_x=px, pivot_y=py,
               radius=0, degrees=0, scale16=0, kind=0, src=0)
    rec["class"] = cls
    rec.update(kw)
    return rec


def main():
    if not os.path.exists(BASE):
        sys.exit("no product bundle at %s — run rebake_bundle.py first" % BASE)
    hdr, secs = ob.read_bundle(BASE)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    els = [dict(e) for e in sec["blob"]["elements"]]
    sc = dict(sec["blob"]["scalars"])
    spr = [dict(s) for s in sec["blob"]["sprites"]]
    art, at = bytearray(sec["art"]), len(sec["art"])

    def planes(imgs, w, h):
        """Append each image as one catalogue plane; return the first row."""
        nonlocal at
        first = len(spr)
        for im in imgs:
            b = bc.rgb565_le(im) + bc.alpha_bytes(im)
            assert len(b) == w * h * 3, "plane not the bank's format"
            art.extend(b)
            spr.append({"off": at, "size": w * h * 3, "w": w, "h": h})
            at += w * h * 3
        return first

    car0 = planes([carriage(i) for i in range(CAR_N)], CAR_SZ, CAR_SZ)
    els.append(row(40, ts.CLASS_HAND, 5, 1, car0, CAR_N,
                   SCR // 2 - CAR_SZ // 2, SCR // 2 - CAR_SZ // 2,
                   CAR_SZ, CAR_SZ, CAR_SZ // 2, CAR_SZ // 2,
                   degrees=CAR_D))
    sc["tourbillon_n"], sc["tourbillon_fps"] = CAR_N, CAR_FPS
    win0 = planes([month_tile(t) for t in MONTHS], WIN_W, WIN_H)
    els.append(row(41, ts.CLASS_BANK, 0, 0, win0, 12,
                   SCR // 2 - WIN_W // 2, SCR // 2 + 130,
                   WIN_W, WIN_H, 0, 0, scale16=16, kind=0, src=2))
    ob.write_bundle(OUT, "d32-bench",
                    [{"slug": "clock", "elements": els, "scalars": sc,
                      "sprites": spr, "art": bytes(art),
                      "scr_w": SCR, "scr_h": SCR}])
    h2, s2 = ob.read_bundle(OUT)     # the reader's whole verdict, on the way out
    print("%s: name %r id %s, %d elements, %d planes, art %d B"
          % (OUT, h2["name"], ot.theme_id(h2["name"]), len(els),
             len(s2[0]["blob"]["sprites"]), h2["art_size"]))


if __name__ == "__main__":
    main()
