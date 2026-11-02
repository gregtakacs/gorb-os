#!/usr/bin/env python3
"""make_factory_apps.py — bake the hub-seeded .orbapp containers (D36).

The clock and the HSB editor ship on the hub, not in the firmware (plan §0.11):
this tool is the one author of those two containers, and it lays each one down
through the SAME writer a card file uses (``orbapp.write_container``) into
``tools/orb-manager/starter/``, where they wait as committed artifacts an
operator copies to a card (S5 deleted the hub's embedded copy and its seed
route; the files outlived the route). Run it and commit the result whenever a
starter container changes:

    python3 tools/make_factory_apps.py

The sentinel lint (tools/host_orbapp_install_test.py) re-runs this module in
--check mode and byte-compares, the same freshness rule ``bake_clock.py`` serves
for the clock themes, so a hand-edit of a seed file is a lint failure and never
a silently-stale app.

Deterministic by construction: the two containers carry no art, no code, and no
timestamps beyond ``baked_ms`` — which is pinned to a constant so a rebuild of
an unchanged spec re-emits identical bytes. The header serial is DERIVED from
version.txt (§4.1), so the string an operator reads and the number the install
ladder orders on are one fact; the same read makes a version bump dirty the
committed pair, which the freshness gate below then refuses to leave stale.
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


OUT_DIR = os.path.join(HERE, "..", "tools", "orb-manager", "apps")

# Behaviour kind ids must match main/app/orb_app.h's enum (C6 append-only).
KIND_COLOR_PICKER = 1
KIND_CLOCK = 3
KIND_FLIGHTS = 4

# The one owned caps each factory app declares (D19: the declaration is the
# consent). The clock walks the card for theme containers and asks for the D11
# install, so it names sd_read|themes, and it ticks and chimes (D39), so it names
# sound — revocable on the dial like the other two; the picker owns nothing — its three
# settings ride the free config cap, which is not declarable (CAP_FREE).
CLOCK_CAPS = orbapp.cap_bit("sd_read") | orbapp.cap_bit("themes") | orbapp.cap_bit("sound")
PICKER_CAPS = 0
# The flight tracker opens sockets (the OS feed, docs/flight-tracker-design.md),
# so it declares net: an app that declared nothing never reaches the network.
FLIGHTS_CAPS = orbapp.cap_bit("net") | orbapp.cap_bit("location")

# S6 (§4.1): the release stamp both starter containers carry, and the serial derived
# from it. The number is not typed here. It is read out of version.txt — the same file
# root CMakeLists.txt stamps into the image through tools/orb_version.sh — because these
# two containers are compiled from sources this repo tracks (the clock's behaviour
# module is main/clock/, and the picker's editor is built by make_clock_region.py from
# composer sources under version control), so the release they belong to IS the release
# of the tree. A VERSIONS table typed beside the script would be a second source, and the
# two would drift the first time a release bumped one and not the other.
#
# The consequence is deliberate and loud: bumping version.txt makes the committed
# containers stale, and host_orbapp_install_test.py's --check re-run says so until
# `make_factory_apps.py` is re-run and the new bytes committed. That is the same rule
# D37's editor region already runs under, and orbpack's refusal already spells out
# ("Write version.txt, reconfigure, rebuild").
#
# The serial rides at major*10000 + minor*100 + patch so the numeric word the install
# ladder orders on and the string an operator reads are one fact stated once. It is not
# 1 any more, and the sentinel that pinned 1 now recomputes it from version.txt rather
# than trading one literal for another.
def app_version():
    """version.txt's stamp, verified against the one grammar the family uses."""
    path = os.path.join(HERE, os.pardir, "version.txt")
    try:
        v = open(path).read().strip()
    except OSError:
        raise SystemExit("make_factory_apps: no version.txt at %s — every container needs "
                         "a release stamp, and this tool reads the tree's rather than "
                         "typing a second one" % path)
    if not orbapp.check_version(v):
        raise SystemExit("make_factory_apps: version.txt holds %r, which is not a release "
                         "stamp the format accepts (v<MAJOR>.<MINOR>.<PATCH>, each part 1 "
                         "or 2 digits, no leading zero)" % v)
    return v


