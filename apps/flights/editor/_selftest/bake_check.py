#!/usr/bin/env python3
"""The flights editor's bake, checked in real Chrome (flight-tracker-design §12.3, E3).

    python3 apps/flights/editor/_selftest/bake_check.py

Stages bake.html beside the runtime's baker and fonts and the editor's art and bake in a
folder Windows Chrome can read, runs it headless, and checks every theme it prints: it
reads back through orbbundle and theme_schema, the shipped orb_clock3.c projects it under
the flights slug, and its rows are the ones apps/flights/main.c finds (the arm at slot 0,
fifteen planes of 49 picked frames, the marker rings at 16 and 17, four fonts, the header,
footer and range at 20, 21 and 23 and the card at 22 or, keeping clear, at 24, the far-contact
dots at 18, 19 and 25..31 when the design shows them, the preview),
the arm's vmax its turn in seconds (§12.5), with the design region carried.
"""
import base64
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402
import host_clock3_test as H3   # noqa: E402
import orbchrome as oc          # noqa: E402

STAGE_NAME, PROFILE_NAME = "flights-bake-check", "flights-bake-profile"
RUNTIME = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
EDITOR = os.path.dirname(HERE)
APPV = open(os.path.join(EDITOR, "..", "version.txt")).read().strip()


def stage(stage_dir):
    for src in (os.path.join(HERE, "bake.html"), os.path.join(EDITOR, "flights-art.js"),
                os.path.join(EDITOR, "flights-bake.js"), os.path.join(EDITOR, "flights-map.js"),
                os.path.join(RUNTIME, "maps@1", "orbmap.js"), os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"),
                os.path.join(RUNTIME, "vendor", "fonts.css")):
        shutil.copy(src, stage_dir)
    shutil.copytree(os.path.join(RUNTIME, "vendor", "fonts"), os.path.join(stage_dir, "fonts"))


def run_chrome(chrome, stage_dir, profile):
    url = oc.file_url(os.path.join(stage_dir, "bake.html"), chrome) + "?appv=" + APPV
    return oc.run_headless(chrome, url, profile, 600, extra=["--virtual-time-budget=300000"]).stdout


VALUES_B = 120                                  # main.c fl_values_t, layout 1


def check(name, raw, exe, card, period, mapped):
    bad = []
    path = os.path.join(tempfile.mkdtemp(prefix="flb."), name + ".orbtheme")
    open(path, "wb").write(raw)
    hdr, secs = ob.read_bundle(path)
    sec = secs[0]
    els = sec["blob"]["elements"]
    if sec["blob"]["slug"] != "flights":
        bad.append("section slug %r" % sec["blob"]["slug"])
    try:
        ts.check_clock_rows(els)
    except ValueError as e:
        bad.append("theme_schema refused it: %s" % e)
    aux = {int(e["slot"]): e for e in els if int(e["class"]) == ts.CLASS_AUX}
    fonts = [e for e in els if int(e["class"]) == ts.CLASS_TEXT]
    if 0 not in aux:
        bad.append("no arm at slot 0")
    planes = [aux.get(s) for s in range(1, 16)]
    if not all(p is not None and int(p["n_frames"]) == 49 and not int(p["delay"]) for p in planes):
        bad.append("the plane pool is not fifteen rows of 49 picked frames")
    if 16 not in aux or 17 not in aux:
        bad.append("a marker ring is missing")
    # the far-contact dots (main.c DOT_SLOTS): all nine or none, 7 band frames or 1, picked
    dot_slots = (18, 19, 25, 26, 27, 28, 29, 30, 31)
    dots = [aux.get(s) for s in dot_slots]
    if any(dots) and not all(d is not None and int(d["n_frames"]) in (1, 7) and not int(d["delay"])
                             and int(d["n_frames"]) == int(dots[0]["n_frames"]) for d in dots):
        bad.append("the dot pool is not nine rows of 1 or 7 picked frames")
    want_fonts = [0, 1, 2, 3, 4] if mapped else [0, 1, 2, 3]    # 4: the map's names
    if sorted(int(f["slot"]) for f in fonts) != want_fonts:
        bad.append("fonts: %s" % [int(f["slot"]) for f in fonts])
    # the map (flights-map-plan §2.2): six names at 32..37 and the values, or neither
    names = [s for s in range(32, 38) if s in aux and int(aux[s]["kind"]) == ts.SHAPE_TEXT]
    if names != (list(range(32, 38)) if mapped else []):
        bad.append("map names at %s" % names)
    vals = [e for e in els if int(e["class"]) == ts.CLASS_VALUES]
    if mapped:
        if len(vals) != 1 or int(vals[0]["src"]) != 1:
            bad.append("no values of layout 1")
        else:
            b = sec["blob"]["sprites"][int(vals[0]["frame0"])]
            if not b or int(b["w"]) != VALUES_B:
                bad.append("values of %r B, not fl_values_t's %d" % (b and b["w"], VALUES_B))
    elif vals:
        bad.append("values with no map")
    labels = [aux.get(s) for s in (20, 21, 23, card)]
    if not all(lb is not None and int(lb["kind"]) == ts.SHAPE_TEXT for lb in labels):
        bad.append("the labels are not at 20, 21, 23 and %d" % card)
    if (22 if card == 24 else 24) in aux:
        bad.append("a second card")
    if 0 in aux and int(aux[0]["vmax"]) != period:
        bad.append("the arm's vmax is %s, not its %d s turn" % (aux[0]["vmax"], period))
    if not any(int(e["class"]) == ts.CLASS_PREVIEW for e in els):
        bad.append("no preview")
    with open(path, "rb") as f:
        head = f.read()
    if b'"wc-design"' not in head:
        bad.append("the design region is not carried")
    rc, out, err = H3.run(exe, "--dump3f", path, "flights")
    if rc != 0:
        bad.append("orb_clock3.c refused it: %s" % (out or err).strip()[:200])
    return bad, len(raw)


def main():
    chrome = oc.find_chrome()
    if chrome is None:
        print(oc.missing() + ": this check runs where a browser is")
        return 1
    stage_dir = oc.stage_dir(STAGE_NAME, chrome)
    stage(stage_dir)
    profile = oc.profile_dir(PROFILE_NAME, chrome)
    dom = run_chrome(chrome, stage_dir, profile)
    exe = H3.build()
    fails, seen = 0, 0
    keep = os.environ.get("FLIGHTS_BAKE_KEEP")    # a folder to keep the themes in, to look at
    for line in re.sub(r"<[^>]+>", "\n", dom).splitlines():
        if line.startswith("ERR "):
            print("FAIL " + line[4:])
            fails += 1
        elif line.startswith("B64 "):
            _, name, ms, card, period, mapped, b64 = line.split(" ", 6)
            seen += 1
            raw = base64.b64decode(b64)
            if keep:
                open(os.path.join(keep, name + ".orbtheme"), "wb").write(raw)
            bad, n = check(name, raw, exe, int(card), int(period), mapped == "1")
            print("%s %-18s %8d B  %s" % ("ok  " if not bad else "FAIL", name, n, "; ".join(bad)))
            fails += bool(bad)
    if "DONE" not in dom:
        print("FAIL the page did not finish (virtual time ran out, or it threw)")
        fails += 1
    print("%d themes baked, %d failed" % (seen, fails))
    return 1 if fails or not seen else 0


if __name__ == "__main__":
    sys.exit(main())
