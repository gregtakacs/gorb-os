#!/usr/bin/env python3
"""The reference side of the composer''s in-browser .orbtheme baker.

    /tmp/bakevenv/bin/python3 tools/watch-composer/_selftest/orbtheme_vectors.py [dir]

Packs one sentinel model with the Python that DEFINES the format (tools/orbtheme.py,
tools/theme_schema.py, tools/bake_clock.py) and leaves, in <dir>: every input plane as raw
RGBA bytes, geom.bin / art.bin / container.bin, and units.json describing the model.
headless-orbtheme.test.js feeds those same planes to tools/watch-composer/orbtheme-baker.js
and requires the geometry blob, the pixel bank and the container to match byte for byte.

Why a mirror test and not just a reader test: the format now has three implementations --
the Python, the device reader in main/art/orb_theme.c (which tools/host_sd_test.py pins),
and this browser writer. Nothing compiles the third against the first, and a field
reordered on one side does not crash: it draws a dial whose hand pivots read as overlay
offsets, which looks like bad art and not like a bug.

Pixels are dumped as raw RGBA rather than PNG on purpose: the JS side must not carry a PNG
decoder, and round-tripping a canvas through a lossless PNG and back can lose the colour
stored under a partly transparent texel -- exactly the texels bake_clock.py''s planar-alpha
rule keeps on purpose ("alpha is IGNORED here", rgb565_le).
"""
import json, os, shutil, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))        # the repo root
sys.path.insert(0, os.path.join(ROOT, "tools"))
from PIL import Image
import bake_clock as bc
import orbtheme as ot
import theme_schema as ts

W = H = bc.SCREEN_W


def plane(w, h, f):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = f(x, y)
    return img


def bg_texel(shift):
    """A gradient that changes everywhere (the packer must cluster it), a moving bar (a
    dense band it must box), and a one-in-nine flicker (the dither the snap erases)."""
    def f(x, y):
        base = ((x * 3 + y * 5) & 0xFF, (y * 11) & 0xFF, ((x + y) * 7) & 0xFF, 255)
        if 120 <= y + shift * 8 < 160 and 90 <= x < 300:
            return (255, 240, 30, 255)
        if (x + y + shift) % 9 == 0:
            return ((base[0] + 40) & 0xFF, base[1], base[2], 255)
        return base
    return f


def sprite_texel(seed):
    def f(x, y):
        a = 0 if (x < 3 or y < 4 or (x + y) % 13 == 0) else (x * 9 + y * 3 + seed) & 0xFF
        return ((x * 5 + seed) & 0xFF, (y * 11 + seed) & 0xFF, ((x ^ y) * 3) & 0xFF, a)
    return f


def dump(img, path):
    open(path, "wb").write(img.tobytes() if hasattr(img, "tobytes") else img)
    return os.path.basename(path)


