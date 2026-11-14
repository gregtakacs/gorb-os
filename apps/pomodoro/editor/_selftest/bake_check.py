#!/usr/bin/env python3
"""The pomodoro editor's bake, checked in real Chrome (docs/specs/pomodoro-editor-design.md §6).

    python3 apps/pomodoro/editor/_selftest/bake_check.py

Stages bake.html beside the runtime's baker and fonts and this app's bake, runs it headless,
and checks every theme it prints: the rows apps/pomodoro/main.c finds (one plate at slot 0 —
the ground with the look's instrument folded into it — fonts
0..2, the labels at 8..12, the dots as two picked rows at the one place — 13 the track, twelve
hollow frames one per cell count, and 14 the fill, thirteen frames of n filled cells over it,
frame 0 empty — three one-shot chimes at slots
1, 2 and 3, sixteen bytes of pm_values_t at version 1 whose chime reads 1,2,3), the two rules
the looks design can be held to — the furniture inside radius 200 and the ten figures of one
width and centred — that nothing the projector paints above the labels stands over a label's
own box, the plate's own offset in plate_off, the design region carried with its
overlay field, the room, and the shipped projector (orb_clock3.c under --dump3f) accepting the
lot. It also checks what no single theme can: that the nine look differently from each other,
that hiding a part changes the furniture, that the same design baked twice is the same bytes,
and that an owner's uploaded wave survives the round trip unchanged and audible. A second pass
runs the five refusals alone (?only=), so each is proven on its own.

Rule 4 and the furniture's own digest come off the R4 lines, measured in the page off the
plane the bake drew, not off the baked plate: once the instrument is folded into an opaque
plate its alpha is gone and the plane's ink cannot be read back out of RGB565. The stacking
guard is the byte check that replaces it: the stage sorts a PLATE row above every other row
(main/stage/orb_stage.c's 0xFFFF, CLK_ROLE_GLASS, user data 0x10000), so Station's split-flap
furniture baked as a plate 1 painted over its own digits and the theme read as blank glass —
and the preview, which flattens ground then furniture then words, looked right the whole time.

POMODORO_BAKE_KEEP=DIR keeps the .orbtheme files; POMODORO_EVIDENCE=DIR also keeps the PNGs and
the WAVs the page prints (looks/ and wavs/), which is the run the looks are judged by eye from.
"""
import base64
import json
import os
import re
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402
import host_clock3_test as H3   # noqa: E402
import orbchrome as oc          # noqa: E402

STAGE_NAME, PROFILE_NAME = "pomodoro-bake-check", "pomodoro-bake-profile"
RUNTIME = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
EDITOR = os.path.dirname(HERE)
APPV = open(os.path.join(EDITOR, "..", "version.txt")).read().strip()
VALUES_B = 16                                        # pomodoro.h pm_values_t
# The dots are two picked rows drawn at the one place (alerts-0 amendment 2): row 13 the
# track, one frame per cell count N = 1..12 of N hollow cells, and row 14 the fill, one
# frame per count n = 0..12 of n filled cells over them, frame 0 empty. Both banks are
# left-aligned on the twelve-seat canvas (eleven of 11 px at a 6 px gap plus the twelfth);
# the app centres the pair with one shared pose offset off apps/pomodoro/pomodoro.c's
# pm_dots, and its PM_DOT_PITCH must equal this bake's cell plus gap.
DOT_N_MAX, DOT_SZ, DOT_GAP = 12, 11, 6
N_TRACK, N_FILL = DOT_N_MAX, DOT_N_MAX + 1
DOT_INK = 128                                        # at a cell's centre this is ink, below it hollow


def pm_dot_pitch():
    """apps/pomodoro/pomodoro.h's PM_DOT_PITCH, read off the header the app builds with."""
    src = open(os.path.join(ROOT, "apps", "pomodoro", "pomodoro.h")).read()
    m = re.search(r"#define\s+PM_DOT_PITCH\s+(\d+)", src)
    return int(m.group(1)) if m else None


