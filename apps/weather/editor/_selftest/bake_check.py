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
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402
import host_clock3_test as H3   # noqa: E402

CHROME = "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe"
STAGE_WIN = r"C:\Temp\weather-bake-check"
STAGE = "/mnt/c/Temp/weather-bake-check"
RUNTIME = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
EDITOR = os.path.dirname(HERE)
APPV = open(os.path.join(EDITOR, "..", "version.txt")).read().strip()
# The pool's room beside the largest clock theme (skeleton, 5.07 MB) and the tracker's
# (modern, 1.34 MB): 8 MB less those, the two table sectors and some slack.
BUDGET = 1750 * 1024


def stage():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    for src in (os.path.join(HERE, "bake.html"), os.path.join(EDITOR, "weather-bake.js"),
                os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"), os.path.join(RUNTIME, "vendor", "fonts.css")):
        shutil.copy(src, STAGE)
    shutil.copytree(os.path.join(RUNTIME, "vendor", "fonts"), os.path.join(STAGE, "fonts"))


def run_chrome():
    r = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-sandbox",
                        "--user-data-dir=C:\\Temp\\weather-bake-profile", "--allow-file-access-from-files",
                        "--virtual-time-budget=300000", "--dump-dom",
                        "file:///" + STAGE_WIN.replace("\\", "/") + "/bake.html?appv=" + APPV],
                       capture_output=True, text=True, timeout=600)
    return r.stdout


def check(name, raw, exe):
    bad = []
    path = os.path.join(tempfile.mkdtemp(prefix="wxb."), name + ".orbtheme")
    open(path, "wb").write(raw)
    hdr, secs = ob.read_bundle(path)
    sec = secs[0]
    els = sec["blob"]["elements"]
    if sec["blob"]["slug"] != "weather":
        bad.append("section slug %r" % sec["blob"]["slug"])
    try:
        ts.check_clock_rows(els)
    except ValueError as e:
        bad.append("theme_schema refused it: %s" % e)
    aux = {int(e["slot"]): e for e in els if int(e["class"]) == ts.CLASS_AUX}
    fonts = [e for e in els if int(e["class"]) == ts.CLASS_TEXT]

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
    if sorted(int(f["slot"]) for f in fonts) != [0, 1, 2]:
        bad.append("fonts: %s" % sorted(int(f["slot"]) for f in fonts))
    labs = [aux.get(s) for s in range(24, 32)]
    if not all(lb is not None and int(lb["kind"]) == ts.SHAPE_TEXT for lb in labs):
        bad.append("the labels are not at 24..31")
    else:
        cols = [int(lb["h"]) for lb in labs[4:]]
        if len(set(cols)) != 1:
            bad.append("the four columns do not share one geometry: %s" % cols)
    live = [e for e in els if int(e["class"]) == ts.CLASS_AUX]
    if len(live) > 32:
        bad.append("%d live rows against the 32 an app seats" % len(live))
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
    return bad, len(raw)


def main():
    if not os.path.exists(CHROME):
        print("no Chrome at %s: this check runs where one is" % CHROME)
        return 1
    stage()
    dom = run_chrome()
    exe = H3.build()
    fails, seen = 0, 0
    keep = os.environ.get("WEATHER_BAKE_KEEP")    # a folder to keep the themes in, to look at
    for line in re.sub(r"<[^>]+>", "\n", dom).splitlines():
        if line.startswith("ERR "):
            print("FAIL " + line[4:])
            fails += 1
        elif line.startswith("B64 "):
            _, name, ms, b64 = line.split(" ", 3)
            seen += 1
            raw = base64.b64decode(b64)
            if keep:
                open(os.path.join(keep, name + ".orbtheme"), "wb").write(raw)
            bad, n = check(name, raw, exe)
            print("%s %-20s %8d B %6s ms  %s" % ("ok  " if not bad else "FAIL", name, n, ms, "; ".join(bad)))
            fails += bool(bad)
    if "DONE" not in dom:
        print("FAIL the page did not finish (virtual time ran out, or it threw)")
        fails += 1
    print("%d themes baked, %d failed" % (seen, fails))
    return 1 if fails or not seen else 0


if __name__ == "__main__":
    sys.exit(main())
