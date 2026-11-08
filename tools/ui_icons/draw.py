#!/usr/bin/env python3
"""The orb's drawn cards (app-platform-plan §61): the carousel's Settings and the theme
carousel's Back, which the firmware carries (owner, 2026-11-08, lifting D22 for these two),
and HSB's app icon, which ships in its own .orbapp (orb.icon).

tools/ui_icons/draw.py OUTDIR    -> OUTDIR/settings.png, back.png, hsb.png (466 px)

Run through tools/ui_icons/make.py, which writes the firmware arrays and the app icon.
Deterministic: seeded noise, fixed supersampling, Pillow's BOX filter.

Each card is drawn SS x supersampled from signed distances and height maps: a dark dial,
a polished bevelled rim, and a raised glyph lit from the top left with a soft drop shadow,
then box-filtered down to 466."""
import math
import sys

import numpy as np
from PIL import Image, ImageFilter

N, SS = 466, 3
M = N * SS
yy, xx = np.mgrid[0:M, 0:M].astype(np.float32)
c = (M - 1) / 2.0
X = (xx - c) / (M / 2.0)
Y = (yy - c) / (M / 2.0)
R = np.sqrt(X * X + Y * Y)
TH = np.arctan2(Y, X)
AA = 1.2 / (M / 2.0)
LIGHT = np.array([-0.5, -0.62, 0.6]); LIGHT /= np.linalg.norm(LIGHT)
HALF = LIGHT + np.array([0, 0, 1.0]); HALF /= np.linalg.norm(HALF)


def cov(d):
    return np.clip(d / AA + 0.5, 0, 1).astype(np.float32)


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def bevel(d, width, height):
    """A rounded bevel: 0 at the edge, `height` from `width` inside."""
    t = np.clip(d / width, 0, 1)
    return (np.sqrt(1 - (1 - t) ** 2) * height).astype(np.float32)


def lit(h, base, spec=0.6, shine=40, amb=0.35):
    gy, gx = np.gradient(h)
    k = M / 2.0
    nx, ny, nz = -gx * k, -gy * k, np.ones_like(h)
    n = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx / n, ny / n, nz / n
    diff = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)
    sp = np.clip(nx * HALF[0] + ny * HALF[1] + nz * HALF[2], 0, 1) ** shine
    return np.clip(base * (amb + (1 - amb) * diff)[..., None] + (spec * sp)[..., None], 0, 1)


def brushed(seed, amount):
    rng = np.random.default_rng(seed)
    rings = np.convolve(rng.normal(0, 1, 8192), np.ones(7) / 7, mode="same").astype(np.float32)
    return (1 + amount * rings[np.clip((R * 4095).astype(int), 0, 8191)]).astype(np.float32)


def shadow(mask, dx=0.02, dy=0.03, blur=9, strength=0.65):
    a = np.asarray(Image.fromarray((mask * 255).astype(np.uint8))
                   .filter(ImageFilter.GaussianBlur(blur * SS)), np.float32) / 255
    a = np.roll(np.roll(a, int(dy * M / 2), 0), int(dx * M / 2), 1)
    return (1 - strength * a).astype(np.float32)


def glow(mask, blur, colour, strength):
    a = np.asarray(Image.fromarray((mask * 255).astype(np.uint8))
                   .filter(ImageFilter.GaussianBlur(blur * SS)), np.float32) / 255
    return (strength * a)[..., None] * np.array(colour, np.float32)


