#!/usr/bin/env python3
"""make_factory_apps.py — bake the hub-seeded .orbapp containers (D36).

The clock and the HSB editor ship on the hub, not in the firmware (plan §0.11):
this tool is the one author of those two containers, and it lays each one down
through the SAME writer a card file uses (``orbapp.write_container``) into
``tools/orb-manager/seed/``, where the hub embeds the bytes and its seed route
pushes them to the dial through the ordinary doors. Run it and commit the
result whenever a seed container changes:

    python3 tools/make_factory_apps.py

The sentinel lint (tools/host_orbapp_install_test.py) re-runs this module in
--check mode and byte-compares, the same freshness rule ``bake_clock.py`` serves
for the clock themes, so a hand-edit of a seed file is a lint failure and never
a silently-stale app.

Deterministic by construction: the two containers carry no art, no code, and no
timestamps beyond ``baked_ms`` — which is pinned to a constant so a rebuild of
an unchanged spec re-emits identical bytes. The header serial rides at 1: the
containers land on a board whose ledger is empty or whose owner removed the
row, and a re-seed of an equal serial is the installer’s quiet no-op.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("orbapp", os.path.join(HERE, "orbapp.py"))
orbapp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(orbapp)

OUT_DIR = os.path.join(HERE, "..", "tools", "orb-manager", "seed")

# Behaviour kind ids must match main/app/orb_app.h's enum (C6 append-only).
KIND_COLOR_PICKER = 1
KIND_CLOCK = 3

# The one owned caps each factory app declares (D19: the declaration is the
# consent). The clock walks the card for theme containers and asks for the D11
# install, so it names sd_read|themes; the picker owns nothing — its three
# settings ride the free config cap, which is not declarable (CAP_FREE).
CLOCK_CAPS = orbapp.cap_bit("sd_read") | orbapp.cap_bit("themes")
PICKER_CAPS = 0

SERIAL = 1          # bump to re-install the pair in place on next boot
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

APPS = (
    # slug, title, kind, caps, settings
    ("clock",        "Clock", KIND_CLOCK,        CLOCK_CAPS,  ()),
    ("color_picker", "HSB",   KIND_COLOR_PICKER, PICKER_CAPS, PICKER_SETTINGS),
)


def containers():
    """Build each container's bytes with the writer's full ladder of checks.

    write_container derives the filename from the header's slug and insists the
    stem match, so the bytes are authored into a temp dir under the slug's own
    name and read back — the exact card-path artifact, minus the card.
    """
    import tempfile
    out = []
    with tempfile.TemporaryDirectory() as td:
        for slug, title, kind, caps, settings in APPS:
            orbapp.write_container(
                os.path.join(td, slug + ".orbapp"), slug, title, caps=caps,
                settings=settings, behaviour_kind=kind, serial=SERIAL,
                baked_ms=BAKED_MS)
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
    if "--check" in sys.argv[1:]:
        rc = 0
        for slug, blob in files.items():
            path = os.path.join(OUT_DIR, slug + ".orbapp")
            have = open(path, "rb").read() if os.path.exists(path) else b""
            if have != blob:
                sys.stderr.write("%s.orbapp is stale — run tools/make_factory_apps.py\n" % slug)
                rc = 1
        if rc == 0:
            print("seed containers fresh (%d containers, %d + %d B)"
                  % (len(files), len(files["clock"]), len(files["color_picker"])))
        return rc
    os.makedirs(OUT_DIR, exist_ok=True)
    for slug, blob in files.items():
        with open(os.path.join(OUT_DIR, slug + ".orbapp"), "wb") as f:
            f.write(blob)
    print("wrote %s: clock %d B, color_picker %d B, serial %u"
          % (os.path.relpath(OUT_DIR, os.path.join(HERE, "..")),
             len(files["clock"]), len(files["color_picker"]), SERIAL))
    return 0


if __name__ == "__main__":
    sys.exit(main())
