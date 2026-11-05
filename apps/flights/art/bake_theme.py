#!/usr/bin/env python3
"""Bake the tracker's themes from the art (flight-tracker-design §11, theme-pool-plan §0
ruling 2: no editor yet, so the themes are baked by hand and stored like any other).

    ~/.venvs/bake/bin/python apps/flights/art/bake_theme.py [--family F] [--form types|single]
                       [--colour bands|off] [--trail DEG] [--out DIR]

With no --family, bakes each family with its pairing (make_art.PAIRS). The art comes from
make_art.py's renderers, rasterised with cairosvg; the theme is assembled by the OS's own
stage bake (baker@1's GorbBake.stage3) under node, so its planes, checks and packing are
the editors'. Needs cairosvg and Pillow (the bake venv) and node.

What a tracker theme holds, and what apps/flights/main.c expects of it:
  - the plate: the background, the whole glass;
  - row 0 (slot 0): the radar arm, pivot on the glass centre. Declared a sweeping seconds
    row at SWEEP_FPS so the scheduler runs the stage at that rate; the app poses it itself;
  - rows 1..PLANE_ROWS (slots 1..15): the plane pool, one bank of 49 frames every row names,
    each row's pivot on the glass centre; the app moves a row by its pose's offset, turns it
    by the track, and picks its frame: shape * 7 + band, in SHAPES and make_art.BANDS order;
  - slots SEL_SLOT and EM_SLOT: the selection and emergency rings, centred on the glass
    centre, which the app moves onto a contact (theme-text-plan §0 ruling 3);
  - four fonts (theme-text-plan §2.1) in the background's own typeface, colours and finish:
    body, accent, quiet and warning;
  - four labels (§2.2) at LABELS' slots, each a panel and its fonts by tone: the header, the
    footer, the info card and the range figure. The app writes their strings.
The range rings and the home dot never move, so they are the plate's (ruling 2).
"""
import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import make_art as A          # noqa: E402

import cairosvg               # noqa: E402
from PIL import Image, ImageChops   # noqa: E402

BAKER = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime", "baker@1", "orbtheme-baker.js")
SHAPES = ("light", "turboprop", "bizjet", "airliner", "heavy", "helicopter", "generic")
PLANE_ROWS = 15               # the contacts drawn at once: with the arm, the 16 rows an app's
                              # layer table holds (ORB_LAYER_MAX), so the direct compositor draws it
SWEEP_FPS = 20                # the arm's declared sweep: the stage's frame rate (15 until
                              # 2026-11-04; an OS before the 20 ceiling clamps it to 15)
GROUND_OPA = 0.45             # colour off: ground traffic recedes (§11.1 ruling 8)
SEL_SLOT, EM_SLOT = 16, 17    # apps/flights/main.c: the marker rings' slots
SEL_R, EM_R = 28, 32          # their radii, px: a ring clears a 38 px plane
EMERGENCY = "#FF2020"

