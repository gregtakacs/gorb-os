#!/usr/bin/env python3
"""The flights editor's bake, checked in real Chrome (flight-tracker-design §12.3, E3).

    python3 apps/flights/editor/_selftest/bake_check.py

Stages bake.html beside the runtime's baker and fonts and the editor's art and bake in a
folder Windows Chrome can read, runs it headless, and checks every theme it prints: it
reads back through orbbundle and theme_schema, the shipped orb_clock3.c projects it under
the flights slug, and its rows are the ones apps/flights/main.c finds (the arm at slot 0,
fifteen planes of 49 picked frames, the marker rings at 16 and 17, four fonts, the header,
footer and range at 20, 21 and 23 and the card at 22 or, keeping clear, at 24, the preview),
the arm's vmax its turn in seconds (§12.5), with the design region carried.
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
STAGE_WIN = r"C:\Temp\flights-bake-check"
STAGE = "/mnt/c/Temp/flights-bake-check"
RUNTIME = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
EDITOR = os.path.dirname(HERE)
APPV = open(os.path.join(EDITOR, "..", "version.txt")).read().strip()


def stage():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    for src in (os.path.join(HERE, "bake.html"), os.path.join(EDITOR, "flights-art.js"),
                os.path.join(EDITOR, "flights-bake.js"), os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"),
                os.path.join(RUNTIME, "vendor", "fonts.css")):
        shutil.copy(src, STAGE)
    shutil.copytree(os.path.join(RUNTIME, "vendor", "fonts"), os.path.join(STAGE, "fonts"))


def run_chrome():
    r = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-sandbox",
                        "--user-data-dir=C:\\Temp\\flights-bake-profile", "--allow-file-access-from-files",
                        "--virtual-time-budget=300000", "--dump-dom",
                        "file:///" + STAGE_WIN.replace("\\", "/") + "/bake.html?appv=" + APPV],
                       capture_output=True, text=True, timeout=600)
    return r.stdout


def check(name, raw, exe, card, period):
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
    if len(fonts) != 4 or sorted(int(f["slot"]) for f in fonts) != [0, 1, 2, 3]:
        bad.append("fonts: %s" % [int(f["slot"]) for f in fonts])
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
    if not os.path.exists(CHROME):
        print("no Chrome at %s: this check runs where one is" % CHROME)
        return 1
    stage()
    dom = run_chrome()
    exe = H3.build()
    fails, seen = 0, 0
    keep = os.environ.get("FLIGHTS_BAKE_KEEP")    # a folder to keep the themes in, to look at
    for line in re.sub(r"<[^>]+>", "\n", dom).splitlines():
        if line.startswith("ERR "):
            print("FAIL " + line[4:])
            fails += 1
        elif line.startswith("B64 "):
            _, name, ms, card, period, b64 = line.split(" ", 5)
            seen += 1
            raw = base64.b64decode(b64)
            if keep:
                open(os.path.join(keep, name + ".orbtheme"), "wb").write(raw)
            bad, n = check(name, raw, exe, int(card), int(period))
            print("%s %-18s %8d B  %s" % ("ok  " if not bad else "FAIL", name, n, "; ".join(bad)))
            fails += bool(bad)
    if "DONE" not in dom:
        print("FAIL the page did not finish (virtual time ran out, or it threw)")
        fails += 1
    print("%d themes baked, %d failed" % (seen, fails))
    return 1 if fails or not seen else 0


if __name__ == "__main__":
    sys.exit(main())
