#!/usr/bin/env python3
"""Framebuffer capture: the glass as bytes, so "it looks right" is a measurement.

Why this exists: the CO5300's GRAM is write-only over QSPI, so the panel cannot
be read back and no serial line can say what a screen looks like. The device's
/api/v1/fb door hands over the PSRAM panel mirror -- the same buffer the QSPI
bands DMA out to the glass -- copied on the LVGL task inside the commit window,
so one fetch is one frame the panel actually received. Everything this tool
prints is a claim about pixels that were displayed.

The target comes from this clone's untracked .orb-target through orb_bind, and
the identity gate is gate_ip() (an `ip neigh` MAC check) rather than the
gate_mac() the serial tools use: gate_mac OPENS the port, and opening the port
resets the board (capf.py's caveat), which would replace the screen being
measured with a boot log. A capture that resets the dial it is capturing is not
a capture.

The body is chunked, so there is no Content-Length to trust. The expected size
is read from the X-Orb-FB header the device itself computed and compared with
the bytes received: a truncated grab fails here loudly instead of reading as a
dark UI. `consistency=` is echoed too -- `commit` frames are whole; `torn` ones
admit they raced a repaint, which is the honest price of ?direct=1.

Usage:
  fbgrab.py                          bmp to /tmp/orb-fb.bmp, stats + thumbnail
  fbgrab.py --fmt raw565 --out f.bin
  fbgrab.py --direct                 copy on the HTTP task, admit the tear
  fbgrab.py --min-ink 1.5            exit 1 if the frame is essentially black
  fbgrab.py --diff a.bmp b.bmp       % pixels changed, mean/max luma delta
Exit: 0 ok, 1 a policy check failed, 2 transport or device refusal.
"""
import argparse, array, os, struct, sys, urllib.error, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind

RAMP = " .:-=+*#%@"


def parse_meta(h):
    d = {}
    for kv in h.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k] = v
    return d


def fetch(base, fmt, wait_ms, direct):
    url = "%s/api/v1/fb?fmt=%s&wait=%d" % (base, fmt, wait_ms)
    if direct:
        url += "&direct=1"
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            return parse_meta(r.headers.get("X-Orb-FB", "")), r.read()
    except urllib.error.HTTPError as e:
        sys.stderr.write("device refused %s: %s %s\n"
                         % (url, e.code, e.read()[:200].decode("utf8", "replace")))
        sys.exit(2)
    except OSError as e:
        sys.stderr.write("no answer from %s: %s\n" % (url, e))
        sys.exit(2)


def luma_rows(fmt, buf, w, h):
    """One bytearray of per-pixel luminance per row, top-down, both formats."""
    rows = []
    if fmt == "bmp":
        rs = (w * 3 + 3) & ~3
        for y in range(h):                      # BMP stores the last row first
            b0 = 54 + (h - 1 - y) * rs
            rows.append(bytearray(
                (buf[b0 + 3 * x + 2] * 77 + buf[b0 + 3 * x + 1] * 151 +
                 buf[b0 + 3 * x] * 28) >> 8 for x in range(w)))
    else:
        a = array.array("H", buf)
        for y in range(h):
            s = y * w
            lum = bytearray(w)
            for x in range(w):
                # The 5/6/5 fields must be EXPANDED to 8 bits before weighting:
                # multiplying a raw 0..31 field by 77 caps red at 9 instead of
                # 255, which read as a dial dimmed to a fifth of its brightness.
                # Only caught by capturing one frame in each format -- the bmp
                # path had just been checked against PIL at the same time.
                p = a[s + x]
                r5 = (p >> 11) & 31
                g6 = (p >> 5) & 63
                b5 = p & 31
                lum[x] = ((((r5 << 3) | (r5 >> 2)) * 77
                           + ((g6 << 2) | (g6 >> 4)) * 151
                           + ((b5 << 3) | (b5 >> 2)) * 28) >> 8) & 0xFF
            rows.append(lum)
    return rows


