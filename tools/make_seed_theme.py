#!/usr/bin/env python3
"""make_seed_theme.py \u2014 rebuild the hub's seed .orbtheme from the shipped slot-A image (D36).

D36 took the default clock theme out of the flash manifest and into the hub's
seed directory, so the same bytes reach the dial through the ordinary theme
doors (POST /api/v1/sd/theme, then /api/v1/sd/bundle stores it into slot A).
The shipped source of that art is ``assets/orb_default.img``: a FORMAT 3
slot-layout image emitted by tools/mk_slot_image.py (header at +0, geometry at
+0x1000, banks at +0x10000). This tool does the exact inverse move: it lifts
the geometry region and the bank span out of the slot image, un-rebases the
index record's art_off back to card coordinates, and lays the bundle out on
the card writer's byte map (header, region, sector-align pad, art). The card
name rides in the slot header's own name field (for the shipped default that
is \"av-kit\"), so the id the device files the theme under does not change with
the route.

No re-pack through unpack_blob: the av-kit rows are the C4 delta dialect, and
the one promise this tool needs is byte fidelity, not interpretation. The proof
is the forward trip \u2014 the emitted bundle, run through ``mk_slot_image.slot_image()``,
must reproduce the input image byte for byte \u2014 plus two stored-CRC cross-checks
the slot image carries: the region CRC over the UN-rebased bytes must equal
header word 10 (the card file's own region CRC, kept by mk_slot_image for
exactly this match), and the rebuilt span CRC must equal word 8.

  python3 tools/make_seed_theme.py                       # default img -> starter/orb-default.orbtheme
  python3 tools/make_seed_theme.py --check               # byte-compare the committed seed
  python3 tools/make_seed_theme.py IN.img OUT.orbtheme   # explicit paths
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orbtheme as _ot            # noqa: E402
import orbbundle as ob            # noqa: E402
import mk_slot_image as msi       # noqa: E402

ROOT = os.path.join(HERE, os.pardir)
SRC_IMG = os.path.join(ROOT, "assets", "orb_default.img")
OUT = os.path.join(ROOT, "tools", "orb-manager", "themes", "orb-default.orbtheme")

REC_BYTES = ob.IDX_WORDS * 4       # 40 B per record
REGION_HEAD = 8                    # n_sections + reserved
ART_OFF_IN_REC = 24               # the record's art_off field (pack <4H7I2H)


def bundle_from_slot(img):
    hdr = list(struct.unpack("<" + "I" * _ot.HDR_WORDS, img[:_ot.HDR_WORDS * 4]))
    if hdr[0] != _ot.MAGIC or hdr[1] != ob.FORMAT3:
        raise SystemExit("refused: the source image is not a FORMAT 3 slot header")
    geom_off, geom_size, region_crc = hdr[4], hdr[5], hdr[6]
    art_size, span_crc, n_sections, card_geom_crc = hdr[7], hdr[8], hdr[9], hdr[10]
    art_off_slot = hdr[13]
    name = _ot._wordstr(hdr, _ot.W_NAME, _ot.NAME_MAX)
    if geom_off != msi.SLOT_GEOM_OFF or art_off_slot != msi.SLOT_ART_OFF:
        raise SystemExit("refused: not a mk_slot_image layout (geom @0x%X art @0x%X)"
                         % (geom_off, art_off_slot))
    if n_sections != 1:
        raise SystemExit("refused: the seed names one section, this image carries %d" % n_sections)
    if len(img) < art_off_slot + art_size:
        raise SystemExit("refused: the bank span runs past the image")
    region = bytearray(img[geom_off:geom_off + geom_size])
    art = bytes(img[art_off_slot:art_off_slot + art_size])
    if zlib.crc32(bytes(region)) & 0xFFFFFFFF != region_crc:
        raise SystemExit("refused: the slot region does not CRC \u2014 damaged source image")
    # Word 8 is the PADDED-span CRC (copy_bundle3's read-back rule), so it is
    # not checked here; the forward trip below recomputes it and fails loudly
    # on a torn span.

    # Un-rebase the single record and pay the writer's own geometry CRC.
    rec0 = REGION_HEAD
    r = dict(zip(ob.IDX_ORDER, struct.unpack_from(ob.IDX_PACK, region, rec0)))
    if r["art_off"] != art_off_slot:
        raise SystemExit("refused: the single bank sits at %d, not the slot base" % r["art_off"])
    card_art_off = _ot.art_offset(geom_size)
    region[rec0 + ART_OFF_IN_REC:rec0 + ART_OFF_IN_REC + 4] = struct.pack("<I", card_art_off)
    geom_crc = zlib.crc32(bytes(region)) & 0xFFFFFFFF
    if geom_crc != card_geom_crc:
        raise SystemExit("refused: the un-rebased region CRC %08x is not the image's stored card CRC %08x"
                         % (geom_crc, card_geom_crc))

    hdr = ob.pack_header3(name, n_sections, geom_size, geom_crc, card_art_off,
                          art_size, zlib.crc32(art) & 0xFFFFFFFF, serial=0, baked_ms=0)
    out = bytearray(hdr)
    out += region
    out += b"\x00" * (card_art_off - len(out))
    out += art
    return bytes(out[:card_art_off + art_size]), name


def build(img_path):
    img = open(img_path, "rb").read()
    bundle, name = bundle_from_slot(img)
    # The forward trip is the contract: what the device's store door writes from
    # this bundle must be the very image we started from.
    rebuilt, info = msi.slot_image(bundle)
    if rebuilt != img:
        raise SystemExit("refused: the rebuilt bundle does not round-trip byte-identical "
                         "(name %r, geom %d B)" % (name, info["geom_size"]))
    return bundle


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    img_path = args[0] if args else SRC_IMG
    out_path = args[1] if len(args) > 1 else OUT
    bundle = build(img_path)
    if "--check" in sys.argv[1:]:
        have = open(out_path, "rb").read() if os.path.exists(out_path) else b""
        if have != bundle:
            sys.stderr.write("%s is stale \u2014 run tools/make_seed_theme.py\n"
                             % os.path.relpath(out_path, ROOT))
            return 1
        print("seed theme fresh (%d B, round-trips byte-identical to the slot image)"
              % len(bundle))
        return 0
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(bundle)
    print("wrote %s: %d B, round-trip verified against %s"
          % (os.path.relpath(out_path, ROOT), len(bundle), os.path.relpath(img_path, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
