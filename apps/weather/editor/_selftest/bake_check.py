#!/usr/bin/env python3
"""The weather editor's bake, checked in real Chrome (docs/weather-plan.md §2.6, W5).

    python3 apps/weather/editor/_selftest/bake_check.py

Stages bake.html beside the runtime's baker and fonts and the editor's bake in a folder
Windows Chrome can read, runs it headless, and checks every theme it prints: it reads
back through orbbundle and theme_schema, the shipped orb_clock3.c projects it under the
weather slug, and its rows are the ones apps/weather/main.c finds: the large icon at slot 0
(ten picked frames), five small icons of ten, twelve hour bars of 24, five day bars of 28,
the dots of ten, three fonts, the eight labels at 24..31 with the four columns sharing one
geometry, the preview, the design region carried, and the whole theme inside the pool's
budget beside the clock's and the tracker's (the owner's 2026-11-06 figures).
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

STAGE_NAME, PROFILE_NAME = "weather-bake-check", "weather-bake-profile"
RUNTIME = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
EDITOR = os.path.dirname(HERE)
APPV = open(os.path.join(EDITOR, "..", "version.txt")).read().strip()
# A theme's room (docs/theme-cache-plan.md C4): the pool is a cache, so a theme need only fit
# it alone, beside the most a voice bank may hold (1 MB, ruling 6): 8 MB less that, the two
# table sectors and 512 KB of slack. The shelf's other defaults no longer count against it.
BUDGET = (8 << 20) - (1 << 20) - 2 * 4096 - (512 << 10)

# §64.9 / gate §8.6: the region's second head word is the OS's overlay colour, tagged 0x01.
OVL_TAG = 0x01000000
EXPECT_OVL = {"scope": OVL_TAG | 0xFFB23C,               # the Scope preset's phosphor bolt (#FFB23C)
              "scope-wears-nebula": OVL_TAG | 0xFF8A5C,  # nebula's accent is its 'orange' role (#FF8A5C), not a warn
              "overlay-set": OVL_TAG | 0x135C79}         # the owner's #135c79, set on the design


def stage(stage_dir):
    for src in (os.path.join(HERE, "bake.html"), os.path.join(EDITOR, "weather-bake.js"), os.path.join(EDITOR, "ground.js"),
                os.path.join(EDITOR, "wx-map.js"),
                os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"), os.path.join(RUNTIME, "vendor", "fonts.css")):
        shutil.copy(src, stage_dir)
    shutil.copytree(os.path.join(RUNTIME, "vendor", "fonts"), os.path.join(stage_dir, "fonts"))


def run_chrome(chrome, stage_dir, profile):
    url = oc.file_url(os.path.join(stage_dir, "bake.html"), chrome) + "?appv=" + APPV
    return oc.run_headless(chrome, url, profile, 600, extra=["--virtual-time-budget=300000"]).stdout


VALUES_B = 108                                  # main.c wx_values_t, layout 1


def check(name, raw, exe, mapped):
    bad = []
    path = os.path.join(tempfile.mkdtemp(prefix="wxb."), name + ".orbtheme")
    open(path, "wb").write(raw)
    hdr, secs = ob.read_bundle(path)
    sec = secs[0]
    els = sec["blob"]["elements"]
    # §64.9 / gate §8.6: the region's second head word is the OS's overlay colour, tagged 0x01;
    # an untouched design bakes its pack's accent (via the roles table), an owner's setting wins.
    ov = ob.file_overlay(path)
    if (ov & 0xFF000000) != OVL_TAG:
        bad.append("overlay word 0x%08x: no 0x01 colour tag" % ov)
    if name in EXPECT_OVL and ov != EXPECT_OVL[name]:
        bad.append("overlay 0x%08x, expected 0x%08x" % (ov, EXPECT_OVL[name]))
    if sec["blob"]["slug"] != "weather":
        bad.append("section slug %r" % sec["blob"]["slug"])
    try:
        ts.check_clock_rows(els)
    except ValueError as e:
        bad.append("theme_schema refused it: %s" % e)
    aux = {int(e["slot"]): e for e in els if int(e["class"]) == ts.CLASS_AUX}
    fonts = [e for e in els if int(e["class"]) == ts.CLASS_TEXT]
    # the map (flights-map-plan §5): the values when it is on, none when it is off
    vals = [e for e in els if int(e["class"]) == ts.CLASS_VALUES]
    if mapped:
        if len(vals) != 1 or int(vals[0]["src"]) != 1:
            bad.append("no values of layout 1")
        else:
            b = sec["blob"]["sprites"][int(vals[0]["frame0"])]
            if not b or int(b["w"]) != VALUES_B:
                bad.append("values of %r B, not wx_values_t's %d" % (b and b["w"], VALUES_B))
    elif vals:
        bad.append("values with no map")

    def picked(slots, n):
        return all(s in aux and int(aux[s]["n_frames"]) == n and not int(aux[s]["delay"]) for s in slots)
    if not picked([0], 10):
        bad.append("no large icon of ten picked frames at slot 0")
    if not picked(range(1, 6), 10):
        bad.append("the small icons are not five rows of ten")
    if not picked(range(6, 18), 24):
        bad.append("the hour bars are not twelve rows of 24")
    if not picked(range(18, 23), 28):
        bad.append("the day bars are not five rows of 28")
    if not picked([23], 10):
        bad.append("no place dots of ten at slot 23")
    # §2.10: three fonts, and three more for Hourly and the 5 days where Now's sizes differ
    want_fonts = list(range(6)) if name == "largest-moved" else [0, 1, 2]
    if sorted(int(f["slot"]) for f in fonts) != want_fonts:
        bad.append("fonts: %s" % sorted(int(f["slot"]) for f in fonts))
    fixed = [s for s in (50, 51, 52, 55) if s in aux and int(aux[s]["kind"]) == ts.SHAPE_TEXT]
    if fixed != ([50, 51, 52, 55] if name == "largest-moved" else []):
        bad.append("Hourly's and the 5 days' own labels: %s" % fixed)
    if (56 in aux) != (name == "largest-moved"):
        bad.append("Hourly's own icon at 56 %s" % ("missing" if name == "largest-moved" else "baked though Now's icon is the default size"))
    labs = [aux.get(s) for s in range(24, 32)]
    if not all(lb is not None and int(lb["kind"]) == ts.SHAPE_TEXT for lb in labs):
        bad.append("the labels are not at 24..31")
    else:
        cols = [int(lb["h"]) for lb in labs[4:]]
        if len(set(cols)) != 1:
            bad.append("the four columns do not share one geometry: %s" % cols)
    live = [e for e in els if int(e["class"]) == ts.CLASS_AUX]
    if len(live) > ts.CLK_ROW_MAX:
        bad.append("%d live rows against the %d an app seats" % (len(live), ts.CLK_ROW_MAX))
    # §2.7: the clock at 32 and the particle pool from 33, one bank for all of them
    if 32 not in aux or int(aux[32].get("kind", -1)) != ts.SHAPE_TEXT:
        bad.append("no clock label at 32")
    if not picked(range(33, 49), 7):
        bad.append("the particle pool is not sixteen rows of seven at 33..48")
    elif len(set(int(aux[s]["frame0"]) for s in range(33, 49))) != 1:
        bad.append("the particles do not share one bank")
    # thunder's bolt, one picture at 57, with the particles
    if 57 not in aux or int(aux[57]["n_frames"]) != 1:
        bad.append("no lightning bolt at 57")
    # §2.8: the 5 days' cursor, one picture at 49
    if 49 not in aux or int(aux[49]["n_frames"]) != 1:
        bad.append("no halo at 49")
    if name == "largest-moved":
        ic = aux.get(0)
        if not ic or int(ic["w"]) != 104 or int(ic["x"]) + int(ic["pivot_x"]) != 233 + 40:
            bad.append("the largest icon is not 104 px moved 40 px right: %r" % ({k: ic[k] for k in ("x", "y", "w", "pivot_x")} if ic else None))
    if not any(int(e["class"]) == ts.CLASS_PREVIEW for e in els):
        bad.append("no preview")
    with open(path, "rb") as f:
        head = f.read()
    if b'"wc-design"' not in head:
        bad.append("the design region is not carried")
    if len(raw) > BUDGET:
        bad.append("%d KB, past the %d KB the pool has beside the clock and the tracker" % (len(raw) // 1024, BUDGET // 1024))
    rc, out, err = H3.run(exe, "--dump3f", path, "weather")
    if rc != 0:
        bad.append("orb_clock3.c refused it: %s" % (out or err).strip()[:200])
    return bad, len(raw), ov


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
    keep = os.environ.get("WEATHER_BAKE_KEEP")    # a folder to keep the themes in, to look at
    for line in re.sub(r"<[^>]+>", "\n", dom).splitlines():
        if line.startswith("ERR "):
            print("FAIL " + line[4:])
            fails += 1
        elif line.startswith("B64 "):
            _, name, ms, mapped, b64 = line.split(" ", 4)
            seen += 1
            raw = base64.b64decode(b64)
            if keep:
                open(os.path.join(keep, name + ".orbtheme"), "wb").write(raw)
            bad, n, ov = check(name, raw, exe, mapped == "1")
            print("%s %-20s %8d B %6s ms  ovl=0x%08x  %s" % ("ok  " if not bad else "FAIL", name, n, ms, ov, "; ".join(bad)))
            fails += bool(bad)
    if "DONE" not in dom:
        print("FAIL the page did not finish (virtual time ran out, or it threw)")
        fails += 1
    print("%d themes baked, %d failed" % (seen, fails))
    return 1 if fails or not seen else 0


if __name__ == "__main__":
    sys.exit(main())
