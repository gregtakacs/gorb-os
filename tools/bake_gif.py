#!/usr/bin/env python3
"""
bake_gif.py — offline GIF -> raw RGB565 frame-bank baker for the orb bench.

Why offline (not an on-device GIF decoder):
  * orb_bg.gif's 20 frames are FULL-screen, all with an alpha transparency index
    and disposal=1 ("do not dispose"). Frame N is a transparent LAYER meant to sit
    on top of frames 0..N-1. A naive per-frame blit (what a simple on-device
    decoder does) renders each frame's transparent pixels as empty, so the orb
    falls apart. We composite on a persistent canvas honouring the disposal method
    per GIF89a, then bake finished full frames.
  * Raw RGB565 is 466*466*2 = 434,312 B/frame; x20 ~ 8.3 MB. Too big for the app
    partition's 1.5 MB and to sit in 8 MB PSRAM with everything else, and GDMA
    can't source pixels from PSRAM/flash without a bounce. So the frames become a
    flash-resident blob that LVGL reads with the CPU and re-flushes via the
    already-fixed partial path (byte-swap + even-align + TE-gated DMA).

Byte order: pixels are emitted as uint16 in the SAME little-endian native order
LVGL uses for LV_COLOR_DEPTH=16, so the on-device flush treats them identically to
anything LVGL rendered itself (including the RGB565 byte-swap fix).

Usage:
  python3 tools/bake_gif.py assets/orb_bg.gif [--mode cover|fit] \
      [--preview-dir assets/preview]
"""
import argparse
import os
import sys
from PIL import Image

GIF_BG = (0, 0, 0)  # true-black: invisible on the AMOLED, matches the clock bg


def decode_frame_rgba(im, i):
    """RGBA image for frame i, transparency index folded into alpha.

    Pillow >=5.3 applies im.info['transparency'] (a palette index) to the alpha
    channel automatically on convert('RGBA') from 'P' mode, so no manual mask is
    needed. We re-open fresh per frame to keep PIL's seek state predictable.
    """
    im.seek(i)
    return im.convert("RGBA")


