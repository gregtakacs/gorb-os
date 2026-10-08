#!/usr/bin/env python3
"""Dump the structural facts about orb_bg.gif that decide the bake pipeline.

Pure stdlib (no PIL/numpy): we only need per-frame descriptors (full-frame vs
partial rect, disposal method, delay, local color table), not decoded pixels.
This tells us whether an on-device simple decoder can just blit each frame, or
whether frames are diffs that must be composited onto a persistent canvas.
"""
import sys
from struct import unpack_from


def walk(path):
    d = open(path, "rb").read()
    print(f"file: {path}  size={len(d)} bytes  sig={d[:6]!r}")

    # --- Logical Screen Descriptor (offset 6, 7 bytes after the 6-byte sig) ---
    lw, lh = unpack_from("<HH", d, 6)
    packed = d[10]
    has_gct = bool(packed & 0x80)
    gct_size = 2 << (packed & 0x07)
    print(f"logical screen: {lw}x{lh}  global_color_table={has_gct} ({gct_size} entries)")

    # Skip the global color table if present (3 bytes per entry).
    i = 13
    if has_gct:
        i += gct_size * 3

    frames = []
    delays = []
    while i < len(d):
        b = d[i]
        if b == 0x3B:  # trailer
            break
        if b == 0x21:  # extension introducer
            label = d[i + 1]
            if label == 0xF9:  # Graphics Control Extension
                # layout: 21 F9 <blockSize=4> <packed> <delay:2> <transIdx> <term>
                block_size = d[i + 2]
                gce_packed = d[i + 3]
                delay_cs = unpack_from("<H", d, i + 4)[0]
                disposal = (gce_packed >> 2) & 0x07
                has_transparency = bool(gce_packed & 0x01)
                delays.append((delay_cs, disposal, has_transparency))
                i += 3 + block_size          # skip to just past the data bytes
                i = skip_sub_blocks(d, i)    # trailing terminator (may be more)
            else:
                i += 2
                i = skip_sub_blocks(d, i)
        elif b == 0x2C:  # Image Descriptor
            # layout: 2C <left:2> <top:2> <width:2> <height:2> <packed>  = 10 bytes
            x, y, w, h = unpack_from("<HHHH", d, i + 1)
            loc = d[i + 9]
            has_lct = bool(loc & 0x80)
            interlace = bool(loc & 0x40)
            lct_size = (2 << (loc & 0x07)) if has_lct else 0
            full = (w == lw and h == lh)
            gce = delays[-1] if len(delays) == len(frames) + 1 else None
            frames.append(dict(x=x, y=y, w=w, h=h, lct=has_lct,
                               interlace=interlace, full=full, gce=gce))
            i += 10
            if has_lct:
                i += lct_size * 3
            i += 1  # LZW min code size
            i = skip_sub_blocks(d, i)  # image data sub-blocks
        else:
            i += 1

    print(f"\ndecoded {len(frames)} frames:")
    for n, fr in enumerate(frames):
        gce = fr["gce"]
        if gce:
            delay_cs, disposal, transp = gce
            delay_ms = delay_cs * 10
        else:
            delay_ms, disposal, transp = 0, 0, False
        tag = "FULL" if fr["full"] else f"PARTIAL@({fr['x']},{fr['y']})"
        print(f"  frame {n:2d}: {fr['w']}x{fr['h']} {tag:16s} "
              f"delay={delay_ms:4d}ms disposal={disposal} "
              f"local_cmap={fr['lct']} interlace={fr['interlace']} "
              f"transparency={transp}")

    # ---- The verdict that drives the pipeline ----
    all_full = all(f["full"] for f in frames)
    any_partial = any(not f["full"] for f in frames)
    any_dispose_bg = any((f["gce"][1] == 2) for f in frames if f["gce"])
    print("\n=== pipeline implications ===")
    print(f"  all frames full-screen: {all_full}")
    print(f"  any partial/diff frames: {any_partial}")
    print(f"  any dispose-to-bg(2):    {any_dispose_bg}")
    if any_partial:
        print("  -> on-device SIMPLE blit-per-frame is WRONG; partial frames must "
              "be composited onto a persistent canvas with disposal handling. "
              "Strongly favors OFFLINE compositing to full frames.")


def skip_sub_blocks(d, i):
    """Walk GIF sub-blocks: count byte, then that many bytes, until 0 count."""
    while i < len(d):
        cnt = d[i]
        i += 1
        if cnt == 0:
            break
        i += cnt
    return i


if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else "assets/orb_bg.gif"
    walk(p)
