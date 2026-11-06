#!/usr/bin/env python3
"""bake_presets.py — bake the clock editor's templates into .orbtheme files (plan §49.3).

The bench themes are the editor's own PRESETS, baked through the real exportTheme3 in
headless Chrome on the Windows side (the only place the composer's pixel path runs),
exactly as tools/make_default_theme.py documents for the shipped Skeleton. One file per
preset lands in OUT as <preset>.orbtheme, plus <preset>.status with the bake's own status
sentence, which carries estimate3's price of the bytes ("est N fps (...)").

Usage: tools/bench/bake_presets.py OUT [preset ...]     # default: every preset
Re-run deploy_test_folder.py first if the editor or the baker changed (this does it).
"""
import base64
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CHROME = "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe"
PAGE = "file:///C:/Users/greg/Downloads/test/_selftest/bank3-drive.html?dial=%s"
EXTRA = os.environ.get("BAKE_QUERY", "")   # e.g. "favour=gears" or "tempo=8" (bank3-drive.html)


def presets():
    src = open(os.path.join(ROOT, "apps", "clock", "editor", "watch-engine.js")).read()
    body = src[src.index("const PRESETS = {"):]
    body = body[:body.index("\n  };")]
    return re.findall(r"^    ([a-z0-9_]+): \{ name:", body, re.M)


def bake(pid, out):
    try:
        r = _chrome(pid)
    except subprocess.TimeoutExpired:
        # one preset that never settles fails alone; the run goes on to the next
        return False, "timed out after 900 s"
    return _parse(pid, out, r)


def _chrome(pid):
    return subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-sandbox",
                        "--user-data-dir=C:\\Temp\\wc-bench-%d" % os.getpid(), "--virtual-time-budget=600000",
                        "--dump-dom", PAGE % pid + ("&" + EXTRA if EXTRA else "")],
                       cwd="/mnt/c", capture_output=True, text=True, timeout=900)


def _parse(pid, out, r):
    dom = r.stdout
    m = re.search(r"B64:([A-Za-z0-9+/=]+)", dom)
    st = re.search(r"status: ([^\n<]*)", dom)
    p4 = re.findall(r"PLAN4 (\{[^\n<]*)", dom)          # the output's line, not the page's own script
    p4 = p4[-1] if p4 else None
    res = re.search(r"RESULT: (PASS|FAIL[^\n<]*)", dom)
    if not m or not res or not res.group(1).startswith("PASS"):
        return False, (res.group(1) if res else "no result") + " | " + (st.group(1) if st else "")
    with open(os.path.join(out, pid + ".orbtheme"), "wb") as f:
        f.write(base64.b64decode(m.group(1)))
    with open(os.path.join(out, pid + ".status"), "w") as f:
        f.write(st.group(1) if st else "")
    lv = re.findall(r"LIVE4 ((?:\{|none)[^\n<]*)", dom)
    if lv:
        with open(os.path.join(out, pid + ".live4"), "w") as f:
            f.write(lv[-1].replace("&quot;", '"'))
    if p4:
        with open(os.path.join(out, pid + ".plan4"), "w") as f:
            f.write(p4.replace("&quot;", '"'))
    return True, st.group(1) if st else ""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ROOT, "apps", "clock", "editor", "deploy_test_folder.py")],
                   check=True, capture_output=True)
    ids = sys.argv[2:] or presets()
    bad = 0
    for pid in ids:
        ok, why = bake(pid, out)
        bad += not ok
        print("%-10s %s  %s" % (pid, "ok  " if ok else "FAIL", why[:220]), flush=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