def scale_to_panel(rgba_img, out_w, out_h, mode):
    """500x500 -> out_w x out_h: 'cover' (uniform-fill + centre-crop, so the flush
    rectangle is fully painted every frame = true full-page refresh), or 'fit'
    (contain + letterbox on black)."""
    iw, ih = rgba_img.size
    if mode == "cover":
        scale = max(out_w / iw, out_h / ih)
        nw, nh = int(round(iw * scale)), int(round(ih * scale))
        r = rgba_img.resize((nw, nh), Image.LANCZOS)
        left, top = (nw - out_w) // 2, (nh - out_h) // 2
        return r.crop((left, top, left + out_w, top + out_h))
    scale = min(out_w / iw, out_h / ih)
    nw, nh = int(round(iw * scale)), int(round(ih * scale))
    r = rgba_img.resize((nw, nh), Image.LANCZOS)
    out = Image.new("RGBA", (out_w, out_h), GIF_BG + (255,))
    out.paste(r, ((out_w - nw) // 2, (out_h - nh) // 2), r)
    return out


def rgba_to_rgb565_le(rgba_img):
    """Flatten alpha over black, then pack RGB565 as little-endian uint16 bytes
    (low byte first) — the in-memory order LVGL uses for LV_COLOR_DEPTH=16."""
    flat = Image.new("RGB", rgba_img.size, GIF_BG)
    flat.paste(rgba_img, (0, 0), rgba_img)  # third arg = alpha mask
    w, h = flat.size
    px = flat.load()
    out = bytearray(w * h * 2)
    o = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[o] = v & 0xFF
            out[o + 1] = (v >> 8) & 0xFF
            o += 2
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif", nargs="?", default="assets/orb_bg.gif")
    ap.add_argument("--bin", default="assets/orb_bg_565.bin")
    ap.add_argument("--header", default="assets/orb_bg_assets.h")
    ap.add_argument("--width", type=int, default=466)
    ap.add_argument("--height", type=int, default=466)
    ap.add_argument("--mode", choices=["cover", "fit"], default="cover")
    ap.add_argument("--preview-dir", default=None,
                    help="also dump composited RGBA PNGs for eyeball QA")
    args = ap.parse_args()

    if not os.path.exists(args.gif):
        sys.exit(f"input gif not found: {args.gif}")

    im = Image.open(args.gif)
    n = getattr(im, "n_frames", 1)
    print(f"source: {im.size[0]}x{im.size[1]}  frames={n}  mode={args.mode}")

    # Disposal methods read once, up front, from each frame's info block.
    disposals = []
    for i in range(n):
        im.seek(i)
        disposals.append(im.info.get("disposal", 0))

    logical = im.size
    canvas = Image.new("RGBA", logical, GIF_BG + (255,))
    frame_bytes = args.width * args.height * 2
    bank = bytearray()
    delays = []
    if args.preview_dir:
        os.makedirs(args.preview_dir, exist_ok=True)

    for i in range(n):
        disposal = disposals[i]
        backup = canvas.copy()                 # for disposal=3 (restore previous)
        frame = decode_frame_rgba(im, i)
        canvas.paste(frame, (0, 0), frame)      # transparent px keep the canvas
        delays.append(im.info.get("duration", 40))

        scaled = scale_to_panel(canvas, args.width, args.height, args.mode)
        bank += rgba_to_rgb565_le(scaled)
        if args.preview_dir:
            scaled.save(os.path.join(args.preview_dir, f"frame_{i:02d}.png"))

        if disposal == 2:                        # restore to background
            canvas = Image.new("RGBA", logical, GIF_BG + (255,))
        elif disposal == 3:                      # restore to previous canvas
            canvas = backup
        # disposal 0/1: keep the accumulated canvas
        print(f"  frame {i:2d}  disposal={disposal}  delay={delays[-1]}ms")

    assert len(bank) == frame_bytes * n, f"bank {len(bank)} != {frame_bytes}*{n}"
    with open(args.bin, "wb") as f:
        f.write(bank)

    with open(args.header, "w") as f:
        f.write(
            "/* AUTO-GENERATED by tools/bake_gif.py — DO NOT EDIT.\n"
            " * Re-bake:  python3 tools/bake_gif.py assets/orb_bg.gif\n"
            " */\n#pragma once\n\n"
            f"#define ORB_BG_FRAME_W     {args.width}\n"
            f"#define ORB_BG_FRAME_H     {args.height}\n"
            f"#define ORB_BG_FRAME_COUNT {n}\n"
            "#define ORB_BG_FRAME_BYTES (ORB_BG_FRAME_W * ORB_BG_FRAME_H * 2)\n"
            "#define ORB_BG_TOTAL_BYTES (ORB_BG_FRAME_BYTES * ORB_BG_FRAME_COUNT)\n\n"
            "/* Per-frame delay (ms) captured from the GIF. */\n"
            f"static const uint16_t orb_bg_delays[{n}] = {{ "
            + ", ".join(str(d) for d in delays) + " };\n\n"
            "/* Symbols from target_add_binary_data(... orb_bg_565.bin BINARY). */\n"
            "extern const uint8_t orb_bg_565_bin_start[] asm(\"_binary_orb_bg_565_bin_start\");\n"
            "extern const uint8_t orb_bg_565_bin_end[]   asm(\"_binary_orb_bg_565_bin_end\");\n"
        )

    print(f"\nwrote {args.bin} ({len(bank)} B, {n} frames)")
    print(f"wrote {args.header}")
    print("next: enlarge the factory app partition (>= ORB_BG_TOTAL_BYTES + headroom) "
          "and wire target_add_binary_data() into main/CMakeLists.txt")


if __name__ == "__main__":
    main()
