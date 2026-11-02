#!/usr/bin/env python3
"""orbdither.py — the bake's RGB565 dither (D43 §5.3, §5.4), the Python twin.

The JS baker (tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js, `dither565` / `ditherAlpha`) is the
one that ships themes; this module is its bit-exact twin for the parity suite
(`_selftest/headless-dither.test.js`) and for any Python tool that writes a still plane
the dial blends. Both read the same 64x64 blue-noise table (generated once by
make_bluenoise.py) and both are pinned to TABLE_SHA256.

Colour: per channel, `level = base[v] + (frac[v] > T(x, y))`. base[v] is the highest
level whose device decode is <= v, frac[v] where v sits between that decode and the
next, scaled to 0..255. A value whose decode is exact has frac 0 and never moves, so a
flat 565-exact colour comes through untouched; the expected decode equals v, so the
truncation's darkening goes too. T is the table tiled across the PANEL: (x, y) are
absolute panel coordinates, so an unchanged input texel bakes to an unchanged output
texel in every keyframe and every delta crop.

Alpha: a still row's coverage is dithered onto the device's own lattice. mix_16_16 reads
mix5 = (a + 4) >> 3, 0..32; the plane stores m5 * 8 (255 for 32), so the device decodes
exactly the step chosen. Its threshold is the table shifted by (32, 32), so the alpha
dither does not correlate with the colour dither of the same plane.

Neither function touches truncation's callers: bake_clock.py's rgb565_le stays the
FORMAT 2 packer the parity vectors pin. Dithering is the caller's choice, per plane.
"""
import hashlib
import base64

N = 64
TABLE_B64 = (
    "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"
)
TABLE = base64.b64decode(TABLE_B64)
TABLE_SHA256 = "683a759b831dc8d0c83205827301e65adbb2d06f6b5b11af800a694f6f8f2bc7"
ALPHA_SHIFT = 32


def _decode(level, bits):
    return (level << (8 - bits)) | (level >> (2 * bits - 8))


def _luts(bits):
    dec = [_decode(l, bits) for l in range(1 << bits)]
    base, frac = [0] * 256, [0] * 256
    l = 0
    for v in range(256):
        while l + 1 < len(dec) and dec[l + 1] <= v:
            l += 1
        base[v] = l
        if l + 1 < len(dec) and v > dec[l]:
            frac[v] = (256 * (v - dec[l])) // (dec[l + 1] - dec[l])
    return base, frac


BASE5, FRAC5 = _luts(5)
BASE6, FRAC6 = _luts(6)


def threshold(x, y):
    return TABLE[(y & (N - 1)) * N + (x & (N - 1))]


def quant565(r, g, b, t):
    """One texel's RGB565 code at threshold t (0..255)."""
    return (((BASE5[r] + (FRAC5[r] > t)) << 11) |
            ((BASE6[g] + (FRAC6[g] > t)) << 5) |
            (BASE5[b] + (FRAC5[b] > t)))


def dither565(rgba, w, h, ox=0, oy=0):
    """RGBA bytes (w*h*4) -> native-LE RGB565 bytes (w*h*2); (ox, oy) is the plane's
    top-left on the panel. Alpha is ignored here, as rgb565_le ignores it."""
    out = bytearray(w * h * 2)
    o = 0
    for y in range(h):
        row = ((oy + y) & (N - 1)) * N
        for x in range(w):
            i = (y * w + x) * 4
            t = TABLE[row + ((ox + x) & (N - 1))]
            v = quant565(rgba[i], rgba[i + 1], rgba[i + 2], t)
            out[o] = v & 0xFF
            out[o + 1] = v >> 8
            o += 2
    return bytes(out)


def alpha_step(a, t):
    """One coverage byte on the mix5 lattice at threshold t."""
    if a == 0 or a == 255:
        return a
    exact = a * 32
    lo = exact // 255
    frac = ((exact - lo * 255) * 256) // 255
    m5 = min(lo + (1 if frac > t else 0), 32)
    return 255 if m5 >= 32 else m5 * 8


def dither_alpha(rgba, w, h, ox=0, oy=0):
    """RGBA bytes -> the planar A8 plane, each byte on the device's 32-step lattice."""
    out = bytearray(w * h)
    for y in range(h):
        row = ((oy + y + ALPHA_SHIFT) & (N - 1)) * N
        for x in range(w):
            out[y * w + x] = alpha_step(rgba[(y * w + x) * 4 + 3],
                                        TABLE[row + ((ox + x + ALPHA_SHIFT) & (N - 1))])
    return bytes(out)


def table_ok():
    return hashlib.sha256(TABLE).hexdigest() == TABLE_SHA256


if __name__ == "__main__":
    print("table sha256 ok" if table_ok() else "TABLE MISMATCH")