# A theme's room, radar's number (docs/theme-cache-plan.md C4): the pool is a cache, so one
# theme need only fit alone, beside the most a voice bank may hold (1 MB) and 512 KB of slack.
BUDGET = (8 << 20) - (1 << 20) - 2 * 4096 - (512 << 10)
OVL_TAG = 0x01000000
NAMES = ["scope", "paper", "brass", "blueprint", "station", "celestial", "silver", "sectional", "blank"]
# the ring colours pomodoro-looks.md's table gives each look, and the overlay it defaults to
RING = {
    "scope": ("#4be38a", "#e0b040", "#5ad1e6"), "paper": ("#c8401e", "#5e8c4a", "#2f5b8c"),
    "brass": ("#e0a43c", "#4fa38a", "#3a6ea5"), "blueprint": ("#ffe07a", "#eaf4ff", "#8ec5ff"),
    "station": ("#e3000f", "#1e8e3e", "#1f4e8c"), "celestial": ("#ffd27a", "#b9a8ff", "#62d2c6"),
    "silver": ("#7fc8ff", "#d6dce4", "#5b7da8"), "sectional": ("#c2187a", "#2e7d32", "#2a5db0"),
    "blank": ("#d62828", "#0fa36b", "#2e93e4"),
}
NEW_DEFAULTS = (0xD62828, 0x0FA36B, 0x2E93E4)        # the values' own defaults: New's colours
EXPECT_OVL = {"scope": OVL_TAG | 0x4BE38A, "ring-set": OVL_TAG | 0x135C79}
REFUSALS = {                                         # each refusal, and the words it must say
    "bad-look": "no such look",
    "bad-px": "64..144", "low-px": "64..144",
    "bad-chime": "no such recipe",
    "no-upload": "names upload",
    "long-upload": "at most 4.0",
}
FONT_ORDER = ts.FONT_CHARSET
DIGITS = [FONT_ORDER.index(c) for c in "0123456789"]
ALPHA_INK = 4                                        # below this an edge is antialiasing, not ink


def stage(stage_dir):
    for src in (os.path.join(HERE, "bake.html"), os.path.join(EDITOR, "pomodoro-bake.js"),
                os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"),
                os.path.join(RUNTIME, "vendor", "fonts.css")):
        shutil.copy(src, stage_dir)
    shutil.copytree(os.path.join(RUNTIME, "vendor", "fonts"), os.path.join(stage_dir, "fonts"))


def run_chrome(chrome, stage_dir, profile, query=""):
    url = oc.file_url(os.path.join(stage_dir, "bake.html"), chrome) + "?appv=" + APPV + query
    return oc.run_headless(chrome, url, profile, 900, extra=["--virtual-time-budget=600000"]).stdout


def rows(els, cls):
    return [e for e in els if int(e["class"]) == cls]


def stack_key(e):
    """How high one row stands in the projector's stack, off its table entry. A PLATE rides
    at the top whatever its order says — main/stage/orb_stage.c sorts it with pick_ord 0xFFFF
    (:2022), builds it as CLK_ROLE_GLASS (:2091) and tags it 0x10000 above every widget (:2450)
    — except the ground at slot 0, which is the plane the words stand on. Everything else
    stands at depth_key(order, twin) = order * 2 + 1 (:1874), equal keys in table order."""
    if int(e["class"]) == ts.CLASS_PLATE:
        return 1 if int(e["slot"]) == 0 else 0xFFFF
    return int(e["order"]) * 2 + 1


PAINTS = (ts.CLASS_PLATE, ts.CLASS_AUX, ts.CLASS_BANK)


