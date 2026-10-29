#!/usr/bin/env python3
"""make_default_theme.py — install and verify the theme the hub ships as its default (D37).

The default theme is what `installTheme` sends onto a board with no face, so it is the
first theme an owner ever sees and the one row the Themes face must picture. The Aviator
seed it replaces was lifted out of `assets/orb_default.img` by `make_seed_theme.py`, and
that image carries no preview plane: the shipped default answered
`/api/v1/sd/preview` with a 404 and the Themes card showed an empty frame beside the name.
A default that cannot picture itself reads as a broken product, not a smaller theme.

So the default ships as a Composer bake named `Skeleton`. The bake runs in real Chrome,
because the plane is composed pixels and `bake_clock.py` refuses class 9 by design ("the
compiled pair has no field for a thumbnail"). `--check` never re-runs it, so the sentinel
stays runnable on a box with no browser; the committed container is the artifact, exactly
as the firmware bundle under `internal/firmware/bundle/` is.

    python3 tools/watch-composer/deploy_test_folder.py
    '/mnt/c/Program Files/Google/Chrome/Application/chrome.exe' \\
      --headless=new --disable-gpu --no-sandbox --user-data-dir='C:\\Temp\\wc' \\
      --virtual-time-budget=600000 --dump-dom \\
      'file:///C:/Users/greg/Downloads/test/_selftest/bank3-drive.html?dial=skeleton&name=Skeleton' \\
      >/tmp/bake.out 2>&1 &
    # lift the B64: line out of the dumped <pre id="out"> into one file, then:
    python3 tools/make_default_theme.py /tmp/skeleton.orbtheme   # verify, then install
    python3 tools/make_default_theme.py --check                  # verify the committed copy

Each check is a promise the shelf depends on: FORMAT 3 with one `clock` section, the
embedded name equal to the expected default (the device files a theme under a stem derived
from the NAME and ignores the filename, so a shelf file whose two disagree is a row no
Themes face can match), and one opaque square PREVIEW row at the bank's running end. A file
that fails any of them is refused by name rather than installed quietly.

The gate reads the element table rather than hashing the file, and that is deliberate: a
re-bake of the same preset is NOT byte-identical to the committed copy. Seven bytes move —
the `baked_ms` wall-clock stamp at header word 12 and the header CRC over it at word 28 —
while the art CRC, the geometry CRC, the serial and the name all match exactly. A byte-pin
would go red on every re-bake for a reason nobody should care about, and would hide the
one change that matters: a default that lost its preview row.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orbbundle as ob          # noqa: E402
import orbtheme as _ot          # noqa: E402
import theme_schema as ts       # noqa: E402

ROOT = os.path.join(HERE, os.pardir)
OUT = os.path.join(ROOT, "tools", "orb-manager", "themes", "skeleton.orbtheme")

# The default theme's identity, spelled once. The shelf filename, the container's own name
# field and these constants must agree, and check() enforces that instead of trusting the
# bake that produced them.
THEME_NAME = "Skeleton"
THEME_SECTION = "clock"
PANEL_PX = 466                    # the glass, so the preview's ceiling is its half
PREVIEW_MAX_PX = PANEL_PX // 2    # the picker reads one plane into a half-res buffer
FIRMWARE_ABIS = (3, 4)            # orb_bundle.c reads GEOM_ABI 3 and 4 and nothing else
ART_CAP = 8 << 20                 # one 8 MB slot, the installer's own ceiling


def _fail(what, detail=""):
    sys.stderr.write("refused: %s%s\n" % (what, (" — " + detail) if detail else ""))
    return 1


def walk(path):
    """The container's header plus each section's decoded element table, read the way the
    device reads it. Every checksum read_bundle makes is part of the gate: a torn copy of a
    good theme must not install."""
    h, secs = ob.read_bundle(path)
    raw = open(path, "rb").read()
    region = raw[h["geom_off"]:h["geom_off"] + h["geom_size"]]
    _, recs = ob.unpack_index(region)
    blobs = []
    for r in recs:
        lo = h["geom_off"] + r["geom_off"]
        blobs.append(ob.unpack_blob(raw[lo:lo + r["geom_size"]], r["abi_max"]))
    return h, secs, recs, blobs


def check(path):
    """The whole gate, in the order a reader hits it. Answers (rc, one-line report)."""
    rel = os.path.relpath(path, ROOT)
    if not os.path.exists(path):
        return _fail("%s does not exist — bake it and name it, or run without --check" % rel), ""
    try:
        h, secs, recs, blobs = walk(path)
    except Exception as e:
        # The reader's own sentence. A copy the device would reject is not a stale default,
        # and calling it stale would send the next person to the wrong tool.
        return _fail("the container does not read", str(e)), ""

    if h["fmt"] != ob.FORMAT3:
        return _fail("a shipped default must be FORMAT %d, this is FORMAT %d"
                     % (ob.FORMAT3, h["fmt"])), ""
    if h["abi"] not in FIRMWARE_ABIS:
        return _fail("the firmware reads geometry v%s and this container is stamped v%d"
                     % (" and ".join(str(a) for a in FIRMWARE_ABIS), h["abi"])), ""
    if len(secs) != 1:
        return _fail("a theme file carries exactly one section, this carries %d" % len(secs)), ""
    if h["name"] != THEME_NAME:
        return _fail("the shipped default must carry the name %r, it carries %r"
                     % (THEME_NAME, h["name"]),
                     "the device files the card copy under the NAME, so a file that "
                     "disagrees is a row the Themes face can never match"), ""
    if len(recs) != 1 or blobs[0]["slug"] != THEME_SECTION:
        return _fail("the shipped default themes %r and this section is %r"
                     % (THEME_SECTION, blobs[0]["slug"])), ""
    if h["art_size"] > ART_CAP:
        return _fail("banks of %d B pass the %d B one-slot ceiling the installer enforces"
                     % (h["art_size"], ART_CAP)), ""

    els = blobs[0]["elements"]
    prev = [e for e in els if e["class"] == ts.CLASS_PREVIEW]
    if not prev:
        return _fail("the default theme carries no preview row",
                     "the Themes card and the dial's own picker both picture a theme from "
                     "the class %d plane — the gap that made the Aviator seed read as "
                     "broken. Bake through the Composer, which attaches one"
                     % ts.CLASS_PREVIEW), ""
    if len(prev) > 1:
        return _fail("a theme carries one preview plane, this names %d" % len(prev)), ""
    p = prev[0]
    if p["n_frames"] != 1:
        return _fail("a preview is one plane, this row carries %d frames" % p["n_frames"]), ""
    if p["w"] != p["h"]:
        return _fail("a preview is square because the OS card slot is square, this is %dx%d"
                     % (p["w"], p["h"])), ""
    if not 0 < p["w"] <= PREVIEW_MAX_PX:
        return _fail("a preview of %d px on a %d px panel is past the half-res buffer the "
                     "picker reads into" % (p["w"], PANEL_PX)), ""
    spr = blobs[0]["sprites"][p["frame0"]]
    want = p["w"] * p["h"] * 2
    if spr["size"] != want:
        return _fail("the preview plane costs %d B and RGB565 at %dx%d costs %d"
                     % (spr["size"], p["w"], p["h"], want)), ""
    # Order, not just extents: a plane inserted ahead of authored rows shifts every frame0
    # after it, and the checks above would still pass on the shifted numbers.
    if p["frame0"] != len(blobs[0]["sprites"]) - 1:
        return _fail("the preview row is not the bank's last extent, so it displaced a row "
                     "authored before it"), ""

    shelf = os.path.splitext(os.path.basename(path))[0]
    if shelf.lower() != h["name"].lower():
        return _fail("the shelf file is %r and the theme is named %r" % (shelf, h["name"]),
                     "the card stem is %s, and the Themes face lines the row up on the name"
                     % _ot.card_stem(blobs[0]["slug"], h["name"])), ""
    return 0, ("%s: FORMAT %d abi %d, %d elements, preview %dx%d %d B, art %d B, serial %d"
               % (shelf, h["fmt"], h["abi"], len(els), p["w"], p["h"], spr["size"],
                  h["art_size"], h["serial"]))


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    if "--check" in sys.argv[1:]:
        rc, report = check(OUT)
        if rc == 0:
            print("default theme shippable — " + report)
        return rc
    if len(args) > 1:
        sys.stderr.write("usage: make_default_theme.py [--check | BAKED.orbtheme]\n")
        return 2
    src = args[0] if args else os.path.join("/tmp", THEME_NAME.lower() + ".orbtheme")
    rc, report = check(src)
    if rc:
        return rc
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    # Copied whole rather than streamed: a half-written default is the one library file a
    # fresh board reaches for mid-provisioning, and a torn copy seeds a refusal.
    shutil.copyfile(src, OUT)
    print("installed " + report)
    print("wrote %s (%d B) from %s" % (os.path.relpath(OUT, ROOT), os.path.getsize(OUT), src))
    return 0


if __name__ == "__main__":
    sys.exit(main())