def hsv_rgb(h, s, v):
    i = np.floor(h * 6).astype(int) % 6
    f = h * 6 - np.floor(h * 6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    choices = [np.stack(x, -1) for x in ((v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q))]
    out = np.zeros(h.shape + (3,), np.float32)
    for k in range(6):
        out = np.where((i == k)[..., None], choices[k], out)
    return out


def card(seed, ground):
    """The dial every card shares: a dark ground with a vignette and faint sunray brushing,
    inside a polished, bevelled bezel."""
    g = np.array(ground, np.float32)
    img = g * (1.18 - 0.5 * R ** 1.5)[..., None] * (1 + 0.03 * np.sin(TH * 140))[..., None]
    r0, r1 = 0.905, 0.995
    d_rim = np.minimum(R - r0, r1 - R)
    h = bevel(d_rim, 0.045, 0.05)
    metal = np.array([0.80, 0.81, 0.84], np.float32) * brushed(seed, 0.035)[..., None]
    metal *= (0.78 + 0.22 * np.cos(2 * (TH + 0.75)))[..., None]
    rim = lit(h, metal, spec=0.9, shine=70, amb=0.25)
    m = cov(d_rim)
    img = img * (1 - m)[..., None] + rim * m[..., None]
    img *= (1 - 0.6 * smooth(r0 - 0.07, r0, R) * (R < r0))[..., None]      # inner shade
    return img.astype(np.float32), cov(1.0 - R)


def lay(img, d, h_bev, colour, spec=0.7, shine=45, amb=0.35, sh=True):
    m = cov(d)
    if sh:
        img = img * shadow(m)[..., None]
    g = lit(bevel(d, h_bev, 0.035), colour, spec=spec, shine=shine, amb=amb)
    return img * (1 - m)[..., None] + g * m[..., None], m


def save(img, disc, path):
    img = np.clip(img * disc[..., None], 0, 1)
    Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGB").resize((N, N), Image.BOX).save(path)


def settings(out):
    img, disc = card(3, (0.11, 0.13, 0.16))
    teeth, r_root, r_tip, r_hole = 10, 0.50, 0.66, 0.155
    f = (TH / (2 * math.pi) * teeth) % 1.0
    prof = smooth(0.10, 0.27, f) * (1 - smooth(0.60, 0.77, f))
    d = np.minimum((r_root + (r_tip - r_root) * prof) - R, R - r_hole)
    for k in range(6):                                   # six windows through the web
        a = k * math.pi / 3 + math.pi / 6
        d = np.minimum(d, np.hypot(X - 0.33 * math.cos(a), Y - 0.33 * math.sin(a)) - 0.072)
    steel = np.array([0.83, 0.85, 0.89], np.float32) * brushed(7, 0.06)[..., None]
    steel *= (1 + 0.035 * np.sin(R * 260))[..., None]
    img, _ = lay(img, d, 0.045, steel, spec=0.85, shine=55, amb=0.3)
    # a ruby jewel in the hub, as a movement carries one
    dj = 0.095 - R
    jm = cov(dj)
    ruby = np.array([0.80, 0.10, 0.14], np.float32) * (1.1 - 0.6 * R / 0.095)[..., None]
    j = lit(bevel(dj, 0.095, 0.05), ruby, spec=1.0, shine=90, amb=0.45)
    img = img * (1 - jm)[..., None] + j * jm[..., None]
    save(img, disc, out + "/settings.png")


def back(out):
    img, disc = card(5, (0.09, 0.115, 0.15))
    rc, w = 0.34, 0.085
    # the arc runs from the arrowhead on the left (angle 180°) over the top to the right and
    # down to about 60° below it; screen y grows downward, so "over the top" is 180°..360°.
    a = np.mod(TH, 2 * math.pi)
    start, end = math.radians(178), math.radians(400)
    aa = np.where(a < math.radians(90), a + 2 * math.pi, a)
    band = np.where((aa >= start) & (aa <= end), w - np.abs(R - rc), -1.0)
    tx_, ty_ = rc * math.cos(end), rc * math.sin(end)
    band = np.maximum(band, w - np.hypot(X - tx_, Y - ty_))          # round tail
    # the head at the start of the arc, pointing down (the arc's direction of travel)
    hx, hy = rc * math.cos(start), rc * math.sin(start)
    ux, uy = 0.0, 1.0
    vx, vy = 1.0, 0.0
    u = (X - hx) * ux + (Y - hy) * uy
    v = (X - hx) * vx + (Y - hy) * vy
    tip, base_u, half = 0.21, -0.07, 0.21
    tt = np.clip((tip - u) / (tip - base_u), 0, 1)
    tri = np.minimum(np.minimum(half * tt - np.abs(v), u - base_u), tip - u)
    d = np.maximum(band, tri)
    m = cov(d)
    img = img + glow(m, 16, (0.10, 0.55, 0.70), 0.35)                  # a cool halo
    white = np.array([0.95, 0.96, 0.98], np.float32) * (1.06 - 0.22 * (Y + 0.4))[..., None]
    img, _ = lay(img, d, 0.05, white, spec=0.6, shine=40, amb=0.5)
    save(img, disc, out + "/back.png")


def hsb(out):
    img, disc = card(11, (0.07, 0.07, 0.08))
    hue = np.mod(-TH / (2 * math.pi) + 0.25, 1.0)
    r_ring0, r_ring1, r_face = 0.60, 0.83, 0.54
    d_ring = np.minimum(R - r_ring0, r_ring1 - R)
    ring_col = hsv_rgb(hue, np.ones_like(R), np.ones_like(R))
    img, _ = lay(img, d_ring, 0.04, ring_col, spec=0.55, shine=30, amb=0.55)
    # the face: saturation out from a white centre, as a picker's wheel draws it
    d_face = r_face - R
    s = np.clip(R / r_face, 0, 1) ** 0.9
    face_col = hsv_rgb(hue, s, np.full_like(R, 0.97))
    fm = cov(d_face)
    face = lit(bevel(d_face, 0.06, 0.03), face_col, spec=0.35, shine=25, amb=0.7)
    img = img * shadow(fm, blur=6, strength=0.5)[..., None]
    img = img * (1 - fm)[..., None] + face * fm[..., None]
    # a glassy dome highlight over the face
    hl = np.exp(-(((X + 0.16) / 0.30) ** 2 + ((Y + 0.24) / 0.17) ** 2)) * 0.35 * fm
    img = np.clip(img + hl[..., None], 0, 1)
    # the selector: a white ring on the ring of hues, at the picker's default (210°)
    sa = -(210 / 360 - 0.25) * 2 * math.pi
    sx, sy = 0.715 * math.cos(sa), 0.715 * math.sin(sa)
    ds = np.hypot(X - sx, Y - sy)
    d_sel = np.minimum(ds - 0.075, 0.115 - ds)
    sel = lit(bevel(d_sel, 0.02, 0.02), np.array([0.98, 0.98, 0.99], np.float32) * np.ones_like(img),
              spec=0.8, shine=50, amb=0.6)
    img = img * shadow(cov(0.115 - ds), blur=5, strength=0.55)[..., None]
    sm = cov(d_sel)
    img = img * (1 - sm)[..., None] + sel * sm[..., None]
    save(img, disc, out + "/hsb.png")


if __name__ == "__main__":
    out = sys.argv[1]
    for f in (settings, back, hsb):
        f(out)
        print("drew", f.__name__)
