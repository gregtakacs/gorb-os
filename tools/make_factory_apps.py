#!/usr/bin/env python3
"""make_factory_apps.py — bake the hub's shelf .orbapp containers (D36).

Apps ship on the hub, not in the firmware (plan §0.11). Every directory under apps/
with an app.json is an app; the ones whose manifest says "shelf": true are written
into ``tools/orb-manager/apps/``, which the hub compiles in and seeds every user's
library from. Each container goes through apps/sdk/orbsdk.py, the same build and the
same writer (``orbapp.write_container``) an app outside this tree uses, so folding an
app in is copying its directory into apps/. Run it and commit the result whenever a
shelf container changes:

    python3 tools/make_factory_apps.py

The sentinel lint (tools/host_orbapp_install_test.py) re-runs this module in
--check mode and byte-compares, the same freshness rule ``bake_clock.py`` serves
for the clock themes, so a hand-edit of a seed file is a lint failure and never
a silently-stale app.

Deterministic by construction: the two containers carry no art, no code, and no
timestamps beyond ``baked_ms`` — which is pinned to a constant so a rebuild of
an unchanged spec re-emits identical bytes. Each app's header version and serial
come from its own apps/<slug>/version.txt (owner 2026-11-02: apps are versioned apart
from the OS), so the string an operator reads and the number the install ladder orders
on are one fact; a bump dirties that app's committed container, which the freshness
gate below then refuses to leave stale, and changed bytes under an unmoved stamp are
refused outright.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("orbapp", os.path.join(HERE, "orbapp.py"))
orbapp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(orbapp)

# S4: the clock's editor travels inside its own container. The region builder is
# its own tool (make_clock_region) because the composer sources are tracked and
# the region is derived bytes; importing it here keeps this file the one author
# of every factory container, so --check covers the region for free.
_cr = importlib.util.spec_from_file_location("make_clock_region",
                                             os.path.join(HERE, "make_clock_region.py"))
clock_region = importlib.util.module_from_spec(_cr)
_cr.loader.exec_module(clock_region)

# §43: an app's code is a wasm module built from apps/<slug>/ against apps/sdk and
# nothing else. orbsdk is the one compiler and packer of those modules; this file picks
# which apps ship and adds the editor regions only this tree can build.
_sdk = importlib.util.spec_from_file_location(
    "orbsdk", os.path.join(HERE, os.pardir, "apps", "sdk", "orbsdk.py"))
orbsdk = importlib.util.module_from_spec(_sdk)
_sdk.loader.exec_module(orbsdk)

APPS_DIR = os.path.normpath(os.path.join(HERE, os.pardir, "apps"))

OUT_DIR = os.path.join(HERE, "..", "tools", "orb-manager", "apps")

# The caps each app declares (D19: the declaration is the consent) are its own
# app.json's "caps". The clock ticks and chimes (D39), so it names sound; the picker owns
# nothing (its settings ride the free config cap); the flight tracker opens sockets and
# reads the home location, so it declares net and location.

# Each app carries its own release (owner, 2026-11-02): an app is atomic and independent
# of the OS, so its stamp is apps/<slug>/version.txt, read here and nowhere typed twice.
# The tree's own version.txt stamps the firmware image and nothing else; an app moves
# when its own bytes do, and an OS release leaves every app's stamp where it was.
#
# One stamp names one set of bytes (D43 ruling 9, now per app). A container whose bytes
# differ from the one committed at HEAD under the same stamp is refused, by name, until
# that app's version.txt moves: the hub serves its shelf copy of an editor only to an orb
# running the same stamp, so two different editors under one stamp would hand an author
# the wrong one. Rebuilding unchanged sources re-emits identical bytes and passes.
#
# The serial rides at major*10000 + minor*100 + patch so the numeric word the install
# ladder orders on and the string an operator reads are one fact stated once; the
# sentinel (host_orbapp_install_test.py) recomputes it from the same file.
def app_version(slug):
    """apps/<slug>/version.txt's stamp, verified against the one grammar the family uses."""
    return orbsdk.version(os.path.join(APPS_DIR, slug))


