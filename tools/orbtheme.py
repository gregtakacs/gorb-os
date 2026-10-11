#!/usr/bin/env python3
"""The `.orbtheme` container — one baked theme in one file, for the SD card.

Why this file exists at all: a bake has always emitted a PAIR,
`assets/clock_assets.bin` (the pixels, into the `assets` partition) and
`assets/clock_assets.h` (the geometry that describes them, COMPILED INTO THE
APP). A theme uploaded from a browser cannot change a compiled-in struct, so the
pair could never leave the bench. The container carries the geometry alongside
the pixels so the firmware can run a theme it was not built around.

The layout below is the single definition. main/art/orb_theme.c mirrors it field
for field, and tools/host_tz_test.py proves the mirror: it builds a container
with a distinct sentinel in every field through THIS module and asserts the
lifted C reader recovers each one. Reordering a field on one side only therefore
fails a test rather than shipping a dial with the hands' pivots in the overlay's
slots — which is the exact class of bug a struct-layout mismatch produces, and it
would look like bad art, not like a bug.

Wire format, all little-endian u32 words (every field widened to 32 bits, so
there is no padding rule for either side to disagree about):

  header   (HDR_WORDS words, pack_header)
  geometry (geom_size bytes at geom_off):
      n_rects, n_first, n_delays, n_bgframes        <- 4 count words
      clk_theme_t scalars per THEME_FIELDS
      hand[5], shadow[5], overlay, tourbillon[16]   <- SPRITE_FIELDS each
      rects      : n_rects * 5  (x, y, w, h, off)
      rect_first : n_first * 1
      gif_delays : n_delays * 1
      bg_frames  : n_bgframes * 1
  pixel payload (art_size bytes at align_up(geom_off + geom_size, 4096))

The payload is byte-identical to `assets/clock_assets.bin`, which is what keeps
one bake serving both paths: `idf.py flash` still takes the pair, the card takes
the container.
"""

import struct
import zlib

MAGIC         = 0x474F5254        # 'T','R','O','G' little-endian on the wire
FORMAT        = 1                 # this file's container layout
GEOM_ABI      = 1                 # the clock_view.c geometry struct it describes
SECTOR        = 4096
HDR_WORDS     = 32                # 128 B, the rest reserved for the next format
NAME_MAX      = 24
TITLE_MAX     = 32
SPRITE_FIELDS = ("w", "h", "offx", "offy", "pivotx", "pivoty",
                 "centerx", "centery", "blend", "show", "color_off",
                 "diffx", "diffy", "diffw", "diffh")

