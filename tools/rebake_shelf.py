#!/usr/bin/env python3
"""rebake_shelf.py — keep each app's shipped default theme baked by the app's own version.

The hub ships one default theme per themeable app (tools/orb-manager/themes/): Skeleton for
the clock, Modern for flights, Scope for the weather, Radarscope for the radar. Every bump of an app's version.txt re-bakes its default with
that release's editor (owner, 2026-11-05), so the shelf never offers a theme "made with" an
older editor than the app it themes. A stale shelf copy is how Modern read "made with v1.2.4"
beside flights v1.2.6 and offered an Update that changed nothing.

    python3 tools/rebake_shelf.py            # re-bake every stale default (any Chrome/Chromium)
    python3 tools/rebake_shelf.py clock      # re-bake one app's default, stale or not
    python3 tools/rebake_shelf.py --check    # no browser: each default's design names the
                                             # release in its app's version.txt

The bakes are the apps' own headless drivers, so a shelf theme is the bytes the editor's
Send would make: the clock's through _selftest/bank3-drive.html (with the editor's sound
engine, D39), flights' through _selftest/bake_check.py. Each new copy passes its gate before
it replaces the old one: make_default_theme.py's for Skeleton, bake_check.py's for Modern.
"""
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir))
sys.path.insert(0, HERE)
import orbbundle as ob          # noqa: E402
import orbchrome as oc          # noqa: E402

SHELF = os.path.join(ROOT, "tools", "orb-manager", "themes")
BAKE_TIMEOUT_S = 180
CLOCK_STAGE_NAME, CLOCK_PROFILE_NAME = "rebake-shelf-clock", "rebake-shelf-profile"

# app slug -> the shelf file of its default theme
DEFAULTS = {"clock": "skeleton.orbtheme", "flights": "flights-scope.orbtheme", "weather": "weather-scope.orbtheme",
            "radar": "radar-scope.orbtheme"}


def app_version(slug):
    return open(os.path.join(ROOT, "apps", slug, "version.txt")).read().strip()


def made_with(path):
    """The release named in the theme's design (its appVersion, build stamp dropped), or None."""
    d, _why = ob.read_design(open(path, "rb").read())
    if d is None:
        return None
    try:
        v = json.loads(d.decode("utf-8")).get("appVersion")
    except (ValueError, UnicodeDecodeError):
        return None
    return str(v).split("+")[0] if v else None


def stale(slug):
    """Why the app's shelf default needs a re-bake, or '' when it names the app's release."""
    path = os.path.join(SHELF, DEFAULTS[slug])
    if not os.path.exists(path):
        return "%s is missing" % os.path.relpath(path, ROOT)
    have, want = made_with(path), app_version(slug)
    if have != want:
        return "%s was made with %s %s, and %s is %s" % (DEFAULTS[slug], slug, have or "no release", slug, want)
    return ""


def bake_clock(v):
    """Skeleton through bank3-drive.html, staged by the selftest deployer (which carries the
    editor's own clock-logic.js, so the stage is the editor as it stands), then installed by
    make_default_theme.py, whose gate it must pass."""
    ed = os.path.join(ROOT, "apps", "clock", "editor")
    sys.path.insert(0, os.path.join(ed, "_selftest"))
    import deploy_test_folder as dep          # noqa: E402
    chrome = oc.find_chrome()
    stage = oc.stage_dir(CLOCK_STAGE_NAME, chrome)
    subprocess.run([sys.executable, os.path.join(ed, "_selftest", "deploy_test_folder.py"), stage], check=True, capture_output=True)
    # The bake takes seconds. A page that never goes idle (a Save dialog nobody answered, E7
    # Stage 1) holds headless Chrome's virtual clock still, so the budget never runs out and
    # only the wall clock ends it: three minutes, then orbchrome stops the browser it started
    # and everything it spawned (a leftover one holds the profile, and the next bake exits 21).
    url = dep.drive_url(stage, chrome) + "?dial=skeleton&name=Skeleton&appv=" + v
    profile = oc.profile_dir(CLOCK_PROFILE_NAME, chrome)
    try:
        r = oc.run_headless(chrome, url, profile, BAKE_TIMEOUT_S,
                            extra=["--autoplay-policy=no-user-gesture-required", "--virtual-time-budget=600000"])
    except subprocess.TimeoutExpired:
        print("  the bake failed: bank3-drive.html did not finish in %d s (the page never went idle: a dialog, a hung promise)" % BAKE_TIMEOUT_S)
        return False
    m = re.search(r'<pre id="out">(.*?)</pre>', r.stdout, re.S)
    text = (m.group(1) if m else r.stdout[-3000:]).replace("&gt;", ">").replace("&lt;", "<").replace("&amp;", "&")
    b64 = re.search(r"^B64:(\S+)", text, re.M)
    if "RESULT: PASS" not in text or not b64:
        print("  the bake failed:\n    " + re.sub(r"^B64:\S+", "", text, flags=re.M).strip()[-1500:].replace("\n", "\n    "))
        return False
    with tempfile.TemporaryDirectory(prefix="rebake.") as td:
        tmp = os.path.join(td, "Skeleton.orbtheme")
        open(tmp, "wb").write(base64.b64decode(b64.group(1)))
        return subprocess.run([sys.executable, os.path.join(HERE, "make_default_theme.py"), tmp]).returncode == 0