def committed(slug):
    """The container committed at HEAD for slug, or None (no git, or not committed yet)."""
    import subprocess
    rel = "tools/orb-manager/apps/%s.orbapp" % slug
    try:
        r = subprocess.run(["git", "-C", os.path.join(HERE, os.pardir), "show", "HEAD:" + rel],
                           capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def stale_stamps(files):
    """Apps whose bytes moved since HEAD while their stamp did not: [(slug, version)]."""
    out = []
    for slug, blob in files.items():
        old = committed(slug)
        if old is None or old == blob:
            continue
        try:
            same = orbapp.parse_header(old)["version"] == orbapp.parse_header(blob)["version"]
        except ValueError:
            same = False
        if same:
            out.append((slug, orbapp.parse_header(blob)["version"]))
    return out


def app_dirs():
    """slug -> manifest for every app in the tree. The directory name is the slug."""
    out = {}
    for d in sorted(os.listdir(APPS_DIR)):
        if not os.path.isfile(os.path.join(APPS_DIR, d, "app.json")):
            continue
        m = orbsdk.manifest(os.path.join(APPS_DIR, d))
        if m["slug"] != d:
            raise SystemExit("make_factory_apps: apps/%s/app.json names slug %r; an in-tree "
                             "app lives in the directory of its slug" % (d, m["slug"]))
        out[d] = m
    return out


# An app whose manifest says "shelf": false is built and checked with the rest, but kept
# off the hub's shelf until the owner releases it (2026-11-01: the flight tracker is not
# ready). `--unreleased DIR` writes those to DIR for a bench install; the shelf never
# carries them, and --check fails if one turns up there.

def editor_region(key):
    """The one place a factory container's editor region is built: app.json's "editor"
    names a builder this tree owns."""
    if key == "clock":
        return clock_region.make_region()
    if key:
        raise SystemExit("make_factory_apps: no editor builder named %r" % key)
    return b""


def containers():
    """Every app's container bytes, through orbsdk's build and the writer's checks."""
    return [(slug, m, orbsdk.container(os.path.join(APPS_DIR, slug),
                                       editor=editor_region(m.get("editor"))))
            for slug, m in app_dirs().items()]


def main():
    apps = containers()
    files = {slug: blob for slug, _m, blob in apps}
    held = {slug: files.pop(slug) for slug, m, _b in apps if not m.get("shelf")}
    args = sys.argv[1:]
    if "--unreleased" in args:
        i = args.index("--unreleased")
        if i + 1 >= len(args):
            sys.stderr.write("usage: make_factory_apps.py --unreleased DIR\n")
            return 2
        dest = args[i + 1]
        os.makedirs(dest, exist_ok=True)
        for slug, blob in held.items():
            with open(os.path.join(dest, slug + ".orbapp"), "wb") as f:
                f.write(blob)
        print("wrote unreleased %s to %s" % (", ".join(held), dest))
        return 0
    stale = stale_stamps(files)
    for slug, v in stale:
        sys.stderr.write("%s.orbapp's bytes changed since HEAD but apps/%s/version.txt still says %s: "
                         "bump it, so one stamp names one set of bytes\n" % (slug, slug, v))
    if "--check" in args:
        rc = 1 if stale else 0
        for slug in held:
            if os.path.exists(os.path.join(OUT_DIR, slug + ".orbapp")):
                sys.stderr.write("%s.orbapp is unreleased and must not ship on the shelf\n" % slug)
                rc = 1
        for slug, blob in files.items():
            path = os.path.join(OUT_DIR, slug + ".orbapp")
            have = open(path, "rb").read() if os.path.exists(path) else b""
            if have != blob:
                sys.stderr.write("%s.orbapp is stale — run tools/make_factory_apps.py\n" % slug)
                rc = 1
        if rc == 0:
            print("seed containers fresh (%d containers: %s)"
                  % (len(files), ", ".join("%s %d B" % (k, len(v)) for k, v in files.items())))
        return rc
    if stale:
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)
    for slug, blob in files.items():
        with open(os.path.join(OUT_DIR, slug + ".orbapp"), "wb") as f:
            f.write(blob)
    print("wrote %s: %s"
          % (os.path.relpath(OUT_DIR, os.path.join(HERE, os.pardir)),
             ", ".join("%s %s %d B" % (k, app_version(k), len(v)) for k, v in files.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