# theme-text-plan §2: the tracker's words. Each background names its typeface, its tones'
# colours (a pair is a vertical gradient: the brass engraving's finish) and the card's panel.
FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
TEXT_LOOKS = {
    "scope": dict(face="DejaVuSans.ttf", body="#E6F0EA", accent="#39FF14", quiet="#8A9A90",
                  warning="#FFB23C", card=("#101820", 0.82), edge="#2A6A44"),
    "chart": dict(face="DejaVuSans.ttf", body="#14202C", accent="#1E6B2E", quiet="#5B5240",
                  warning="#B85C00", card=("#F4EEDC", 0.94), edge="#2F5C8C"),
    "brass": dict(face="DejaVuSerif.ttf", body="#F2E6C8", accent=("#F8E7A8", "#B8892E"),
                  quiet="#A8946A", warning="#FF9A3C", card=("#161815", 0.88), edge="#C9A24E",
                  # the bezel is engraved to the rim, so the small words ride enamel plaques
                  plaque=True, boxes={"header": (163, 12, 140, 24), "footer": (163, 430, 140, 24),
                                      "range": (340, 364, 62, 20)}),
    "blueprint": dict(face="DejaVuSansMono.ttf", body="#FFFFFF", accent="#9FE3FF",
                      quiet="#B8CCE6", warning="#FFD27A", card=("#1C4A84", 0.88), edge="#DCE9F8"),
    "galaxy": dict(face="DejaVuSans.ttf", body="#EDEBFF", accent=("#FFD27A", "#FF8A5C"),
                   quiet="#9C95C9", warning="#FF6B6B", card=("#0B0820", 0.8), edge="#8C7BFF"),
}
# The fonts by slot: (tone colour, px). Body and accent at 16 px, quiet and warning at 14
# (flight-tracker-design §11.5: 14 px is the smallest the glass reads).
FONTS = (("body", 16), ("accent", 16), ("quiet", 14), ("warning", 14))
CHARSET = tuple(chr(c) for c in range(0x20, 0x7F)) + ("\u2191", "\u2193", "\u2192", "\u2190", "\u00b0")
NONE = 0xF
# The labels, by slot (apps/flights/main.c): box, padding, alignment (digit across, end
# down), fonts by tone (normal, accent, warning, quiet), and whether the box is a panel.
LABELS = (
    dict(slot=20, name="header", box=(113, 10, 240, 26), pad=2, digit=1, end=1, tones=(0, 1, NONE, NONE)),
    dict(slot=21, name="footer", box=(113, 428, 240, 24), pad=2, digit=1, end=1, tones=(2, NONE, 3, NONE)),
    dict(slot=22, name="card", box=(103, 326, 260, 96), pad=8, digit=1, end=1, tones=(0, NONE, NONE, NONE),
         panel=True),
    dict(slot=23, name="range", box=(333, 362, 80, 22), pad=2, digit=1, end=1, tones=(2, NONE, NONE, NONE)),
)
SS = 4                        # supersampling for the raster-built outlines


def svg_doc(body, vb="0 0 100 100", defs=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s">%s%s</svg>'
            % (vb, "<defs>%s</defs>" % defs if defs else "", body))


def rast(svg, w, h=None):
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=w, output_height=h or w)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def outline(p, colour, px):
    """Blueprint's outline, built from rasters: cairo ignores SVG masks, so the silhouette
    stroked wide less the silhouette itself is the line, in the band's colour."""
    s = A._sil(p)
    big = rast(svg_doc('<g fill="#fff" stroke="#fff" stroke-width="9" stroke-linejoin="round">%s</g>' % s), px * SS)
    inner = rast(svg_doc('<g fill="#fff">%s</g>' % s), px * SS)
    a = ImageChops.subtract(big.getchannel("A"), inner.getchannel("A")).resize((px, px), Image.LANCZOS)
    img = Image.new("RGBA", (px, px), colour)
    img.putalpha(a)
    return img


def plane(fam, form, shape, band, px):
    """One frame: `shape` of `fam` in `band` (a colour, or None for an untinted metal)."""
    p, fn = A._pick(fam, "single" if form == "single" else shape)
    grads = A.FAMILIES[fam][5]
    if fam == "blueprint":
        img = outline(p, band, px)
        if form == "single":   # the drawing's own lines, which cairo strokes fine
            img = Image.alpha_composite(img, rast(svg_doc(_blueprint_detail(p, band)), px))
        return img
    return rast(svg_doc(fn(p, band, "m"), defs=A.gradient_defs(grads)), px)


def _blueprint_detail(p, band):
    j = A._j
    return ('<g fill="none" stroke="%s" stroke-width="1.6" stroke-opacity="0.85" stroke-linecap="round">%s</g>'
            '<line x1="50" y1="12" x2="50" y2="88" stroke="%s" stroke-width="1.2" stroke-dasharray="4 3" '
            'stroke-opacity="0.7"/>' % (band, j(p["engines"], p["lines"], p["canopy"]), band))


