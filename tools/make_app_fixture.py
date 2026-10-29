#!/usr/bin/env python3
"""Writes the .orbapp fixtures every non-Python reader is pinned against.

D37 put a fourth reader in the family. tools/orbapp.py remains the authority — it is
the only writer — but main/app/orb_app_manifest.c reads the header on the dial, and
tools/orb-manager/internal/orbnet/appcontainer.go now reads it in the hub to answer
"which apps does this binary ship, and does this one carry an editor". Multiple
readers of one layout is the condition tools/host_art_residency_test.py exists to
police, and the theme family already solved it this way: a fixture written by the
authority, a .json beside it holding the authority's OWN read-back of those bytes,
and each reader pointed at the pair rather than at a hand-written expectation. A
reader that disagrees then fails against a number the writer produced.

Two containers, chosen to be the two shapes the hub must distinguish:

  * app-editor.orbapp — the D37 shape: the `themes` cap (so it must surface as a
    theme surface with zero themes written), a net cap, one setting, one asset row,
    a nonzero serial and baked_ms, and a real editor region carrying editor.json. A
    fixture of zeroes everywhere would let a parser that never learned the editor
    words pass by accident.
  * app-plain.orbapp — the shape before D37: no region at all (offset, size and CRC
    all zero, not derived), no themes cap, no settings, no assets. The hub must call
    this one unthemed and uneditable.

serial is nonzero and distinct from baked_ms on purpose. The identity tuple the dial
matches uploads on (orb_app_install.c: "geom_crc + the code pair + serial + slug")
reads serial, so a fixture where serial == baked_ms would let a reader that mixed the
two words pass.

Both stamps are fixed, not clocked: a fixture that changed on every run could not be
the one file several readers are pinned against, and its diff would say nothing about
whether a reader broke.

Re-bake:   python3 tools/make_app_fixture.py
Then gate: python3 tools/make_app_fixture.py --verify
"""
import io
import json
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbapp

FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                   "tests", "fixtures")
SIDE = os.path.join(FIX, "app-pair.json")

# The word-map keys the sidecar records. Named here rather than reflected off the module
# so an added word shows up as a missing sidecar key, which is a loud failure, rather than
# as a reader that quietly started comparing against a different layout.
WORD_KEYS = ("magic", "format", "geom_abi", "hdr_size", "serial", "baked_ms",
             "abi_min", "abi_max", "sig", "app_id", "slug", "title", "behaviour",
             "art_req", "n_layer", "n_comp", "n_aux", "n_bank", "period_ms", "caps",
             "n_settings", "set_off", "geom_off", "geom_size", "geom_crc", "art_off",
             "art_size", "art_crc", "code_off", "code_size", "code_crc", "wasm_mem",
             "editor_off", "editor_size", "editor_crc", "version", "hdr_crc",
             "asset_off", "n_assets")

SERIAL = 20261027        # distinct from BAKED_MS: see the docstring
# A duration, not an epoch: the app header keeps baked_ms in one u32 word and the
# writer refuses anything wider (orbapp._u), where the theme header's twin is a
# masked 64-bit value. A fixture that could not be written is not a fixture.
BAKED_MS = 1761400

# The region, built by hand so its bytes are reproducible. zipfile stamps an mtime and
# an OS byte by default, and this archive's CRC lands in a container header the dial
# checksums — a fixture drifting by two bytes of timestamp would read as a reader bug.
# Entries are therefore sorted, their timestamps pinned to the ZIP epoch, and each
# carries the same external attributes. The entries are STORED, not deflated, because
# that is the rule the format's own doc states (plan §5.1): the firmware never reads the
# region, but the hub seeks one member at a time, and a seekable uncompressed archive is
# what lets it do that without unpacking the whole thing.
EDITOR_FILES = (
    ("editor.json", b'{"entry": "index.html", "title": "Sentinel Composer"}\n'),
    ("index.html", b"<!doctype html><title>Sentinel Composer</title>"
                   b"<div id=root></div><script src=ui.js></script>\n"),
    ("ui.js", b"// app-owned editor UI. The bridge posts orbtheme:save.\n"),
)


def editor_region():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for name, data in sorted(EDITOR_FILES):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    return buf.getvalue()


# The two containers, stated once and used by both the bake and the --verify leg, so
# the reproduction check cannot quietly differ from what was committed.
# S6 (§4.1): one fixture per version path, the same reason one carries an editor region
# and the other does not. `sentinel` stamps v1.4.2-rc.1+build.7 — a prerelease AND build
# metadata at once, the widest legal stamp the grammar admits, so a reader that stops at
# the third number or that mishandles the '+' is caught here rather than on a dial.
# `plain` stamps nothing and must read back as "" and not as a refusal.
THEMED = dict(slug="sentinel", title="Sentinel",
              caps=orbapp.cap_bit("net") | orbapp.cap_bit("themes"),
              period_ms=5000, serial=SERIAL, baked_ms=BAKED_MS,
              version="v1.4.2-rc.1+build.7",
              settings=[{"id": "ring", "type": "int", "min": 0, "max": 12,
                         "default": 3}],
              assets=[{"cls": "hand", "name": "minute", "x": 8, "y": 8,
                       "w": 120, "h": 120}])
PLAIN = dict(slug="plain", title="Plain", caps=orbapp.cap_bit("net"),
             serial=SERIAL + 1, baked_ms=BAKED_MS)