def bake_flights(v):
    """Every flights theme through bake_check.py (which stamps flights' version.txt and gates
    each one), keeping Modern."""
    with tempfile.TemporaryDirectory(prefix="rebake.") as keep:
        return _bake_flights(v, keep)


def _bake_flights(v, keep):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "apps", "flights", "editor", "_selftest", "bake_check.py")],
                       env=dict(os.environ, FLIGHTS_BAKE_KEEP=keep), capture_output=True, text=True)
    # flights' bake keeps its default under the bake's own name (modern.orbtheme, the look
    # titled Scope); the shelf files it as flights-scope since ae048c4
    got = os.path.join(keep, "modern.orbtheme")
    if r.returncode or not os.path.exists(got):
        print("  the bake failed:\n    " + (r.stdout + r.stderr).strip()[-1500:].replace("\n", "\n    "))
        return False
    if made_with(got) != v:
        print("  the bake names flights %s, not %s" % (made_with(got), v))
        return False
    shutil.copyfile(got, os.path.join(SHELF, DEFAULTS["flights"]))
    return True


def bake_weather(v):
    """Every weather theme through its bake_check.py (which stamps weather's version.txt and
    gates each one against the pool budget), keeping Scope (docs/weather-plan.md §2.4)."""
    with tempfile.TemporaryDirectory(prefix="rebake.") as keep:
        r = subprocess.run([sys.executable, os.path.join(ROOT, "apps", "weather", "editor", "_selftest", "bake_check.py")],
                           env=dict(os.environ, WEATHER_BAKE_KEEP=keep), capture_output=True, text=True)
        got = os.path.join(keep, "scope.orbtheme")
        if r.returncode or not os.path.exists(got):
            print("  the bake failed:\n    " + (r.stdout + r.stderr).strip()[-1500:].replace("\n", "\n    "))
            return False
        if made_with(got) != v:
            print("  the bake names weather %s, not %s" % (made_with(got), v))
            return False
        shutil.copyfile(got, os.path.join(SHELF, DEFAULTS["weather"]))
        return True


def bake_radar(v):
    """The radar's starters through its bake_check.py (which stamps radar's version.txt and
    gates each one against the pool's room), keeping Scope (docs/radar-plan.md §2.5)."""
    with tempfile.TemporaryDirectory(prefix="rebake.") as keep:
        r = subprocess.run([sys.executable, os.path.join(ROOT, "apps", "radar", "editor", "_selftest", "bake_check.py")],
                           env=dict(os.environ, RADAR_BAKE_KEEP=keep), capture_output=True, text=True)
        got = os.path.join(keep, "scope.orbtheme")
        if r.returncode or not os.path.exists(got):
            print("  the bake failed:\n    " + (r.stdout + r.stderr).strip()[-1500:].replace("\n", "\n    "))
            return False
        if made_with(got) != v:
            print("  the bake names radar %s, not %s" % (made_with(got), v))
            return False
        shutil.copyfile(got, os.path.join(SHELF, DEFAULTS["radar"]))
        return True


BAKE = {"clock": bake_clock, "flights": bake_flights, "weather": bake_weather, "radar": bake_radar}


def main():
    args = sys.argv[1:]
    if args == ["--check"]:
        bad = [s for s in (stale(slug) for slug in DEFAULTS) if s]
        for b in bad:
            sys.stderr.write("refused: %s — python3 tools/rebake_shelf.py re-bakes it\n" % b)
        if not bad:
            print("shelf defaults current — " + ", ".join("%s %s" % (DEFAULTS[s], app_version(s)) for s in DEFAULTS))
        return 1 if bad else 0
    unknown = [a for a in args if a not in DEFAULTS]
    if unknown:
        sys.stderr.write("usage: rebake_shelf.py [--check | %s ...]\n" % " | ".join(DEFAULTS))
        return 2
    if oc.find_chrome() is None:
        sys.stderr.write(oc.missing() + ": the bakes run in a browser\n")
        return 1
    todo = args or [s for s in DEFAULTS if stale(s)]
    if not todo:
        print("every shelf default is current")
        return 0
    fails = 0
    for slug in todo:
        v = app_version(slug)
        print("re-baking %s for %s %s…" % (DEFAULTS[slug], slug, v))
        ok = BAKE[slug](v)
        print(("  ok: " if ok else "  FAILED: ") + DEFAULTS[slug] + (" made with %s" % made_with(os.path.join(SHELF, DEFAULTS[slug])) if ok else ""))
        fails += not ok
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
