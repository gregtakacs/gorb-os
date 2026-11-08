#!/usr/bin/env python3
"""The radar editor's bake, checked in real Chrome (docs/radar-plan.md §2.5, R5).

    python3 apps/radar/editor/_selftest/bake_check.py

Stages bake.html beside the runtime's baker and fonts and the editor's bake in a folder
Windows Chrome can read, runs it headless, and checks every theme it prints: it reads back
through orbbundle and theme_schema, the shipped orb_clock3.c projects it under the radar
slug, and its rows are the ones apps/radar/main.c finds: the plate and the glass, the
place's dot at slot 0, the time cursor at 1 turning about the glass's centre, three fonts,
the labels at 8..17, the values in layout 1 at the size radar.h's rd_values_t is, the
preview, the design region carried, and the whole theme inside the pool's room beside the
shelf's other defaults.
"""
import base64
import os
import re
import shutil
import struct
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
STAGE_WIN = r"C:\Temp\radar-bake-check"
STAGE = "/mnt/c/Temp/radar-bake-check"
RUNTIME = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
EDITOR = os.path.dirname(HERE)
APPV = open(os.path.join(EDITOR, "..", "version.txt")).read().strip()
VALUES_B = 64 + 16 + 16 + 4 + 4 + 1024 + 16 + 16    # radar.h rd_values_t, layout 3
# The pool's room beside the shelf's other defaults (8 MB less those, the two table
# sectors and 512 KB of slack), read off the shelf so it follows them.
SHELF = os.path.join(ROOT, "tools", "orb-manager", "themes")
BUDGET = (8 << 20) - sum(os.path.getsize(os.path.join(SHELF, f)) for f in
                         ("skeleton.orbtheme", "modern.orbtheme", "scope.orbtheme")) - 2 * 4096 - (512 << 10)


def stage():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    for src in (os.path.join(HERE, "bake.html"), os.path.join(EDITOR, "radar-bake.js"),
                os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"), os.path.join(RUNTIME, "vendor", "fonts.css")):
        shutil.copy(src, STAGE)
    shutil.copytree(os.path.join(RUNTIME, "vendor", "fonts"), os.path.join(STAGE, "fonts"))


def run_chrome():
    r = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-sandbox",
                        "--user-data-dir=C:\\Temp\\radar-bake-profile", "--allow-file-access-from-files",
                        "--virtual-time-budget=300000", "--dump-dom",
                        "file:///" + STAGE_WIN.replace("\\", "/") + "/bake.html?appv=" + APPV],
                       capture_output=True, text=True, timeout=600)
    return r.stdout


def check(name, raw, exe):
    bad = []
    path = os.path.join(tempfile.mkdtemp(prefix="rdb."), name + ".orbtheme")
    open(path, "wb").write(raw)
    hdr, secs = ob.read_bundle(path)
    sec = secs[0]
    els = sec["blob"]["elements"]
    if sec["blob"]["slug"] != "radar":
        bad.append("section slug %r" % sec["blob"]["slug"])
    try:
        ts.check_clock_rows(els)
    except ValueError as e:
        bad.append("theme_schema refused it: %s" % e)
    aux = {int(e["slot"]): e for e in els if int(e["class"]) == ts.CLASS_AUX}
    plates = sorted(int(e["slot"]) for e in els if int(e["class"]) == ts.CLASS_PLATE)
    if plates != [0, 1]:
        bad.append("plates %s: the ground and the glass" % plates)
    if 0 not in aux or int(aux[0]["kind"]) == ts.SHAPE_TEXT:
        bad.append("no place dot at slot 0")
    h = aux.get(1)
    if not h or int(h["x"]) + int(h["pivot_x"]) != 233 or int(h["y"]) + int(h["pivot_y"]) != 233:
        bad.append("the time cursor at 1 does not turn about the glass's centre")
    fonts = sorted(int(e["slot"]) for e in els if int(e["class"]) == ts.CLASS_TEXT)
    if fonts != [0, 1, 2, 3, 4]:                   # 4: the scale bar's length (layout 3)
        bad.append("fonts: %s" % fonts)
    labs = [s for s in range(8, 19) if s in aux and int(aux[s]["kind"]) == ts.SHAPE_TEXT]
    if labs != list(range(8, 19)):
        bad.append("labels at %s, not 8..18" % labs)
    vals = [e for e in els if int(e["class"]) == ts.CLASS_VALUES]
    if len(vals) != 1 or int(vals[0]["src"]) != 3:
        bad.append("no values of layout 3")
    else:
        b = sec["blob"]["sprites"][int(vals[0]["frame0"])]
        w = int(b["w"]) if b else None
        if w != VALUES_B:
            bad.append("values of %r B, not rd_values_t's %d" % (w, VALUES_B))
    if not any(int(e["class"]) == ts.CLASS_PREVIEW for e in els):
        bad.append("no preview")
    if b'"wc-design"' not in open(path, "rb").read():
        bad.append("the design region is not carried")
    if len(raw) > BUDGET:
        bad.append("%d KB, past the %d KB the pool has beside the shelf's others" % (len(raw) // 1024, BUDGET // 1024))
    rc, out, err = H3.run(exe, "--dump3f", path, "radar")
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
    keep = os.environ.get("RADAR_BAKE_KEEP")
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
            print("%s %-12s %8d B %6s ms  %s" % ("ok  " if not bad else "FAIL", name, n, ms, "; ".join(bad)))
            fails += bool(bad)
    if "DONE" not in dom:
        print("FAIL the page did not finish (virtual time ran out, or it threw)")
        fails += 1
    print("%d themes baked, %d failed" % (seen, fails))
    return 1 if fails or not seen else 0


if __name__ == "__main__":
    sys.exit(main())