def build_units(out):
    """Pack the sentinel model, in bake_clock.py's blob order: bank keyframe, delta rects,
    overlay, each hand with its shadow, the carriage bank, the window tiles, the pointers.
    Every offset the geometry carries is therefore an offset the JS packer has to reproduce,
    not merely a value it agrees with."""
    blob = bc.Blob()
    IMGS = {}                                          # the planes, kept for the packers
    m = {"slug": "mirror-check", "title": "Mirror — a theme with a long display name",
         "serial": 41, "bakedMs": 1600000000456 & 0xFFFFFFFF, "scrW": W, "scrH": H,
         "pad": 2, "fps": 8, "diffThresh": 12, "denoise": 2, "maxRects": 16, "maxFlush": 0.62}

    frames = [plane(W, H, bg_texel(k)) for k in range(4)]
    m["frames"] = [dump(im, os.path.join(out, "frame_%d.rgba" % k)) for k, im in enumerate(frames)]
    gif = bc.pack_bg_bank(blob, frames, [125, 125, 125, 125], 12, denoise=2,
                          max_rects=16, max_flush=0.62, src="units", kind="frames")
    m["bank"] = {"frames": gif["frames"], "keyOff": gif["key_off"],
                 "rects": [v for r in gif["rects"] for v in r], "first": list(gif["first"]),
                 "delays": list(gif["delays"])}

    def spec(name, w, h, seed, pivot, centre, blend=0, show=1):
        im = plane(w, h, sprite_texel(seed))
        f = dump(im, os.path.join(out, name + ".rgba"))
        IMGS[f] = im
        return {"file": f, "w": w, "h": h, "pivot": pivot, "centre": centre,
                "blend": blend, "show": show}

    def pack_sprite(s):
        im = IMGS[s["file"]]
        crop, ox, oy = bc.crop_layer(im, m["pad"])
        px, py = s["pivot"] if s["pivot"] else (ox, oy)
        cx, cy = s["centre"] if s["centre"] else (ox, oy)
        if crop is None:
            return bc.blank_sprite(cx, cy, 0)
        return bc.emit_sprite(blob, crop, ox, oy, px, py, cx, cy, s["blend"], s["show"])

    m["overlaySpec"] = spec("overlay", 220, 190, 3, None, None)
    m["handSpecs"] = [spec("hand_%d" % i, w, h, 10 + i, [w // 2, h // 2], [233, 233])
                      for i, (w, h) in enumerate([(120, 40), (60, 210), (90, 30), (400, 400), (12, 9)])]
    m["shadowSpecs"] = [spec("shadow_%d" % i, w + 6, h + 6, 40 + i, [w // 2 + 3, h // 2 + 3],
                             [233 + i, 233 - 3 - i])
                        for i, (w, h) in enumerate([(120, 40), (60, 210), (90, 30), (400, 400), (12, 9)])]
    m["overlayRec"] = pack_sprite(m["overlaySpec"])
    m["handRecs"] = [pack_sprite(s) for s in m["handSpecs"]]
    m["shadowRecs"] = [pack_sprite(s) for s in m["shadowSpecs"]]

    tb = [plane(W, H, sprite_texel(70 + k)) for k in range(3)]
    m["tourbillon"] = [dump(im, os.path.join(out, "tb_%d.rgba" % k)) for k, im in enumerate(tb)]
    m["tourbillonRecs"] = bc.pack_tourbillon(blob, tb, 233, 233, 233, 233, m["pad"], 12, 8)

    tiles = [plane(48 + k, 22, sprite_texel(90 + k)) for k in range(6)]
    m["tiles"] = [{"file": dump(t, os.path.join(out, "tile_%d.rgba" % k)), "w": t.size[0],
                   "h": t.size[1], "value": n}
                  for k, (t, n) in enumerate(zip(tiles, ["MON", "TUE", "WED", "THU", "FRI", "SAT"]))]
    offs = [bc._pack_window_tile(blob, t) for t in tiles]
    comp = {n: 0 for n, _ in ts.COMP_FIELDS}
    comp.update(tag=2, kind=ts.kind_of("window"), src=ts.source_of("day"), x=199, y=133,
                w=tiles[0].size[0], h=tiles[0].size[1], count=len(tiles), first=0, off=offs[0],
                frame_bytes=ts.window_bytes({"w": tiles[0].size[0], "h": tiles[0].size[1], "flags": 0}),
                value_min=0, scale=16, blend=0, show=1, flags=0, reserve=0)
    m["comps"] = [comp]

    auxImg = plane(70, 26, sprite_texel(120))
    m["auxSpec"] = {"file": dump(auxImg, os.path.join(out, "aux_0.rgba")), "pivot": [233, 233],
                    "centre": [233, 233], "src": "date", "tag": 3, "host": 3, "span": 3600,
                    "zero": 120, "valueMin": 1, "valueMax": 31, "jump": True}
    acrop, aox, aoy = bc.crop_layer(auxImg, m["pad"])
    aspr = bc.emit_sprite(blob, acrop, aox, aoy, 233, 233, 233, 233, 0, True)
    m["auxRecs"] = [bc._aux_record(ts, aspr, m["auxSpec"], ts.SOURCES.index("date"), ts.FLAG_JUMP)]

    art = bytes(blob.buf)
    m["artFile"] = dump(art, os.path.join(out, "art.bin"))
    m["scalars"] = {
        "bg": 0x2468AC, "plate_follow": 0, "shadow_on": 1, "shadow_dx": 0, "shadow_dy": -3,
        "second_sweep": 1, "second_railway": 0, "text_over_hands": 0, "tick_rate": 8,
        "bg_frames": 0, "bg_fps": 8, "bg_loop": 0, "bg_every": 3600, "plate_off": gif["key_off"],
        "order0": 2, "order1": 3, "order2": 0, "order3": 1, "order4": 4, "order_n": 5,
        "matte_mask": 0, "bg_mode": 1, "bg_key_off": gif["key_off"],
        "gif_frames": gif["frames"], "tourbillon_n": len(m["tourbillonRecs"]), "tourbillon_fps": 8}
    geom = ot.pack_geom(m["scalars"], m["handRecs"], m["shadowRecs"], m["overlayRec"],
                        m["tourbillonRecs"], [int(v) for v in m["bank"]["rects"]],
                        [int(v) for v in m["bank"]["first"]], [int(v) for v in m["bank"]["delays"]],
                        [0], comps=m["comps"], aux=m["auxRecs"], bank=[])
    dump(geom, os.path.join(out, "geom.bin"))
    ot.write_container(os.path.join(out, "container.bin"), m["slug"], m["title"], geom, art,
                       W, H, serial=m["serial"], baked_ms=m["bakedMs"])
    m["geomHex"] = geom.hex()
    m["geomSize"], m["artSize"] = len(geom), len(art)
    m["geomCrc"], m["artCrc"] = zlib.crc32(geom) & 0xFFFFFFFF, zlib.crc32(art) & 0xFFFFFFFF
    # The layer merges (the sub-dial into the dial, the tint into the overlay) run through
    # PIL's alpha_composite, whose integer maths the JS reproduces texel for texel. Three
    # planes here: the two inputs and the expected result, over the alpha values where a
    # rounding rule shows (0, 1, 2, 127, 128, 129, 250, 254, 255).
    vals = [0, 1, 2, 127, 128, 129, 250, 254, 255]
    cw, chh = len(vals) * 3, len(vals)
    dst = Image.new("RGBA", (cw, chh))
    src = Image.new("RGBA", (cw, chh))
    for i, da in enumerate(vals):
        for j, dc in enumerate(vals):
            for k, sa in enumerate(vals):
                sc = vals[(i + j + k) % len(vals)]
                dst.putpixel((j, i), (dc, 0, 0, da))
                src.putpixel((j, i), (sc, 255 - sc, (sc * 3) & 0xFF, sa))
    got = Image.alpha_composite(dst, src)
    m["composite"] = {"w": cw, "h": chh,
                      "dst": dump(dst, os.path.join(out, "comp_dst.rgba")),
                      "src": dump(src, os.path.join(out, "comp_src.rgba")),
                      "out": dump(got, os.path.join(out, "comp_out.rgba"))}
    return m


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orbtheme_vectors"
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    json.dump(build_units(out), open(os.path.join(out, "units.json"), "w"), indent=1)
    print("wrote the reference model into " + out)


if __name__ == "__main__":
    main()