def plate(bg_name):
    """The background with the range rings (full range and half) and the home dot drawn in,
    in the background's own ring and home colours."""
    _t, _d, fn, look = A.BACKGROUNDS[bg_name]
    rings = "".join('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="%s" stroke-opacity="%s" '
                    'stroke-width="1.5"/>' % (A.CX, A.CX, r, look["ring"], look["ring_opa"])
                    for r in (A.RING_R, A.RING_R // 2))
    home = '<circle cx="%d" cy="%d" r="3" fill="%s"/>' % (A.CX, A.CX, look["home"])
    svg = A.bg_svg(fn)
    return rast(svg.replace("</svg>", rings + home + "</svg>"), A.GLASS)


def marker(r, colour):
    """A marker ring, 2 px, on a box 2r across: what the app's ring widget drew."""
    d = 2 * r
    return rast('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d"><circle cx="%d" cy="%d" '
                'r="%d" fill="none" stroke="%s" stroke-width="2"/></svg>' % (d, d, r, r, r - 1, colour), d)


def tone_str(tones):
    """A label's `str`: its fonts by tone, a nibble each, tone 0 lowest."""
    return sum(f << (4 * k) for k, f in enumerate(tones))


def font(face, px, colour):
    """One font: every CHARSET glyph in a cell of one size, its ink from the cell's left
    edge on a shared baseline, in `colour` (a pair is a top-to-bottom gradient). Returns
    (cell w, cell h, glyphs, space advance, tracking)."""
    from PIL import ImageDraw, ImageFont
    f = ImageFont.truetype(FONT_DIR + face, px)
    boxes = {c: f.getbbox(c) for c in CHARSET}
    top = min(b[1] for b in boxes.values() if b[2] > b[0])
    bottom = max(b[3] for b in boxes.values() if b[2] > b[0])
    asc, desc = f.getmetrics()
    top, bottom = min(top, 0), max(bottom, asc + desc)
    w = max(b[2] - b[0] for b in boxes.values())
    h = bottom - top
    glyphs = []
    for c in CHARSET:
        mask = Image.new("L", (w, h), 0)
        l, _t, r, _b = boxes[c]
        if r > l:
            ImageDraw.Draw(mask).text((-l, -top), c, font=f, fill=255)
        if isinstance(colour, tuple):
            ink = Image.linear_gradient("L").resize((w, h))
            a, b = Image.new("RGB", (w, h), colour[0]), Image.new("RGB", (w, h), colour[1])
            img = Image.composite(b, a, ink).convert("RGBA")
        else:
            img = Image.new("RGBA", (w, h), colour)
        img.putalpha(mask)
        glyphs.append(img)
    space = max(1, round(f.getlength(" ")))
    return w, h, glyphs, space, 1


def label_box(lab, look):
    """A label's box: the background's own placement where it names one."""
    return look.get("boxes", {}).get(lab["name"], lab["box"])


def label_panel(lab, look):
    """A label's panel: the card a rounded fill with the background's edge colour, and on a
    background with plaques every label one; otherwise clear, the text alone on the glass."""
    x, y, w, h = label_box(lab, look)
    if not (lab.get("panel") or look.get("plaque")):
        return Image.new("RGBA", (w, h), (0, 0, 0, 0))
    fill, opa = look["card"]
    rx = 10 if lab.get("panel") else 6
    return rast('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d"><rect x="0.75" y="0.75" '
                'width="%.1f" height="%.1f" rx="%d" fill="%s" fill-opacity="%s" stroke="%s" '
                'stroke-width="1.5"/></svg>' % (w, h, w - 1.5, h - 1.5, rx, fill, opa, look["edge"]), w, h)


def frames(fam, form, colour, px):
    """The bank: 7 shapes x 7 bands, shape-major. Colour off: every band the family's own
    colour, the ground's frames at GROUND_OPA."""
    neutral = A.FAMILIES[fam][4]
    out = []
    for shape in SHAPES:
        for b, (_name, c) in enumerate(A.BANDS):
            band = c if colour == "bands" else neutral
            if band is None and fam == "blueprint":
                band = "#EAF3FF"
            img = plane(fam, form, shape, band, px)
            if colour == "off" and b == 0:
                img.putalpha(img.getchannel("A").point(lambda v: int(v * GROUND_OPA)))
            out.append(img)
    return out


ARM_FNS = {"wedge": A.arm_wedge, "needle": A.arm_needle, "pointer": A.arm_pointer, "drafting": A.arm_drafting}


def arm(name, trail):
    fn = (lambda: A.arm_wedge(trail=trail)) if name == "wedge" else ARM_FNS[name]
    (x, y, w, h), body = fn()
    img = rast('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s %s %s %s">%s</svg>' % (x, y, w, h, body), w, h)
    return img, (-x, -y)


def preview(bg, arm_img, piv, frames_, px):
    """The picker's picture: the scope with its arm and a few contacts, at 232 px."""
    g = bg.copy()
    g.alpha_composite(arm_img, (A.CX - piv[0], A.CX - piv[1]))
    for (shape, band, dx, dy, trk) in ((3, 6, 70, -90, 220), (4, 5, -110, -20, 95), (0, 2, 40, 60, 330),
                                       (2, 4, -60, 100, 15), (5, 3, 110, 40, 120)):
        f = frames_[shape * 7 + band].rotate(-trk, resample=Image.BICUBIC)
        g.alpha_composite(f, (A.CX + dx - px // 2, A.CX + dy - px // 2))
    return g.resize((232, 232), Image.LANCZOS)


STAGE_JS = r"""
const B = require(process.argv[1]), m = JSON.parse(require('fs').readFileSync(process.argv[2], 'utf8'));
const fs = require('fs'), rd = (f, w, h) => ({ w, h, rgba: new Uint8Array(fs.readFileSync(f)) });
const S = B.stage3({ dither: true }), AUX = B.classId('aux');
S.row({ n: S.id(), class: B.classId('plate'), slot: 0, x: 0, y: 0, w: m.glass, h: m.glass, order: 0,
        alpha: 255, n_frames: 1 }, rd(m.bg, m.glass, m.glass), false, true);
// the arm: a sweeping seconds row, so the stage runs at its rate; the app poses it itself
S.row({ n: S.id(), class: AUX, slot: 0, x: m.cx - m.arm.px, y: m.cx - m.arm.py, w: m.arm.w, h: m.arm.h,
        pivot_x: m.arm.px, pivot_y: m.arm.py, order: 1, alpha: 255, n_frames: 1,
        src: B.SOURCES.indexOf('second'), span: 3600, vmax: 60, cadence: 1, cad_arg: m.fps, end: 0 },
      rd(m.arm.file, m.arm.w, m.arm.h), true, true);
const planes = m.frames.map((f) => rd(f, m.px, m.px));
const rows = Array.from({ length: m.rows }, (_, i) => ({ slot: 1 + i, x: m.cx - (m.px >> 1), y: m.cx - (m.px >> 1), order: 2 }));
S.pool({ class: AUX, w: m.px, h: m.px, pivot_x: m.px >> 1, pivot_y: m.px >> 1, alpha: 255 }, planes, rows, true, true);
for (const k of m.markers)
  S.row({ n: S.id(), class: AUX, slot: k.slot, x: m.cx - (k.d >> 1), y: m.cx - (k.d >> 1), w: k.d, h: k.d,
          pivot_x: k.d >> 1, pivot_y: k.d >> 1, order: 3, alpha: 255, n_frames: 1 },
        rd(k.file, k.d, k.d), true, true);
for (const f of m.fonts)
  S.rowFrames({ n: S.id(), class: B.classId('text'), slot: f.slot, x: 0, y: 0, w: f.w, h: f.h, order: 0,
                n_frames: f.files.length, pivot_x: f.space, pivot_y: f.track },
              f.files.map((g) => rd(g, f.w, f.h)), true, false);
for (const l of m.labels)
  S.row({ n: S.id(), class: AUX, slot: l.slot, kind: 2, src: B.SOURCES.indexOf('text'), x: l.x, y: l.y,
          w: l.w, h: l.h, pivot_x: l.w >> 1, pivot_y: l.h >> 1, order: 4, alpha: 255, n_frames: 1,
          str: l.str, radius: l.pad, digit: l.digit, end: l.end }, rd(l.file, l.w, l.h), true, false);
S.preview({ plane: rd(m.preview, 232, 232), x: 0, y: 0, order: 3 });
const b = S.finish({ name: m.name, slug: 'flights', serial: m.serial, abi: B.GEOM_ABI6 });
fs.writeFileSync(m.out, Buffer.from(b.bytes));
process.stdout.write(JSON.stringify({ bytes: b.bytes.length, meter: b.meter || null }));
"""


def bake(fam, form, colour, trail, outdir, node):
    bg_name, arm_name = A.PAIRS[fam]
    title = A.FAMILIES[fam][0]
    name = "Flights %s%s%s" % (title, " detailed" if form == "single" else "",
                               " plain" if colour == "off" else "")
    px = A.PLANE_PX
    tmp = tempfile.mkdtemp(prefix="flights-bake.")
    raw = lambda img, n: (img.save(os.path.join(tmp, n + ".png")),
                          open(os.path.join(tmp, n), "wb").write(img.tobytes()), os.path.join(tmp, n))[2]
    bg = plate(bg_name)
    sel = A.BACKGROUNDS[bg_name][3]["sel"]
    arm_img, piv = arm(arm_name, trail)
    fr = frames(fam, form, colour, px)
    look = TEXT_LOOKS[bg_name]
    fonts = []
    for slot, (tone, size) in enumerate(FONTS):
        w, h, glyphs, space, track = font(look["face"], size, look[tone])
        fonts.append({"slot": slot, "w": w, "h": h, "space": space, "track": track,
                      "files": [raw(g, "font%d_%03d" % (slot, i)) for i, g in enumerate(glyphs)]})
    labels = []
    for lab in LABELS:
        x, y, w, h = label_box(lab, look)
        labels.append({"slot": lab["slot"], "x": x, "y": y, "w": w, "h": h, "pad": lab["pad"],
                       "digit": lab["digit"], "end": lab["end"], "str": tone_str(lab["tones"]),
                       "file": raw(label_panel(lab, look), "label_" + lab["name"])})
    m = {"glass": A.GLASS, "cx": A.CX, "px": px, "rows": PLANE_ROWS, "fps": SWEEP_FPS,
         "bg": raw(bg, "bg"), "arm": {"file": raw(arm_img, "arm"), "w": arm_img.width, "h": arm_img.height,
                                       "px": piv[0], "py": piv[1]},
         "frames": [raw(f, "f%02d" % i) for i, f in enumerate(fr)],
         "markers": [{"slot": SEL_SLOT, "d": 2 * SEL_R, "file": raw(marker(SEL_R, sel), "sel")},
                     {"slot": EM_SLOT, "d": 2 * EM_R, "file": raw(marker(EM_R, EMERGENCY), "em")}],
         "fonts": fonts, "labels": labels,
         "preview": raw(preview(bg, arm_img, piv, fr, px), "preview"),
         "name": name, "serial": 1,
         "out": os.path.join(outdir, "%s.orbtheme" % name.lower().replace(" ", "-"))}
    mf = os.path.join(tmp, "m.json")
    json.dump(m, open(mf, "w"))
    r = subprocess.run([node, "-e", STAGE_JS, BAKER, mf], capture_output=True, text=True, timeout=600)
    if r.returncode:
        sys.exit("the stage bake refused %s:\n%s" % (name, (r.stderr or r.stdout)[-2000:]))
    shutil.rmtree(tmp)
    got = json.loads(r.stdout)
    print("baked %-40s %8d B  (%s, %s arm, %s background)" % (m["out"], got["bytes"], fam, arm_name, bg_name))
    return m["out"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--family", choices=list(A.FAMILIES), action="append")
    ap.add_argument("--form", choices=("types", "single"), default="types")
    ap.add_argument("--colour", choices=("bands", "off"), default="bands")
    ap.add_argument("--trail", type=int, default=30, help="the wedge's trail in degrees (its area is its cost)")
    ap.add_argument("--out", default=os.path.join(ROOT, "apps", "flights", "build", "themes"))
    a = ap.parse_args()
    node = shutil.which("node") or os.path.expanduser("~/.local/share/nodeenv-gorb/bin/node")
    os.makedirs(a.out, exist_ok=True)
    for fam in a.family or list(A.FAMILIES):
        bake(fam, a.form, a.colour, a.trail, a.out, node)


if __name__ == "__main__":
    main()
