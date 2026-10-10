#!/usr/bin/env python3
"""Predict a baked GIF delta bank's per-tick flush drivers, with no flash.

Replays tools/bake_clock.py's bake_bg_bank warm/replay simulation exactly (3
warm cycles, last cycle baked, same _temporal_snap/_gif_transition/_pack_mask
defaults the shipped recipe uses) on a PNG frames dir, and reports the two
quantities the flush cost is a function of:

  pushed px   the packed delta-rect area the device memcpy's + force-marks
  changed px  the accepted diff mask (what genuinely moves)
  rows        the row union the bands will DMA (466 px wide per row)

Trust it only rect-for-rect against the committed bank: run it on the shipped
frames dir and the fidelity line must read 8/8 identical. That is the gate the
cadence-probe session validated it against, and it is what makes a pre-flash
prediction worth the flash it saves.

Usage:
  rectaudit.py FRAMES_DIR [--fps N] [--thresh T] [--denoise E] [--tick]
  rectaudit.py FRAMES_DIR --check-committed
"""
import argparse
import importlib.util
import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(os.path.dirname(_HERE))
_spec = importlib.util.spec_from_file_location(
    "bake_clock", os.path.join(_REPO, "tools", "bake_clock.py"))
bc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bc)
import numpy as np  # noqa: E402  (comes from the bake venv)

W, H = bc.SCREEN_W, bc.SCREEN_H


def bake_rects(frames_dir, fps=8.0, thresh=12, denoise=2, frame_glob=None):
    """-> (per_frame, n) with per_frame[f] = (rects, changed_mask) for the LAST
    warm cycle, i.e. exactly what bake_bg_bank writes into the blob."""
    imgs, delays = bc._load_png_frames(frames_dir, frame_glob=frame_glob,
                                       fps=fps, fit="cover")
    F = [bc._frames_to_u16(im) for im in imgs]
    F, quiet = bc._temporal_snap(F, denoise)
    n = len(F)
    canvas = F[0].copy()
    per = {}
    for warm in range(3):
        painted = np.zeros(canvas.shape, dtype=bool)
        per = {}
        for k in range(n):
            tgt_i = (k + 1) % n
            lst, mask = bc._gif_transition(canvas, F[tgt_i], painted, thresh)
            for x, y, w, h in lst:
                canvas[y:y + h, x:x + w] = F[tgt_i][y:y + h, x:x + w]
                painted[y:y + h, x:x + w] = True
            per[tgt_i] = (lst, mask)
    return per, n


def committed_rects(header=os.path.join(_REPO, "assets", "clock_assets.h")):
    """-> list of per-frame rect lists as recorded in the shipped bank header."""
    hdr = open(header).read()
    i = hdr.index("clk_bg_rects[")
    blk = hdr[i:hdr.index("};", i)]
    flat = [tuple(int(v) for v in m) for m in
            re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*\d+\s*\}", blk)]
    fi = hdr.index("clk_bg_rect_first[")
    blk_f = hdr[hdr.index("{", fi):hdr.index("}", fi)]   # skip the "[N]" arity
    firsts = [int(x) for x in re.findall(r"\d+", blk_f)]
    n = len(firsts) - 1
    return [flat[firsts[j]:firsts[j + 1]] for j in range(n)]


def row_union(per, n):
    rows = set()
    for f in sorted(per):
        for x, y, w, h in per[f][0]:
            rows.update(range(y, y + h))
    return rows


def report(frames_dir, fps, thresh, denoise, frame_glob, check_committed):
    per, n = bake_rects(frames_dir, fps=fps, thresh=thresh, denoise=denoise,
                        frame_glob=frame_glob)
    push = sum(w * h for f in per for x, y, w, h in per[f][0]) // n
    chg = sum(int(per[f][1].sum()) if per[f][1] is not None else 0
              for f in per) // n
    rows = row_union(per, n)
    per_frame_rows = []
    for f in sorted(per):
        r = set()
        for x, y, w, h in per[f][0]:
            r.update(range(y, y + h))
        per_frame_rows.append(len(r))
    print(f"{n} frames from {frames_dir}: pushed {push} px/tick, "
          f"changed {chg} px/tick, over-cover {push / max(1, chg):.2f}")
    print(f"row union: {len(rows)} rows"
          f"{' [' + str(min(rows)) + '..' + str(max(rows)) + ']' if rows else ''}, "
          f"full-width DMA {len(rows) * W} px/tick")
    print(f"per-frame rows: {per_frame_rows}")
    if check_committed:
        cr = committed_rects()
        mine = [tuple(sorted(per[f][0])) for f in sorted(per)]
        ship = [tuple(sorted(t)) for t in cr]
        exact = sum(a == b for a, b in zip(mine, ship))
        rot = max(sum(a == b[s % len(b)] for a, b in zip(mine, ship))
                  for s in range(len(ship)))
        sp = sum(w * h for t in ship for x, y, w, h in t) // len(ship)
        print(f"fidelity vs committed bank: {exact}/{len(ship)} transitions "
              f"identical (best rotation {rot}/{len(ship)}), "
              f"committed push {sp} px/tick "
              f"{'MATCH' if exact == len(ship) else 'DIVERGENT'}")
        return 0 if exact == len(ship) else 1
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("frames_dir")
    ap.add_argument("--fps", type=float, default=8.0)
    ap.add_argument("--thresh", type=int, default=12)
    ap.add_argument("--denoise", type=int, default=2)
    ap.add_argument("--bg-glob", default=None)
    ap.add_argument("--check-committed", action="store_true",
                    help="fail (exit 1) unless the replay is rect-for-rect "
                         "identical to assets/clock_assets.h's bank")
    a = ap.parse_args()
    raise SystemExit(report(a.frames_dir, a.fps, a.thresh, a.denoise,
                            a.bg_glob, a.check_committed))