VERSION = app_version()
SERIAL = orbapp.serial_of_version(VERSION)
BAKED_MS = 0        # pinned constant: the file is content, not a build event

# The picker's three state numbers, declared so they persist inside the row's
# own NVS namespace exactly like any card app's config (C10). The defaults are
# the values color_picker.c's statics carried before D35 — the seed the "mover"
# needs, and the reason there is no migration code.
PICKER_SETTINGS = (
    {"id": "hue", "type": "int", "min": 0, "max": 359, "default": 210},
    {"id": "sat", "type": "int", "min": 0, "max": 100, "default": 80},
    {"id": "bri", "type": "int", "min": 0, "max": 100, "default": 90},
)

# The tracker's settings (design §6.3). Home is not among them: it is the OS
# home location (§6.4), read through the location cap. Enums are indices the
# view names: range 10/20/40/80 nm, source adsb.lol/adsb.fi, bg off/1/2/5 min.
# The defaults are the light ones (2026-11-01, after api.adsb.lol banned the
# owner's IP): adsb.fi first, background every 2 min, boost off. On the glass
# the poll is 15 s whatever these say.
FLIGHTS_SETTINGS = (
    {"id": "range",  "type": "enum",   "min": 0, "max": 3, "default": 2},
    {"id": "source", "type": "enum",   "min": 0, "max": 1, "default": 1},
    {"id": "bg",     "type": "enum",   "min": 0, "max": 3, "default": 2},
    {"id": "boost",  "type": "toggle", "default": 0},
    {"id": "ground", "type": "toggle", "default": 1},
    {"id": "key",    "type": "string", "def_str": ""},
)

APPS = (
    # slug, title, kind, caps, settings, editor-region bytes (b"()" when none)
    ("clock",        "Clock", KIND_CLOCK,        CLOCK_CAPS,  (), "_clock_editor"),
    ("color_picker", "HSB",   KIND_COLOR_PICKER, PICKER_CAPS, PICKER_SETTINGS, None),
    ("flights",      "Flights", KIND_FLIGHTS,    FLIGHTS_CAPS, FLIGHTS_SETTINGS, None),
)

# Built and checked with the rest, but kept off the hub's shelf until the owner
# releases it (2026-11-01: the flight tracker is not ready). `--unreleased DIR`
# writes these to DIR for a bench install; the shelf never carries them, and
# --check fails if one turns up there.
UNRELEASED = ("flights",)


def editor_region(key):
    """The one place a factory container's editor region is built."""
    if key == "_clock_editor":
        return clock_region.make_region()
    return b""


def containers():
    """Build each container's bytes with the writer's full ladder of checks.

    write_container derives the filename from the header's slug and insists the
    stem match, so the bytes are authored into a temp dir under the slug's own
    name and read back — the exact card-path artifact, minus the card.
    """
    import tempfile
    out = []
    with tempfile.TemporaryDirectory() as td:
        for slug, title, kind, caps, settings, editor in APPS:
            orbapp.write_container(
                os.path.join(td, slug + ".orbapp"), slug, title, caps=caps,
                settings=settings, behaviour_kind=kind, serial=SERIAL,
                baked_ms=BAKED_MS, editor=editor_region(editor), version=VERSION)
            with open(os.path.join(td, slug + ".orbapp"), "rb") as f:
                blob = f.read()
            out.append((slug, title, kind, caps, blob))
    return out



def emit_files(apps):
    lines = {}
    for slug, title, kind, caps, blob in apps:
        lines[slug] = blob
    return lines


def main():
    apps = containers()
    files = emit_files(apps)
    held = {k: files.pop(k) for k in UNRELEASED}
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
    if "--check" in args:
        rc = 0
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
    os.makedirs(OUT_DIR, exist_ok=True)
    for slug, blob in files.items():
        with open(os.path.join(OUT_DIR, slug + ".orbapp"), "wb") as f:
            f.write(blob)
    print("wrote %s: %s, version %s (serial %u)"
          % (os.path.relpath(OUT_DIR, os.path.join(HERE, os.pardir)),
             ", ".join("%s %d B" % (k, len(v)) for k, v in files.items()), VERSION, SERIAL))
    return 0


if __name__ == "__main__":
    sys.exit(main())
