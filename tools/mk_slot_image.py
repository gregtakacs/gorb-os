#!/usr/bin/env python3
"""mk_slot_image.py - rebase a FORMAT 3 card bundle into a flashable slot-A image (§26 P6.5).

A `.orbtheme` bundle puts its geometry at file +128 and its banks right after the
region. A device slot is laid out differently: header at +0, geometry at +0x1000
(SLOT_GEOM_OFF), banks at +0x10000 (SLOT_ART_OFF), each bank's art_off rebased to
the slot. The install door does that move at run time with orb_bundle_rebase() +
copy_bundle3() + build_slot_hdr3(); this tool does the SAME move offline so a FORMAT
3 default can be FLASHED into slot A (assets offset 0) instead of a FORMAT 2 blob.

It re-emits what the installer would write, built from the shipped Python writers
(orbbundle/orbtheme) so there is no second byte layout to drift. Output so
`esptool ... write_flash <assets_offset> out.img` lands a boot-readable slot A:

    +0x00000   128 B slot header (build_slot_hdr3's field map; word 8 = the padded
                                 art span's CRC, exactly what copy_bundle3 read back)
    +0x001000  the rebased region (geom_size B), sector-padded
    +0x010000  the bank span (art_size B), sector-padded
    everything else erased (0xFF)

Every checksum is zlib CRC-32 == esp_rom_crc32_le (orbtheme's rule).

  python3 tools/mk_slot_image.py IN.orbtheme OUT.img
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbtheme as _ot            # noqa: E402
import orbbundle as ob            # noqa: E402

SLOT_GEOM_OFF = 0x00001000         # orb_theme.c
SLOT_ART_OFF  = 0x00010000         # orb_theme.c
SLOT_CAP      = 0x00400000         # orb_theme.c
SECTOR        = _ot.SECTOR
HDR_BYTES     = _ot.W_HDR_CRC * 4 + 16   # 32 words * 4 == 128


def _up(n):
    return (n + SECTOR - 1) & ~(SECTOR - 1)


def slot_image(raw):
    h = ob.parse_header3(raw)
    region = bytearray(raw[h["geom_off"]:h["geom_off"] + h["geom_size"]])
    _n, recs = ob.unpack_index(region)
    delta = SLOT_ART_OFF - h["art_off"]

    art_end = SLOT_ART_OFF
    for i, r in enumerate(recs):
        moved = r["art_off"] + delta
        if moved > SLOT_CAP or r["art_size"] > SLOT_CAP - moved:
            raise SystemExit("refused: section bank would run past the end of the slot")
        r["art_off"] = moved
        rec = 8 + i * ob.IDX_WORDS * 4 + 24          # art_off = record's 7th word
        region[rec:rec + 4] = struct.pack("<I", moved & 0xFFFFFFFF)
        art_end = max(art_end, moved + r["art_size"])
    geom_crc = zlib.crc32(bytes(region)) & 0xFFFFFFFF

    art_len = _up(art_end - SLOT_ART_OFF)
    span = bytearray(b"\xFF" * art_len)
    for r in recs:
        f0 = r["art_off"] - delta                    # file offset this bank came from
        if r["art_size"]:
            span[r["art_off"] - SLOT_ART_OFF:
                 r["art_off"] - SLOT_ART_OFF + r["art_size"]] = raw[f0:f0 + r["art_size"]]
    art_crc = zlib.crc32(bytes(span)) & 0xFFFFFFFF   # == copy_bundle3's read-back crc

    hdr = [0] * 32
    hdr[0], hdr[1], hdr[2], hdr[3] = _ot.MAGIC, ob.FORMAT3, ob.GEOM_ABI3, HDR_BYTES
    hdr[4], hdr[5], hdr[6] = SLOT_GEOM_OFF, h["geom_size"], geom_crc
    hdr[7], hdr[8] = h["art_size"], art_crc
    hdr[9] = len(recs)                              # F3: section count where F2 had scr_w
    hdr[10] = h["geom_crc"]                         # F3: the CARD file's region crc (active match)
    hdr[13] = SLOT_ART_OFF
    _ot._setword(hdr, _ot.W_NAME, h["name"], _ot.NAME_MAX)
    hdr[_ot.W_HDR_CRC] = _ot._hdr_crc(hdr)
    hdr_bytes = b"".join(struct.pack("<I", v) for v in hdr)
    assert len(hdr_bytes) == 128

    total = SLOT_ART_OFF + art_len
    out = bytearray(b"\xFF" * total)
    out[0:128] = hdr_bytes
    out[SLOT_GEOM_OFF:SLOT_GEOM_OFF + len(region)] = region
    out[SLOT_ART_OFF:SLOT_ART_OFF + art_len] = span
    return bytes(out), {"name": h["name"], "geom_size": h["geom_size"],
                        "art_size": h["art_size"], "art_len": art_len,
                        "sections": len(recs), "geom_crc": geom_crc,
                        "art_crc": art_crc, "file_geom_crc": h["geom_crc"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bundle")
    ap.add_argument("out")
    a = ap.parse_args()
    raw = open(a.bundle, "rb").read()
    img, info = slot_image(raw)
    open(a.out, "wb").write(img)
    print("wrote %s: %d B, name %r, %d section(s), geom %d B (crc %08x), "
          "art %d B in %d B span (crc %08x), file geom crc %08x"
          % (a.out, len(img), info["name"], info["sections"], info["geom_size"],
             info["geom_crc"], info["art_size"], info["art_len"],
             info["art_crc"], info["file_geom_crc"]))


if __name__ == "__main__":
    main()