def thumb(rows, n):
    """Coarse luminance map: enough to see where the ink is without a viewer."""
    h, w = len(rows), len(rows[0])
    sy, sx = h / n, w / n
    out = []
    for cy in range(n):
        line = ""
        for cx in range(n):
            y0, y1 = int(cy * sy), max(int((cy + 1) * sy), int(cy * sy) + 1)
            x0, x1 = int(cx * sx), max(int((cx + 1) * sx), int(cx * sx) + 1)
            tot = cnt = 0
            for y in range(y0, min(y1, h)):
                r = rows[y]
                tot += sum(r[x0:x1]); cnt += x1 - x0
            line += RAMP[min(int(tot / max(cnt, 1) * len(RAMP) / 256), len(RAMP) - 1)]
        out.append(line)
    return out


def stats(rows):
    tot = ink = 0
    hist = set()
    for r in rows:
        tot += len(r)
        ink += sum(1 for v in r if v > 8)
        hist.update(set(r))
    return {"px": tot, "ink": ink / tot, "mean": sum(sum(r) for r in rows) / tot,
            "levels": len(hist)}


def do_diff(fa, fb):
    def load(p):
        raw = open(p, "rb").read()
        if raw[:2] == b"BM":
            w, hh = struct.unpack_from("<ii", raw, 18)
            return "bmp", w, abs(hh), raw
        return "raw565", 466, 466, raw
    af, aw, ah, ab = load(fa)
    bf, bw, bh, bb = load(fb)
    if (aw, ah) != (bw, bh):
        sys.stderr.write("geometry differs: %dx%d vs %dx%d\n" % (aw, ah, bw, bh))
        sys.exit(2)
    ra, rb = luma_rows(af, ab, aw, ah), luma_rows(bf, bb, bw, bh)
    n = changed = worst = 0
    dsum = 0
    for y in range(ah):
        x0, x1 = ra[y], rb[y]
        for d in ((abs(a - b) for a, b in zip(x0, x1))):
            n += 1
            if d > 8: changed += 1
            dsum += d
            if d > worst: worst = d
    print("diff %s vs %s: %.2f%% pixels changed, mean delta %.1f, max %d"
          % (fa, fb, 100.0 * changed / n, dsum / n, worst))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fmt", choices=["bmp", "raw565"], default="bmp")
    ap.add_argument("--out", default="")
    ap.add_argument("--wait", type=int, default=2000)
    ap.add_argument("--direct", action="store_true")
    ap.add_argument("--thumb", type=int, default=32)
    ap.add_argument("--min-ink", type=float, default=-1.0)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"))
    a = ap.parse_args()

    if a.diff:
        return do_diff(a.diff[0], a.diff[1])

    cfg = orb_bind.load()
    base = "http://%s" % orb_bind.gate_ip(cfg)
    meta, body = fetch(base, a.fmt, a.wait, a.direct)
    if not meta:
        sys.stderr.write("no X-Orb-FB header: the door is not in this build "
                         "(CONFIG_ORB_DEBUG_HTTP off?)\n")
        return 2
    want = int(meta.get("bytes", "0"))
    w, h = int(meta.get("w", "0")), int(meta.get("h", "0"))
    if len(body) != want:
        sys.stderr.write("truncated: header says %d B, got %d B\n" % (want, len(body)))
        return 2
    out = a.out or ("/tmp/orb-fb.bmp" if a.fmt == "bmp" else "/tmp/orb-fb.bin")
    open(out, "wb").write(body)
    rows = luma_rows(a.fmt, body, w, h)
    s = stats(rows)
    if not a.quiet:
        for line in thumb(rows, a.thumb): print(line)
    print("saved %s (%s %dx%d, seq=%s, %s, %.1f KB)"
          % (out, meta["fmt"], w, h, meta.get("seq", "?"),
             meta.get("consistency", "?"), len(body) / 1024.0))
    print("ink %.2f%% of pixels, mean luma %.1f, %d distinct levels"
          % (100 * s["ink"], s["mean"], s["levels"]))
    if a.min_ink >= 0 and 100 * s["ink"] < a.min_ink:
        sys.stderr.write("FAIL: ink %.2f%% below --min-ink %.2f%%\n"
                         % (100 * s["ink"], a.min_ink))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