# Header word map. Kept in one place because slug and title are byte arrays
# spread over whole words, and their ranges must stay DISJOINT: the first draft
# of this put the 24-byte slug at word 14 (words 14..19) and the 32-byte title at
# word 18, so a long slug silently overwrote the head of the title. Two short
# strings hid it — tools/host_tz_test.py now fills both to their maximum, and
# HDR_WORDS is derived from the fields rather than fixed at 32, so the ranges
# cannot collide again without the size and the test both moving.
W_MAGIC     = 0
W_FORMAT    = 1
W_GEOM_ABI  = 2
W_HDR_SIZE  = 3
W_GEOM_OFF  = 4
W_GEOM_SIZE = 5
W_GEOM_CRC  = 6
W_ART_SIZE  = 7
W_ART_CRC   = 8
W_SCR_W     = 9
W_SCR_H     = 10
W_SERIAL    = 11
W_BAKED_MS  = 12
W_ART_OFF   = 13
W_SLUG      = 14                                  # NAME_MAX  bytes
W_TITLE     = W_SLUG  + ((NAME_MAX  + 3) // 4)    # TITLE_MAX bytes
W_HDR_CRC   = W_TITLE + ((TITLE_MAX + 3) // 4)    # crc32 over the whole header
HDR_WORDS   = W_HDR_CRC + 4                       # +4 words spare for v2


# clk_theme_t's scalars in clock_view.c's declaration order, as (name, type).
# The type only drives sign extension on read; every field is one u32 word on
# the wire. Keep this list and orb_theme.c's parse table in step — the sentinel
# test in tools/host_tz_test.py is what notices when they are not.
THEME_FIELDS = (
    ("bg", "u32"),
    ("plate_follow", "u8"), ("shadow_on", "u8"),
    ("shadow_dx", "i8"), ("shadow_dy", "i8"),
    ("second_sweep", "u8"), ("second_railway", "u8"),
    ("text_over_hands", "u8"), ("tick_rate", "u8"),
    ("bg_frames", "u16"), ("bg_fps", "i16"),
    ("bg_loop", "u8"), ("bg_every", "u16"),
    ("plate_off", "u32"),
    ("order0", "u8"), ("order1", "u8"), ("order2", "u8"),
    ("order3", "u8"), ("order4", "u8"),
    ("order_n", "u8"), ("matte_mask", "u8"),
    ("bg_mode", "u8"), ("bg_key_off", "u32"),
    ("gif_frames", "u16"), ("tourbillon_n", "u16"), ("tourbillon_fps", "u16"),
)


def _range(t):
    """The C field's own range for a declared wire type."""
    return {"i8": (-128, 127), "i16": (-32768, 32767), "i32": (-2**31, 2**31 - 1),
            "u8": (0, 255), "u16": (0, 65535), "u32": (0, 2**32 - 1)}[t]


def _u(v):
    """Any signed or unsigned int, wrapped into the u32 word the wire carries.

    Use _val() for anything that maps to a typed C field: wrapping a value that
    does not fit its own field is how -5 in an i8 becomes 4294967035 on the wire
    and -5 again on the device only by luck of the reader's mask.
    """
    return int(v) & 0xFFFFFFFF


def _val(what, t, v):
    """_u() with the range the C field actually has.

    The bake computes these numbers in Python and the device reads them as C
    types, so a value outside the field's range would be silently reinterpreted
    rather than reported: a shadow_dx of 1757 arrives as -35 and the shadow sits
    two thousand pixels off the dial. The baker's whole discipline is refusing to
    emit art it knows to be wrong, so it refuses here too.
    """
    v = int(v)
    lo, hi = _range(t)
    if v < lo or v > hi:
        raise ValueError("%s: %d does not fit a %s field (%d..%d) — the bake "
                         "computed something the firmware cannot store"
                         % (what, v, t, lo, hi))
    return v & 0xFFFFFFFF


def _sext(v, t):
    """Undo _u() for the signed types; unsigned types pass through.

    The mask first, then the extend: _u() wrapped the value into 32 bits on the
    way out, so an i8 field arrives as 0xFFFFFFFB, not as 0xFB. Extending without
    masking turns -5 into -2051 and the shadow offset lands two thousand pixels
    off the dial.
    """
    bits = {"i8": 8, "i16": 16, "i32": 32}.get(t)
    if bits is None:
        return v
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >= (1 << (bits - 1)) else v


def _sptype(f):
    """clk_sprite_t field types, for the same sign-extension reason."""
    return {"w": "u16", "h": "u16", "color_off": "u32", "blend": "u8",
            "show": "u8", "diffw": "u16", "diffh": "u16"}.get(f, "i16")


def blank_spr():
    return {f: 0 for f in SPRITE_FIELDS}


def pack_spr(rec):
    """One clk_sprite_t as len(SPRITE_FIELDS) u32s, in SPRITE_FIELDS order, each
    range-checked against the type the device reads it as."""
    return [_val("sprite." + f, _sptype(f), rec[f]) for f in SPRITE_FIELDS]


def pack_geom(scalars, hands, shadows, overlay, tourb,
              rects, rect_first, gif_delays, bg_frames):
    """The geometry blob, from the same values _finish() prints as C.

    scalars is keyed by THEME_FIELDS names; hands/shadows carry 5 records each
    and tourb up to 16 (short lists are padded with blanks so the reader always
    advances the same amount, exactly as the generated header does).
    """
    if len(rects) % 5:
        raise ValueError("rects must be (x, y, w, h, off) quintuples, got %d words"
                         % len(rects))
    w = [_val("n_rects", "u32", len(rects) // 5), _val("n_first", "u32", len(rect_first)),
         _val("n_delays", "u32", len(gif_delays)), _val("n_frames", "u32", len(bg_frames))]
    w += [_val(n, t, scalars[n]) for n, t in THEME_FIELDS]
    for h in hands:
        w += pack_spr(h)
    for s in shadows:
        w += pack_spr(s)
    w += pack_spr(overlay)
    for t in list(tourb)[:16]:
        w += pack_spr(t)
    for _ in range(16 - min(16, len(tourb))):
        w += pack_spr(blank_spr())
    for i, v in enumerate(rects):
        # x, y, w, h are uint16_t in clk_bg_rect_t; off is uint32_t.
        w.append(_val("rect[%d].%s" % (i // 5, ("x", "y", "w", "h", "off")[i % 5]),
                      "u32" if i % 5 == 4 else "u16", v))
    w += [_val("rect_first[%d]" % i, "u16", v) for i, v in enumerate(rect_first)]
    w += [_val("gif_delays[%d]" % i, "u16", v) for i, v in enumerate(gif_delays)]
    w += [_val("bg_frames[%d]" % i, "u32", v) for i, v in enumerate(bg_frames)]
    return b"".join(struct.pack("<I", v) for v in w)


def unpack_geom(b):
    """Inverse of pack_geom, as dicts. Used by the host test and --inspect."""
    w = list(struct.unpack("<" + "I" * (len(b) // 4), b))
    n_rects, n_first, n_delays, n_frames = w[:4]
    i = 4
    scalars = {}
    for n, t in THEME_FIELDS:
        scalars[n] = _sext(w[i], t); i += 1

    def take_spr():
        nonlocal i
        rec = {f: _sext(w[i + k], _sptype(f)) for k, f in enumerate(SPRITE_FIELDS)}
        i += len(SPRITE_FIELDS)
        return rec

    hands   = [take_spr() for _ in range(5)]
    shadows = [take_spr() for _ in range(5)]
    overlay = take_spr()
    tourb   = [take_spr() for _ in range(16)]
    rects   = w[i:i + n_rects * 5]; i += n_rects * 5
    first   = w[i:i + n_first];     i += n_first
    delays  = w[i:i + n_delays];    i += n_delays
    frames  = w[i:i + n_frames];    i += n_frames
    if i != len(w):
        raise ValueError("geometry blob has %d words left over at word %d"
                         % (len(w) - i, i))
    return {"scalars": scalars, "hands": hands, "shadows": shadows,
            "overlay": overlay, "tourbillon": tourb, "rects": rects,
            "rect_first": first, "gif_delays": delays, "bg_frames": frames}


def _wordstr(hdr, base, nbytes):
    out = bytearray()
    for i in range(nbytes):
        c = (hdr[base + i // 4] >> (8 * (i % 4))) & 0xFF
        if c == 0:
            break
        out.append(c)
    return out.decode("utf-8", "replace")


def _setword(hdr, base, text, nbytes):
    for i, c in enumerate(text.encode("utf-8")[:nbytes - 1]):
        hdr[base + i // 4] |= c << (8 * (i % 4))


def art_offset(geom_size):
    """Payload start for a given blob size, sector-aligned: the device mmaps it
    straight out of the `assets` partition, and an unaligned start costs an MMU
    page entry to round."""
    return (HDR_WORDS * 4 + geom_size + SECTOR - 1) & ~(SECTOR - 1)


def pack_header(slug, title, geom_size, art_size, geom_crc, art_crc,
                scr_w, scr_h, serial=0, baked_ms=0):
    hdr = [0] * HDR_WORDS
    hdr[0], hdr[1], hdr[2], hdr[3] = (_u(MAGIC), _u(FORMAT), _u(GEOM_ABI),
                                      _u(HDR_WORDS * 4))
    hdr[4] = _u(HDR_WORDS * 4)              # geom_off: the blob follows the header
    hdr[5], hdr[6] = _u(geom_size), _u(geom_crc)
    hdr[7], hdr[8] = _u(art_size), _u(art_crc)
    hdr[9], hdr[10] = _u(scr_w), _u(scr_h)
    hdr[11], hdr[12], hdr[13] = _u(serial), _u(baked_ms), _u(art_offset(geom_size))
    _setword(hdr, W_SLUG, slug, NAME_MAX)
    _setword(hdr, W_TITLE, title, TITLE_MAX)
    # The header checksum covers the whole 128 bytes with its own word zeroed, so
    # a torn header (a bad count, a wrong art_off) is caught by the 128 bytes read
    # first rather than after streaming a multi-megabyte payload to the card.
    hdr[W_HDR_CRC] = _hdr_crc(hdr)
    return b"".join(struct.pack("<I", v) for v in hdr)


def _hdr_crc(hdr):
    """crc32 over the header bytes with the checksum word itself zeroed."""
    z = list(hdr)
    z[W_HDR_CRC] = 0
    return zlib.crc32(b"".join(struct.pack("<I", v) for v in z)) & 0xFFFFFFFF


def write_container(path, slug, title, geom_bytes, art_bytes, scr_w, scr_h,
                    serial=0, baked_ms=0):
    """Emit one `.orbtheme`. The checksums are zlib CRC-32 — the same polynomial
    as esp_rom_crc32_le on the device — so a value computed here is checkable
    there without a second implementation free to drift."""
    hdr = pack_header(slug, title, len(geom_bytes), len(art_bytes),
                      zlib.crc32(geom_bytes) & 0xFFFFFFFF,
                      zlib.crc32(art_bytes) & 0xFFFFFFFF,
                      scr_w, scr_h, serial, baked_ms)
    out = bytearray(hdr)
    out += geom_bytes
    out += b"\0" * (art_offset(len(geom_bytes)) - len(out))
    out += art_bytes
    with open(path, "wb") as f:
        f.write(bytes(out))
    return len(out)


def parse_header(raw):
    """Decode a container header from the first bytes of a file.

    Raises ValueError with the reason, in words the page can show verbatim. The
    three rejections a person actually hits are a file that is not a container
    at all, one from a newer format, and one baked for a different geometry ABI.
    The last is the dangerous case — reading it anyway is a dial with garbage
    sprites — so it is refused, not repaired.
    """
    if len(raw) < HDR_WORDS * 4:
        raise ValueError("too short to hold a theme header")
    hdr = struct.unpack("<" + "I" * HDR_WORDS, raw[:HDR_WORDS * 4])
    if hdr[0] != MAGIC:
        raise ValueError("not an .orbtheme file")
    if hdr[1] != FORMAT:
        raise ValueError("theme format %d; this build reads format %d" % (hdr[1], FORMAT))
    if hdr[2] != GEOM_ABI:
        raise ValueError("theme geometry v%d; this build reads v%d — re-bake it"
                         % (hdr[2], GEOM_ABI))
    if hdr[W_HDR_CRC] != _hdr_crc(hdr):
        # Say "header" and not "file damaged": at this point not one byte of the
        # pixels has been looked at, and the difference is whether the owner
        # re-copies the file (a torn copy) or re-bakes it (a broken bake).
        raise ValueError("container header checksum mismatch — re-copy the file onto the card")
    return {"fmt": hdr[1], "abi": hdr[2], "hdr_size": hdr[3], "geom_off": hdr[4],
            "geom_size": hdr[5], "geom_crc": hdr[6], "art_size": hdr[7],
            "art_crc": hdr[8], "scr_w": hdr[9], "scr_h": hdr[10],
            "serial": hdr[W_SERIAL], "baked_ms": hdr[W_BAKED_MS], "art_off": hdr[W_ART_OFF],
            "slug": _wordstr(hdr, W_SLUG, NAME_MAX),
            "title": _wordstr(hdr, W_TITLE, TITLE_MAX)}


def read_container(path):
    """Whole-file read + both checksums. For the host test and the bench; the
    device streams instead, so it never holds a 3.7 MB theme in RAM."""
    with open(path, "rb") as f:
        raw = f.read()
    h = parse_header(raw)
    if h["geom_off"] + h["geom_size"] > len(raw):
        raise ValueError("geometry blob runs past the end of the file")
    if h["art_off"] + h["art_size"] > len(raw):
        raise ValueError("pixel payload runs past the end of the file")
    geom = raw[h["geom_off"]:h["geom_off"] + h["geom_size"]]
    art = raw[h["art_off"]:h["art_off"] + h["art_size"]]
    if zlib.crc32(geom) & 0xFFFFFFFF != h["geom_crc"]:
        raise ValueError("geometry checksum mismatch — the copy on the card is damaged")
    if zlib.crc32(art) & 0xFFFFFFFF != h["art_crc"]:
        raise ValueError("pixel checksum mismatch — the copy on the card is damaged")
    return h, geom, art


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 2 and sys.argv[1] == "--inspect":
        h, geom, art = read_container(sys.argv[2])
        g = unpack_geom(geom)
        print("%-24s '%s'  %dx%d  geom %d B  art %d B  serial %d"
              % (h["slug"], h["title"], h["scr_w"], h["scr_h"],
                 h["geom_size"], h["art_size"], h["serial"]))
        print("  hands show=%s  tourbillon_n=%d  gif_frames=%d  rects=%d  bg_frames=%d"
              % ([x["show"] for x in g["hands"]], g["scalars"]["tourbillon_n"],
                 g["scalars"]["gif_frames"], len(g["rects"]) // 5,
                 len(g["bg_frames"])))
    else:
        print("usage: python3 tools/orbtheme.py --inspect <file.orbtheme>")