def stacking(sec):
    """The guard Station's blank glass teaches: no row the stack paints above the labels may
    stand over a label's own box. The furniture as a plate 1 passed every byte check of the
    old file — it was a well-formed masked panel, inside r 200, one plane, its own — and the
    preview flattens ground, furniture and words in the editor's order, so the pictures were
    right too. Only the stack knew the flaps were on top of the digits."""
    els = sec["blob"]["elements"]
    labs = [e for e in rows(els, ts.CLASS_AUX)
            if int(e["kind"]) == ts.SHAPE_TEXT and 8 <= int(e["slot"]) <= 12]
    if not labs:
        return ["no text labels to keep clear"]
    below = min(stack_key(l) for l in labs)
    bad = []
    for e in els:
        if int(e["class"]) not in PAINTS or stack_key(e) <= below:
            continue
        x, y, w, h = (int(e[k]) for k in ("x", "y", "w", "h"))
        for l in labs:
            lx, ly, lw, lh = (int(l[k]) for k in ("x", "y", "w", "h"))
            if x < lx + lw and lx < x + w and y < ly + lh and ly < y + h:
                bad.append("the row at slot %d (class %d, order %d, key %d) paints over label %d's "
                           "box at %d,%d %dx%d: the words would stand under it"
                           % (int(e["slot"]), int(e["class"]), int(e["order"]), stack_key(e),
                              int(l["slot"]), lx, ly, lw, lh))
    return bad


def digit_cells(sec, rec):
    """Rule 3, measured off the baked font row: the ten figures' cells and their ink.
    Answers (bad, cell_w, worst_off, tabular) — every cell the row's width, every visible
    ink box centred within a pixel of the cell's centre, and the strict ink (any alpha at
    all, which is what the device's advance rule sees) reaching both cell edges."""
    bad, art = [], sec["art"]
    f0, n, w, h = int(rec["frame0"]), int(rec["n_frames"]), int(rec["w"]), int(rec["h"])
    sprs = sec["blob"]["sprites"]
    worst, tabular, seen = 0, True, 0
    for gi in DIGITS:
        if gi >= n:
            bad.append("the time font carries %d cells, not the charset's %d" % (n, len(FONT_ORDER)))
            break
        sp = sprs[f0 + gi]
        if sp["w"] != w or sp["size"] != w * h:
            bad.append("figure %r is a %dx%d cell of %d B in a %dx%d font" % (FONT_ORDER[gi], sp["w"], sp["h"], sp["size"], w, h))
            continue
        a = art[sp["off"]:sp["off"] + w * h]
        cols = [x for x in range(w) if any(a[y * w + x] for y in range(h))]
        vis = [x for x in range(w) if any(a[y * w + x] >= ALPHA_INK for y in range(h))]
        if not cols:
            bad.append("figure %r carries no ink at all" % FONT_ORDER[gi])
            continue
        seen += 1
        if cols[0] != 0 or cols[-1] != w - 1:
            tabular = False
        if vis:
            worst = max(worst, abs((vis[0] + vis[-1]) / 2 - (w - 1) / 2))
    if seen < 10:
        bad.append("only %d of the ten figures could be measured" % seen)
    if worst > 1:
        bad.append("a figure's ink centre is %.2f px off the cell's centre (rule 3 lets 1)" % worst)
    if seen == 10 and not tabular:
        bad.append("the ten figures do not fill their cells edge to edge, so they would advance by different widths")
    return bad, w, worst, tabular


def rule4(name, r4):
    """Rule 4, off the page's R4 line: the furniture plane's own ink — how far out it stands
    and how much of it there is. Measured at the source because the plane ships folded into an
    opaque plate, where its alpha no longer exists to read back."""
    if r4 is None:
        return ["the page printed no R4 line for this look: rule 4 went unmeasured"]
    rad, ink = r4[0], r4[1]
    bad = []
    if rad > 200:
        bad.append("furniture ink out to radius %d: the rim is the ring's (rule 4)" % rad)
    if not ink and name != "no-parts":
        bad.append("the look draws no furniture at all")
    return bad


