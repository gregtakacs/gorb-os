#!/usr/bin/env python3
"""Draw a baked tracker theme's labels as the orb draws them (theme-text-plan §2.3).

    ~/.venvs/bake/bin/python apps/flights/art/label_proof.py THEME.orbtheme... --out PNG

Compiles the shipped main/stage/orb_label.c into a host library and hands it each theme's
own fonts and panels, read back from the .orbtheme, with sample strings in every tone. The
plate goes under, so the proof shows the words where the glass shows them. One column per
theme. Needs gcc and Pillow.
"""
import argparse
import ctypes
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import orbbundle as ob             # noqa: E402
import theme_schema as ts          # noqa: E402
from PIL import Image              # noqa: E402

# What the app says, by label slot: (string, tone). The card shows a route and a climb.
SAMPLES = {
    20: [("25 aircraft", 0), ("zoom  40 nm", 1)],
    21: [("live  adsb.fi", 0), ("feed down 3 min", 2)],
    22: [("UAL1532  B38M\nDFW → SFO\n12450 ft  ↑ 2200 fpm\n318 kt  trk 287", 0)],
    23: [("80 nm", 0)],
}


class Font(ctypes.Structure):
    _fields_ = [("glyphs", ctypes.c_void_p), ("w", ctypes.c_uint16), ("h", ctypes.c_uint16),
                ("n", ctypes.c_uint16), ("space", ctypes.c_uint8), ("track", ctypes.c_uint8),
                ("measured", ctypes.c_bool), ("adv", ctypes.c_uint8 * 100)]


def lib():
    d = tempfile.mkdtemp(prefix="labelproof.")
    so = os.path.join(d, "liblabel.so")
    subprocess.run(["gcc", "-shared", "-fPIC", "-O1", "-I", os.path.join(ROOT, "main", "stage"),
                    "-I", os.path.join(ROOT, "main", "art"),
                    os.path.join(ROOT, "main", "stage", "orb_label.c"), "-o", so], check=True)
    L = ctypes.CDLL(so)
    L.orb_label_compose.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                    ctypes.POINTER(Font), ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    return L


def rgba(plane, w, h):
    """An RGB565A8 plane as a Pillow image."""
    n = w * h
    img = Image.new("RGBA", (w, h))
    px = []
    for i in range(n):
        c = plane[2 * i] | (plane[2 * i + 1] << 8)
        px.append((((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31, plane[2 * n + i]))
    img.putdata(px)
    return img


def proof(L, path):
    _h, secs = ob.read_bundle(path)
    sec = secs[0]
    els, spr, art = sec["blob"]["elements"], sec["blob"]["sprites"], sec["art"]
    plate = next(e for e in els if int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 0)
    p = spr[int(plate["frame0"])]
    raw = art[p["off"]:p["off"] + p["size"]]
    glass = Image.frombytes("RGB", (466, 466), bytes(
        b for i in range(0, len(raw), 2) for b in (
            ((raw[i] | raw[i + 1] << 8) >> 11 & 31) * 255 // 31,
            ((raw[i] | raw[i + 1] << 8) >> 5 & 63) * 255 // 63,
            ((raw[i] | raw[i + 1] << 8) & 31) * 255 // 31))).convert("RGBA")
    fonts, keep = {}, []
    for e in els:
        if int(e["class"]) != ts.CLASS_TEXT:
            continue
        g0 = spr[int(e["frame0"])]
        buf = ctypes.create_string_buffer(art[g0["off"]:g0["off"] + g0["size"] * int(e["n_frames"])])
        keep.append(buf)
        fonts[int(e["slot"])] = Font(ctypes.cast(buf, ctypes.c_void_p), int(e["w"]), int(e["h"]),
                                     int(e["n_frames"]), int(e["pivot_x"]), int(e["pivot_y"]), False)
    shots = []
    for which in range(2):
        g = glass.copy()
        for e in els:
            if int(e["class"]) != ts.CLASS_AUX or int(e["kind"]) != ts.SHAPE_TEXT:
                continue
            slot = int(e["slot"])
            text, tone = SAMPLES[slot][min(which, len(SAMPLES[slot]) - 1)]
            fi = ts.label_fonts(e["str"])[tone]
            if fi == ts.FONT_NONE:
                fi = ts.label_fonts(e["str"])[0]
            w, h = int(e["w"]), int(e["h"])
            pp = spr[int(e["frame0"])]
            panel = ctypes.create_string_buffer(art[pp["off"]:pp["off"] + pp["size"]])
            plane = ctypes.create_string_buffer(w * h * 3)
            L.orb_label_compose(plane, w, h, panel, ctypes.byref(fonts[fi]), text.encode(),
                                int(e["radius"]), int(e["digit"]), int(e["end"]))
            g.alpha_composite(rgba(plane.raw, w, h), (int(e["x"]), int(e["y"])))
        shots.append(g)
    return shots


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("themes", nargs="+")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    L = lib()
    cols = [proof(L, t) for t in a.themes]
    sheet = Image.new("RGBA", (466 * len(cols), 466 * 2), (0, 0, 0, 255))
    for x, shots in enumerate(cols):
        for y, s in enumerate(shots):
            sheet.paste(s, (466 * x, 466 * y))
    sheet.save(a.out)
    print("wrote %s: %d themes, normal tones above, accent and warning below" % (a.out, len(cols)))


if __name__ == "__main__":
    main()