def bake(path, spec, region=b""):
    """Write one container, returning where it actually landed.

    write_container derives the filename from the header's own slug — a writer that
    honoured the caller's name could be talked into filing slug "bar" as
    "foo.orbapp" — so the path handed in is a directory hint, not the answer.
    """
    orbapp.write_container(path, spec["slug"], spec["title"], caps=spec["caps"],
                           period_ms=spec.get("period_ms", 0), serial=spec["serial"],
                           baked_ms=spec["baked_ms"], settings=spec.get("settings", ()),
                           assets=spec.get("assets", ()), editor=region,
                           version=spec.get("version", ""))
    return os.path.join(os.path.dirname(str(path)) or ".", spec["slug"] + ".orbapp")


def read_back(path):
    """The authority's own words, off the bytes it just wrote.

    The region is sliced from the file rather than taken off read_container's tuple:
    that tuple's fourth member is ART, which D21 made permanently empty, and there is
    no fifth member for the editor yet. The header's own editor_off is an absolute
    file offset and editor_size its length, which is all a reader needs — and the
    absence of an accessor is what step 2 of D37's plan adds for the install test.
    """
    h, settings, geom, _art = orbapp.read_container(path)
    raw = open(path, "rb").read()
    off, size = h["editor_off"], h["editor_size"]
    editor = raw[off:off + size] if size else b""
    if size and len(editor) != size:
        raise SystemExit("fixture region is short: header says %d B, file ends at %d"
                         % (size, len(raw) - off))
    names = []
    if editor:
        with zipfile.ZipFile(io.BytesIO(editor)) as z:
            names = sorted(z.namelist())
    return {
        "slug": h["slug"], "title": h["title"],
        "caps": h["caps"], "caps_named": orbapp.caps_names(h["caps"]),
        "abi": h["abi"], "abi_min": h["abi_min"], "abi_max": h["abi_max"],
        "format": h["fmt"], "serial": h["serial"], "baked_ms": h["baked_ms"],
        "app_id": h["app_id"], "behaviour_kind": h["behaviour_kind"],
        "period_ms": h["period_ms"], "hdr_size": h["hdr_size"],
        "geom_off": h["geom_off"], "geom_size": h["geom_size"],
        "geom_crc": h["geom_crc"], "art_off": h["art_off"],
        "art_size": h["art_size"], "art_crc": h["art_crc"],
        "code_off": h["code_off"], "code_size": h["code_size"],
        "code_crc": h["code_crc"], "wasm_mem_max": h["wasm_mem_max"],
        "editor_off": h["editor_off"], "editor_size": h["editor_size"],
        "editor_crc": h["editor_crc"], "editor_names": names,
        # S6: the release stamp as the writer's own reader spelled it — "" for the
        # fixture that names no release, so the sidecar carries both answers and a
        # reader that reports None, 0, or "v0.0.0" for the empty case fails here.
        "version": h["version"],
        "n_settings": len(settings), "n_assets": h["n_assets"],
        "asset_off": h["asset_off"], "geom_bytes": len(geom),
        "bytes": os.path.getsize(path),
    }



def main(argv):
    verify = "--verify" in argv
    region = editor_region()

    if verify:
        # Re-bake into a scratch dir and demand byte equality with what is committed.
        # The header's CRC covers the editor triple, so a region that drifted by an
        # mtime byte would change the container's checksum and the dial would refuse
        # the file this same tree wrote minutes earlier.
        import tempfile
        d = tempfile.mkdtemp()
        fail = 0
        for spec, reg in ((THEMED, region), (PLAIN, b"")):
            gen = bake(os.path.join(d, spec["slug"] + ".orbapp"), spec, reg)
            committed = os.path.join(FIX, os.path.basename(gen))
            if open(committed, "rb").read() != open(gen, "rb").read():
                print("FAIL %s is not reproducible from this tree"
                      % os.path.basename(gen))
                fail = 1
        print("== APP FIXTURE DETERMINISM: %s"
              % ("FAILED" if fail else "both fixtures reproduce byte for byte"))
        return 1 if fail else 0

    os.makedirs(FIX, exist_ok=True)
    themed = bake(os.path.join(FIX, "sentinel.orbapp"), THEMED, region)
    plain = bake(os.path.join(FIX, "plain.orbapp"), PLAIN)
    side = {
        "note": ("generated by tools/make_app_fixture.py; the fields under apps are "
                 "orbapp.read_container's own read-back of those bytes, and `words` is "
                 "the writer's own W_* table, which is what main/app/"
                 "orb_app_manifest.c and orbnet/appcontainer.go are pinned against. "
                 "Re-bake with the same script; --verify re-bakes and demands byte "
                 "equality rather than rewriting."),
        "writer_abi": orbapp.GEOM_ABI,
        "hdr_words": orbapp.HDR_WORDS,
        "hdr_bytes": orbapp.HDR_BYTES,
        # The word map, not a hand-typed copy of it. A reader test that hard-codes
        # indices can only catch a reader; this lets a test catch a WRITER change too,
        # which is the case that actually moves the checksum.
        "words": {n: getattr(orbapp, "W_" + n.upper()) for n in WORD_KEYS},
        "apps": {os.path.splitext(os.path.basename(p))[0]: read_back(p)
                 for p in (themed, plain)},
    }
    with open(SIDE, "w") as f:
        json.dump(side, f, indent=2, sort_keys=True)
        f.write("\n")
    for slug, d in sorted(side["apps"].items()):
        print("%-9s %6d B  caps=%-12s serial=%-9d region=%4d B  entries=%d"
              % (slug, d["bytes"], "|".join(d["caps_named"]) or "-", d["serial"],
                 d["editor_size"], len(d["editor_names"])))
    print("wrote %s" % SIDE)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