def dot_cells(sec, rec, i):
    """A dots frame i, measured off the baked bytes: (bad, cells, filled, x0, off_pitch).
    The frame's twelve seats are the expected windows of a left-aligned bank — cell k covers
    columns 17k .. 17k+11 — and a bank draws its leading cells, so each window is read for
    ink (a cell is drawn when its window carries any: a look's mark may reach past its box,
    and a stroked star's miters merge neighbouring windows), and its centre pixel is read
    for ink (a filled cell is lit at its centre, a hollow one is only an outline). `x0` is
    the first ink column — a left-aligned bank starts at 0 — and `off_pitch` the worst gap
    between the drawn windows' first ink columns and the pitch. Two rules land in `bad`
    directly: no ink may stand outside the drawn windows' reach (a wrong gap leaks into the
    seats), and the frame's last ink column must land on the last drawn cell's own box."""
    bad = []
    w, h = int(rec["w"]), int(rec["h"])
    sp = sec["blob"]["sprites"][int(rec["frame0"]) + i]
    if sp["w"] != w or sp["h"] != h or sp["size"] != w * h * 3:
        return ["dots frame %d is a %dx%d sprite of %d B in a %dx%d row"
                % (i, sp["w"], sp["h"], sp["size"], w, h)], 0, 0, 0.0, 0.0
    a = sec["art"][sp["off"] + 2 * w * h: sp["off"] + 3 * w * h]     # the alpha plane, after RGB565
    cols = [x for x in range(w) if any(a[y * w + x] for y in range(h))]
    if not cols:
        return bad, 0, 0, float(w), 0.0                              # an empty frame: fill 0
    pitch = DOT_SZ + DOT_GAP
    mid = h // 2
    cells, firsts, filled = 0, [], 0
    for k in range(DOT_N_MAX):
        win = [x for x in cols if pitch * k <= x < pitch * k + DOT_SZ]
        if not win:
            break
        cells += 1
        firsts.append(win[0])
        filled += 1 if a[mid * w + (pitch * k * 2 + DOT_SZ - 1) // 2] >= DOT_INK else 0
    reach = 3                                                        # a mark's bleed past its box
    if [x for x in cols if pitch * cells - reach <= x]:
        bad.append("dots frame %d carries ink past its %d drawn cells (first at %d)"
                   % (i, cells, min(x for x in cols if pitch * cells - reach <= x)))
    if cols[-1] < pitch * (cells - 1) + DOT_SZ - reach:
        bad.append("dots frame %d's last ink stands %d px short of its last cell's box"
                   % (i, pitch * (cells - 1) + DOT_SZ - reach - cols[-1]))
    off = max(abs(firsts[j] - firsts[j - 1] - pitch) for j in range(1, len(firsts))) if cells > 1 else 0.0
    return bad, cells, filled, float(cols[0]), float(off)


def check(name, raw, exe, r4):
    """One theme, through the door's own reader and the projector that will run it."""
    bad = []
    path = os.path.join(tempfile.mkdtemp(prefix="pmb."), name + ".orbtheme")
    open(path, "wb").write(raw)
    hdr, secs = ob.read_bundle(path)
    sec = secs[0]
    els = sec["blob"]["elements"]
    ov = ob.file_overlay(path)
    if (ov & 0xFF000000) != OVL_TAG:
        bad.append("overlay word 0x%08x: no 0x01 colour tag" % ov)
    if name in EXPECT_OVL and ov != EXPECT_OVL[name]:
        bad.append("overlay 0x%08x, expected 0x%08x" % (ov, EXPECT_OVL[name]))
    if sec["blob"]["slug"] != "pomodoro":
        bad.append("section slug %r" % sec["blob"]["slug"])
    try:
        ts.check_clock_rows(els)
    except ValueError as e:
        bad.append("theme_schema refused it: %s" % e)
    try:
        ts.check_clock_sounds(els, [tuple(s.get(k, 0) for k in ("off", "size", "w", "h")) for s in sec["blob"]["sprites"]], False, 0)
    except ValueError as e:
        bad.append("the chimes: %s" % e)
    plates = sorted(int(e["slot"]) for e in rows(els, ts.CLASS_PLATE))
    if plates != [0]:
        bad.append("plates %s: the look is one plate — the ground with its instrument folded "
                   "in — and nothing at slot 1, which is the OS's glass" % plates)
    bad += stacking(sec)
    fonts = sorted(int(e["slot"]) for e in rows(els, ts.CLASS_TEXT))
    if fonts != [0, 1, 2]:
        bad.append("fonts: %s" % fonts)
    aux = {int(e["slot"]): e for e in rows(els, ts.CLASS_AUX)}
    labs = [s for s in range(8, 13) if s in aux and int(aux[s]["kind"]) == ts.SHAPE_TEXT]
    if labs != [8, 9, 10, 11, 12]:
        bad.append("labels at %s, not 8..12" % labs)
    track, fill = aux.get(13), aux.get(14)
    if not track:
        bad.append("no dots track row at 13")
    if not fill:
        bad.append("no dots fill row at 14")
    if track and fill:
        if int(track["kind"]) == ts.SHAPE_TEXT or int(track["n_frames"]) != N_TRACK:
            bad.append("the dots track at 13 is %d frames of kind %s: %d hollow tracks, one per cell count"
                       % (int(track["n_frames"]), int(track["kind"]), N_TRACK))
        if int(fill["kind"]) == ts.SHAPE_TEXT or int(fill["n_frames"]) != N_FILL:
            bad.append("the dots fill at 14 is %d frames of kind %s: %d fills, one per count, frame 0 empty"
                       % (int(fill["n_frames"]), int(fill["kind"]), N_FILL))
        for what, e in (("track", track), ("fill", fill)):
            if int(e.get("delay", 0)):
                bad.append("the dots %s carries a frame time of %d ms: the app picks it" % (what, int(e["delay"])))
        if not (int(track["x"]) == int(fill["x"]) and int(track["y"]) == int(fill["y"])
                and int(track["w"]) == int(fill["w"]) and int(track["h"]) == int(fill["h"])):
            bad.append("the dots rows do not draw at the one place: track %d,%d of %dx%d, fill %d,%d of %dx%d"
                       % (int(track["x"]), int(track["y"]), int(track["w"]), int(track["h"]),
                          int(fill["x"]), int(fill["y"]), int(fill["w"]), int(fill["h"])))
        if els.index(fill) < els.index(track) or int(fill["order"]) < int(track["order"]):
            bad.append("the fill row is not emitted after the track: it would draw under it, not over it")
        pitch = DOT_SZ + DOT_GAP
        if pm_dot_pitch() != pitch:
            bad.append("pomodoro.h's PM_DOT_PITCH is %s, the bake's own cell plus gap is %d" % (pm_dot_pitch(), pitch))
        # amendment 2's samples: a 4-cell track, 3 of the cells lit, and the empty fill — the
        # three shapes the app can put together, each left-aligned and at the bake's own pitch
        for label, rec, i, want_cells, want_filled in (("track N=4", track, 3, 4, 0),
                                                       ("fill n=3", fill, 3, 3, 3),
                                                       ("fill 0", fill, 0, 0, 0)):
            dbad, cells, got, x0, off = dot_cells(sec, rec, i)
            bad += dbad
            if dbad:
                continue
            if cells != want_cells or got != want_filled:
                bad.append("the dots %s draws %d cells, %d filled" % (label, cells, got))
            if want_cells and x0 > 2:
                bad.append("the dots %s starts %d px in: the bank is left-aligned, the app poses the centring"
                           % (label, int(x0)))
            if want_cells > 1 and off > 2:
                bad.append("the dots %s stands up to %.0f px off the bake's %d px pitch (pomodoro.h says %s)"
                           % (label, off, pitch, pm_dot_pitch()))
    snd = sorted((int(e["slot"]), int(e["kind"]), int(e["flags"])) for e in rows(els, ts.CLASS_SOUND))
    if [s[0] for s in snd] != [1, 2, 3]:
        bad.append("sound slots %s: a chime per phase end, at 1, 2 and 3" % [s[0] for s in snd])
    if name in NAMES:      # the starter's short and long are the same brighter variant, and
        f023 = {int(e["slot"]): int(e["frame0"]) for e in rows(els, ts.CLASS_SOUND)}   # the bank
        if f023.get(2) != f023.get(3):   # owes it one plane, not two (owner question 1)
            bad.append("slots 2 and 3 are separate planes: equal chimes should share one wave")
    for slot, kind, flags in snd:
        if kind != ts.HOUR_PATTERNS.index("none"):
            bad.append("sound slot %d names hour pattern %d: a timer strikes no hour" % (slot, kind))
        if flags & 1:
            bad.append("sound slot %d is flagged to loop (§3 refuses it)" % slot)
    vals = rows(els, ts.CLASS_VALUES)
    if len(vals) != 1 or int(vals[0]["src"]) != 1:
        bad.append("no values at layout 1")
        rings = None
    else:
        sp = sec["blob"]["sprites"][int(vals[0]["frame0"])]
        b = sec["art"][sp["off"]:sp["off"] + sp["w"]]
        if sp["w"] != VALUES_B:
            bad.append("values of %d B, not pm_values_t's %d" % (sp["w"], VALUES_B))
        rings = list(struct.unpack_from("<3I", b, 0))
        if list(b[12:15]) != [1, 2, 3]:
            bad.append("values name chime slots %s, not 1, 2, 3" % list(b[12:15]))
        if b[15]:
            bad.append("the values' reserved byte is %d" % b[15])
        if name in RING:
            want = [int(c[1:], 16) for c in RING[name]]
            if rings != want:
                bad.append("ring colours %s, the looks table's %s" % (["%06x" % v for v in rings], ["%06x" % v for v in want]))
        if name != "blank" and rings == list(NEW_DEFAULTS):
            bad.append("New's default ring colours on a look that has its own")
    if not rows(els, ts.CLASS_PREVIEW):
        bad.append("no preview")
    plate0 = [e for e in rows(els, ts.CLASS_PLATE) if int(e["slot"]) == 0]
    if not plate0:
        bad.append("no plate 0 to place")
    else:
        off = sec["blob"]["sprites"][int(plate0[0]["frame0"])]["off"]
        got = sec["blob"]["scalars"].get("plate_off")
        if got != off:
            bad.append("plate_off is %r and the plate's own plane offset is %r" % (got, off))
    bad += rule4(name, r4)
    fr = (r4[0], r4[1]) if r4 else (None, None)
    if b'"pm-design"' not in raw:
        bad.append("the design region carries no pm-design document")
    if b'"overlay"' not in raw:
        bad.append("the design region carries no overlay field (§64.9)")
    if len(raw) > BUDGET:
        bad.append("%d KB, past the %d KB the pool has" % (len(raw) // 1024, BUDGET // 1024))
    rc, out, err = H3.run(exe, "--dump3f", path, "pomodoro")
    if rc != 0:
        bad.append("orb_clock3.c refused it: %s" % (out or err).strip()[:200])
    f0 = [f for f in rows(els, ts.CLASS_TEXT) if int(f["slot"]) == 0]
    dg = digit_cells(sec, f0[0]) if f0 else (["no font 0"], 0, 0, False)
    bad += dg[0]
    return bad, len(raw), ov, dg[1], dg[3], fr


def furn_digest(r4s, name):
    """A look's furniture as the page measured its own plane: its digest for the gates that
    compare two looks' instruments. The plate the bytes carry cannot answer these — the
    instrument is folded into it, and the ground under it differs per look anyway."""
    return r4s.get(name, [None, None, None])[2]


def _tmp(raw):
    p = os.path.join(tempfile.mkdtemp(prefix="pm1."), "t.orbtheme")
    open(p, "wb").write(raw)
    return p


def sound_peak(raw, slot):
    """Loudness of one baked chime plane: the round trip's proof that the wave is there."""
    hdr, secs = ob.read_bundle(_tmp(raw))
    sec = secs[0]
    for e in rows(sec["blob"]["elements"], ts.CLASS_SOUND):
        if int(e["slot"]) != slot:
            continue
        sp = sec["blob"]["sprites"][int(e["frame0"])]
        b = sec["art"][sp["off"]:sp["off"] + sp["size"]]
        n = int(sp["w"])
        pcm = ts.adpcm_decode(b, n) if int(e["src"]) == ts.SOUND_ENC_ADPCM else \
            list(struct.unpack("<%dh" % n, b[:2 * n]))
        return max(abs(x) for x in pcm)
    return 0


def cross(themes, refused, r4s):
    """The gates one theme cannot state on its own."""
    bad = 0
    miss = [n for n in NAMES if n not in themes]
    if miss:
        print("FAIL no theme for %s" % ", ".join(miss))
        bad += 1
    furn = {}
    for n in NAMES:
        if n in themes:
            h = furn_digest(r4s, n)
            if h is None:
                print("FAIL %s printed no furniture plane to digest" % n)
                bad += 1
                continue
            if h in furn:
                print("FAIL %s and %s draw the same furniture: plan §64.4" % (n, furn[h]))
                bad += 1
            furn[h] = n
    for a, b in (("scope", "no-parts"), ("brass", "parts-off")):
        ha, hb = furn_digest(r4s, a), furn_digest(r4s, b)
        if ha is not None and hb is not None:
            if ha == hb:
                print("FAIL %s and %s have the same furniture with its parts hidden" % (a, b))
                bad += 1
    for pair in (("det-a", "det-b"), ("rt-a", "rt-b")):
        if all(k in themes for k in pair):
            if themes[pair[0]] != themes[pair[1]]:
                print("FAIL %s and %s are not the same bytes for the same design" % pair)
                bad += 1
    if "rt-a" in themes:
        p = sound_peak(themes["rt-a"], 2)
        if not p:
            print("FAIL the uploaded wave baked nothing: the chime plane is silence")
            bad += 1
        else:
            print("roundtrip ok (uploaded wave peaks at %d of 32767)" % p)
    for key, words in REFUSALS.items():
        got = refused.get(key)
        if got is None:
            print("FAIL %s: the bake was asked to refuse and did not" % key)
            bad += 1
        elif words not in got:
            print("FAIL %s refused, but said %r instead of naming %r" % (key, got, words))
            bad += 1
    return bad


def evidence(d, pngs, wavs):
    """The by-eye set: nine looks at true size, and every chime plain and brighter."""
    from PIL import Image
    import io
    bad = 0
    for n, raw in pngs.items():
        im = Image.open(io.BytesIO(raw))
        if im.size != (466, 466):
            print("FAIL %s.png is %s, not 466 square" % (n, im.size))
            bad += 1
            continue
        im.save(os.path.join(d, "looks", n + ".png"), dpi=(177, 177))
    for n, raw in wavs.items():
        open(os.path.join(d, "wavs", n + ".wav"), "wb").write(raw)
    if len(pngs) < 9:
        print("FAIL %d PNGs, not the nine the looks are judged from" % len(pngs))
        bad += 1
    if len(wavs) < 18:
        print("FAIL %d WAVs, not the eighteen (every chime plain and brighter)" % len(wavs))
        bad += 1
    return bad


def main():
    chrome = oc.find_chrome()
    if chrome is None:
        print(oc.missing() + ": this check runs where a browser is")
        return 1
    stage_dir = oc.stage_dir(STAGE_NAME, chrome)
    stage(stage_dir)
    profile = oc.profile_dir(PROFILE_NAME, chrome)
    keep = os.environ.get("POMODORO_BAKE_KEEP")
    evid = os.environ.get("POMODORO_EVIDENCE")
    dom = run_chrome(chrome, stage_dir, profile)
    exe = H3.build()
    if keep:
        os.makedirs(keep, exist_ok=True)
    if evid:
        os.makedirs(os.path.join(evid, "looks"), exist_ok=True)
        os.makedirs(os.path.join(evid, "wavs"), exist_ok=True)
    themes, pngs, wavs, fails, seen = {}, {}, {}, 0, 0
    refused, stats, errs, r4s = {}, {}, {}, {}
    for line in re.sub(r"<[^>]+>", "\n", dom).splitlines():
        line = line.strip()
        if line.startswith("ERR "):
            errs.append(line[4:])
        elif line.startswith("FAIL "):
            _, key, why = line.split(" ", 2)
            refused[key] = why
        elif line.startswith("R4 "):
            _, name, rad, ink, dig = line.split(" ")
            r4s[name] = (int(rad), int(ink), dig)
        elif line.startswith("STAT "):
            _, name, blob = line.split(" ", 2)
            stats[name] = json.loads(blob)
        elif line.startswith("B64 "):
            _, name, ms, b64 = line.split(" ", 3)
            themes[name] = (ms, base64.b64decode(b64))
        elif line.startswith("PNG ") and evid:
            _, name, b64 = line.split(" ", 2)
            pngs[name] = base64.b64decode(b64)
        elif line.startswith("WAV ") and evid:
            _, name, b64 = line.split(" ", 2)
            wavs[name] = base64.b64decode(b64)
    for e in errs:
        print("FAIL " + e)
        fails += 1
    for name, (ms, raw) in themes.items():
        seen += 1
        if keep:
            open(os.path.join(keep, name + ".orbtheme"), "wb").write(raw)
        bad, n, ov, cell, tab, fr = check(name, raw, exe, r4s.get(name))
        st = stats.get(name)
        meter = "-"
        if not st:
            bad.append("the bake claimed nothing about itself")
        else:
            if int(st["cell"]["w"]) != cell:
                bad.append("the bake claims a %d px figure cell, the file carries %d px" % (int(st["cell"]["w"]), cell))
            if [int(s["slot"]) for s in st["sounds"]] != [1, 2, 3]:
                bad.append("the bake itself says it wrote sound slots %s" % [s["slot"] for s in st["sounds"]])
            meter = "geom %.1f%% art %.1f%%" % (float(st["meter"]["geomPct"]), float(st["meter"]["artPct"]))
        print("%s %-10s %8d B %5s ms  %s  cells %3d px %s  furniture r %3d  ovl=0x%08x  %s"
              % ("ok  " if not bad else "FAIL", name, n, ms, meter, cell, "tabular" if tab else "SHIFTING",
                 fr[0] if fr[0] is not None else -1, ov, "; ".join(bad)))
        fails += bool(bad)
    if "DONE" not in dom:
        print("FAIL the page did not finish (virtual time ran out, or it threw)")
        fails += 1
    # the refusals again, this time alone: each must stand on its own, not ride in beside
    # the themes it was listed after
    only = ",".join(REFUSALS)
    dom2 = run_chrome(chrome, stage_dir, profile, "&only=" + only)
    alone = {}
    for line in re.sub(r"<[^>]+>", "\n", dom2).splitlines():
        if line.strip().startswith("FAIL "):
            _, key, why = line.strip().split(" ", 2)
            alone[key] = why
        elif line.strip().startswith("ERR "):
            print("FAIL refusals alone: " + line.strip()[4:])
            fails += 1
    for key in REFUSALS:
        if key not in alone:
            print("FAIL %s: not refused when asked for alone" % key)
            fails += 1
    if "DONE" not in dom2:
        print("FAIL the refusals-alone pass did not finish")
        fails += 1
    if len(alone) == len(REFUSALS):
        print("refusals ok (%s, each refused alone)" % only)
    fails += cross({k: v[1] for k, v in themes.items()}, refused, r4s)
    if evid:
        # the pictures and the waves are their own pass: one DOM carrying themes and PNGs
        # alike runs past what a headless dump-dom delivers whole, and a cut-off page is a
        # false failure on every gate that reads it
        domE = run_chrome(chrome, stage_dir, profile, "&evidence=1&only=" + ",".join(NAMES))
        for line in re.sub(r"<[^>]+>", "\n", domE).splitlines():
            line = line.strip()
            if line.startswith("PNG "):
                _, name, b64 = line.split(" ", 2)
                pngs[name] = base64.b64decode(b64)
            elif line.startswith("WAV "):
                _, name, b64 = line.split(" ", 2)
                wavs[name] = base64.b64decode(b64)
            elif line.startswith("ERR "):
                print("FAIL evidence: " + line[4:])
                fails += 1
        if "DONE" not in domE:
            print("FAIL the evidence pass did not finish")
            fails += 1
        fails += evidence(evid, pngs, wavs)
    print("%d themes baked, %d failed" % (seen, fails))
    return 1 if fails or not seen else 0


if __name__ == "__main__":
    sys.exit(main())
