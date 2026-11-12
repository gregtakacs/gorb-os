#!/usr/bin/env python3
"""The pomodoro editor's bake, checked in real Chrome (docs/specs/pomodoro-editor-design.md §6).

    python3 apps/pomodoro/editor/_selftest/bake_check.py

Stages bake.html beside the runtime's baker and fonts and this app's bake, runs it headless,
and checks every theme it prints: the rows apps/pomodoro/main.c finds (plates 0 and 1, fonts
0..2, the labels at 8..12, the dots at 13 with thirteen frames, three one-shot chimes at slots
1, 2 and 3, sixteen bytes of pm_values_t at version 1 whose chime reads 1,2,3), the two rules
the looks design can be held to — the furniture inside radius 200 and the ten figures of one
width and centred — the plate's own offset in plate_off, the design region carried with its
overlay field, the room, and the shipped projector (orb_clock3.c under --dump3f) accepting the
lot. It also checks what no single theme can: that the nine look differently from each other,
that hiding a part changes the furniture, that the same design baked twice is the same bytes,
and that an owner's uploaded wave survives the round trip unchanged and audible. A second pass
runs the five refusals alone (?only=), so each is proven on its own.

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


def plate_alpha(sec, slot):
    """A full-panel masked plate's alpha, as a dict of (x, y) -> byte (orb_disc.h spans)."""
    r = [e for e in rows(sec["blob"]["elements"], ts.CLASS_PLATE) if int(e["slot"]) == slot]
    if not r:
        return None
    art = sec["art"]
    sp = sec["blob"]["sprites"][int(r[0]["frame0"])]
    n = ts.disc_px()
    if sp["size"] != n * 3:
        return None
    a = art[sp["off"] + 2 * n:sp["off"] + 3 * n]
    out = {}, n
    d = out[0]
    at = 0
    for y in range(ts.DISC_SIDE):
        x0, x1 = ts.disc_span(y)
        for x in range(x0, x1 + 1):
            if a[at]:
                d[(x, y)] = a[at]
            at += 1
    return d


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


def furniture_radius(sec, slot):
    """Rule 4, measured: how far out the furniture's own ink stands, and how much of it there is."""
    a = plate_alpha(sec, slot)
    if a is None:
        return None, None
    r = max((int(round(((x - ts.DISC_SIDE // 2 + 0.5) ** 2 + (y - ts.DISC_SIDE // 2 + 0.5) ** 2) ** 0.5))
             for (x, y), v in a.items() if v >= ALPHA_INK), default=0)
    return r, sum(1 for v in a.values() if v >= ALPHA_INK)


def check(name, raw, exe):
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
    if plates != [0, 1]:
        bad.append("plates %s: the ground and the furniture" % plates)
    fonts = sorted(int(e["slot"]) for e in rows(els, ts.CLASS_TEXT))
    if fonts != [0, 1, 2]:
        bad.append("fonts: %s" % fonts)
    aux = {int(e["slot"]): e for e in rows(els, ts.CLASS_AUX)}
    labs = [s for s in range(8, 13) if s in aux and int(aux[s]["kind"]) == ts.SHAPE_TEXT]
    if labs != [8, 9, 10, 11, 12]:
        bad.append("labels at %s, not 8..12" % labs)
    dots = aux.get(13)
    if not dots:
        bad.append("no dots row at 13")
    elif int(dots["kind"]) == ts.SHAPE_TEXT or int(dots["n_frames"]) != 13:
        bad.append("the dots row at 13 is %d frames of kind %s: thirteen frames of art" % (int(dots["n_frames"]), int(dots["kind"])))
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
    fr = furniture_radius(sec, 1)
    if fr[0] is None:
        bad.append("plate 1 is not a masked full panel")
    elif fr[0] > 200:
        bad.append("furniture ink out to radius %d: the rim is the ring's (rule 4)" % fr[0])
    elif not fr[1] and name not in ("no-parts",):
        bad.append("plate 1 carries no ink at all")
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


def plate1(raw):
    """A theme's furniture as bytes (its masked disc, colour and alpha) and its digest, for
    the gates that compare two themes' furniture rather than one theme's rows."""
    hdr, secs = ob.read_bundle(_tmp(raw))
    sec = secs[0]
    e = [r for r in rows(sec["blob"]["elements"], ts.CLASS_PLATE) if int(r["slot"]) == 1]
    if not e:
        return None, None
    sp = sec["blob"]["sprites"][int(e[0]["frame0"])]
    b = sec["art"][sp["off"]:sp["off"] + sp["size"]]
    return b, hash(b)


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


def cross(themes, refused):
    """The gates one theme cannot state on its own."""
    bad = 0
    miss = [n for n in NAMES if n not in themes]
    if miss:
        print("FAIL no theme for %s" % ", ".join(miss))
        bad += 1
    furn = {}
    for n in NAMES:
        if n in themes:
            b, h = plate1(themes[n])
            if b is None:
                print("FAIL %s has no furniture plate" % n)
                bad += 1
                continue
            if h in furn:
                print("FAIL %s and %s draw the same furniture: plan §64.4" % (n, furn[h]))
                bad += 1
            furn[h] = n
    for a, b in (("scope", "no-parts"), ("brass", "parts-off")):
        if a in themes and b in themes:
            ba, ha = plate1(themes[a])
            bb, hb = plate1(themes[b])
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
    refused, stats, errs = {}, {}, []
    for line in re.sub(r"<[^>]+>", "\n", dom).splitlines():
        line = line.strip()
        if line.startswith("ERR "):
            errs.append(line[4:])
        elif line.startswith("FAIL "):
            _, key, why = line.split(" ", 2)
            refused[key] = why
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
        bad, n, ov, cell, tab, fr = check(name, raw, exe)
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
    fails += cross({k: v[1] for k, v in themes.items()}, refused)
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
